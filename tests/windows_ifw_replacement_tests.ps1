[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repositoryRoot = Split-Path -Parent $PSScriptRoot
. (Join-Path $repositoryRoot 'packaging/windows/ifw/replacement_support.ps1')

function Assert-Replacement
{
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) {
        throw "FAIL: $Message"
    }
}

function Assert-ReplacementFailure
{
    param([scriptblock]$Action, [string]$Pattern)
    $failed = $false
    try {
        & $Action | Out-Null
    }
    catch {
        $failed = $true
        Assert-Replacement ($_.Exception.Message -match $Pattern) $_.Exception.Message
    }
    Assert-Replacement $failed "Expected refusal matching $Pattern"
}

$fixtureRoot = Join-Path ([IO.Path]::GetTempPath()) ('vnm_terminal-replacement-' + [Guid]::NewGuid().ToString('N'))
[void](New-Item -ItemType Directory -Path $fixtureRoot)
$maintenancePath = Join-Path $fixtureRoot 'vnm_terminal_maintenance.exe'
$metadataPath = Join-Path $fixtureRoot 'components.xml'
$publisher = 'CN=Varinomics Ltd, O=Varinomics Ltd, L=Limassol, S=Limassol, C=CY'
$script:signatureStatus = [Management.Automation.SignatureStatus]::Valid
$script:signatureSubject = $publisher
$script:timestamp = [pscustomobject]@{}
$nativeSignatureProbe = (Get-Item Function:Get-VnmTerminalReplacementSignature).ScriptBlock

# Synthetic boundary fixtures exercise validation and sequencing. They never
# launch maintenance, elevate, install, remove an installation, or stop a process.
function Get-VnmTerminalReplacementSignature
{
    param([string]$Path)
    return [pscustomobject]@{
        Status = $script:signatureStatus
        SignerCertificate = [pscustomobject]@{ Subject = $script:signatureSubject }
        TimeStamperCertificate = $script:timestamp
    }
}

try {
    [IO.File]::WriteAllText($maintenancePath, 'unsigned synthetic signature fixture')
    $originalInputEncoding = [Console]::InputEncoding
    try {
        [Console]::InputEncoding = [Text.UTF8Encoding]::new($true)
        $unsignedSignature = & $nativeSignatureProbe $maintenancePath
    }
    finally {
        [Console]::InputEncoding = $originalInputEncoding
    }
    Assert-Replacement ([string]$unsignedSignature.Status -cne 'Valid') `
        'The bounded native read-only probe must refuse unsigned data'
    Assert-ReplacementFailure { Get-VnmTerminalReplacementTarget 'relative-folder' } 'absolute local'
    Assert-ReplacementFailure { Get-VnmTerminalReplacementTarget '\\server\share\vnm_terminal' } 'absolute local'
    Assert-ReplacementFailure { Get-VnmTerminalReplacementTarget $fixtureRoot } 'protected vnm_terminal'
    [IO.File]::WriteAllText($metadataPath,
        '<Packages><ApplicationName>vnm_terminal</ApplicationName>' +
        '<Package><Name>com.varinomics.vnm_terminal</Name></Package>' +
        '<Package><Name>com.varinomics.vnm_terminal.maintenance</Name></Package></Packages>')
    Assert-VnmTerminalReplacementMetadata $fixtureRoot
    [IO.File]::WriteAllText($metadataPath,
        '<Packages><ApplicationName>Another app</ApplicationName></Packages>')
    Assert-ReplacementFailure { Assert-VnmTerminalReplacementMetadata $fixtureRoot } 'expected vnm_terminal'
    [IO.File]::WriteAllText($metadataPath,
        '<Packages><ApplicationName>vnm_terminal</ApplicationName>' +
        '<Package><Name>com.varinomics.vnm_terminal</Name></Package>' +
        '<Package><Name>com.varinomics.vnm_terminal</Name></Package></Packages>')
    Assert-ReplacementFailure { Assert-VnmTerminalReplacementMetadata $fixtureRoot } 'expected vnm_terminal'
    [IO.File]::WriteAllText($metadataPath,
        '<Packages><ApplicationName>vnm_terminal</ApplicationName>' +
        '<Package><Name>com.varinomics.vnm_terminal</Name></Package>' +
        '<Package><Name>com.varinomics.vnm_terminal.maintenance</Name></Package>' +
        '<Package><Name>com.varinomics.another</Name></Package></Packages>')
    Assert-ReplacementFailure { Assert-VnmTerminalReplacementMetadata $fixtureRoot } 'expected vnm_terminal'
    [IO.File]::WriteAllText($metadataPath,
        '<!DOCTYPE Packages [<!ENTITY x SYSTEM "file:///C:/Windows/win.ini">]><Packages>&x;</Packages>')
    Assert-ReplacementFailure { Assert-VnmTerminalReplacementMetadata $fixtureRoot } 'DTD'

    [IO.File]::WriteAllText($maintenancePath, 'synthetic maintenance image')
    $held = Open-VnmTerminalReplacementMaintenanceTool $fixtureRoot
    try {
        Assert-ReplacementFailure { [IO.File]::WriteAllText($maintenancePath, 'changed') } 'used by another process'
    }
    finally {
        $held.Dispose()
    }
    $script:signatureSubject = 'CN=Other publisher'
    Assert-ReplacementFailure { Open-VnmTerminalReplacementMaintenanceTool $fixtureRoot } 'signature'
    $script:signatureSubject = $publisher
    $script:signatureStatus = [Management.Automation.SignatureStatus]::NotSigned
    Assert-ReplacementFailure { Open-VnmTerminalReplacementMaintenanceTool $fixtureRoot } 'signature'
    $script:signatureStatus = [Management.Automation.SignatureStatus]::Valid
    $script:timestamp = $null
    Assert-ReplacementFailure { Open-VnmTerminalReplacementMaintenanceTool $fixtureRoot } 'signature'
    $script:timestamp = [pscustomobject]@{}

    $script:processes = @()
    function Get-CimInstance {
        param([string]$ClassName, [int]$OperationTimeoutSec)
        Assert-Replacement ($OperationTimeoutSec -eq 10) 'The process inventory must have a finite operation timeout'
        return $script:processes
    }
    Assert-VnmTerminalReplacementIdle $fixtureRoot
    $script:processes = @([pscustomobject]@{
        ExecutablePath = $maintenancePath; Name = 'vnm_terminal.exe'
    })
    Assert-ReplacementFailure { Assert-VnmTerminalReplacementIdle $fixtureRoot } 'still running'
    $script:processes = @([pscustomobject]@{
        ExecutablePath = ''; Name = 'vnm_terminal.exe'
    })
    Assert-ReplacementFailure { Assert-VnmTerminalReplacementIdle $fixtureRoot } 'cannot verify'
    $script:processes = @([pscustomobject]@{
        ExecutablePath = $fixtureRoot + '-other\vnm_terminal.exe'; Name = 'vnm_terminal.exe'
    })
    Assert-VnmTerminalReplacementIdle $fixtureRoot

    $script:protectedAcl = [Security.AccessControl.DirectorySecurity]::new()
    $script:protectedAcl.SetSecurityDescriptorSddlForm(
        'O:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;FRFX;;;BU)')
    $script:ancestorAcl = $script:protectedAcl
    function Get-Acl {
        param([string]$LiteralPath)
        if ($LiteralPath -eq $fixtureRoot -or $LiteralPath -eq [IO.Path]::GetDirectoryName($fixtureRoot)) {
            return $script:protectedAcl
        }
        return $script:ancestorAcl
    }
    Assert-VnmTerminalReplacementPermissions $fixtureRoot
    $script:ancestorAcl = [Security.AccessControl.DirectorySecurity]::new()
    $script:ancestorAcl.SetSecurityDescriptorSddlForm('O:BAD:P(A;;FW;;;BU)')
    Assert-ReplacementFailure { Assert-VnmTerminalReplacementPermissions $fixtureRoot } 'permits changes'
    $script:ancestorAcl.SetSecurityDescriptorSddlForm('O:BUD:P(A;;FRFX;;;BU)')
    Assert-ReplacementFailure { Assert-VnmTerminalReplacementPermissions $fixtureRoot } 'not owned'
    $script:ancestorAcl = $script:protectedAcl
    $script:protectedAcl = [Security.AccessControl.DirectorySecurity]::new()
    $script:protectedAcl.SetSecurityDescriptorSddlForm('O:BAD:(A;OICI;FA;;;BA)(A;OICI;FRFX;;;BU)')
    Assert-VnmTerminalReplacementPermissions $fixtureRoot
    $script:protectedAcl.SetSecurityDescriptorSddlForm('O:BAD:(A;OICI;FA;;;BA)(A;OICI;FW;;;BU)')
    Assert-ReplacementFailure { Assert-VnmTerminalReplacementPermissions $fixtureRoot } 'permits changes'
    $script:protectedAcl.SetSecurityDescriptorSddlForm('O:BUD:(A;OICI;FRFX;;;BU)')
    Assert-ReplacementFailure { Assert-VnmTerminalReplacementPermissions $fixtureRoot } 'not owned'

    $script:removalExitCode = 0
    $script:removalFinished = $true
    $script:removalDisposed = $false
    $script:launchFailure = ''
    function Start-Process {
        param($FilePath, $Verb, [switch]$PassThru, $WindowStyle, $WorkingDirectory, $ArgumentList)
        Assert-Replacement ($FilePath -ceq $maintenancePath -and $Verb -ceq 'RunAs' -and
            $PassThru -and $WindowStyle -ceq 'Hidden' -and
            $WorkingDirectory -ceq [Environment]::SystemDirectory) `
            'Removal must request separate permissions for the validated image'
        Assert-Replacement (($ArgumentList -join ',') -ceq '--reject-messages,--confirm-command,purge') `
            'Persistent undo failures must not select Retry'
        Assert-Replacement ($null -eq [Environment]::GetEnvironmentVariable('QT_PLUGIN_PATH', 'Process') -and
            [Environment]::GetEnvironmentVariable('PATH', 'Process') -ceq [Environment]::SystemDirectory) `
            'Removal must not inherit loader injection paths'
        if ($script:launchFailure -ceq 'cancel') {
            throw [InvalidOperationException]::new('Wrapped permission refusal',
                [ComponentModel.Win32Exception]::new(1223))
        }
        if ($script:launchFailure -ceq 'unknown') {
            throw 'controlled unknown launch failure'
        }
        if ($script:launchFailure -ceq 'unobserved') {
            return $null
        }
        $fake = [pscustomobject]@{ ExitCode = $script:removalExitCode }
        $fake | Add-Member ScriptMethod WaitForExit {
            param($Milliseconds)
            Assert-Replacement ($Milliseconds -eq 300000) 'The removal process wait must have a finite deadline'
            return $script:removalFinished
        }
        $fake | Add-Member ScriptMethod Dispose { $script:removalDisposed = $true }
        return $fake
    }
    $originalEnvironment = [Environment]::GetEnvironmentVariables('Process')
    try {
        [Environment]::SetEnvironmentVariable('QT_PLUGIN_PATH', $fixtureRoot, 'Process')
        Invoke-VnmTerminalReplacementRemoval $maintenancePath
        Assert-Replacement $script:VnmTerminalReplacementRemovalMayHaveStarted `
            'Observed launch must retain uncertain-change status'
        Assert-Replacement $script:removalDisposed 'Successful removal must release the process observation handle'
        $script:removalExitCode = 1
        $script:removalDisposed = $false
        Assert-ReplacementFailure { Invoke-VnmTerminalReplacementRemoval $maintenancePath } 'exit code 1'
        Assert-Replacement $script:removalDisposed 'Unsuccessful removal must release the process observation handle'
        $script:removalExitCode = 0
        $script:removalFinished = $false
        $script:removalDisposed = $false
        Assert-ReplacementFailure { Invoke-VnmTerminalReplacementRemoval $maintenancePath } 'may still be running'
        Assert-Replacement $script:removalDisposed 'Timeout must release the process observation handle'
        $script:launchFailure = 'cancel'
        Assert-ReplacementFailure { Invoke-VnmTerminalReplacementRemoval $maintenancePath } 'permission request was canceled'
        Assert-Replacement (-not $script:VnmTerminalReplacementRemovalMayHaveStarted) `
            'Windows permission cancellation must report that removal never started'
        $script:launchFailure = 'unknown'
        Assert-ReplacementFailure { Invoke-VnmTerminalReplacementRemoval $maintenancePath } `
            'controlled unknown launch failure'
        Assert-Replacement $script:VnmTerminalReplacementRemovalMayHaveStarted 'Generic launch failure must stay uncertain'
        $script:launchFailure = 'unobserved'
        Assert-ReplacementFailure { Invoke-VnmTerminalReplacementRemoval $maintenancePath } 'could not observe'
        Assert-Replacement $script:VnmTerminalReplacementRemovalMayHaveStarted `
            'Missing process observation must stay uncertain'
    }
    finally {
        foreach ($name in @($originalEnvironment.Keys)) {
            if ($name -match '^(COR_|COMPLUS_|CORECLR_|DOTNET_|QT_|QML_|QML2_|PATH$)') {
                [Environment]::SetEnvironmentVariable($name, $originalEnvironment[$name], 'Process')
            }
        }
        if (-not $originalEnvironment.Contains('QT_PLUGIN_PATH')) {
            [Environment]::SetEnvironmentVariable('QT_PLUGIN_PATH', $null, 'Process')
        }
    }

    $script:steps = [Collections.Generic.List[string]]::new()
    $script:administrator = $true
    function Test-VnmTerminalReplacementAdministrator { return $script:administrator }
    function Get-VnmTerminalReplacementTarget {
        param([string]$TargetDirectory)
        $script:steps.Add('target')
        return $TargetDirectory
    }
    function Assert-VnmTerminalReplacementMetadata {
        param([string]$TargetDirectory)
        $script:steps.Add('metadata')
    }
    function Assert-VnmTerminalReplacementPermissions {
        param([string]$TargetDirectory)
        $script:steps.Add('permissions')
    }
    function Assert-VnmTerminalReplacementIdle {
        param([string]$TargetDirectory)
        $script:steps.Add('idle')
    }
    function Open-VnmTerminalReplacementMaintenanceTool {
        param([string]$TargetDirectory)
        $script:steps.Add('signature')
        $script:heldImage = [IO.MemoryStream]::new()
        return $script:heldImage
    }
    function Invoke-VnmTerminalReplacementRemoval {
        param([string]$MaintenancePath)
        $script:VnmTerminalReplacementRemovalMayHaveStarted = $true
        $script:steps.Add('remove')
        Assert-Replacement $script:heldImage.CanRead 'Removal must retain the validated image handle'
        throw 'controlled removal failure'
    }
    Assert-ReplacementFailure { Invoke-VnmTerminalSetupReplacement $fixtureRoot Remove } 'administrator rights'
    Assert-Replacement ($script:steps.Count -eq 0) `
        'Elevated setup must refuse before inspecting or executing selected code'
    Assert-Replacement (-not $script:VnmTerminalReplacementRemovalMayHaveStarted) `
        'Pre-launch refusal must report that removal never started'
    $script:administrator = $false
    $validation = Invoke-VnmTerminalSetupReplacement $fixtureRoot Validate
    Assert-Replacement ($validation -ceq 'validated') 'Read-only preflight must report validation'
    Assert-Replacement (($script:steps -join ',') -ceq 'target,metadata,permissions,idle,signature') `
        'Preflight must not remove'
    Assert-Replacement (-not $script:heldImage.CanRead) 'Preflight must release its image handle'
    $script:steps.Clear()
    Assert-ReplacementFailure { Invoke-VnmTerminalSetupReplacement $fixtureRoot Remove } 'controlled removal failure'
    Assert-Replacement $script:VnmTerminalReplacementRemovalMayHaveStarted `
        'A removal failure after launch must stay uncertain'
    Assert-Replacement (($script:steps -join ',') -ceq 'target,metadata,permissions,idle,signature,remove') `
        'Removal must revalidate its target'
    Assert-Replacement (-not $script:heldImage.CanRead) 'Failed removal must release its image handle'
    Assert-ReplacementFailure {
        Wait-VnmTerminalReplacementDirectoryRemoval $fixtureRoot -TimeoutSeconds 0
    } 'folder is still present'
    Wait-VnmTerminalReplacementDirectoryRemoval (Join-Path $fixtureRoot 'absent') -TimeoutSeconds 0

    function Invoke-VnmTerminalReplacementRemoval {
        param([string]$MaintenancePath)
        $script:VnmTerminalReplacementRemovalMayHaveStarted = $true
        $script:steps.Add('remove')
        Assert-Replacement $script:heldImage.CanRead 'Removal must retain the validated image handle'
    }
    function Wait-VnmTerminalReplacementDirectoryRemoval {
        param([string]$TargetDirectory)
        $script:steps.Add('cleanup')
        Assert-Replacement (-not $script:heldImage.CanRead) 'Delayed cleanup must start after releasing the image'
    }
    $script:steps.Clear()
    $removed = Invoke-VnmTerminalSetupReplacement $fixtureRoot Remove
    Assert-Replacement ($removed -ceq 'removed') 'Observed removal and cleanup must allow continuation'
    Assert-Replacement (($script:steps -join ',') -ceq 'target,metadata,permissions,idle,signature,remove,cleanup') `
        'Continuation must wait for delayed cleanup'
    function Wait-VnmTerminalReplacementDirectoryRemoval {
        param([string]$TargetDirectory)
        throw 'controlled cleanup failure'
    }
    Assert-ReplacementFailure { Invoke-VnmTerminalSetupReplacement $fixtureRoot Remove } 'controlled cleanup failure'

    $node = Get-Command node.exe -ErrorAction Stop
    & $node.Source (Join-Path $PSScriptRoot 'windows_ifw_replacement_controller_tests.js')
    Assert-Replacement ($LASTEXITCODE -eq 0) 'Controller source and exact Windows PowerShell stdin fixtures must pass'
    & $node.Source (Join-Path $PSScriptRoot 'windows_ifw_process_guard_tests.js')
    Assert-Replacement ($LASTEXITCODE -eq 0) 'Targeted installation process guards must pass'
    Write-Output ('Windows IFW replacement validation and sequencing fixtures passed ' +
        '(no installation lifecycle coverage).')
}
finally {
    $resolved = [IO.Path]::GetFullPath($fixtureRoot)
    $temporaryRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
    if (-not $resolved.StartsWith($temporaryRoot, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Refusing cleanup outside the owned temporary fixture directory.'
    }
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
