[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$IfwRoot,

    [Parameter(Mandatory = $true)]
    [string]$InstallerBasePath,

    [string]$OutputRoot,

    [switch]$RequireSigned
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Assert-PublicationContract {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) {
        throw "Maintenance publication contract violation: $Message"
    }
}

function Invoke-FixtureCommand {
    param([string]$Executable, [string[]]$Arguments, [string]$LogPath)
    $quotedArguments = @($Arguments | ForEach-Object { '"' + $_ + '"' })
    $process = [Diagnostics.Process]::new()
    $process.StartInfo.FileName               = $Executable
    $process.StartInfo.Arguments              = $quotedArguments -join ' '
    $process.StartInfo.UseShellExecute        = $false
    $process.StartInfo.CreateNoWindow         = $true
    $process.StartInfo.WindowStyle            = 'Hidden'
    $process.StartInfo.RedirectStandardOutput = $true
    $process.StartInfo.RedirectStandardError  = $true
    $null = $process.Start()
    $stdout = $process.StandardOutput.ReadToEndAsync()
    $stderr = $process.StandardError.ReadToEndAsync()
    if (-not $process.WaitForExit(120000)) {
        $process.Kill()
        throw "Fixture command timed out: $Executable"
    }
    ($stdout.GetAwaiter().GetResult() + $stderr.GetAwaiter().GetResult()) |
        Set-Content -LiteralPath $LogPath
    return $process.ExitCode
}

$ifwRootPath = (Resolve-Path -LiteralPath $IfwRoot).Path
$basePath    = (Resolve-Path -LiteralPath $InstallerBasePath).Path
$baseHash    = (Get-FileHash -LiteralPath $basePath -Algorithm SHA256).Hash
$baseSize    = (Get-Item -LiteralPath $basePath).Length
$signature  = Get-AuthenticodeSignature -LiteralPath $basePath
if ($RequireSigned) {
    Assert-PublicationContract ($signature.Status -eq 'Valid') 'the fixture base must be signed'
    Assert-PublicationContract ($null -ne $signature.TimeStamperCertificate) `
        'the fixture base must have a timestamp'
}

if (-not $OutputRoot) {
    $OutputRoot = Join-Path ([IO.Path]::GetTempPath()) `
        ('vnm-ifw-publication-' + [Guid]::NewGuid().ToString('N'))
}
$OutputRoot = [IO.Path]::GetFullPath($OutputRoot)
Assert-PublicationContract (-not (Test-Path -LiteralPath $OutputRoot)) `
    'use a fresh fixture output directory'
$fixtureId   = [Guid]::NewGuid().ToString('N')
$packageName = "org.varinomics.publication.fixture.$fixtureId"
$configRoot  = Join-Path $OutputRoot 'config'
$packageRoot = Join-Path $OutputRoot "packages/$packageName"
$metaRoot    = Join-Path $packageRoot 'meta'
$dataRoot    = Join-Path $packageRoot 'data'
$payloadRoot = Join-Path $OutputRoot "packages/$packageName.payload"
$payloadMeta = Join-Path $payloadRoot 'meta'
$payloadData = Join-Path $payloadRoot 'data'
New-Item -ItemType Directory -Path $configRoot, $metaRoot, $dataRoot, $payloadMeta, $payloadData |
    Out-Null

@"
<?xml version="1.0" encoding="UTF-8"?>
<Installer>
    <Name>VNM maintenance publication fixture $fixtureId</Name>
    <Version>1.0.0</Version>
    <Title>Isolated maintenance publication test</Title>
    <Publisher>Local installer validation</Publisher>
    <TargetDir>@HomeDir@/unused-publication-fixture</TargetDir>
    <MaintenanceToolName>fixture_maintenance</MaintenanceToolName>
    <SupportsModify>false</SupportsModify>
    <RemoveTargetDir>true</RemoveTargetDir>
    <AllowRepositoriesForOfflineInstaller>false</AllowRepositoriesForOfflineInstaller>
    <SaveDefaultRepositories>false</SaveDefaultRepositories>
    <DisableAuthorizationFallback>true</DisableAuthorizationFallback>
</Installer>
"@ | Set-Content -LiteralPath (Join-Path $configRoot 'config.xml') -Encoding UTF8
@"
<?xml version="1.0" encoding="UTF-8"?>
<Package>
    <DisplayName>Maintenance publication fixture</DisplayName>
    <Description>Signed base publication and failure handling</Description>
    <Version>1.0.0</Version>
    <ReleaseDate>2026-10-13</ReleaseDate>
    <Name>$packageName</Name>
    <Default>true</Default>
    <Script>installscript.qs</Script>
</Package>
"@ | Set-Content -LiteralPath (Join-Path $metaRoot 'package.xml') -Encoding UTF8
@"
<?xml version="1.0" encoding="UTF-8"?>
<Package>
    <DisplayName>Fixture application payload</DisplayName>
    <Description>Payload rollback marker</Description>
    <Version>1.0.0</Version>
    <ReleaseDate>2026-10-13</ReleaseDate>
    <Name>$packageName.payload</Name>
    <Default>true</Default>
</Package>
"@ | Set-Content -LiteralPath (Join-Path $payloadMeta 'package.xml') -Encoding UTF8

$sourceRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$script = Get-Content -LiteralPath (Join-Path $sourceRoot `
    'packaging/windows/ifw/maintenance_installscript.qs') -Raw
$script += @'

Component.prototype.createOperations = function()
{
    if (component.archives.length === 0)
        throw new Error("The embedded fixture archive was not enumerated.");
    component.createOperations();
    var failurePath = "";
    if (installer.value("FixtureFailure", "") === "publication")
        failurePath = "@TargetDir@/fixture_maintenance.exe";
    if (installer.value("FixtureFailure", "") === "metadata")
        failurePath = "@TargetDir@/fixture_maintenance.dat.new";
    if (failurePath !== "") {
        component.addOperation("Mkdir", failurePath);
        component.addOperation("Copy", "@TargetDir@/sentinel.txt", failurePath + "/sentinel.txt");
    }
}
'@
$script | Set-Content -LiteralPath (Join-Path $metaRoot 'installscript.qs') -Encoding UTF8
Copy-Item -LiteralPath $basePath -Destination (Join-Path $dataRoot 'installerbase.exe')
# IFW exempts the maintenance archive from extraction undo, so the application
# marker belongs to a separate component just as it does in the product package.
'Owned maintenance publication fixture' |
    Set-Content -LiteralPath (Join-Path $payloadData 'sentinel.txt') -Encoding UTF8
$payloadBytes = $baseSize + (Get-Item -LiteralPath (Join-Path $payloadData 'sentinel.txt')).Length

# Run this test through queued-build --slots 1 on managed workstations.
$fixtureInstaller = Join-Path $OutputRoot 'fixture.exe'
$packageExit = Invoke-FixtureCommand (Join-Path $ifwRootPath 'bin/binarycreator.exe') @(
    '--offline-only', '-t', (Join-Path $ifwRootPath 'bin/installerbase.exe'),
    '-c', (Join-Path $configRoot 'config.xml'), '-p', (Join-Path $OutputRoot 'packages'),
    $fixtureInstaller
) (Join-Path $OutputRoot 'package.log')
Assert-PublicationContract ($packageExit -eq 0) 'fixture packaging failed'

$results = @()
foreach ($scenario in @('success', 'publication', 'metadata')) {
    $target      = Join-Path $OutputRoot "target-$scenario"
    $maintenance = Join-Path $target 'fixture_maintenance.exe'
    $productId   = [Guid]::NewGuid().ToString('B')
    $registryKey = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\$productId"
    Assert-PublicationContract (-not (Test-Path -LiteralPath $registryKey)) `
        'the fixture registry identity must be unused'
    try {
        $installExit = Invoke-FixtureCommand $fixtureInstaller @(
            'install', '--root', $target, '--accept-licenses', '--accept-messages',
            '--confirm-command', 'AllUsers=false', "ProductUUID=$productId",
            "FixtureFailure=$scenario"
        ) (Join-Path $OutputRoot "$scenario-install.log")
        if ($scenario -eq 'success') {
            Assert-PublicationContract ($installExit -eq 0) 'fixture installation failed'
            Assert-PublicationContract (Test-Path -LiteralPath (Join-Path $target 'sentinel.txt')) `
                'the archive payload must actually be extracted'
            Assert-PublicationContract `
                ((Get-FileHash -LiteralPath $maintenance -Algorithm SHA256).Hash -ceq $baseHash) `
                'the maintenance executable must preserve the exact packaged base bytes'
            Assert-PublicationContract (-not (Test-Path -LiteralPath (Join-Path $target 'installerbase.exe'))) `
                'publication must consume the extracted base'
            Assert-PublicationContract (-not (Test-Path -LiteralPath "$maintenance.new")) `
                'publication must leave one maintenance executable'
            $registration = Get-ItemProperty -LiteralPath $registryKey
            $expectedKiB  = [Math]::Floor($payloadBytes / 1024)
            Assert-PublicationContract ($registration.EstimatedSize -eq $expectedKiB) `
                'Windows EstimatedSize must account for one packaged base'
            $results += [pscustomobject]@{
                Scenario = $scenario; ExitCode = $installExit
                EstimatedSizeKiB = $registration.EstimatedSize; BaseSha256 = $baseHash
            }
            $purgeExit = Invoke-FixtureCommand $maintenance @(
                'purge', '--accept-messages', '--confirm-command'
            ) (Join-Path $OutputRoot 'success-purge.log')
            Assert-PublicationContract ($purgeExit -eq 0) 'fixture maintenance purge failed'
            Assert-PublicationContract (-not (Test-Path -LiteralPath $registryKey)) `
                'purge must remove the fixture registration'
        }
        else {
            Assert-PublicationContract ($installExit -ne 0) "$scenario failure must fail installation"
            Assert-PublicationContract (-not (Test-Path -LiteralPath $registryKey)) `
                "$scenario failure must not register an unusable installation"
            Assert-PublicationContract (-not (Test-Path -LiteralPath $maintenance)) `
                "$scenario failure must not publish an unsigned fallback"
            Assert-PublicationContract `
                ((Get-FileHash -LiteralPath (Join-Path $target 'installerbase.exe') -Algorithm SHA256).Hash -ceq $baseHash) `
                "$scenario failure must preserve the original signed base"
            Assert-PublicationContract (-not (Test-Path -LiteralPath (Join-Path $target 'sentinel.txt'))) `
                "$scenario failure must roll back the extracted payload"
            $results += [pscustomobject]@{
                Scenario = $scenario; ExitCode = $installExit; BaseSha256 = $baseHash
            }
        }
    }
    finally {
        # A failed assertion must still clean up this fixture's own registration.
        if (Test-Path -LiteralPath $registryKey) {
            $registration = Get-ItemProperty -LiteralPath $registryKey
            Assert-PublicationContract ($registration.InstallLocation -ieq $target) `
                'cleanup is restricted to this fixture target'
            if (Test-Path -LiteralPath $maintenance -PathType Leaf) {
                $null = Invoke-FixtureCommand $maintenance @(
                    'purge', '--accept-messages', '--confirm-command'
                ) (Join-Path $OutputRoot "$scenario-cleanup.log")
            }
            if (Test-Path -LiteralPath $registryKey) {
                Remove-Item -LiteralPath $registryKey -Recurse -Force
            }
        }
    }
}
$results | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $OutputRoot 'results.json')
Write-Output "Native publication, signed-byte preservation, size accounting and failure rollback passed: $OutputRoot"
