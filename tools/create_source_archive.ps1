param(
    [string] $OutputPath = "",
    [string] $Prefix = ""
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$repoRoot = (& git rev-parse --show-toplevel).Trim()
if ($LASTEXITCODE -ne 0 -or $repoRoot -eq "") {
    throw "Could not locate the repository root."
}

$cmakeLists = Join-Path $repoRoot "CMakeLists.txt"
$cmakeText = Get-Content -Raw -Path $cmakeLists
if ($cmakeText -notmatch "project\(\s*vnm_terminal\s+LANGUAGES\s+C\s+CXX\s+VERSION\s+([0-9]+(\.[0-9]+)*)\s*\)") {
    throw "Could not derive the vnm_terminal project version from CMakeLists.txt."
}
$version = $Matches[1]

if ($Prefix -eq "") {
    $Prefix = "vnm_terminal-$version/"
}
if (-not $Prefix.EndsWith("/")) {
    $Prefix += "/"
}

if ($OutputPath -eq "") {
    $OutputPath = Join-Path $repoRoot "dist/vnm_terminal_v${version}_source.zip"
}
$resolvedOutputPath = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath(
    $OutputPath)
$outputDirectory = Split-Path -Parent $resolvedOutputPath
if ($outputDirectory -ne "" -and -not (Test-Path -LiteralPath $outputDirectory)) {
    New-Item -ItemType Directory -Path $outputDirectory | Out-Null
}

& git -C $repoRoot archive `
    --format=zip `
    "--prefix=$Prefix" `
    "--output=$resolvedOutputPath" `
    HEAD
if ($LASTEXITCODE -ne 0) {
    throw "git archive failed."
}

Add-Type -AssemblyName System.IO.Compression.FileSystem
$archive = [System.IO.Compression.ZipFile]::OpenRead($resolvedOutputPath)
try {
    $forbidden = @(
        "(^|/)THIRD_PARTY/CMDG/CMDG/(bin|obj)(/|$)"
    )
    $badEntries = @()
    foreach ($entry in $archive.Entries) {
        foreach ($pattern in $forbidden) {
            if ($entry.FullName -match $pattern) {
                $badEntries += $entry.FullName
                break
            }
        }
    }
    if ($badEntries.Count -ne 0) {
        throw "Source archive contains forbidden entries: $($badEntries[0])"
    }
}
finally {
    $archive.Dispose()
}

Write-Host "Created clean source archive: $resolvedOutputPath"
