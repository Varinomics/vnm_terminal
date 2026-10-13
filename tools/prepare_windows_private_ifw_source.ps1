[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$IfwOutputRoot,
    [Parameter(Mandatory = $true)][string]$InputMaterialRoot
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$buildRoot = Join-Path $IfwOutputRoot 'build'
& (Join-Path $PSScriptRoot 'assert_windows_private_ifw.ps1') -IfwRoot $buildRoot
$provenancePath = Join-Path $buildRoot 'vnm-terminal-ifw-provenance.json'
$provenance = Get-Content -LiteralPath $provenancePath -Raw | ConvertFrom-Json
$inputReceiptPath = Join-Path $InputMaterialRoot 'input-provenance.json'
$inputReceipt = Get-Content -LiteralPath $inputReceiptPath -Raw | ConvertFrom-Json
$sourceLock = Get-Content -LiteralPath (Join-Path $InputMaterialRoot 'source-lock.json') -Raw | ConvertFrom-Json
$expectedLock = Get-Content -LiteralPath (Join-Path $repositoryRoot `
    'packaging/windows/private-ifw/source-lock.json') -Raw | ConvertFrom-Json
if ($sourceLock.qt_ifw.commit -cne $provenance.qt_ifw.commit -or
    $sourceLock.qt.commit -cne $expectedLock.qt.commit -or
    $sourceLock.ifw_dependencies.xz.commit -cne $expectedLock.ifw_dependencies.xz.commit -or
    $sourceLock.ifw_dependencies.bzip2.commit -cne $expectedLock.ifw_dependencies.bzip2.commit)
{
    throw 'The dependency source material does not match the installer inputs.'
}
foreach ($nativeInput in $provenance.native_inputs) {
    if (-not @($inputReceipt.files | Where-Object { $_.sha256 -ceq $nativeInput.sha256 }).Count) {
        throw "The dependency material does not identify a linked build input: $($nativeInput.path)"
    }
}
foreach ($entry in $inputReceipt.files) {
    if ($entry.path.StartsWith('source-material/', [StringComparison]::Ordinal)) {
        $path = Join-Path $InputMaterialRoot $entry.path.Substring('source-material/'.Length)
        if ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() -cne $entry.sha256) {
            throw "The dependency source material changed: $($entry.path)"
        }
    }
}
$ifwArchive = Join-Path $InputMaterialRoot 'sources/ifw.tar'
if ((Get-FileHash -LiteralPath $ifwArchive -Algorithm SHA256).Hash.ToLowerInvariant() -cne
    $provenance.source_archive.sha256)
{
    throw 'The supplied IFW source archive differs from the source used for the engine.'
}
$outputRoot = Join-Path $IfwOutputRoot 'source-material'
if (Test-Path -LiteralPath $outputRoot) { throw "Use a fresh source-material destination: $outputRoot" }
New-Item -ItemType Directory -Path $outputRoot | Out-Null
Copy-Item -LiteralPath (Join-Path $InputMaterialRoot 'sources') -Destination $outputRoot -Recurse
foreach ($directory in @('build-inputs', 'notices')) {
    $path = Join-Path $InputMaterialRoot $directory
    if (Test-Path -LiteralPath $path -PathType Container) {
        Copy-Item -LiteralPath $path -Destination $outputRoot -Recurse
    }
}
Copy-Item -LiteralPath $inputReceiptPath, $provenancePath -Destination $outputRoot
$toolsRoot = Join-Path $outputRoot 'tools'
$packagingRoot = Join-Path $outputRoot 'packaging/windows/private-ifw'
New-Item -ItemType Directory -Path $toolsRoot, $packagingRoot | Out-Null
foreach ($script in @('build_windows_private_ifw.ps1', 'assert_windows_private_ifw.ps1',
    'provision_windows_private_ifw_inputs.ps1', 'prepare_windows_private_ifw_source.ps1'))
{
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot $script) -Destination $toolsRoot
}
foreach ($file in @('source-lock.json', 'static-inputs.cmake', 'bzip2-static')) {
    Copy-Item -LiteralPath (Join-Path $InputMaterialRoot $file) -Destination $packagingRoot -Recurse
}
foreach ($file in @('qt-ifw-4.11-terminal.patch', 'README.md', 'RELINK-INSTRUCTIONS.txt')) {
    Copy-Item -LiteralPath (Join-Path $repositoryRoot "packaging/windows/private-ifw/$file") `
        -Destination $packagingRoot
}
Copy-Item -LiteralPath (Join-Path $repositoryRoot 'LICENSE') -Destination $outputRoot
Write-Host "Matching installer source material: $outputRoot"
