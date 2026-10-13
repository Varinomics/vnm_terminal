#Requires -Version 7.2
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('Capture', 'Verify')][string]$Mode,
    [Parameter(Mandatory = $true)][string]$RecipeRoot,
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[0-9a-f]{40}$')][string]$PackagingCommit,
    [string]$SourceRoot = (Join-Path $PSScriptRoot '..')
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$SourceRoot = (Resolve-Path -LiteralPath $SourceRoot).Path
$RecipeRoot = [IO.Path]::GetFullPath($RecipeRoot)
$manifestName = 'packaging-recipe.json'
$manifestPath = Join-Path $RecipeRoot $manifestName

# This snapshot is the source consumed by both packaging stages. The application
# payload and native engine retain their own independent provenance.
$resourceDirectory = 'packaging/windows/ifw'
$files = @(
    'LICENSE',
    'THIRD_PARTY_NOTICES.md',
    'src/vnm_terminal.ico',
    'tools/build_windows_ifw_installer.ps1',
    'tools/assert_windows_private_ifw.ps1',
    'tools/windows_installer_recipe.ps1',
    'packaging/windows/private-ifw/source-lock.json',
    'packaging/windows/private-ifw/qt-ifw-4.11-terminal.patch'
)
$sourcePaths = $files + $resourceDirectory
$files += @(Get-ChildItem -LiteralPath (Join-Path $SourceRoot $resourceDirectory) -File -Recurse -Force |
    ForEach-Object { [IO.Path]::GetRelativePath($SourceRoot, $_.FullName).Replace('\', '/') })
$files = @($files | Sort-Object)

$actualCommit = & git -C $SourceRoot rev-parse --verify "$PackagingCommit^{commit}"
if ($LASTEXITCODE -ne 0 -or $actualCommit -cne $PackagingCommit) {
    throw 'The packaging commit is unavailable in the recipe source checkout.'
}
& git -C $SourceRoot diff --quiet $PackagingCommit -- @sourcePaths
if ($LASTEXITCODE -ne 0) { throw 'The installer recipe differs from its packaging commit.' }
$trackedFiles = @(& git -C $SourceRoot ls-tree -r --name-only $PackagingCommit -- @sourcePaths)
if ($LASTEXITCODE -ne 0 -or @(Compare-Object $files $trackedFiles).Count) {
    throw 'The installer recipe contains files outside its packaging commit.'
}
$sourceFiles = foreach ($relativePath in $files) {
    [ordered]@{
        path = $relativePath
        sha256 = (Get-FileHash -LiteralPath (Join-Path $SourceRoot $relativePath) `
            -Algorithm SHA256).Hash.ToLowerInvariant()
    }
}

if ($Mode -eq 'Capture') {
    if (Test-Path -LiteralPath $RecipeRoot) { throw "Use a fresh recipe directory: $RecipeRoot" }
    foreach ($file in $sourceFiles) {
        $destination = Join-Path $RecipeRoot $file.path
        New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
        Copy-Item -LiteralPath (Join-Path $SourceRoot $file.path) -Destination $destination
    }
    $manifest = [ordered]@{ packaging_commit = $PackagingCommit; files = @($sourceFiles) }
    [IO.File]::WriteAllText($manifestPath, ($manifest | ConvertTo-Json -Depth 5) + "`n",
        [Text.UTF8Encoding]::new($false))
}

$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
if ($manifest.packaging_commit -cne $PackagingCommit -or
    @($manifest.files).Count -ne $sourceFiles.Count) {
    throw 'The installer recipe identity differs from the selected packaging commit.'
}
$snapshotFiles = @(Get-ChildItem -LiteralPath $RecipeRoot -File -Recurse -Force |
    ForEach-Object { [IO.Path]::GetRelativePath($RecipeRoot, $_.FullName).Replace('\', '/') } |
    Where-Object { $_ -cne $manifestName })
if (@(Compare-Object $files $snapshotFiles).Count) {
    throw 'The installer recipe snapshot contains missing or unexpected files.'
}
foreach ($sourceFile in $sourceFiles) {
    $records = @($manifest.files | Where-Object { $_.path -ceq $sourceFile.path })
    if ($records.Count -ne 1 -or $records[0].sha256 -cne $sourceFile.sha256 -or
        (Get-FileHash -LiteralPath (Join-Path $RecipeRoot $sourceFile.path) `
            -Algorithm SHA256).Hash.ToLowerInvariant() -cne $sourceFile.sha256) {
        throw "The installer recipe file differs from its selected source: $($sourceFile.path)"
    }
}
Write-Host "Verified installer recipe $PackagingCommit at $RecipeRoot"
