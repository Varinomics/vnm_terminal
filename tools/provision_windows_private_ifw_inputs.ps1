#Requires -Version 7.2
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$OutputRoot,
    [string]$CMakePath = 'cmake',
    [ValidateSet('FASTBuild', 'Ninja')][string]$Generator = 'FASTBuild',
    [string]$MakeProgramPath,
    [ValidateRange(1, 4)][int]$Jobs = 4
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Invoke-Checked {
    param([string]$Program, [string[]]$Arguments)
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Program failed with exit code $LASTEXITCODE" }
}

function Get-Hash {
    param([string]$Path)
    (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Invoke-CompilerCommand {
    param([int]$Slots, [string[]]$Arguments, [switch]$FastBuild)
    if (Get-Command queued-build -ErrorAction SilentlyContinue) {
        $output = & queued-build --slots $Slots -- $CMakePath @Arguments 2>&1
    }
    elseif ($env:GITHUB_ACTIONS -eq 'true' -and $Generator -eq 'Ninja') {
        # Hosted runners have no workstation queue or FASTBuild-enabled CMake.
        $output = & $CMakePath @Arguments 2>&1
    }
    else {
        throw 'queued-build is required outside the hosted Ninja workflow.'
    }
    $exitCode = $LASTEXITCODE
    $output | Out-Host
    if ($exitCode -ne 0 -or ($FastBuild -and "$output" -notmatch 'FBuild: OK:')) {
        throw "CMake failed: $($Arguments -join ' ')"
    }
}

function Prepare-Source {
    param([string]$Path, $Specification)
    if (-not (Test-Path -LiteralPath (Join-Path $Path '.git'))) {
        Invoke-Checked git @('-c', 'core.autocrlf=false', 'clone', '--depth', '1',
            '--branch', $Specification.tag, $Specification.repository, $Path)
    }
    $commit = & git -C $Path rev-parse HEAD
    if ($LASTEXITCODE -ne 0 -or $commit -cne $Specification.commit) {
        throw "Source revision differs from the lock: $Path"
    }
    $tree = & git -C $Path rev-parse 'HEAD^{tree}'
    if ($LASTEXITCODE -ne 0 -or $tree -cne $Specification.tree) {
        throw "Source tree differs from the lock: $Path"
    }
    $status = & git -C $Path status --porcelain --untracked-files=all
    if ($LASTEXITCODE -ne 0 -or $status) { throw "Source is not clean: $Path" }
}

function Assert-ReleaseCache {
    param([string]$BuildRoot)
    $cache = Get-Content -LiteralPath (Join-Path $BuildRoot 'CMakeCache.txt') -Raw
    foreach ($language in @('C', 'CXX')) {
        $compiler = [regex]::Match($cache, "(?m)^CMAKE_${language}_COMPILER:[^=]+=(.+)$")
        if ($compiler.Success -and
            -not $compiler.Groups[1].Value.Trim().Replace('\', '/').StartsWith(
                $visualStudioRoot.Replace('\', '/') + '/', [StringComparison]::OrdinalIgnoreCase)) {
            throw "Unexpected $language compiler in $BuildRoot"
        }
        $flags = [regex]::Matches($cache, "(?m)^CMAKE_${language}_FLAGS(?:_RELEASE)?:[^=]+=(.*)$")
        if (($flags | ForEach-Object { $_.Groups[1].Value }) -match
            '(?i)(?:^|\s)[/-](?:Zi|Z7)(?:\s|$)') {
            throw "Release compiler debug data is enabled: $BuildRoot"
        }
    }
    if ($cache -notmatch '(?m)^CMAKE_BUILD_TYPE:[^=]+=Release\r?$' -or
        $cache -notmatch '(?m)^CMAKE_MSVC_RUNTIME_LIBRARY:[^=]+=MultiThreaded\r?$' -or
        $cache -notmatch '(?m)^BUILD_SHARED_LIBS:[^=]+=OFF\r?$') {
        throw "The input build is not static Release: $BuildRoot"
    }
}

function Build-Input {
    param([string]$Name, [string]$SourceRoot, [string[]]$Options = @())
    $buildRoot = Join-Path $OutputRoot "build/$Name"
    $prefix = Join-Path $OutputRoot $Name
    $arguments = @('-S', $SourceRoot, '-B', $buildRoot, '-G', $Generator,
        '-C', $cachePath, "-DCMAKE_INSTALL_PREFIX=$prefix") + $Options
    if ($MakeProgramPath) { $arguments += "-DCMAKE_MAKE_PROGRAM=$MakeProgramPath" }
    Invoke-CompilerCommand 1 $arguments
    Assert-ReleaseCache $buildRoot
    if ($Generator -eq 'FASTBuild') {
        Invoke-CompilerCommand 2 @('--build', $buildRoot, '--', '-dist', '-nolocalrace') -FastBuild
    }
    else {
        Invoke-CompilerCommand $Jobs @('--build', $buildRoot, '--parallel', [string]$Jobs)
    }
    Invoke-CompilerCommand 1 @('--install', $buildRoot)
}

$repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$inputRoot = Join-Path $repositoryRoot 'packaging/windows/private-ifw'
$lockPath = Join-Path $inputRoot 'source-lock.json'
$cachePath = Join-Path $inputRoot 'static-inputs.cmake'
$bzip2Project = Join-Path $inputRoot 'bzip2-static'
$sourceLock = Get-Content -LiteralPath $lockPath -Raw | ConvertFrom-Json
$OutputRoot = [IO.Path]::GetFullPath($OutputRoot)
$receiptPath = Join-Path $OutputRoot 'input-provenance.json'
$recipe = @($lockPath, $PSCommandPath, $cachePath, (Join-Path $bzip2Project 'CMakeLists.txt')) |
    ForEach-Object { [ordered]@{ name = [IO.Path]::GetFileName($_); sha256 = Get-Hash $_ } }

if (Test-Path -LiteralPath $receiptPath) {
    $receipt = Get-Content -LiteralPath $receiptPath -Raw | ConvertFrom-Json
    if (($receipt.recipe | ConvertTo-Json -Compress) -cne ($recipe | ConvertTo-Json -Compress)) {
        throw 'Cached build inputs were produced with a different recipe; use a fresh OutputRoot.'
    }
    foreach ($file in $receipt.files) {
        if ((Get-Hash (Join-Path $OutputRoot $file.path)) -cne $file.sha256) {
            throw "Cached build input changed: $($file.path)"
        }
    }
    Write-Host "Verified cached installer build inputs: $OutputRoot"
    return
}
if (Test-Path -LiteralPath $OutputRoot) {
    throw 'The incomplete build-input directory must be retained for diagnosis; use a fresh OutputRoot.'
}

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$visualStudioRoot = (& $vswhere -latest -products '*' -version '[18.0,19.0)' `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath).Trim()
if ($LASTEXITCODE -ne 0 -or -not $visualStudioRoot) { throw 'Visual Studio 2026 is required.' }
$vcvars = Join-Path $visualStudioRoot 'VC/Auxiliary/Build/vcvarsall.bat'
$developerEnvironment = & cmd.exe /d /c "call `"$vcvars`" x64 -vcvars_ver=14.51 >nul && set"
if ($LASTEXITCODE -ne 0) { throw 'The Visual Studio 2026 x64 developer environment could not be loaded.' }
foreach ($line in $developerEnvironment) {
    if ($line -match '^([^=]+)=(.*)$') {
        [Environment]::SetEnvironmentVariable($matches[1], $matches[2], 'Process')
    }
}
if ($env:VCToolsVersion -notlike '14.51.*' -or $env:VSCMD_ARG_TGT_ARCH -ne 'x64') {
    throw 'The installer inputs require MSVC v145 x64.'
}
if ($Generator -eq 'FASTBuild') {
    Invoke-Checked queued-build @('--check-xenia')
}
elseif ($env:GITHUB_ACTIONS -ne 'true') {
    throw 'Local builds require FASTBuild. Hosted runners explicitly select Ninja.'
}

$sourcesRoot = Join-Path $OutputRoot 'sources'
New-Item -ItemType Directory -Path $sourcesRoot | Out-Null
$qtSource = Join-Path $sourcesRoot 'qt'
Prepare-Source (Join-Path $sourcesRoot 'ifw') $sourceLock.qt_ifw
Prepare-Source $qtSource $sourceLock.qt
$modules = @($sourceLock.qt.modules.PSObject.Properties)
foreach ($module in $modules) {
    Invoke-Checked git @('-C', $qtSource, '-c', 'core.autocrlf=false',
        'submodule', 'update', '--init', '--depth', '1', '--', $module.Name)
    $moduleRoot = Join-Path $qtSource $module.Name
    $moduleCommit = & git -C $moduleRoot rev-parse HEAD
    if ($LASTEXITCODE -ne 0 -or $moduleCommit -cne $module.Value) {
        throw "Qt module revision differs from its lock: $($module.Name)"
    }
}
Prepare-Source (Join-Path $sourcesRoot 'xz') $sourceLock.ifw_dependencies.xz
Prepare-Source (Join-Path $sourcesRoot 'bzip2') $sourceLock.ifw_dependencies.bzip2

Build-Input 'qt' $qtSource
Build-Input 'xz' (Join-Path $sourcesRoot 'xz')
Build-Input 'bzip2' $bzip2Project @("-DBZIP2_SOURCE_DIR=$(Join-Path $sourcesRoot 'bzip2')")

$materialRoot = Join-Path $OutputRoot 'source-material'
$archivesRoot = Join-Path $materialRoot 'sources'
New-Item -ItemType Directory -Path $archivesRoot | Out-Null
Copy-Item -LiteralPath $lockPath, $cachePath, $PSCommandPath -Destination $materialRoot
Copy-Item -LiteralPath $bzip2Project -Destination $materialRoot -Recurse
$archiveSources = [ordered]@{ qt = $qtSource; xz = Join-Path $sourcesRoot 'xz';
    bzip2 = Join-Path $sourcesRoot 'bzip2'; ifw = Join-Path $sourcesRoot 'ifw' }
foreach ($module in $modules) { $archiveSources[$module.Name] = Join-Path $qtSource $module.Name }
foreach ($name in $archiveSources.Keys) {
    Invoke-Checked git @('-C', $archiveSources[$name], 'archive', '--format=tar',
        "--output=$(Join-Path $archivesRoot "$name.tar")", 'HEAD')
}
$files = foreach ($directory in @('qt', 'xz', 'bzip2', 'source-material')) {
    Get-ChildItem -LiteralPath (Join-Path $OutputRoot $directory) -File -Recurse |
        ForEach-Object {
            [ordered]@{ path = [IO.Path]::GetRelativePath($OutputRoot, $_.FullName).Replace('\', '/');
                sha256 = Get-Hash $_.FullName }
        }
}
$receipt = [ordered]@{
    schema_version = 1
    recipe = @($recipe)
    sources = $sourceLock
    generator = $Generator
    compiler_version = $env:VCToolsVersion
    windows_sdk_version = $env:WindowsSDKVersion
    files = @($files)
}
[IO.File]::WriteAllText($receiptPath, ($receipt | ConvertTo-Json -Depth 12) + "`n",
    [Text.UTF8Encoding]::new($false))
Copy-Item -LiteralPath $receiptPath -Destination $materialRoot
Write-Host "Installer build inputs: $OutputRoot"
