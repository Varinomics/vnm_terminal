param(
    [Parameter(Mandatory = $true)][string]$PayloadRoot,
    [Parameter(Mandatory = $true)][string]$OutputRoot,
    [string]$DebuggerPath = ''
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'windows_process_timeout_diagnostics.ps1')
$payload = (Resolve-Path -LiteralPath $PayloadRoot).Path
if (Test-Path -LiteralPath $OutputRoot) {
    throw "Argument smoke output must be unused: $OutputRoot"
}
$output = (New-Item -ItemType Directory -Path $OutputRoot).FullName
$reporter = Join-Path $output 'child argument reporter.ps1'
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'windows_argument_reporter.ps1') -Destination $reporter
$shell = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
$expectedArguments = @(
    "unicode-$([char]0x03bb)-$([char]0x4e2d)-$([char]::ConvertFromUtf32(0x1f600))",
    'alpha beta',
    'embedded"quote',
    'trailing slash\',
    ''
)
$entrypoints = @(
    @{ Name = 'runtime'; Path = (Join-Path $payload 'vnm_terminal_runtime\vnm_terminal.exe') },
    @{ Name = 'launcher'; Path = (Join-Path $payload 'vnm_terminal.exe') }
)
$failures = [Collections.Generic.List[string]]::new()
$results = [Collections.Generic.List[object]]::new()
:entrypoints foreach ($entrypoint in $entrypoints) {
    foreach ($exitCode in @(0, 23)) {
        $caseName = "$($entrypoint.Name)-exit-$exitCode"
        $caseDirectory = (New-Item -ItemType Directory -Path (Join-Path $output $caseName)).FullName
        $report = Join-Path $caseDirectory 'arguments.json'
        $capture = Join-Path $caseDirectory 'capture'
        $process = [Diagnostics.Process]::new()
        $started = $false
        $stdout = $null
        $stderr = $null
        $passed = $false
        $caseError = $null
        $processId = $null
        $supervisorExpired = $false
        $retirementRequested = $false
        $retired = $false
        $nativeProcess = $null
        $cleanupErrors = [Collections.Generic.List[string]]::new()
        try {
            $startInfo = [Diagnostics.ProcessStartInfo]::new()
            $startInfo.FileName = $entrypoint.Path
            $startInfo.UseShellExecute = $false
            $startInfo.CreateNoWindow = $true
            $startInfo.WindowStyle = [Diagnostics.ProcessWindowStyle]::Hidden
            $startInfo.RedirectStandardOutput = $true
            $startInfo.RedirectStandardError = $true
            $startInfo.Environment['VNM_TERMINAL_SETTINGS_NO_PERSIST'] = '1'
            $arguments = @(
                '--timeout-ms', '15000', '--require-output', '--capture-output', $capture, '--',
                $shell, '-NoLogo', '-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass',
                '-File', $reporter, $report, "$exitCode"
            ) + $expectedArguments
            foreach ($argument in $arguments) {
                $startInfo.ArgumentList.Add($argument)
            }
            $process.StartInfo = $startInfo
            $started = $process.Start()
            if (-not $started) {
                throw 'The packaged executable did not start.'
            }
            $processId = $process.Id
            $stdout = $process.StandardOutput.ReadToEndAsync()
            $stderr = $process.StandardError.ReadToEndAsync()
            if (-not $process.WaitForExit(30000)) {
                $supervisorExpired = $true
                try {
                    Save-WindowsProcessTimeoutEvidence -Process $process `
                        -OutputDirectory $caseDirectory -DebuggerPath $DebuggerPath
                } catch {
                    $cleanupErrors.Add("Timeout observation failed: $($_.Exception.Message)")
                }
                throw 'The packaged executable exceeded its supervisor deadline.'
            }
            if ($process.ExitCode -ne $exitCode) {
                throw "Expected exit $exitCode, got $($process.ExitCode)."
            }
            $received = @(Get-Content -LiteralPath $report -Raw | ConvertFrom-Json)
            if ($received.Count -ne $expectedArguments.Count) {
                throw "Expected $($expectedArguments.Count) child arguments, got $($received.Count)."
            }
            for ($index = 0; $index -lt $expectedArguments.Count; ++$index) {
                if ($received[$index] -cne $expectedArguments[$index]) {
                    throw "Child argument $index was changed."
                }
            }
            $capturedText = (@(Get-ChildItem -LiteralPath $caseDirectory -Filter 'capture.vnm-segment-*.raw' |
                ForEach-Object { [IO.File]::ReadAllText($_.FullName) }) -join '')
            if (-not $capturedText.Contains('VNM_PACKAGE_ARGUMENTS_PRESERVED')) {
                throw 'The identifiable child output was not captured.'
            }
            $passed = $true
        } catch {
            $message = "${caseName}: $($_.Exception.Message)"
            $caseError = $message
            $failures.Add($message)
            [Console]::Error.WriteLine($message)
        } finally {
            try {
                if ($started -and -not $process.HasExited) {
                    $retirementRequested = $true
                    $process.Kill($true)
                    if (-not $process.WaitForExit(5000)) {
                        throw 'The exact packaged process did not retire within five seconds.'
                    }
                }
                $retired = $started -and $process.HasExited
            } catch {
                $cleanupErrors.Add("Process retirement failed: $($_.Exception.Message)")
            }
            try {
                if ($stdout -and $stderr) {
                    $drained = [Threading.Tasks.Task]::WhenAll([Threading.Tasks.Task[]]@($stdout, $stderr))
                    if (-not $drained.Wait(5000)) {
                        throw 'Packaged process output did not drain within five seconds.'
                    }
                }
            } catch {
                $cleanupErrors.Add("Output drain failed: $($_.Exception.Message)")
            }
            foreach ($stream in @(@{ Name = 'stdout'; Task = $stdout }, @{ Name = 'stderr'; Task = $stderr })) {
                if ($stream.Task) {
                    try {
                        if (-not $stream.Task.IsCompletedSuccessfully) {
                            throw 'The stream is incomplete or faulted; its contents are unavailable.'
                        }
                        [IO.File]::WriteAllText((Join-Path $caseDirectory "$($stream.Name).txt"),
                            $stream.Task.GetAwaiter().GetResult())
                    } catch {
                        $cleanupErrors.Add("$($stream.Name) preservation failed: $($_.Exception.Message)")
                    }
                }
            }
            try {
                if ($started -and (-not $passed -or $cleanupErrors.Count)) {
                    $creation = $process.StartTime.ToUniversalTime()
                    $nativeProcess = [pscustomobject]@{
                        ProcessId = $processId
                        NativeCreateUtc = $creation.ToString('O')
                        NativeCreateFileTime100ns = [string]$creation.ToFileTimeUtc()
                        HasExited = $process.HasExited
                        ExitCode = $(if ($process.HasExited) { $process.ExitCode })
                        NativeExitUtc = $(if ($process.HasExited) { $process.ExitTime.ToUniversalTime().ToString('O') })
                    }
                }
            } catch {
                $cleanupErrors.Add("Native process state unavailable: $($_.Exception.Message)")
            } finally {
                $process.Dispose()
            }
            foreach ($cleanupError in $cleanupErrors) {
                $message = "${caseName}: $cleanupError"
                $caseError = (@($caseError, $message) -ne $null) -join [Environment]::NewLine
                $failures.Add($message)
                $passed = $false
                [Console]::Error.WriteLine($message)
            }
            $result = [pscustomobject]@{
                Case = $caseName
                Passed = $passed
                Error = $caseError
                SupervisorExpired = $supervisorExpired
                ProcessId = $processId
                RetirementRequested = $retirementRequested
                Retired = $retired
                NativeProcess = $nativeProcess
                Files = @(Get-ChildItem -LiteralPath $caseDirectory -File | Select-Object Name, Length, LastWriteTimeUtc)
            }
            $results.Add($result)
            $result | ConvertTo-Json -Depth 5 |
                Set-Content -LiteralPath (Join-Path $caseDirectory 'result.json') -Encoding utf8
            if ($passed) {
                Write-Host "PASS: $caseName preserved Unicode and quoted arguments, output, and exit status."
            }
        }
        if ($retirementRequested -and -not $retired) {
            break entrypoints
        }
    }
}
$results | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $output 'results.json') -Encoding utf8
if ($failures.Count -gt 0) {
    try {
        [pscustomobject]@{
            VerifierCommit = (& git -C $PSScriptRoot rev-parse HEAD)
            PayloadCommit = (& git -C $payload rev-parse HEAD)
            VerifierFiles = @($PSCommandPath, $reporter,
                (Join-Path $PSScriptRoot 'windows_process_timeout_diagnostics.ps1') |
                ForEach-Object { Get-FileHash -Algorithm SHA256 -LiteralPath $_ })
            PayloadFiles = @($entrypoints | ForEach-Object { Get-FileHash -Algorithm SHA256 -LiteralPath $_.Path })
            Debugger = $(if ($DebuggerPath) { Get-FileHash -Algorithm SHA256 -LiteralPath $DebuggerPath })
        } | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $output 'sources.json') -Encoding utf8
    } catch {
        $_.Exception.Message | Set-Content -LiteralPath (Join-Path $output 'source-observation-error.txt') -Encoding utf8
    }
    throw ($failures -join [Environment]::NewLine)
}
