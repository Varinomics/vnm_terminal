Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Test-VnmTerminalReplacementAdministrator
{
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    try {
        $principal = [Security.Principal.WindowsPrincipal]::new($identity)
        return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
    }
    finally {
        $identity.Dispose()
    }
}

function Get-VnmTerminalReplacementTarget
{
    param([Parameter(Mandatory = $true)][string]$TargetDirectory)

    if ($TargetDirectory -notmatch '^[A-Za-z]:[\\/]') {
        throw 'Replacement requires an absolute local installation folder.'
    }
    $target = [IO.Path]::GetFullPath($TargetDirectory).TrimEnd('\')
    $expected = Join-Path ([Environment]::GetFolderPath('ProgramFiles')) 'vnm_terminal'
    if ($target -ine $expected) {
        throw 'Automatic replacement supports the protected vnm_terminal installation folder only.'
    }
    $ancestor = $target
    while ($ancestor) {
        $item = Get-Item -LiteralPath $ancestor -Force
        if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw 'The installation folder must not pass through a reparse point.'
        }
        $ancestor = [IO.Path]::GetDirectoryName($ancestor)
    }
    return $target
}

function Assert-VnmTerminalReplacementMetadata
{
    param([Parameter(Mandatory = $true)][string]$TargetDirectory)

    $settings = [Xml.XmlReaderSettings]::new()
    $settings.DtdProcessing = [Xml.DtdProcessing]::Prohibit
    $settings.XmlResolver = $null
    $settings.MaxCharactersInDocument = 1048576
    $reader = [Xml.XmlReader]::Create((Join-Path $TargetDirectory 'components.xml'), $settings)
    try {
        $document = [Xml.XmlDocument]::new()
        $document.XmlResolver = $null
        $document.Load($reader)
    }
    finally {
        $reader.Dispose()
    }
    $packages = @($document.SelectNodes('/Packages/Package/Name') |
        ForEach-Object InnerText)
    $applicationName = $document.SelectSingleNode('/Packages/ApplicationName').InnerText
    if ($applicationName -cne 'vnm_terminal' -or
        $packages.Count -ne 2 -or
        $packages -cnotcontains 'com.varinomics.vnm_terminal' -or
        $packages -cnotcontains 'com.varinomics.vnm_terminal.maintenance')
    {
        throw 'The folder does not contain the expected vnm_terminal installation records.'
    }
}

function Assert-VnmTerminalReplacementPermissions
{
    param([Parameter(Mandatory = $true)][string]$TargetDirectory)

    # Administrator-owned paths prevent ordinary users from replacing the
    # maintenance image or its installation records before elevation.
    $privileged = @('S-1-5-18', 'S-1-5-32-544',
        'S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464')
    $ancestor = $TargetDirectory
    while ($ancestor) {
        $acl = Get-Acl -LiteralPath $ancestor
        if ($privileged -cnotcontains $acl.GetOwner([Security.Principal.SecurityIdentifier]).Value) {
            throw 'An installation ancestor is not owned by a system administrator.'
        }
        foreach ($rule in $acl.GetAccessRules($true, $true, [Security.Principal.SecurityIdentifier])) {
            if ($rule.AccessControlType -eq [Security.AccessControl.AccessControlType]::Allow -and
                ($rule.PropagationFlags -band [Security.AccessControl.PropagationFlags]::InheritOnly) -eq 0 -and
                $privileged -cnotcontains $rule.IdentityReference.Value -and
                ([long]$rule.FileSystemRights -band 0x500D0152L) -ne 0)
            {
                throw 'An installation ancestor permits changes by a nonadministrator.'
            }
        }
        $ancestor = [IO.Path]::GetDirectoryName($ancestor)
    }
}

function Assert-VnmTerminalReplacementIdle
{
    param([Parameter(Mandatory = $true)][string]$TargetDirectory)

    $prefix = $TargetDirectory.TrimEnd('\') + '\'
    foreach ($process in Get-CimInstance -ClassName Win32_Process -OperationTimeoutSec 10) {
        $image = [string]$process.ExecutablePath
        if ($image -and $image.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
            throw ('A program in the installation folder is still running: ' + $image +
                '. Close vnm_terminal and all surviving terminals through their normal controls, then retry.')
        }
        # A hidden product process cannot be treated as proof that the folder is idle.
        if (-not $image -and $process.Name -match '^(vnm_terminal|OpenConsole)[.]exe$') {
            throw 'Setup cannot verify that vnm_terminal and its terminal processes have stopped.'
        }
    }
}

function Get-VnmTerminalReplacementSignature
{
    param([Parameter(Mandatory = $true)][string]$Path)

    # Certificate retrieval may wait on the network. Bound that read-only
    # operation without terminating an installed program or removal process.
    $start = [Diagnostics.ProcessStartInfo]::new()
    $start.FileName = Join-Path ([Environment]::SystemDirectory) 'WindowsPowerShell\v1.0\powershell.exe'
    $start.Arguments = '-NoLogo -NoProfile -NonInteractive -Command -'
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardInput = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $start.StandardOutputEncoding = [Text.UTF8Encoding]::new($false)
    # Let Windows PowerShell resolve its own modules instead of inheriting PowerShell 7 modules.
    $start.EnvironmentVariables.Remove('PSModulePath')
    $script = '[Console]::OutputEncoding = [Text.UTF8Encoding]::new($false); ' +
        '$s = Get-AuthenticodeSignature -LiteralPath ''' + $Path.Replace("'", "''") + '''; ' +
        '[pscustomobject]@{ Status = [string]$s.Status; ' +
        'SignerCertificate = if ($s.SignerCertificate) { @{ Subject = $s.SignerCertificate.Subject } } else { $null }; ' +
        'TimeStamperCertificate = if ($s.TimeStamperCertificate) { @{} } else { $null } } | ConvertTo-Json -Depth 3 -Compress'
    $process = [Diagnostics.Process]::Start($start)
    try {
        $output = $process.StandardOutput.ReadToEndAsync()
        $errors = $process.StandardError.ReadToEndAsync()
        $process.StandardInput.WriteLine($script)
        $process.StandardInput.Close()
        if (-not $process.WaitForExit(30000)) {
            $process.Kill()
            throw 'The read-only signature check timed out. No removal was started.'
        }
        if ($process.ExitCode -ne 0 -or $errors.Result -ne '') {
            throw 'The read-only signature check could not complete. No removal was started.'
        }
        return ($output.Result | ConvertFrom-Json)
    }
    finally {
        $process.Dispose()
    }
}

function Open-VnmTerminalReplacementMaintenanceTool
{
    param([Parameter(Mandatory = $true)][string]$TargetDirectory)

    $path = Join-Path $TargetDirectory 'vnm_terminal_maintenance.exe'
    if (((Get-Item -LiteralPath $path -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw 'The maintenance tool must be a regular file.'
    }
    # Keep the validated image immutable until its process exits. Its delayed
    # self-removal can complete after this handle is released.
    $stream = [IO.File]::Open($path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    try {
        $signature = Get-VnmTerminalReplacementSignature $path
        if ($signature.Status -ne [Management.Automation.SignatureStatus]::Valid -or
            $null -eq $signature.SignerCertificate -or
            $signature.SignerCertificate.Subject -notmatch '(?:^|,\s*)CN=Varinomics Ltd(?:,|$)' -or
            $null -eq $signature.TimeStamperCertificate)
        {
            throw 'The maintenance tool does not have a valid timestamped Varinomics signature.'
        }
        return $stream
    }
    catch {
        $stream.Dispose()
        throw
    }
}

function Invoke-VnmTerminalReplacementRemoval
{
    param([Parameter(Mandatory = $true)][string]$MaintenancePath)

    # Headless IFW cannot request elevation itself. Request it only after the
    # protected path, permissions and held signed image have been validated.
    foreach ($name in @([Environment]::GetEnvironmentVariables('Process').Keys)) {
        if ($name -match '^(COR_|COMPLUS_|CORECLR_|DOTNET_|QT_|QML_|QML2_)') {
            [Environment]::SetEnvironmentVariable($name, [NullString]::Value, 'Process')
        }
    }
    [Environment]::SetEnvironmentVariable('PATH', [Environment]::SystemDirectory, 'Process')
    # A launch error is uncertain unless Windows explicitly reports that the
    # permission request was canceled. Do not infer safety from a missing handle.
    $script:VnmTerminalReplacementRemovalMayHaveStarted = $true
    try {
        $process = Start-Process -FilePath $MaintenancePath -Verb RunAs -PassThru -WindowStyle Hidden `
            -WorkingDirectory ([Environment]::SystemDirectory) `
            -ArgumentList '--reject-messages', '--confirm-command', 'purge'
    }
    catch {
        $exception = $_.Exception
        while ($null -ne $exception) {
            if ($exception -is [ComponentModel.Win32Exception] -and $exception.NativeErrorCode -eq 1223) {
                $script:VnmTerminalReplacementRemovalMayHaveStarted = $false
                throw 'The permission request was canceled. No removal was started.'
            }
            $exception = $exception.InnerException
        }
        throw
    }
    if ($null -eq $process) {
        throw 'Setup could not observe the removal process. Replacement has not started.'
    }
    try {
        # Wait only for the maintenance process. Waiting for its entire tree
        # would also wait for self-deletion while the image handle is held.
        if (-not $process.WaitForExit(300000)) {
            throw ('Removal has not finished after five minutes. Its process may still be running. ' +
                'Replacement has not started; do not retry until removal has ended.')
        }
        if ($process.ExitCode -ne 0) {
            throw ('Removal did not complete successfully (exit code ' + $process.ExitCode + ').')
        }
    }
    finally {
        $process.Dispose()
    }
}

function Wait-VnmTerminalReplacementDirectoryRemoval
{
    param(
        [Parameter(Mandatory = $true)][string]$TargetDirectory,
        [ValidateRange(0, 30)][int]$TimeoutSeconds = 30
    )

    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    while ((Test-Path -LiteralPath $TargetDirectory) -and [DateTime]::UtcNow -lt $deadline) {
        Start-Sleep -Milliseconds 100
    }
    if (Test-Path -LiteralPath $TargetDirectory) {
        throw 'Removal returned success, but the installation folder is still present. Replacement has not started.'
    }
}

function Invoke-VnmTerminalSetupReplacement
{
    param(
        [Parameter(Mandatory = $true)][string]$TargetDirectory,
        [Parameter(Mandatory = $true)][ValidateSet('Validate', 'Remove')][string]$Action
    )

    $script:VnmTerminalReplacementRemovalMayHaveStarted = $false
    if (Test-VnmTerminalReplacementAdministrator) {
        throw ('Setup is running with administrator rights. Close it and start setup normally. ' +
            'Replacement needs a separate permission request for the validated maintenance tool.')
    }
    $target = Get-VnmTerminalReplacementTarget $TargetDirectory
    Assert-VnmTerminalReplacementMetadata $target
    Assert-VnmTerminalReplacementPermissions $target
    Assert-VnmTerminalReplacementIdle $target
    $heldImage = Open-VnmTerminalReplacementMaintenanceTool $target
    try {
        if ($Action -eq 'Validate') {
            return 'validated'
        }
        Invoke-VnmTerminalReplacementRemoval (Join-Path $target 'vnm_terminal_maintenance.exe')
    }
    finally {
        $heldImage.Dispose()
    }
    # IFW removes its running maintenance image and folder with a delayed helper.
    # Never start a replacement while that helper can still delete the new files.
    Wait-VnmTerminalReplacementDirectoryRemoval $target
    return 'removed'
}
