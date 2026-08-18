[CmdletBinding()]
param(
    [string]$PayloadPath,

    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string]$IfwRoot,

    [string]$SignToolPath,
    [string]$TrustedSigningDlibPath,
    [string]$TrustedSigningMetadataPath
)

$ErrorActionPreference = 'Stop'

# Qt IFW identity and the publisher the signature must carry are release
# metadata shared with the provisioner, the packaging contract test and the
# installation lifecycle test, so they are read rather than restated. The
# timestamp service is this script's own operational choice and has no second
# home to drift from.
$repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$releaseManifest =
    Get-Content -LiteralPath (Join-Path $repositoryRoot 'release\manifest.json') -Raw |
        ConvertFrom-Json
if ($releaseManifest.schema -ne 1) {
    throw "Unsupported release manifest schema $($releaseManifest.schema)."
}
$ifwVersion = [string]$releaseManifest.qt_ifw.version
$expectedPublisher = [string]$releaseManifest.signing.publisher
$publisherSubjectPattern = [string]$releaseManifest.signing.publisher_subject_pattern
$checksumSuffix = [string]$releaseManifest.checksum.suffix
$timestampUrl = 'http://timestamp.acs.microsoft.com'

function Assert-FileExists {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,

        [Parameter(Mandatory = $true)]
        [string]$Description
    )

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "$Description was not found: $Path"
    }
}

function Assert-DirectoryExists {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,

        [Parameter(Mandatory = $true)]
        [string]$Description
    )

    if (-not (Test-Path -LiteralPath $Path -PathType Container)) {
        throw "$Description was not found: $Path"
    }
}

function Get-Sha256FileHash {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    $sha256 = [System.Security.Cryptography.SHA256]::Create()
    try {
        $stream = [System.IO.File]::OpenRead($Path)
        try {
            $hashBytes = $sha256.ComputeHash($stream)
            return ([System.BitConverter]::ToString($hashBytes)).Replace('-', '').ToLowerInvariant()
        }
        finally {
            $stream.Dispose()
        }
    }
    finally {
        $sha256.Dispose()
    }
}

function Test-PeHasCertificateTable {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    $stream = [System.IO.File]::OpenRead($Path)
    $reader = [System.IO.BinaryReader]::new($stream)
    try {
        if ($stream.Length -lt 64) {
            throw "Portable executable is too small to contain a DOS header: $Path"
        }

        if ($reader.ReadUInt16() -ne 0x5a4d) {
            throw "Portable executable does not have an MZ header: $Path"
        }

        $stream.Position = 0x3c
        $peHeaderOffset = $reader.ReadUInt32()
        if ($peHeaderOffset -gt $stream.Length - 24) {
            throw "Portable executable has an invalid PE header offset: $Path"
        }

        $stream.Position = $peHeaderOffset
        if ($reader.ReadUInt32() -ne 0x00004550) {
            throw "Portable executable does not have a PE header: $Path"
        }

        $stream.Position = $peHeaderOffset + 20
        $optionalHeaderSize = $reader.ReadUInt16()
        $optionalHeaderOffset = $peHeaderOffset + 24
        if ($optionalHeaderSize -lt 2 -or
            $optionalHeaderOffset + $optionalHeaderSize -gt $stream.Length) {
            throw "Portable executable has a truncated optional header: $Path"
        }

        $stream.Position = $optionalHeaderOffset
        $optionalHeaderMagic = $reader.ReadUInt16()
        if ($optionalHeaderMagic -eq 0x010b) {
            $dataDirectoryOffset = 96
            $directoryCountOffset = 92
        }
        elseif ($optionalHeaderMagic -eq 0x020b) {
            $dataDirectoryOffset = 112
            $directoryCountOffset = 108
        }
        else {
            throw "Portable executable has an unsupported optional header: $Path"
        }

        if ($optionalHeaderSize -lt $directoryCountOffset + 4) {
            throw "Portable executable has no complete data-directory count: $Path"
        }

        $stream.Position = $optionalHeaderOffset + $directoryCountOffset
        $directoryCount = $reader.ReadUInt32()
        if ($directoryCount -lt 5) {
            return $false
        }

        $certificateEntryOffset = $dataDirectoryOffset + 32
        if ($optionalHeaderSize -lt $certificateEntryOffset + 8) {
            throw "Portable executable has no complete security directory: $Path"
        }

        $stream.Position = $optionalHeaderOffset + $certificateEntryOffset
        $certificateOffset = $reader.ReadUInt32()
        $certificateSize = $reader.ReadUInt32()
        if ($certificateOffset -eq 0 -and $certificateSize -eq 0) {
            return $false
        }
        if ($certificateOffset -eq 0 -or $certificateSize -eq 0) {
            throw "Portable executable has an inconsistent security directory: $Path"
        }
        return $true
    }
    finally {
        $reader.Dispose()
        $stream.Dispose()
    }
}

function Set-PeGuiSubsystem {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    $guiSubsystem = 2
    $consoleSubsystem = 3
    $checkSumFieldOffset = 64
    $subsystemFieldOffset = 68

    $stream = [System.IO.File]::Open(
        $Path,
        [System.IO.FileMode]::Open,
        [System.IO.FileAccess]::ReadWrite,
        [System.IO.FileShare]::None)
    $reader = [System.IO.BinaryReader]::new($stream)
    $writer = [System.IO.BinaryWriter]::new($stream)
    try {
        if ($stream.Length -lt 64) {
            throw "Portable executable is too small to contain a DOS header: $Path"
        }

        if ($reader.ReadUInt16() -ne 0x5a4d) {
            throw "Portable executable does not have an MZ header: $Path"
        }

        $stream.Position = 0x3c
        $peHeaderOffset = $reader.ReadUInt32()
        if ($peHeaderOffset -gt $stream.Length - 24) {
            throw "Portable executable has an invalid PE header offset: $Path"
        }

        $stream.Position = $peHeaderOffset
        if ($reader.ReadUInt32() -ne 0x00004550) {
            throw "Portable executable does not have a PE header: $Path"
        }

        $stream.Position = $peHeaderOffset + 20
        $optionalHeaderSize = $reader.ReadUInt16()
        $optionalHeaderOffset = $peHeaderOffset + 24
        if ($optionalHeaderSize -lt $subsystemFieldOffset + 2 -or
            $optionalHeaderOffset + $optionalHeaderSize -gt $stream.Length) {
            throw "Portable executable has a truncated optional header: $Path"
        }

        # CheckSum and Subsystem occupy the same optional-header offsets in PE32
        # and PE32+, so the image magic needs no special case here.
        $stream.Position = $optionalHeaderOffset + $checkSumFieldOffset
        if ($reader.ReadUInt32() -ne 0) {
            throw ('Portable executable carries a header checksum that this ' +
                "rewrite would invalidate: $Path")
        }

        $stream.Position = $optionalHeaderOffset + $subsystemFieldOffset
        $subsystem = $reader.ReadUInt16()
        if ($subsystem -eq $guiSubsystem) {
            return
        }
        if ($subsystem -ne $consoleSubsystem) {
            throw "Portable executable has an unsupported subsystem ${subsystem}: $Path"
        }

        $stream.Position = $optionalHeaderOffset + $subsystemFieldOffset
        $writer.Write([uint16]$guiSubsystem)
        $writer.Flush()

        $stream.Position = $optionalHeaderOffset + $subsystemFieldOffset
        if ($reader.ReadUInt16() -ne $guiSubsystem) {
            throw "Portable executable did not retain the GUI subsystem: $Path"
        }
    }
    finally {
        $writer.Dispose()
        $reader.Dispose()
        $stream.Dispose()
    }
}

function Get-DirectoryInventory {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    $resolvedPath = (Resolve-Path -LiteralPath $Path).Path
    $inventory = [ordered]@{}
    Get-ChildItem -LiteralPath $resolvedPath -File -Recurse |
        Sort-Object FullName |
        ForEach-Object {
            $relativePath = $_.FullName.Substring($resolvedPath.Length + 1)
            $inventory[$relativePath] = Get-Sha256FileHash $_.FullName
        }
    return $inventory
}

function Assert-DirectoryCopy {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Source,

        [Parameter(Mandatory = $true)]
        [string]$Destination
    )

    $sourceInventory = Get-DirectoryInventory $Source
    $destinationInventory = Get-DirectoryInventory $Destination
    if ($sourceInventory.Count -ne $destinationInventory.Count) {
        throw 'The staged IFW payload does not contain the same file set as the portable payload.'
    }

    foreach ($relativePath in $sourceInventory.Keys) {
        if (-not $destinationInventory.Contains($relativePath) -or
            $destinationInventory[$relativePath] -ne $sourceInventory[$relativePath]) {
            throw "The staged IFW payload differs from the portable payload: $relativePath"
        }
    }
}

function Get-PayloadVersion {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    $buildInfoPath = Join-Path $Path 'vnm_terminal_runtime\vnm_terminal_build_info.txt'
    Assert-FileExists $buildInfoPath 'Portable payload build information'
    $versionLine = Get-Content -LiteralPath $buildInfoPath |
        Where-Object { $_ -match '^Version:\s*(?<version>[0-9]+(?:\.[0-9]+)+)\s*$' } |
        Select-Object -First 1
    if (-not $versionLine) {
        throw "Could not derive the package version from $buildInfoPath"
    }

    [void]($versionLine -match '^Version:\s*(?<version>[0-9]+(?:\.[0-9]+)+)\s*$')
    return $Matches.version
}

function Write-Template {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Source,

        [Parameter(Mandatory = $true)]
        [string]$Destination,

        [Parameter(Mandatory = $true)]
        [hashtable]$Values
    )

    $content = [System.IO.File]::ReadAllText($Source)
    foreach ($entry in $Values.GetEnumerator()) {
        $content = $content.Replace("@$($entry.Key)@", $entry.Value)
    }
    if ($content -match '@VNM_TERMINAL_[A-Z_]+@') {
        throw "An IFW template placeholder was not resolved in $Source"
    }

    [System.IO.File]::WriteAllText(
        $Destination,
        $content,
        [System.Text.UTF8Encoding]::new($false))
}

function Resolve-SigningConfiguration {
    $signingValues = @(
        $SignToolPath,
        $TrustedSigningDlibPath,
        $TrustedSigningMetadataPath
    )
    $configuredValues = @($signingValues | Where-Object { $_ }).Count
    if ($configuredValues -eq 0) {
        return $false
    }
    if ($configuredValues -ne $signingValues.Count) {
        throw 'Signing requires SignToolPath, TrustedSigningDlibPath, and TrustedSigningMetadataPath together.'
    }

    Assert-FileExists $SignToolPath 'SignTool'
    Assert-FileExists $TrustedSigningDlibPath 'Trusted Signing client library'
    Assert-FileExists $TrustedSigningMetadataPath 'Trusted Signing metadata'
    return $true
}

function Invoke-TrustedSigning {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    & $SignToolPath sign /v /fd SHA256 /tr $timestampUrl /td SHA256 `
        /dlib $TrustedSigningDlibPath /dmdf $TrustedSigningMetadataPath $Path
    if ($LASTEXITCODE -ne 0) {
        throw "Trusted Signing failed for $Path with exit code $LASTEXITCODE"
    }

    $signature = Get-AuthenticodeSignature -LiteralPath $Path
    if ($signature.Status -ne 'Valid') {
        throw "The signature on $Path is not valid: $($signature.StatusMessage)"
    }
    if ($null -eq $signature.SignerCertificate -or
        $signature.SignerCertificate.Subject -notmatch $publisherSubjectPattern)
    {
        throw "The signature on $Path does not identify $expectedPublisher"
    }
    if ($null -eq $signature.TimeStamperCertificate) {
        throw "The signature on $Path is not timestamped"
    }
}

if (-not $PayloadPath) {
    $PayloadPath = Join-Path $repositoryRoot 'dist\portable_candidate'
}
$PayloadPath = (Resolve-Path -LiteralPath $PayloadPath).Path
Assert-DirectoryExists $PayloadPath 'Portable payload'
Assert-FileExists (Join-Path $PayloadPath 'vnm_terminal.exe') 'Portable launcher'
Assert-FileExists `
    (Join-Path $PayloadPath 'vnm_terminal_runtime\vnm_terminal.exe') `
    'Portable runtime executable'

$packageVersion = Get-PayloadVersion $PayloadPath
$releaseDate = Get-Date -Format 'yyyy-MM-dd'
$resolvedIfwRoot = (Resolve-Path -LiteralPath $IfwRoot).Path
Assert-DirectoryExists $resolvedIfwRoot 'Qt IFW root'
$binaryCreatorPath = Join-Path $resolvedIfwRoot 'bin\binarycreator.exe'
$installerBaseSourcePath = Join-Path $resolvedIfwRoot 'bin\installerbase.exe'
Assert-FileExists $binaryCreatorPath 'Qt IFW binarycreator'
Assert-FileExists $installerBaseSourcePath 'Qt IFW installerbase'

$ifwVersionOutput = & $installerBaseSourcePath --version 2>&1 | Out-String
if ($LASTEXITCODE -ne 0 -or $ifwVersionOutput -notmatch "IFW Version:\s*$([regex]::Escape($ifwVersion))") {
    throw "The selected Qt IFW root is not version $ifwVersion"
}

$signingEnabled = Resolve-SigningConfiguration
$stageRoot = Join-Path $repositoryRoot 'build_ifw\vnm_terminal_installer'
$configRoot = Join-Path $stageRoot 'config'
$packagesRoot = Join-Path $stageRoot 'packages'
$packageRoot = Join-Path $packagesRoot 'com.varinomics.vnm_terminal'
$packageDataRoot = Join-Path $packageRoot 'data'
$packageMetaRoot = Join-Path $packageRoot 'meta'
$maintenancePackageRoot = Join-Path $packagesRoot 'com.varinomics.vnm_terminal.maintenance'
$maintenanceDataRoot = Join-Path $maintenancePackageRoot 'data'
$maintenanceMetaRoot = Join-Path $maintenancePackageRoot 'meta'
$privateInstallerBasePath = Join-Path $maintenanceDataRoot 'installerbase.exe'

if (Test-Path -LiteralPath $stageRoot) {
    Remove-Item -LiteralPath $stageRoot -Recurse -Force
}
New-Item -ItemType Directory -Force -Path `
    $configRoot, `
    $packageDataRoot, `
    $packageMetaRoot, `
    $maintenanceDataRoot, `
    $maintenanceMetaRoot |
        Out-Null

# Sign the portable payload itself before it is copied into the installer.
# The release publishes this same directory as the portable ZIP, so signing only
# the IFW staging copy would leave one of the two public Windows distributions
# carrying unsigned executables.
if ($signingEnabled) {
    Invoke-TrustedSigning (Join-Path $PayloadPath 'vnm_terminal.exe')
    Invoke-TrustedSigning `
        (Join-Path $PayloadPath 'vnm_terminal_runtime\vnm_terminal.exe')
}

Copy-Item -Path (Join-Path $PayloadPath '*') -Destination $packageDataRoot -Recurse -Force
Assert-DirectoryCopy $PayloadPath $packageDataRoot

$ifwSourceRoot = Join-Path $repositoryRoot 'packaging\windows\ifw'
Write-Template `
    -Source (Join-Path $ifwSourceRoot 'config.xml.in') `
    -Destination (Join-Path $configRoot 'config.xml') `
    -Values @{ VNM_TERMINAL_VERSION = $packageVersion }
Write-Template `
    -Source (Join-Path $ifwSourceRoot 'package.xml.in') `
    -Destination (Join-Path $packageMetaRoot 'package.xml') `
    -Values @{
        VNM_TERMINAL_VERSION = $packageVersion
        VNM_TERMINAL_RELEASE_DATE = $releaseDate
    }
Write-Template `
    -Source (Join-Path $ifwSourceRoot 'maintenance_package.xml.in') `
    -Destination (Join-Path $maintenanceMetaRoot 'package.xml') `
    -Values @{
        VNM_TERMINAL_IFW_VERSION = $ifwVersion
        VNM_TERMINAL_RELEASE_DATE = $releaseDate
    }
Copy-Item -LiteralPath (Join-Path $ifwSourceRoot 'installscript.qs') `
    -Destination $packageMetaRoot
Copy-Item -LiteralPath (Join-Path $ifwSourceRoot 'maintenance_installscript.qs') `
    -Destination $maintenanceMetaRoot
Copy-Item -LiteralPath (Join-Path $repositoryRoot 'LICENSE') `
    -Destination (Join-Path $packageMetaRoot 'LICENSE.txt')
Copy-Item -LiteralPath (Join-Path $repositoryRoot 'THIRD_PARTY_NOTICES.md') `
    -Destination (Join-Path $packageDataRoot 'THIRD_PARTY_NOTICES.md')
Copy-Item -LiteralPath (Join-Path $ifwSourceRoot 'Qt-GPL-exception-1.0.txt') `
    -Destination (Join-Path $packageDataRoot 'QT_INSTALLER_FRAMEWORK_LICENSE_EXCEPTION.txt')
Copy-Item -LiteralPath (Join-Path $repositoryRoot 'src\vnm_terminal.ico') `
    -Destination (Join-Path $configRoot 'vnm_terminal.ico')
Copy-Item -LiteralPath (Join-Path $ifwSourceRoot 'style.qss') `
    -Destination $configRoot
Copy-Item -LiteralPath (Join-Path $ifwSourceRoot 'controller.qs') `
    -Destination $configRoot
Copy-Item -LiteralPath (Join-Path $ifwSourceRoot 'checkbox_check.svg') `
    -Destination $configRoot
Copy-Item -LiteralPath (Join-Path $ifwSourceRoot 'radio_dot.svg') `
    -Destination $configRoot
Copy-Item -LiteralPath (Join-Path $ifwSourceRoot 'combo_arrow.svg') `
    -Destination $configRoot
Copy-Item -LiteralPath (Join-Path $ifwSourceRoot 'varinomics_logo.png') `
    -Destination $configRoot
Copy-Item -LiteralPath (Join-Path $ifwSourceRoot 'varinomics_geometry.svg') `
    -Destination $configRoot
Copy-Item -LiteralPath (Join-Path $ifwSourceRoot 'varinomics_banner.png') `
    -Destination $configRoot
Copy-Item -LiteralPath (Join-Path $ifwSourceRoot 'varinomics_banner@2x.png') `
    -Destination $configRoot
Copy-Item -LiteralPath (Join-Path $ifwSourceRoot 'varinomics_geometry.png') `
    -Destination $configRoot
Copy-Item -LiteralPath (Join-Path $ifwSourceRoot 'varinomics_geometry@2x.png') `
    -Destination $configRoot
Copy-Item -LiteralPath (Join-Path $ifwSourceRoot 'log_path_probe.ps1') `
    -Destination $configRoot
Copy-Item -LiteralPath (Join-Path $ifwSourceRoot 'theme_resources.qrc') `
    -Destination $configRoot
Copy-Item -LiteralPath $installerBaseSourcePath -Destination $privateInstallerBasePath
# Qt ships installerbase as a console-subsystem image so that its headless
# commands can print. Windows therefore allocates a console for every graphical
# launch, and on Windows 11 that console is handed to the default terminal,
# which shows and destroys an empty window before installerbase detaches. Both
# delivered binaries are published as graphical images so no terminal window
# ever appears; stdout still reaches an inherited console or pipe, so the
# headless commands keep working. This runs before signing so the signature
# covers the rewritten header.
Set-PeGuiSubsystem $privateInstallerBasePath

if ($signingEnabled) {
    Invoke-TrustedSigning $privateInstallerBasePath
}

$distRoot = Join-Path $repositoryRoot 'dist'
New-Item -ItemType Directory -Force -Path $distRoot | Out-Null
$artifactSuffix = if ($signingEnabled) { '' } else { '_unsigned' }
$artifactName = "vnm_terminal_v${packageVersion}_windows_x64${artifactSuffix}.exe"
$artifactPath = Join-Path $distRoot $artifactName
$checksumPath = "$artifactPath$checksumSuffix"
if (Test-Path -LiteralPath $artifactPath) {
    Remove-Item -LiteralPath $artifactPath -Force
}
if (Test-Path -LiteralPath $checksumPath) {
    Remove-Item -LiteralPath $checksumPath -Force
}

& $binaryCreatorPath --offline-only --ignore-translations `
    --config (Join-Path $configRoot 'config.xml') `
    --packages $packagesRoot `
    --resources (Join-Path $configRoot 'theme_resources.qrc') `
    --template $installerBaseSourcePath `
    --archive-format 7z `
    --compression 9 `
    $artifactPath
if ($LASTEXITCODE -ne 0) {
    throw "Qt IFW binarycreator failed with exit code $LASTEXITCODE"
}
Assert-FileExists $artifactPath 'Qt IFW installer artifact'
# binarycreator appends the payload to the unsigned console-subsystem template,
# so the finished installer inherits the same console allocation as the
# maintenance-tool base above and needs the same header before it is signed.
Set-PeGuiSubsystem $artifactPath

if ($signingEnabled) {
    Invoke-TrustedSigning $artifactPath
}
else {
    if (Test-PeHasCertificateTable $artifactPath) {
        throw 'Unsigned artifact unexpectedly contains an Authenticode certificate table.'
    }
}

$artifactHash = Get-Sha256FileHash $artifactPath
[System.IO.File]::WriteAllText(
    $checksumPath,
    "$artifactHash  $artifactName`n",
    [System.Text.ASCIIEncoding]::new())

Write-Host "Qt IFW version: $ifwVersion"
Write-Host "Payload version: $packageVersion"
Write-Host "Signed: $signingEnabled"
Write-Host "Artifact: $artifactPath"
Write-Host "SHA-256: $artifactHash"
