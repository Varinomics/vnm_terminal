[CmdletBinding()]
param(
    [string]$ArchivePath,
    [string]$DestinationPath
)

$ErrorActionPreference = 'Stop'

# The Qt IFW identity is release metadata: the Windows CI cache key and the
# packaging contract test have to agree with the archive this script verifies,
# so all of them read release/manifest.json instead of restating it.
$repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$releaseManifest =
    Get-Content -LiteralPath (Join-Path $repositoryRoot 'release\manifest.json') -Raw |
        ConvertFrom-Json
if ($releaseManifest.schema -ne 1) {
    throw "Unsupported release manifest schema $($releaseManifest.schema)."
}
$ifwVersion = [string]$releaseManifest.qt_ifw.version
$ifwArchiveUrl = [string]$releaseManifest.qt_ifw.archive_url
$ifwArchiveSha256 = [string]$releaseManifest.qt_ifw.archive_sha256

function Get-Sha256FileHash {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    $sha256 = [System.Security.Cryptography.SHA256]::Create()
    try {
        $stream = [System.IO.File]::OpenRead($Path)
        try {
            $hashBytes = $sha256.ComputeHash($stream)
            return ([System.BitConverter]::ToString($hashBytes)).Replace('-', '').ToLowerInvariant()
        }
        finally {
            $stream.Dispose()
        }
    }
    finally {
        $sha256.Dispose()
    }
}

function Get-SevenZipPath {
    $sevenZip = Get-Command 7z.exe -ErrorAction SilentlyContinue
    if ($sevenZip) {
        return $sevenZip.Source
    }

    $standardPath = 'C:\Program Files\7-Zip\7z.exe'
    if (-not (Test-Path -LiteralPath $standardPath -PathType Leaf)) {
        throw "7-Zip command-line tool was not found: $standardPath"
    }
    return $standardPath
}

function Invoke-IfwDownload {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    $curl = Get-Command curl.exe -ErrorAction SilentlyContinue
    if (-not $curl) {
        throw 'curl.exe is required to download Qt Installer Framework.'
    }

    $parentPath = Split-Path -Parent $Path
    [void](New-Item -ItemType Directory -Force -Path $parentPath)
    $partPath = "$Path.part"
    Remove-Item -LiteralPath $partPath -Force -ErrorAction SilentlyContinue

    Write-Host "Downloading Qt Installer Framework $ifwVersion..."
    & $curl.Source `
        --fail `
        --location `
        --retry 4 `
        --retry-all-errors `
        --retry-delay 2 `
        --connect-timeout 30 `
        --max-time 600 `
        --output $partPath `
        $ifwArchiveUrl
    if ($LASTEXITCODE -ne 0) {
        Remove-Item -LiteralPath $partPath -Force -ErrorAction SilentlyContinue
        throw "Qt IFW download failed with exit code $LASTEXITCODE"
    }

    Move-Item -LiteralPath $partPath -Destination $Path -Force
}

function Assert-IfwRoot {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    $binaryCreatorPath = Join-Path $Path 'bin\binarycreator.exe'
    $installerBasePath = Join-Path $Path 'bin\installerbase.exe'
    if (-not (Test-Path -LiteralPath $binaryCreatorPath -PathType Leaf)) {
        throw "Qt IFW binarycreator was not found after extraction: $binaryCreatorPath"
    }
    if (-not (Test-Path -LiteralPath $installerBasePath -PathType Leaf)) {
        throw "Qt IFW installerbase was not found after extraction: $installerBasePath"
    }

    $binaryCreatorHelp = & $binaryCreatorPath --help 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0 -or $binaryCreatorHelp -notmatch 'Usage:\s+binarycreator') {
        throw 'The extracted Qt IFW binarycreator is not executable.'
    }

    $versionOutput = & $installerBasePath --version 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0 -or
        $versionOutput -notmatch "IFW Version:\s*$([regex]::Escape($ifwVersion))") {
        throw "The extracted Qt IFW root is not version $ifwVersion"
    }
}

if (-not $ArchivePath) {
    $ArchivePath = Join-Path $repositoryRoot "build_ifw\qt-ifw-$ifwVersion\ifw-win-x64.7z"
}
if (-not $DestinationPath) {
    $DestinationPath = Join-Path $repositoryRoot "build_ifw\qt-ifw-$ifwVersion\root"
}
$ArchivePath = [System.IO.Path]::GetFullPath($ArchivePath)
$DestinationPath = [System.IO.Path]::GetFullPath($DestinationPath)

if (-not (Test-Path -LiteralPath $ArchivePath -PathType Leaf)) {
    Invoke-IfwDownload $ArchivePath
}

$archiveHash = Get-Sha256FileHash $ArchivePath
if ($archiveHash -ne $ifwArchiveSha256) {
    throw "Qt IFW archive checksum mismatch: expected $ifwArchiveSha256, got $archiveHash"
}

$destinationParent = Split-Path -Parent $DestinationPath
[void](New-Item -ItemType Directory -Force -Path $destinationParent)
$stagingPath = Join-Path $destinationParent (
    ([System.IO.Path]::GetFileName($DestinationPath)) +
    '.staging-' + [Guid]::NewGuid().ToString('N'))
$backupPath = Join-Path $destinationParent (
    ([System.IO.Path]::GetFileName($DestinationPath)) +
    '.backup-' + [Guid]::NewGuid().ToString('N'))
$extractionOutputPath = "$stagingPath.stdout.log"

try {
    [void](New-Item -ItemType Directory -Path $stagingPath)
    $sevenZipPath = Get-SevenZipPath
    & $sevenZipPath x $ArchivePath "-o$stagingPath" -y *> $extractionOutputPath
    $extractionExitCode = $LASTEXITCODE
    if ($extractionExitCode -ne 0) {
        throw "Qt IFW extraction failed with exit code $extractionExitCode"
    }

    Assert-IfwRoot $stagingPath
    $marker = [ordered]@{
        version = $ifwVersion
        archive_url = $ifwArchiveUrl
        archive_sha256 = $archiveHash
    }
    [System.IO.File]::WriteAllText(
        (Join-Path $stagingPath '.vnm-ifw-provisioned.json'),
        ($marker | ConvertTo-Json) + "`n",
        [System.Text.UTF8Encoding]::new($false))

    $hadDestination = Test-Path -LiteralPath $DestinationPath
    if ($hadDestination) {
        Move-Item -LiteralPath $DestinationPath -Destination $backupPath
    }
    try {
        Move-Item -LiteralPath $stagingPath -Destination $DestinationPath
    }
    catch {
        if ($hadDestination -and
            -not (Test-Path -LiteralPath $DestinationPath) -and
            (Test-Path -LiteralPath $backupPath)) {
            Move-Item -LiteralPath $backupPath -Destination $DestinationPath
        }
        throw
    }
    if ($hadDestination) {
        Remove-Item -LiteralPath $backupPath -Recurse -Force
    }
}
finally {
    Remove-Item -LiteralPath $stagingPath -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $extractionOutputPath -Force -ErrorAction SilentlyContinue
}

Write-Host "Qt IFW $ifwVersion provisioned at $DestinationPath"
