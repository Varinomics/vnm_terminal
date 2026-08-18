[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string]$InstallerPath,

    [switch]$RequireSigned
)

$ErrorActionPreference = 'Stop'

# The publisher the installed binaries must be signed by is the same fact the
# builder enforces at signing time, so both read it from the release manifest
# instead of carrying byte-identical copies of the subject pattern.
$repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$releaseManifest =
    Get-Content -LiteralPath (Join-Path $repositoryRoot 'release\manifest.json') -Raw |
        ConvertFrom-Json
if ($releaseManifest.schema -ne 1) {
    throw "Unsupported release manifest schema $($releaseManifest.schema)."
}
$expectedPublisher = [string]$releaseManifest.signing.publisher
$publisherSubjectPattern = [string]$releaseManifest.signing.publisher_subject_pattern

function Assert-InstallationContract {
    param(
        [Parameter(Mandatory = $true)]
        [bool]$Condition,

        [Parameter(Mandatory = $true)]
        [string]$Message
    )

    if (-not $Condition) {
        throw "Qt IFW installation contract violation: $Message"
    }
}

function Invoke-IfwCommand {
    param(
        [Parameter(Mandatory = $true)]
        [string]$ExecutablePath,

        [Parameter(Mandatory = $true)]
        [string[]]$Arguments,

        [Parameter(Mandatory = $true)]
        [string]$Description
    )

    $output = & $ExecutablePath @Arguments 2>&1 | Out-String
    $exitCode = $LASTEXITCODE
    if ($output.Trim() -ne '') {
        Write-Host $output.TrimEnd()
    }
    Assert-InstallationContract ($exitCode -eq 0) `
        "$Description failed with exit code ${exitCode}."
}

function Get-InstallationRegistrations {
    param(
        [Parameter(Mandatory = $true)]
        [string]$InstallRoot
    )

    $registrationRoots = @(
        'Registry::HKEY_LOCAL_MACHINE\Software\Microsoft\Windows\CurrentVersion\Uninstall',
        'Registry::HKEY_LOCAL_MACHINE\Software\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall',
        'Registry::HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Uninstall'
    )
    $normalizedInstallRoot = $InstallRoot.TrimEnd('\')
    return @($registrationRoots | ForEach-Object {
        $registrationRoot = $_
        if (Test-Path -LiteralPath $registrationRoot) {
            Get-ChildItem -LiteralPath $registrationRoot | ForEach-Object {
                $registration = Get-ItemProperty -LiteralPath $_.PSPath
                if ([string]$registration.InstallLocation -and
                    ([string]$registration.InstallLocation).TrimEnd('\') -ieq
                        $normalizedInstallRoot)
                {
                    [pscustomobject]@{
                        Path = $_.PSPath
                        IsMachine = $registrationRoot -like `
                            'Registry::HKEY_LOCAL_MACHINE*'
                    }
                }
            }
        }
    })
}

function Get-InstallerLogFiles {
    $candidateRoots = @(
        $env:LOCALAPPDATA,
        $env:TEMP,
        $env:USERPROFILE
    ) | Where-Object { -not [string]::IsNullOrWhiteSpace($_) } |
        Select-Object -Unique

    return @($candidateRoots | ForEach-Object {
        $logDirectory = Join-Path $_ 'Varinomics\vnm_terminal'
        if (Test-Path -LiteralPath $logDirectory -PathType Container) {
            Get-ChildItem -LiteralPath $logDirectory `
                -Filter 'InstallationLog-*.txt' -File -ErrorAction SilentlyContinue
        }
    })
}

function Get-ShortcutTarget {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    $shell = New-Object -ComObject WScript.Shell
    try {
        $shortcut = $shell.CreateShortcut($Path)
        try {
            return $shortcut.TargetPath
        }
        finally {
            [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject(
                $shortcut)
        }
    }
    finally {
        [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($shell)
    }
}

function Assert-ProductSignature {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    $signature = Get-AuthenticodeSignature -FilePath $Path
    $acceptedStatuses = if ($RequireSigned) { @('Valid') } else { @('Valid', 'NotSigned') }
    $statusMessage = if ($RequireSigned) {
        "$Path must carry a valid release signature"
    }
    else {
        "$Path must be either validly signed or explicitly unsigned"
    }
    Assert-InstallationContract `
        ($signature.Status -in $acceptedStatuses) `
        $statusMessage
    if ($signature.Status -eq 'Valid') {
        Assert-InstallationContract `
            ($null -ne $signature.SignerCertificate -and
                $signature.SignerCertificate.Subject -match
                    $publisherSubjectPattern) `
            "$Path must be signed by $expectedPublisher"
        if ($RequireSigned) {
            Assert-InstallationContract `
                ($null -ne $signature.TimeStamperCertificate) `
                "$Path must carry a timestamped release signature"
        }
    }
}

Assert-InstallationContract ($env:GITHUB_ACTIONS -eq 'true') `
    'the destructive lifecycle gate may run only on an ephemeral GitHub Actions runner'
Assert-InstallationContract `
    (-not [string]::IsNullOrWhiteSpace($env:RUNNER_TEMP)) `
    'RUNNER_TEMP must identify the ephemeral test root'
function Assert-PeGuiSubsystem {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    $guiSubsystem = 2
    $stream = [IO.File]::OpenRead($Path)
    $reader = [IO.BinaryReader]::new($stream)
    try {
        Assert-InstallationContract ($reader.ReadUInt16() -eq 0x5a4d) `
            "$Path must be a portable executable"
        $stream.Position = 0x3c
        $peHeaderOffset = $reader.ReadUInt32()
        $stream.Position = $peHeaderOffset
        Assert-InstallationContract ($reader.ReadUInt32() -eq 0x00004550) `
            "$Path must carry a PE header"

        # Subsystem sits 68 bytes into the optional header in PE32 and PE32+.
        $stream.Position = $peHeaderOffset + 24 + 68
        Assert-InstallationContract ($reader.ReadUInt16() -eq $guiSubsystem) `
            "$Path must be a graphical image so that Windows allocates it no console"
    }
    finally {
        $reader.Dispose()
        $stream.Dispose()
    }
}

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
$isAdministrator = $principal.IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)
$identity.Dispose()
Assert-InstallationContract `
    $isAdministrator `
    'the all-users lifecycle gate requires an elevated runner'

$resolvedInstallerPath = (Resolve-Path -LiteralPath $InstallerPath).Path
$resolvedRunnerTemp = (Resolve-Path -LiteralPath $env:RUNNER_TEMP).Path
$testId = [Guid]::NewGuid().ToString('N')
$installRoot = Join-Path $resolvedRunnerTemp "vnm-terminal-ifw-install-$testId"
$startMenuGroup = "vnm_terminal_ci_$testId"
$commonPrograms = [Environment]::GetFolderPath(
    [Environment+SpecialFolder]::CommonPrograms)
$userPrograms = [Environment]::GetFolderPath(
    [Environment+SpecialFolder]::Programs)
$startMenuRoot = Join-Path $commonPrograms $startMenuGroup
$userStartMenuRoot = Join-Path $userPrograms $startMenuGroup
$shortcutPath = Join-Path $startMenuRoot 'vnm_terminal.lnk'

$runnerPrefix = $resolvedRunnerTemp.TrimEnd('\') + '\'
Assert-InstallationContract `
    ($installRoot.StartsWith(
        $runnerPrefix,
        [StringComparison]::OrdinalIgnoreCase) -and
        (Split-Path $installRoot -Leaf) -eq "vnm-terminal-ifw-install-$testId") `
    'the installation target must be a unique child of RUNNER_TEMP'
Assert-InstallationContract `
    (-not [string]::IsNullOrWhiteSpace($commonPrograms) -and
        [IO.Path]::IsPathRooted($commonPrograms)) `
    'the common Start Menu Programs root must be available'
Assert-InstallationContract `
    ((Split-Path $startMenuRoot -Leaf) -eq $startMenuGroup) `
    'the Start Menu target must use the unique test identifier'
Assert-InstallationContract (-not (Test-Path -LiteralPath $installRoot)) `
    'the unique installation target must not exist before the test'
Assert-InstallationContract (-not (Test-Path -LiteralPath $startMenuRoot)) `
    'the unique Start Menu target must not exist before the test'
Assert-InstallationContract `
    (-not (Test-Path -LiteralPath $userStartMenuRoot)) `
    'the corresponding per-user Start Menu target must not exist before the test'
Assert-InstallationContract `
    (@(Get-InstallationRegistrations $installRoot).Count -eq 0) `
    'the unique installation must not be registered before the test'

$existingLogs = @{}
foreach ($logFile in Get-InstallerLogFiles) {
    $existingLogs[$logFile.FullName] = $true
}
$maintenancePath = Join-Path $installRoot 'vnm_terminal_maintenance.exe'
$purgeCompleted = $false

try {
    Invoke-IfwCommand `
        -ExecutablePath $resolvedInstallerPath `
        -Arguments @(
            'install',
            '--root', $installRoot,
            '--accept-licenses',
            '--accept-messages',
            '--confirm-command',
            "StartMenuDir=$startMenuGroup",
            'AllUsers=true'
        ) `
        -Description 'generated installer commit'

    $launcherPath = Join-Path $installRoot 'vnm_terminal.exe'
    $runtimePath = Join-Path `
        $installRoot 'vnm_terminal_runtime\vnm_terminal.exe'
    $windowsPluginPath = Join-Path `
        $installRoot 'vnm_terminal_runtime\platforms\qwindows.dll'
    foreach ($requiredFile in @(
        $launcherPath,
        $runtimePath,
        $windowsPluginPath,
        $maintenancePath,
        $shortcutPath
    )) {
        Assert-InstallationContract `
            (Test-Path -LiteralPath $requiredFile -PathType Leaf) `
            "the committed installation must contain $requiredFile"
    }
    Assert-InstallationContract `
        (-not (Test-Path -LiteralPath $userStartMenuRoot)) `
        'the all-users installation must not create a per-user shortcut group'

    $shortcutTarget = Get-ShortcutTarget $shortcutPath
    Assert-InstallationContract ($shortcutTarget -ieq $launcherPath) `
        'the Start Menu shortcut must target the installed portable launcher'

    $registrations = @(Get-InstallationRegistrations $installRoot)
    Assert-InstallationContract `
        ($registrations.Count -eq 1 -and $registrations[0].IsMachine) `
        'the committed all-users installation must have one machine registration'

    Assert-ProductSignature $resolvedInstallerPath
    # The binaries signed inside the payload are declared once, in the manifest
    # the builder signs them from, so a payload binary added there is verified
    # here instead of in a second list somebody has to remember. The maintenance
    # tool is written by the installer from the signed installer base and is not
    # part of the payload.
    foreach ($payloadBinary in $releaseManifest.signing.payload_binaries) {
        Assert-ProductSignature (
            Join-Path $installRoot ($payloadBinary -replace '/', '\'))
    }
    Assert-ProductSignature $maintenancePath

    # A console-subsystem installer makes Windows create, show and destroy an
    # empty terminal window before its wizard appears, and the maintenance tool
    # is written from the same base binary.
    Assert-PeGuiSubsystem $resolvedInstallerPath
    Assert-PeGuiSubsystem $maintenancePath

    foreach ($executable in @($launcherPath, $runtimePath)) {
        $process = Start-Process -FilePath $executable `
            -ArgumentList '--help' -Wait -PassThru
        Assert-InstallationContract ($process.ExitCode -eq 0) `
            "$executable --help must exit successfully"
    }

    Invoke-IfwCommand `
        -ExecutablePath $maintenancePath `
        -Arguments @(
            'purge',
            '--accept-messages',
            '--confirm-command'
        ) `
        -Description 'maintenance-tool purge'

    $deadline = [DateTime]::UtcNow.AddSeconds(30)
    while (((Test-Path -LiteralPath $installRoot) -or
            (Test-Path -LiteralPath $startMenuRoot) -or
            (@(Get-InstallationRegistrations $installRoot).Count -ne 0)) -and
        [DateTime]::UtcNow -lt $deadline) {
        Start-Sleep -Milliseconds 250
    }

    Assert-InstallationContract (-not (Test-Path -LiteralPath $installRoot)) `
        'purge must remove the complete unique installation target'
    Assert-InstallationContract (-not (Test-Path -LiteralPath $startMenuRoot)) `
        'purge must remove the complete unique Start Menu target'
    Assert-InstallationContract `
        (-not (Test-Path -LiteralPath $userStartMenuRoot)) `
        'purge must leave no corresponding per-user Start Menu target'
    Assert-InstallationContract `
        (@(Get-InstallationRegistrations $installRoot).Count -eq 0) `
        'purge must remove the unique installation registration'
    $purgeCompleted = $true
}
catch {
    $newLogs = @(Get-InstallerLogFiles | Where-Object {
        -not $existingLogs.ContainsKey($_.FullName)
    } | Sort-Object LastWriteTimeUtc -Descending)
    foreach ($logFile in $newLogs | Select-Object -First 3) {
        Write-Host "Qt IFW diagnostic log: $($logFile.FullName)"
        Get-Content -LiteralPath $logFile.FullName -ErrorAction SilentlyContinue |
            Write-Host
    }
    throw
}
finally {
    if (-not $purgeCompleted -and
        (Test-Path -LiteralPath $maintenancePath -PathType Leaf)) {
        & $maintenancePath purge --accept-messages --confirm-command 2>&1 |
            Out-Host
        Start-Sleep -Milliseconds 500
    }

    if (Test-Path -LiteralPath $installRoot) {
        Remove-Item -LiteralPath $installRoot -Recurse -Force `
            -ErrorAction SilentlyContinue
    }
    if (Test-Path -LiteralPath $startMenuRoot) {
        Remove-Item -LiteralPath $startMenuRoot -Recurse -Force `
            -ErrorAction SilentlyContinue
    }
}

Write-Host "Qt IFW installation lifecycle passed: $resolvedInstallerPath"
