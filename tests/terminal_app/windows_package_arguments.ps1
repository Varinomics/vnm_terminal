param(
    [Parameter(Mandatory = $true)][string]$PayloadRoot,
    [Parameter(Mandatory = $true)][string]$OutputRoot
)

$ErrorActionPreference = 'Stop'
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
foreach ($entrypoint in $entrypoints) {
    foreach ($exitCode in @(0, 23)) {
        $caseName = "$($entrypoint.Name)-exit-$exitCode"
        $caseDirectory = (New-Item -ItemType Directory -Path (Join-Path $output $caseName)).FullName
        $report = Join-Path $caseDirectory 'arguments.json'
        $capture = Join-Path $caseDirectory 'capture'
        $process = [Diagnostics.Process]::new()
        $started = $false
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
            $stdout = $process.StandardOutput.ReadToEndAsync()
            $stderr = $process.StandardError.ReadToEndAsync()
            if (-not $process.WaitForExit(30000)) {
                throw 'The packaged executable exceeded its supervisor deadline.'
            }
            [IO.File]::WriteAllText((Join-Path $caseDirectory 'stdout.txt'), $stdout.GetAwaiter().GetResult())
            [IO.File]::WriteAllText((Join-Path $caseDirectory 'stderr.txt'), $stderr.GetAwaiter().GetResult())
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
            $results.Add([pscustomobject]@{ Case = $caseName; Passed = $true })
            Write-Host "PASS: $caseName preserved Unicode and quoted arguments, output, and exit status."
        } catch {
            $message = "${caseName}: $($_.Exception.Message)"
            $failures.Add($message)
            $results.Add([pscustomobject]@{ Case = $caseName; Passed = $false; Error = $message })
            [Console]::Error.WriteLine($message)
        } finally {
            if ($started -and -not $process.HasExited) {
                $process.Kill($true)
                if (-not $process.WaitForExit(5000)) {
                    throw "Could not retire timed-out package smoke process: $caseName"
                }
            }
            $process.Dispose()
        }
    }
}
$results | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $output 'results.json') -Encoding utf8
if ($failures.Count -gt 0) {
    throw ($failures -join [Environment]::NewLine)
}
