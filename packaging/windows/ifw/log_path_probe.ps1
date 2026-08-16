[CmdletBinding()]
param(
    [string[]]$CandidatePath
)

$ErrorActionPreference = 'Stop'

if (-not $PSBoundParameters.ContainsKey('CandidatePath')) {
    $CandidatePath = @(
        [Environment]::GetFolderPath(
            [Environment+SpecialFolder]::LocalApplicationData),
        [IO.Path]::GetTempPath(),
        [Environment]::GetFolderPath(
            [Environment+SpecialFolder]::UserProfile)
    )
}

foreach ($candidate in $CandidatePath) {
    $logPath = $null
    $logStream = $null
    try {
        if ([string]::IsNullOrWhiteSpace($candidate) -or
            -not [IO.Path]::IsPathRooted($candidate) -or
            -not [IO.Directory]::Exists($candidate)) {
            continue
        }

        $absoluteCandidate = [IO.Path]::GetFullPath($candidate)
        if (-not [IO.Path]::IsPathRooted($absoluteCandidate)) {
            continue
        }

        $logDirectory = Join-Path $absoluteCandidate 'Varinomics\vnm_terminal'
        [void][IO.Directory]::CreateDirectory($logDirectory)

        # A run that is closed before installing leaves its proven file empty,
        # and IFW never removes it, so these accumulate for every cancelled
        # launch. Reclaim them before adding another. An exclusive open proves
        # the file belongs to no installer that is still running, and a file
        # with content is a diagnostic log that must survive. Losing this race
        # only costs a reclaim: IFW recreates a missing log when it appends.
        foreach ($staleLog in [IO.Directory]::GetFiles(
            $logDirectory, 'InstallationLog-*.txt'))
        {
            if ([IO.Path]::GetFileName($staleLog) -notmatch
                '^InstallationLog-[0-9a-f]{32}\.txt$') {
                continue
            }

            $staleStream = $null
            try {
                $staleStream = [IO.FileStream]::new(
                    $staleLog,
                    [IO.FileMode]::Open,
                    [IO.FileAccess]::ReadWrite,
                    [IO.FileShare]::None)
                $staleIsEmpty = $staleStream.Length -eq 0
                $staleStream.Dispose()
                $staleStream = $null
                if ($staleIsEmpty) {
                    [IO.File]::Delete($staleLog)
                }
            }
            catch {
            }
            finally {
                if ($null -ne $staleStream) {
                    $staleStream.Dispose()
                }
            }
        }

        $logPath = Join-Path $logDirectory (
            'InstallationLog-{0}.txt' -f [Guid]::NewGuid().ToString('N'))
        $logStream = [IO.FileStream]::new(
            $logPath,
            [IO.FileMode]::CreateNew,
            [IO.FileAccess]::Write,
            [IO.FileShare]::None)
        $logStream.Flush($true)
        $logStream.Dispose()
        $logStream = $null

        [Console]::Out.WriteLine($logPath)
        exit 0
    }
    catch {
        continue
    }
    finally {
        if ($null -ne $logStream) {
            $logStream.Dispose()
        }
        if ($null -ne $logStream -and
            $null -ne $logPath -and
            [IO.File]::Exists($logPath)) {
            [IO.File]::Delete($logPath)
        }
    }
}

exit 4
