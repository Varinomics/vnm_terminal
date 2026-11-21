[CmdletBinding()]
param([switch]$RequireArm64)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Get-VisualStudioInstallation {
    $instances = @(& $vswhere -latest -products '*' -version '[18.0,19.0)' `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -format json | ConvertFrom-Json)
    if ($LASTEXITCODE -ne 0 -or $instances.Count -ne 1 -or
        !$instances[0].isComplete -or !$instances[0].isLaunchable) {
        throw 'A complete, launchable Visual Studio 2026 C++ installation is required.'
    }
    return $instances[0]
}

function Get-MsvcToolset($installationPath) {
    Get-ChildItem -LiteralPath (Join-Path $installationPath 'VC/Tools/MSVC') -Directory |
        Where-Object Name -Match '^14[.]51[.][0-9]+$' |
        Sort-Object { [version]$_.Name } -Descending |
        Select-Object -First 1
}

function Get-MissingMsvcComponents($toolset) {
    if (!$toolset -or !(Test-Path -LiteralPath (Join-Path $toolset.FullName 'bin/Hostx64/x64/cl.exe') -PathType Leaf)) {
        'Microsoft.VisualStudio.Component.VC.14.51.x86.x64'
    }
    if ($RequireArm64 -and (!$toolset -or
        !(Test-Path -LiteralPath (Join-Path $toolset.FullName 'bin/Hostx64/arm64/cl.exe') -PathType Leaf) -or
        !(Test-Path -LiteralPath (Join-Path $toolset.FullName 'lib/arm64/libcmt.lib') -PathType Leaf))) {
        'Microsoft.VisualStudio.Component.VC.14.51.ARM64'
    }
}

$installerDirectory = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer'
$vswhere = Join-Path $installerDirectory 'vswhere.exe'
$installation = Get-VisualStudioInstallation
$toolset = Get-MsvcToolset $installation.installationPath
$missingComponents = @(Get-MissingMsvcComponents $toolset)
if ($missingComponents.Count) {
    if ($env:GITHUB_ACTIONS -cne 'true' -or $env:RUNNER_ENVIRONMENT -cne 'github-hosted') {
        throw 'Automatic MSVC toolset provisioning requires a GitHub-hosted runner.'
    }
    # The hosted image can advance its default compiler. Add the versioned
    # components to the existing instance so its SDK and vcpkg remain in use.
    $installer = Join-Path $installerDirectory 'setup.exe'
    $arguments = @('modify', '--installPath', ('"{0}"' -f $installation.installationPath),
        '--channelId', 'VisualStudio.18.Release', '--quiet', '--norestart')
    foreach ($component in $missingComponents) {
        $arguments += @('--add', $component)
    }
    Write-Host "Installing missing MSVC 14.51 components: $($missingComponents -join ', ')"
    $setup = Start-Process -FilePath $installer -ArgumentList $arguments `
        -WorkingDirectory $env:RUNNER_TEMP -WindowStyle Hidden -Wait -PassThru
    if ($setup.ExitCode -notin @(0, 3010)) {
        throw "MSVC 14.51 component installation failed with exit code $($setup.ExitCode)."
    }
    if ($setup.ExitCode -eq 3010) {
        Write-Host 'Visual Studio Setup reported a pending restart.'
    }

    $installation = Get-VisualStudioInstallation
    $toolset = Get-MsvcToolset $installation.installationPath
    $missingComponents = @(Get-MissingMsvcComponents $toolset)
    if ($missingComponents.Count) {
        throw "MSVC 14.51 components are still unavailable: $($missingComponents -join ', ')"
    }
}

$vcvars = Join-Path $installation.installationPath 'VC\Auxiliary\Build\vcvarsall.bat'
$probe = & cmd.exe /d /c "call `"$vcvars`" x64 -vcvars_ver=14.51 && where cl"
if ($LASTEXITCODE -ne 0) {
    throw 'The MSVC 14.51 x64 developer environment could not be initialized.'
}
$compiler = @($probe | Where-Object { $_ -match '\\cl\.exe$' })
$expected = Join-Path $toolset.FullName 'bin\Hostx64\x64\cl.exe'
if ($compiler.Count -eq 0 -or $compiler[0] -ine $expected) {
    throw "The developer environment did not select the expected MSVC 14.51 compiler: $expected"
}
Write-Host "Visual Studio $($installation.installationVersion) at $($installation.installationPath)"
Write-Host "MSVC x64 compiler: $expected"
if ($RequireArm64) {
    Write-Host "MSVC ARM64 compiler: $(Join-Path $toolset.FullName 'bin\Hostx64\arm64\cl.exe')"
}
[pscustomobject]@{
    InstallationPath = $installation.installationPath
    VcVarsAll = $vcvars
    CompilerDirectory = Split-Path -Parent $expected
}
