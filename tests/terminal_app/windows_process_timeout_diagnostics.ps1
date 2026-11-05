function Save-WindowsNativeStack {
    param(
        [Parameter(Mandatory = $true)][Diagnostics.Process]$Process,
        [Parameter(Mandatory = $true)][string]$DebuggerPath,
        [Parameter(Mandatory = $true)][string]$OutputPrefix
    )

    $commands = "$OutputPrefix.gdb"
    @(
        'show version',
        'set pagination off',
        'set confirm off',
        'set print frame-arguments none',
        "attach $($Process.Id)",
        'info threads',
        'thread apply all bt',
        'info sharedlibrary',
        'detach'
    ) | Set-Content -LiteralPath $commands -Encoding utf8
    $debugger = [Diagnostics.Process]::new()
    $started = $false
    try {
        $info = [Diagnostics.ProcessStartInfo]::new()
        $info.FileName = $DebuggerPath
        $info.UseShellExecute = $false
        $info.CreateNoWindow = $true
        $info.WindowStyle = [Diagnostics.ProcessWindowStyle]::Hidden
        $info.RedirectStandardOutput = $true
        $info.RedirectStandardError = $true
        $info.Environment['DEBUGINFOD_URLS'] = ''
        foreach ($argument in @('-nx', '-batch', '-x', $commands)) {
            $info.ArgumentList.Add($argument)
        }
        $debugger.StartInfo = $info
        $started = $debugger.Start()
        if (-not $started) {
            throw 'The native stack debugger did not start.'
        }
        $stdout = $debugger.StandardOutput.ReadToEndAsync()
        $stderr = $debugger.StandardError.ReadToEndAsync()
        $completed = $debugger.WaitForExit(10000)
        if (-not $completed) {
            $debugger.Kill($true)
            if (-not $debugger.WaitForExit(3000)) {
                throw 'The exact native stack debugger could not be retired.'
            }
        }
        $drained = [Threading.Tasks.Task]::WhenAll([Threading.Tasks.Task[]]@($stdout, $stderr))
        if (-not $drained.Wait(3000)) {
            throw 'The native stack debugger output did not drain.'
        }
        $text = $stdout.GetAwaiter().GetResult()
        [IO.File]::WriteAllText("$OutputPrefix.stdout.txt", $text)
        [IO.File]::WriteAllText("$OutputPrefix.stderr.txt", $stderr.GetAwaiter().GetResult())
        return [pscustomobject]@{
            ProcessId = $Process.Id
            Completed = $completed
            DebuggerExitCode = $debugger.ExitCode
            HasStack = ($text -match '(?m)^#0\s')
        }
    } finally {
        if ($started -and -not $debugger.HasExited) {
            $debugger.Kill($true)
            [void]$debugger.WaitForExit(3000)
        }
        $debugger.Dispose()
    }
}

function Save-WindowsProcessTimeoutEvidence {
    param(
        [Parameter(Mandatory = $true)][Diagnostics.Process]$Process,
        [Parameter(Mandatory = $true)][string]$OutputDirectory,
        [string]$DebuggerPath = ''
    )

    # Observation begins after the original supervisor failure so it cannot
    # change the command's pre-deadline startup or event-loop timing.
    for ($sample = 0; $sample -lt 2; ++$sample) {
        $prefix = Join-Path $OutputDirectory "timeout-$sample"
        $owned = @()
        $observationErrors = [ordered]@{}
        try {
            $Process.Refresh()
            $threads = @()
            if (-not $Process.HasExited) {
                foreach ($thread in $Process.Threads) {
                    try {
                        $threads += [pscustomobject]@{
                            Id = $thread.Id
                            State = "$($thread.ThreadState)"
                            WaitReason = $(if ($thread.ThreadState -eq 'Wait') { "$($thread.WaitReason)" })
                            CpuTimeMs = $thread.TotalProcessorTime.TotalMilliseconds
                        }
                    } catch {
                        $threads += [pscustomobject]@{ ObservationError = $_.Exception.Message }
                    }
                }
            }
            [pscustomobject]@{
                TimeUtc = [DateTime]::UtcNow
                ProcessId = $Process.Id
                HasExited = $Process.HasExited
                CpuTimeMs = $Process.TotalProcessorTime.TotalMilliseconds
                Threads = $threads
                Files = @(Get-ChildItem -LiteralPath $OutputDirectory -File |
                    Select-Object Name, Length, LastWriteTimeUtc)
            } | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath "$prefix.json" -Encoding utf8
        } catch {
            $observationErrors['state'] = $_.Exception.Message
        }
        try {
            if ($DebuggerPath -and -not $Process.HasExited) {
                $stack = Save-WindowsNativeStack -Process $Process `
                    -DebuggerPath $DebuggerPath -OutputPrefix $prefix
                $stack | ConvertTo-Json | Set-Content -LiteralPath "$prefix.stack-result.json" -Encoding utf8
            }
        } catch {
            $observationErrors['stack'] = $_.Exception.Message
        }
        try {
            if (-not $Process.HasExited) {
                $native = @(Get-CimInstance -ClassName Win32_Process -OperationTimeoutSec 3 `
                    -Property ProcessId, ParentProcessId, Name, ExecutablePath, CreationDate)
                $ownedIds = [Collections.Generic.HashSet[uint32]]::new()
                [void]$ownedIds.Add([uint32]$Process.Id)
                $startTime = $Process.StartTime.ToUniversalTime()
                do {
                    $added = $false
                    foreach ($candidate in $native) {
                        if ($ownedIds.Contains([uint32]$candidate.ParentProcessId) -and
                            $candidate.CreationDate.ToUniversalTime() -ge $startTime -and
                            $ownedIds.Add([uint32]$candidate.ProcessId)) {
                            $added = $true
                        }
                    }
                } while ($added)
                $owned = @($native | Where-Object { $ownedIds.Contains([uint32]$_.ProcessId) } |
                    Select-Object ProcessId, ParentProcessId, Name, ExecutablePath, CreationDate)
                $owned | ConvertTo-Json -Depth 4 |
                    Set-Content -LiteralPath "$prefix.owned-processes.json" -Encoding utf8
            }
        } catch {
            $observationErrors['descendants'] = $_.Exception.Message
        }
        foreach ($hostProcess in @($owned | Where-Object Name -eq 'OpenConsole.exe' | Select-Object -First 1)) {
            try {
                if ($DebuggerPath) {
                    $hostHandle = [Diagnostics.Process]::GetProcessById($hostProcess.ProcessId)
                    try {
                        if ([Math]::Abs(($hostHandle.StartTime.ToUniversalTime() -
                                $hostProcess.CreationDate.ToUniversalTime()).TotalMilliseconds) -lt 100) {
                            $hostStack = Save-WindowsNativeStack -Process $hostHandle `
                                -DebuggerPath $DebuggerPath -OutputPrefix "$prefix.openconsole"
                            $hostStack | ConvertTo-Json |
                                Set-Content -LiteralPath "$prefix.openconsole.stack-result.json" -Encoding utf8
                        }
                    } finally {
                        $hostHandle.Dispose()
                    }
                }
            } catch {
                $observationErrors['openconsole'] = $_.Exception.Message
            }
        }
        foreach ($stage in $observationErrors.Keys) {
            try {
                $observationErrors[$stage] | Set-Content -LiteralPath "$prefix.$stage-error.txt" -Encoding utf8
            } catch {
                [Console]::Error.WriteLine("Timeout $stage evidence unavailable: $($observationErrors[$stage])")
            }
        }
        if ($sample -eq 0 -and -not $Process.HasExited) {
            Start-Sleep -Seconds 1
        }
    }
}
