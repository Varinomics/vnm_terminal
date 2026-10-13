[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$SourceRoot,
    [Parameter(Mandatory = $true)][string]$OutputRoot,
    [string]$PackageDirectory,
    [ValidateRange(1, 4)][int]$Slots = 2,
    [switch]$HostedCi,
    [switch]$CacheKeyOnly
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if ($HostedCi -and ($env:GITHUB_ACTIONS -cne 'true' -or $env:RUNNER_ENVIRONMENT -cne 'github-hosted')) {
    throw 'HostedCi is reserved for GitHub-hosted runners; local builds use queued-build.'
}
$SourceRoot = (Resolve-Path -LiteralPath $SourceRoot).Path
$OutputRoot = [IO.Path]::GetFullPath($OutputRoot)
if (-not $PackageDirectory) { $PackageDirectory = Join-Path $OutputRoot 'package' }
$PackageDirectory = [IO.Path]::GetFullPath($PackageDirectory)
$sourceCommit = '5a830b2bf7c053d5c7ac22208fe5a346cb5dd3dc'
$producerRoot = Join-Path $SourceRoot 'tools'

function Get-Hash {
    param([string]$Path)
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Invoke-Checked {
    param([string]$Program, [string[]]$Arguments)
    & $Program @Arguments | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "$Program failed with exit code $LASTEXITCODE" }
}

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$visualStudio = & $vswhere -latest -version '[18,19)' -products '*' `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if ($LASTEXITCODE -ne 0 -or -not $visualStudio) { throw 'Visual Studio 2026 is required for ConPTY.' }
$compiler = Get-ChildItem -LiteralPath (Join-Path $visualStudio 'VC\Tools\MSVC') -Directory |
    Where-Object { $_.Name -match '^14[.]51[.][0-9]+$' } |
    Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
if (-not $compiler) { throw 'MSVC v145 (14.51) is required for ConPTY.' }
$compilerX64 = Join-Path $compiler.FullName 'bin\Hostx64\x64\cl.exe'
$compilerArm64 = Join-Path $compiler.FullName 'bin\Hostx64\arm64\cl.exe'
$vcpkg = Join-Path $visualStudio 'VC\vcpkg\vcpkg.exe'
foreach ($required in @($compilerX64, $compilerArm64, $vcpkg,
        (Join-Path $compiler.FullName 'lib\arm64\libcmt.lib'))) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) { throw "Missing ConPTY toolchain input: $required" }
}
$vcvars = Join-Path $visualStudio 'VC\Auxiliary\Build\vcvarsall.bat'
$sdkEnvironment = & $env:ComSpec /d /c "call `"$vcvars`" x64 -vcvars_ver=14.51 >nul && set WindowsSdkDir"
if ($LASTEXITCODE -ne 0) { throw 'The Visual Studio 2026 SDK environment could not be loaded.' }
$sdkRoot = ($sdkEnvironment | Where-Object { $_ -match '^WindowsSdkDir=' }) -replace '^WindowsSdkDir=', ''
if (-not $sdkRoot) { throw 'The Visual Studio environment did not select a Windows SDK.' }
$sdk = Get-ChildItem -LiteralPath (Join-Path $sdkRoot 'bin') -Directory |
    Where-Object {
        $_.Name -match '^10[.]0[.][0-9]+[.]0$' -and
        (Test-Path -LiteralPath (Join-Path $_.FullName 'x64\fxc.exe')) -and
        (Test-Path -LiteralPath (Join-Path $sdkRoot "Lib\$($_.Name)\um\arm64\kernel32.lib"))
    } | Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
if (-not $sdk) { throw 'A Windows SDK with fxc and ARM64 libraries is required for ConPTY.' }

$producerFiles = [ordered]@{
    script_sha256 = 'prepare_windows_conpty.ps1'
    policy_sha256 = 'conpty-release.targets'
    source_patch_sha256 = 'conpty-shared-shaders.patch'
    shader_loader_sha256 = 'conpty-shared-shaders.h'
    shader_preparation_sha256 = 'prepare_conpty_shaders.ps1'
}
$producerHashes = [ordered]@{}
foreach ($name in $producerFiles.Keys) {
    $producerHashes[$name] = Get-Hash (Join-Path $producerRoot $producerFiles[$name])
}
$tripletHashes = [ordered]@{}
foreach ($name in @('x64-windows-static.cmake', 'arm64-windows-static.cmake', 'release-flags.cmake')) {
    $tripletHashes[$name] = Get-Hash (Join-Path $producerRoot "conpty-triplets\$name")
}
$identity = [ordered]@{
    source_commit = $sourceCommit
    wrapper = Get-Hash $PSCommandPath
    producer = $producerHashes
    triplets = $tripletHashes
    msvc = $compiler.Name
    compiler_x64 = Get-Hash $compilerX64
    compiler_arm64 = Get-Hash $compilerArm64
    sdk = $sdk.Name
    fxc = Get-Hash (Join-Path $sdk.FullName 'x64\fxc.exe')
    vcpkg = Get-Hash $vcpkg
}
if ($CacheKeyOnly) {
    $bytes = [Text.Encoding]::UTF8.GetBytes(($identity | ConvertTo-Json -Depth 5 -Compress))
    $sha256 = [Security.Cryptography.SHA256]::Create()
    try { $hash = [BitConverter]::ToString($sha256.ComputeHash($bytes)).Replace('-', '').ToLowerInvariant() }
    finally { $sha256.Dispose() }
    Write-Output "conpty-source-$hash"
    return
}

function Assert-Package {
    $receipt = Get-Content -LiteralPath (Join-Path $PackageDirectory 'conpty-source-build.json') -Raw | ConvertFrom-Json
    if ($receipt.schema_version -ne 1 -or $receipt.capability_id -cne 'conpty-source-build' -or
        $receipt.upstream_commit -cne $sourceCommit -or $receipt.package_version -cne '1.24.260710001' -or
        $receipt.configuration -cne 'Release' -or $receipt.release_policy.debug_symbols -ne $false -or
        $receipt.release_policy.hybrid_crt -ne $true -or
        $receipt.release_policy.sha256 -cne $producerHashes.policy_sha256 -or
        $receipt.toolchain.platform_toolset -cne 'v145' -or $receipt.toolchain.compiler_id -cne 'MSVC' -or
        $receipt.toolchain.msvc -cne $identity.msvc -or $receipt.toolchain.windows_sdk -cne $identity.sdk -or
        $receipt.toolchain.compiler_x64_sha256 -cne $identity.compiler_x64 -or
        $receipt.toolchain.compiler_arm64_sha256 -cne $identity.compiler_arm64) {
        throw 'The ConPTY package does not match the selected Release source/toolchain.'
    }
    foreach ($name in $producerHashes.Keys) {
        if ($receipt.producer.$name -cne $producerHashes[$name]) { throw "ConPTY producer differs: $name" }
    }
    foreach ($name in $tripletHashes.Keys) {
        if ($receipt.producer.triplets_sha256.$name -cne $tripletHashes[$name]) {
            throw "ConPTY triplet differs: $name"
        }
    }
    $locations = @{
        'conpty.dll' = 'runtimes/win-x64/native/conpty.dll'
        'x64/OpenConsole.exe' = 'build/native/runtimes/x64/OpenConsole.exe'
        'arm64/OpenConsole.exe' = 'build/native/runtimes/arm64/OpenConsole.exe'
    }
    if ($receipt.shared_shader_resources.schema_version -ne 1 -or
        @($receipt.shared_shader_resources.files).Count -ne 4 -or @($receipt.files).Count -ne 3) {
        throw 'ConPTY requires three binaries and four shared shaders.'
    }
    foreach ($name in @('shader_vs', 'shader_ps', 'custom_shader_vs', 'custom_shader_ps')) {
        $locations["conpty-shaders/$name.dxbc"] = "build/native/runtimes/conpty-shaders/$name.dxbc"
    }
    foreach ($file in (@($receipt.files) + @($receipt.shared_shader_resources.files))) {
        if (-not $locations.ContainsKey($file.path)) { throw "Unexpected/repeated ConPTY file: $($file.path)" }
        $path = Join-Path $PackageDirectory $locations[$file.path]
        if ((Get-Item -LiteralPath $path).Length -ne $file.size -or (Get-Hash $path) -cne $file.sha256) {
            throw "ConPTY receipt does not match $path"
        }
        $locations.Remove($file.path)
    }
    if ($locations.Count -ne 0 -or (Get-Hash (Join-Path $PackageDirectory 'LICENSE')) -cne $receipt.license_sha256) {
        throw 'ConPTY package is incomplete or its notices differ.'
    }
    $entries = @((Get-Item -LiteralPath $PackageDirectory)) + @(Get-ChildItem -LiteralPath $PackageDirectory -Recurse)
    if (@($entries | Where-Object { $_.Attributes -band [IO.FileAttributes]::ReparsePoint }).Count -ne 0 -or
        @($entries | Where-Object { -not $_.PSIsContainer }).Count -ne 9) {
        throw 'ConPTY package contains redirected or unexpected content.'
    }
}

if (Test-Path -LiteralPath $PackageDirectory) {
    Assert-Package
    Write-Output "Verified source-built ConPTY package: $PackageDirectory"
    return
}

$upstreamSource = Join-Path $OutputRoot 'source'
if (Test-Path -LiteralPath $upstreamSource) { throw 'Use a fresh ConPTY source directory after an interrupted build.' }
[void][IO.Directory]::CreateDirectory($OutputRoot)
Invoke-Checked git @('-c', 'core.autocrlf=false', 'clone', '--filter=blob:none', '--no-checkout',
    'https://github.com/microsoft/terminal.git', $upstreamSource)
Invoke-Checked git @('-C', $upstreamSource, 'fetch', '--depth', '1', 'origin', $sourceCommit)
Invoke-Checked git @('-C', $upstreamSource, 'checkout', '--detach', $sourceCommit)

$queueCommand = 'queued-build'
if ($HostedCi) {
    # The upstream producer still declares each command's bounded width. Only
    # hosted runners replace the workstation queue with direct execution.
    $queueCommand = Join-Path $OutputRoot 'hosted-conpty-command.ps1'
    @'
param([Parameter(ValueFromRemainingArguments = $true)][object[]]$CommandArguments)
$ErrorActionPreference = 'Stop'
if ($env:GITHUB_ACTIONS -cne 'true' -or $env:RUNNER_ENVIRONMENT -cne 'github-hosted' -or
    $CommandArguments.Count -lt 3 -or $CommandArguments[0] -cne '--slots' -or
    [int]$CommandArguments[1] -lt 1 -or [int]$CommandArguments[1] -gt 4) {
    throw 'Invalid hosted ConPTY command or width.'
}
# PowerShell consumes the producer's bare -- separator before binding arguments.
$program = [string]$CommandArguments[2]
$arguments = @($CommandArguments | Select-Object -Skip 3)
& $program @arguments
exit $LASTEXITCODE
'@ | Set-Content -LiteralPath $queueCommand -Encoding utf8
} else {
    Invoke-Checked queued-build @('--check-xenia')
}
& (Join-Path $producerRoot 'prepare_windows_conpty.ps1') -SourceDirectory $upstreamSource `
    -OutputDirectory $PackageDirectory -VisualStudioDirectory $visualStudio `
    -VCToolsVersion $compiler.Name -WindowsSdkVersion $sdk.Name -Slots $Slots -QueueCommand $queueCommand
Assert-Package
Write-Output "Verified source-built ConPTY package: $PackageDirectory"
