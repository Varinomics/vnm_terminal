[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$QtIfwRepositoryPath,
    [Parameter(Mandatory = $true)][string]$QtRoot,
    [Parameter(Mandatory = $true)][string]$XzRoot,
    [Parameter(Mandatory = $true)][string]$Bzip2Root,
    [Parameter(Mandatory = $true)][string]$OutputRoot,
    [string]$InputMaterialRoot,
    [string]$JomPath = 'C:\Qt\Tools\QtCreator\bin\jom\jom.exe',
    [ValidateRange(1, 4)][int]$Jobs = 4,
    [switch]$HostedBuild
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Invoke-Checked {
    param([string]$Program, [string[]]$Arguments)
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$Program failed with exit code $LASTEXITCODE"
    }
}

function Get-Identity {
    param([string]$Path)
    $file = Get-Item -LiteralPath $Path
    return [ordered]@{
        path = $file.FullName
        size = $file.Length
        sha256 = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
    }
}

if ($HostedBuild -and ($env:GITHUB_ACTIONS -cne 'true' -or $Jobs -ne 1)) {
    throw 'HostedBuild is reserved for serial GitHub Actions builds; use -Jobs 1.'
}

$repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$patchPath = Join-Path $repositoryRoot 'packaging\windows\private-ifw\qt-ifw-4.11-terminal.patch'
$sourceLockPath = Join-Path $repositoryRoot 'packaging\windows\private-ifw\source-lock.json'
$sourceLock = Get-Content -LiteralPath $sourceLockPath -Raw | ConvertFrom-Json
$ifwRepository = (Resolve-Path -LiteralPath $QtIfwRepositoryPath).Path
$QtRoot = (Resolve-Path -LiteralPath $QtRoot).Path
$XzRoot = (Resolve-Path -LiteralPath $XzRoot).Path
$Bzip2Root = (Resolve-Path -LiteralPath $Bzip2Root).Path
$OutputRoot = [IO.Path]::GetFullPath($OutputRoot)
if (Test-Path -LiteralPath $OutputRoot) {
    throw "Use a fresh output directory: $OutputRoot"
}
if (-not $HostedBuild) {
    Invoke-Checked queued-build @('--check-xenia')
}

# qmake's generated MSVC Makefiles require the same developer environment at
# generation and build time. Keep it in this process without recording secrets.
$visualStudioRoot = 'C:\Program Files\Microsoft Visual Studio\18\Community'
if ($HostedBuild) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    $visualStudioRoot = & $vswhere -latest -version '[18,19)' -products '*' `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ($LASTEXITCODE -ne 0 -or -not $visualStudioRoot) { throw 'Visual Studio 2026 was not found.' }
}
$vcvarsPath = Join-Path $visualStudioRoot 'VC\Auxiliary\Build\vcvarsall.bat'
$developerEnvironment = & cmd.exe /d /c ('call "' + $vcvarsPath + '" x64 -vcvars_ver=14.51 >nul && set')
if ($LASTEXITCODE -ne 0) { throw 'The Visual Studio 2026 x64 environment could not be loaded.' }
foreach ($line in $developerEnvironment) {
    if ($line -match '^([^=]+)=(.*)$') {
        [Environment]::SetEnvironmentVariable($matches[1], $matches[2], 'Process')
    }
}
$compilerPath = (Get-Command cl.exe -ErrorAction Stop).Source
if (-not $compilerPath.StartsWith($visualStudioRoot + '\', [StringComparison]::OrdinalIgnoreCase) -or
    $env:VCToolsVersion -notlike '14.51.*' -or $env:VSCMD_ARG_TGT_ARCH -ne 'x64')
{
    throw 'The private installer requires Visual Studio 2026 x64 and MSVC v145.'
}

$qmakePath = Join-Path $QtRoot 'bin\qmake.exe'
$qtVersion = (& $qmakePath -query QT_VERSION).Trim()
if ($LASTEXITCODE -ne 0 -or $qtVersion -ne $sourceLock.qt_version) {
    throw "The static Qt prefix must provide Qt $($sourceLock.qt_version)."
}
$qtConfiguration = Get-Content -LiteralPath (Join-Path $QtRoot 'mkspecs\qconfig.pri') -Raw
if ($qtConfiguration -notmatch '(?m)^QT_CONFIG\s*\+?=.*\bstatic\b') {
    throw 'The installer requires a static Qt build.'
}

$nativePaths = @(
    (Join-Path $QtRoot 'lib\Qt6BundledZLIB.lib'),
    (Join-Path $QtRoot 'include\QtZlib\zlib.h'),
    (Join-Path $XzRoot 'lib\lzma.lib'),
    (Join-Path $XzRoot 'include\lzma.h'),
    (Join-Path $Bzip2Root 'lib\bz2.lib'),
    (Join-Path $Bzip2Root 'include\bzlib.h')
)
$nativePaths += @(Get-ChildItem -LiteralPath (Join-Path $QtRoot 'lib') -Filter '*.lib' -File |
    ForEach-Object { $_.FullName })
$nativePaths += @(Get-ChildItem -LiteralPath (Join-Path $QtRoot 'plugins') -Filter '*.lib' -File -Recurse |
    ForEach-Object { $_.FullName })
$nativeInputs = @($nativePaths | Sort-Object -Unique | ForEach-Object { Get-Identity $_ })
$patchIdentity = Get-Identity $patchPath

$sourceRoot = Join-Path $OutputRoot 'source'
$buildRoot = Join-Path $OutputRoot 'build'
$inputsRoot = Join-Path $OutputRoot 'inputs'
New-Item -ItemType Directory -Path $sourceRoot, $buildRoot, $inputsRoot | Out-Null
$sourceArchive = Join-Path $inputsRoot 'qt-ifw-source.tar'
$gitArguments = @('-c', "safe.directory=$($ifwRepository.Replace('\', '/'))", '-C', $ifwRepository)
$actualTree = & git @gitArguments rev-parse "$($sourceLock.qt_ifw.commit)^{tree}"
if ($LASTEXITCODE -ne 0 -or $actualTree -ne $sourceLock.qt_ifw.tree) {
    throw 'The Qt Installer Framework source tree does not match its source lock.'
}
Invoke-Checked git ($gitArguments + @('archive', '--format=tar', "--output=$sourceArchive", $sourceLock.qt_ifw.commit))
Invoke-Checked tar.exe @('-xf', $sourceArchive, '-C', $sourceRoot)
Copy-Item -LiteralPath $patchPath, $sourceLockPath -Destination $inputsRoot
Invoke-Checked git @('-C', $sourceRoot, 'apply', '--check', $patchPath)
Invoke-Checked git @('-C', $sourceRoot, 'apply', $patchPath)

$qmakeArguments = @(
    (Join-Path $sourceRoot 'installerfw.pro'), '-r',
    'CONFIG+=release',
    'CONFIG-=debug debug_and_release debug_info separate_debug_info force_debug_info',
    'QMAKE_CFLAGS_RELEASE-= /Zi /ZI /Z7',
    'QMAKE_CXXFLAGS_RELEASE-= /Zi /ZI /Z7',
    'QMAKE_LFLAGS_RELEASE+= /DEBUG:NONE /NOCOFFGRPINFO /INCREMENTAL:NO',
    "INCLUDEPATH+=$($QtRoot.Replace('\', '/'))/include/QtZlib",
    "INCLUDEPATH+=$($XzRoot.Replace('\', '/'))/include",
    "INCLUDEPATH+=$($Bzip2Root.Replace('\', '/'))/include",
    "IFW_ZLIB_LIBRARY=$($QtRoot.Replace('\', '/'))/lib/Qt6BundledZLIB.lib",
    "IFW_LZMA_LIBRARY=$($XzRoot.Replace('\', '/'))/lib/lzma.lib",
    "IFW_BZIP2_LIBRARY=$($Bzip2Root.Replace('\', '/'))/lib/bz2.lib"
)
Push-Location $buildRoot
try {
    if ($HostedBuild) {
        # Hosted runners do not have the workstation's user-wide build queue.
        Invoke-Checked $qmakePath $qmakeArguments
        Invoke-Checked nmake.exe @('/NOLOGO')
    }
    else {
        Invoke-Checked queued-build (@('--slots', '1', '--', $qmakePath) + $qmakeArguments)
        # Qt IFW uses qmake; its generated MSVC Makefiles have no FASTBuild backend.
        Invoke-Checked queued-build @('--slots', [string]$Jobs, '--', $JomPath, '-j', [string]$Jobs)
    }
}
finally {
    Pop-Location
}

foreach ($input in $nativeInputs) {
    $after = Get-Identity $input.path
    if ($after.sha256 -cne $input.sha256) { throw "A native input changed during the build: $($input.path)" }
}
if ((Get-Identity $patchPath).sha256 -cne $patchIdentity.sha256) {
    throw 'The installer patch changed during the build.'
}
$artifacts = foreach ($name in @('installerbase', 'binarycreator', 'devtool', 'repogen')) {
    $identity = Get-Identity (Join-Path $buildRoot "bin\$name.exe")
    $identity.path = "bin/$name.exe"
    $identity
}
$provenance = [ordered]@{
    schema_version = 1
    capability_id = 'vnm-terminal-installer'
    qt_ifw = $sourceLock.qt_ifw
    qt_version = $qtVersion
    patch_sha256 = $patchIdentity.sha256
    source_archive = Get-Identity $sourceArchive
    compiler = Get-Identity $compilerPath
    msvc_tools_version = $env:VCToolsVersion
    windows_sdk_version = $env:WindowsSDKVersion
    qmake = Get-Identity $qmakePath
    qmake_arguments = $qmakeArguments
    native_inputs = $nativeInputs
    artifacts = @($artifacts)
}
[IO.File]::WriteAllText((Join-Path $buildRoot 'vnm-terminal-ifw-provenance.json'),
    ($provenance | ConvertTo-Json -Depth 12) + "`n", [Text.UTF8Encoding]::new($false))
Copy-Item -LiteralPath (Join-Path $repositoryRoot 'packaging\windows\private-ifw\RELINK-INSTRUCTIONS.txt') `
    -Destination $OutputRoot
if ($InputMaterialRoot) {
    & (Join-Path $PSScriptRoot 'prepare_windows_private_ifw_source.ps1') `
        -IfwOutputRoot $OutputRoot -InputMaterialRoot $InputMaterialRoot
}
Write-Host "Private Qt IFW: $buildRoot"
Write-Host "Retain $OutputRoot with the matching Qt and compression sources and relinking material."
