[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string]$IfwRoot
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$privateSourceRoot = Join-Path $repositoryRoot 'packaging\windows\private-ifw'
$sourceLock = Get-Content -LiteralPath (Join-Path $privateSourceRoot 'source-lock.json') -Raw | ConvertFrom-Json
$patchHash = (Get-FileHash -LiteralPath (Join-Path $privateSourceRoot 'qt-ifw-4.11-terminal.patch') `
    -Algorithm SHA256).Hash.ToLowerInvariant()
$provenancePath = Join-Path $IfwRoot 'vnm-terminal-ifw-provenance.json'
if (-not (Test-Path -LiteralPath $provenancePath -PathType Leaf)) {
    throw "Build the vnm_terminal installer engine with tools/build_windows_private_ifw.ps1: $IfwRoot"
}
$provenance = Get-Content -LiteralPath $provenancePath -Raw | ConvertFrom-Json
if ($provenance.schema_version -ne 1 -or $provenance.capability_id -cne 'vnm-terminal-installer' -or
    $provenance.qt_ifw.commit -cne $sourceLock.qt_ifw.commit -or
    $provenance.qt_ifw.version -cne $sourceLock.qt_ifw.version -or
    $provenance.qt_version -cne $sourceLock.qt_version -or $provenance.patch_sha256 -cne $patchHash)
{
    throw 'The installer engine provenance does not match the selected source and appearance patch.'
}
$expectedNames = @('binarycreator', 'devtool', 'installerbase', 'repogen')
if (@($provenance.artifacts).Count -ne $expectedNames.Count) {
    throw 'The installer engine provenance has an incomplete artifact inventory.'
}
foreach ($name in $expectedNames) {
    $relativePath = "bin/$name.exe"
    $record = @($provenance.artifacts | Where-Object { $_.path -ceq $relativePath })
    if ($record.Count -ne 1) { throw "The installer engine inventory must identify $relativePath once." }
    $path = Join-Path $IfwRoot $relativePath
    $file = Get-Item -LiteralPath $path
    if ($file.Length -ne $record[0].size -or
        (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() -cne $record[0].sha256)
    {
        throw "The installer engine binary differs from its provenance: $relativePath"
    }
}
Write-Host "Verified vnm_terminal installer engine: $IfwRoot"
