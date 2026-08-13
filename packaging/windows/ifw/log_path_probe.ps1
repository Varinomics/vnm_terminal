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
