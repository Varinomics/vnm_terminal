[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$SourceRoot,

    [string]$ArtifactPath,

    [string]$DumpPath
)

$ErrorActionPreference = 'Stop'

function Assert-IfwContract {
    param(
        [Parameter(Mandatory = $true)]
        [bool]$Condition,

        [Parameter(Mandatory = $true)]
        [string]$Message
    )

    if (-not $Condition) {
        throw "Qt IFW contract violation: $Message"
    }
}

function Get-IfwRelativeLuminance {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Color
    )

    if ($Color -notmatch '^#[0-9A-Fa-f]{6}$') {
        throw "Invalid IFW theme color: $Color"
    }

    $channels = 0, 2, 4 | ForEach-Object {
        [Convert]::ToInt32($Color.Substring($_ + 1, 2), 16) / 255.0
    }
    $linearChannels = $channels | ForEach-Object {
        if ($_ -le 0.04045) {
            $_ / 12.92
        }
        else {
            [Math]::Pow(($_ + 0.055) / 1.055, 2.4)
        }
    }

    return 0.2126 * $linearChannels[0] +
        0.7152 * $linearChannels[1] +
        0.0722 * $linearChannels[2]
}

function Assert-IfwContrast {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Foreground,

        [Parameter(Mandatory = $true)]
        [string]$Background,

        [Parameter(Mandatory = $true)]
        [double]$Minimum,

        [Parameter(Mandatory = $true)]
        [string]$Message
    )

    $foregroundLuminance = Get-IfwRelativeLuminance $Foreground
    $backgroundLuminance = Get-IfwRelativeLuminance $Background
    $ratio =
        ([Math]::Max($foregroundLuminance, $backgroundLuminance) + 0.05) /
        ([Math]::Min($foregroundLuminance, $backgroundLuminance) + 0.05)
    Assert-IfwContract ($ratio -ge $Minimum) `
        "$Message (contrast ratio $([Math]::Round($ratio, 2)):1)"
}

function Get-IfwPngDimensions {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    Add-Type -AssemblyName System.Drawing
    $image = [Drawing.Image]::FromFile($Path)
    try {
        return @($image.Width, $image.Height)
    }
    finally {
        $image.Dispose()
    }
}

function Assert-IfwReadyPageRuntime {
    param(
        [Parameter(Mandatory = $true)]
        [string]$ControllerScriptPath
    )

    $node = Get-Command node.exe -ErrorAction SilentlyContinue
    Assert-IfwContract ($null -ne $node) `
        'the Ready-page runtime contract requires node.exe'

    $harnessPath = [IO.Path]::GetTempFileName() + '.js'
    try {
        $harness = @'
const fs = require("fs");
const controllerScript = fs.readFileSync(process.argv[2], "utf8");

let hiddenColumn = null;
let lookupCount = 0;
const installComponentsTreeview = {
    hideColumn(column) {
        hiddenColumn = column;
    },
};
const readyPage = { subTitle: "" };

global.QInstaller = { ComponentSelection: 1 };
global.installer = {
    isInstaller() { return true; },
    setDefaultPageVisible() {},
    setValue() {},
    toNativeSeparators(value) { return value; },
    value() { return "C:\\"; },
    readFile() { return ""; },
    fileExists() { return false; },
};
global.gui = {
    pageWidgetByObjectName(name) {
        if (name !== "ReadyForInstallationPage")
            throw new Error("unexpected page lookup: " + name);
        return readyPage;
    },
    findChild(parent, name) {
        if (parent !== readyPage || name !== "InstallComponentsTreeview")
            throw new Error("unexpected recursive child lookup");
        ++lookupCount;
        return installComponentsTreeview;
    },
};

eval(controllerScript);
new Controller();
Controller.prototype.ReadyForInstallationPageCallback();

if (lookupCount !== 1 || hiddenColumn !== 5 ||
    readyPage.subTitle !== "Review your choices before installation.")
{
    throw new Error("Ready-page callback did not satisfy its runtime contract");
}
'@
        [IO.File]::WriteAllText(
            $harnessPath,
            $harness,
            [Text.UTF8Encoding]::new($false))
        $runtimeOutput = & $node.Source $harnessPath $ControllerScriptPath 2>&1 |
            Out-String
        Assert-IfwContract ($LASTEXITCODE -eq 0) `
            "the Ready-page callback must use IFW's recursive object lookup without a runtime exception: $runtimeOutput"
    }
    finally {
        Remove-Item -LiteralPath $harnessPath -Force -ErrorAction SilentlyContinue
    }
}

$resolvedSourceRoot = (Resolve-Path -LiteralPath $SourceRoot).Path
$ifwRoot = Join-Path $resolvedSourceRoot 'packaging\windows\ifw'
$configPath = Join-Path $ifwRoot 'config.xml.in'
$styleSheetPath = Join-Path $ifwRoot 'style.qss'
$controllerScriptPath = Join-Path $ifwRoot 'controller.qs'
$checkboxCheckPath = Join-Path $ifwRoot 'checkbox_check.svg'
$radioDotPath = Join-Path $ifwRoot 'radio_dot.svg'
$comboArrowPath = Join-Path $ifwRoot 'combo_arrow.svg'
$brandLogoPath = Join-Path $ifwRoot 'varinomics_logo.png'
$brandGeometryPath = Join-Path $ifwRoot 'varinomics_geometry.svg'
$brandBannerPath = Join-Path $ifwRoot 'varinomics_banner.png'
$brandBannerHighDpiPath = Join-Path $ifwRoot 'varinomics_banner@2x.png'
$brandGeometryPngPath = Join-Path $ifwRoot 'varinomics_geometry.png'
$brandGeometryHighDpiPath = Join-Path $ifwRoot 'varinomics_geometry@2x.png'
$brandProvenancePath = Join-Path $ifwRoot 'brand_assets.provenance.json'
$logPathProbePath = Join-Path $ifwRoot 'log_path_probe.ps1'
$themeResourcesPath = Join-Path $ifwRoot 'theme_resources.qrc'
$packagePath = Join-Path $ifwRoot 'package.xml.in'
$installScriptPath = Join-Path $ifwRoot 'installscript.qs'
$maintenancePackagePath = Join-Path $ifwRoot 'maintenance_package.xml.in'
$maintenanceInstallScriptPath = Join-Path $ifwRoot 'maintenance_installscript.qs'
$buildScriptPath = Join-Path $resolvedSourceRoot 'tools\build_windows_ifw_installer.ps1'
$brandRendererPath = Join-Path $resolvedSourceRoot 'tools\render_windows_ifw_brand_assets.py'
$noticesPath = Join-Path $resolvedSourceRoot 'THIRD_PARTY_NOTICES.md'

[xml]$config = Get-Content -Raw -LiteralPath $configPath
$styleSheet = Get-Content -Raw -LiteralPath $styleSheetPath
$controllerScript = Get-Content -Raw -LiteralPath $controllerScriptPath
$checkboxCheck = Get-Content -Raw -LiteralPath $checkboxCheckPath
$radioDot = Get-Content -Raw -LiteralPath $radioDotPath
$comboArrow = Get-Content -Raw -LiteralPath $comboArrowPath
$brandGeometry = Get-Content -Raw -LiteralPath $brandGeometryPath
[xml]$brandGeometryXml = $brandGeometry
$brandProvenance = Get-Content -Raw -LiteralPath $brandProvenancePath |
    ConvertFrom-Json
$logPathProbe = Get-Content -Raw -LiteralPath $logPathProbePath
[xml]$themeResources = Get-Content -Raw -LiteralPath $themeResourcesPath
[xml]$package = Get-Content -Raw -LiteralPath $packagePath
[xml]$maintenancePackage = Get-Content -Raw -LiteralPath $maintenancePackagePath
$installScript = Get-Content -Raw -LiteralPath $installScriptPath
$maintenanceInstallScript = Get-Content -Raw -LiteralPath $maintenanceInstallScriptPath
$buildScript = Get-Content -Raw -LiteralPath $buildScriptPath
$brandRenderer = Get-Content -Raw -LiteralPath $brandRendererPath
$notices = Get-Content -Raw -LiteralPath $noticesPath

Assert-IfwReadyPageRuntime $controllerScriptPath

Assert-IfwContract ($config.Installer.Name -eq 'vnm_terminal') `
    'the product name must match the application'
Assert-IfwContract ($config.Installer.Publisher -eq 'Varinomics Ltd') `
    'the publisher must be Varinomics Ltd'
Assert-IfwContract `
    ($config.Installer.TargetDir -eq '@ApplicationsDirX64@/vnm_terminal') `
    'the target must be 64-bit Program Files'
Assert-IfwContract `
    ($config.Installer.MaintenanceToolName -eq 'vnm_terminal_maintenance') `
    'the maintenance tool name must remain stable'
Assert-IfwContract `
    ($config.Installer.RunProgram -eq '@TargetDir@/vnm_terminal.exe') `
    'launch-after-install must target the portable launcher'
Assert-IfwContract `
    ($config.Installer.RunProgramDescription -eq 'Launch vnm_terminal') `
    'launch-after-install must have a user-facing label'
Assert-IfwContract `
    ($config.Installer.InstallerApplicationIcon -eq 'vnm_terminal') `
    'the installer must use the product icon'
Assert-IfwContract ($config.Installer.WizardStyle -eq 'Modern') `
    'the installer must use a consistent cross-theme wizard layout'
Assert-IfwContract ($config.Installer.Banner -eq 'varinomics_banner.png') `
    'the Modern header must use the supported IFW banner pixmap hook'
Assert-IfwContract ($null -eq $config.Installer.PageListPixmap) `
    'nonessential geometry must not alter the stable sidebar geometry'
Assert-IfwContract ($config.Installer.StyleSheet -eq 'style.qss') `
    'the installer must load its explicit color palette'
Assert-IfwContract ($config.Installer.ControlScript -eq 'controller.qs') `
    'wizard page visibility must be owned by the pre-display control script'
Assert-IfwContract ($config.Installer.TitleColor -eq '#E0E0E0') `
    'wizard titles and subtitles must remain visible on the dark header'
Assert-IfwContract `
    ($config.Installer.AllowRepositoriesForOfflineInstaller -eq 'false') `
    'the offline installer must reject external repositories'
Assert-IfwContract ($config.Installer.SaveDefaultRepositories -eq 'false') `
    'the maintenance tool must not retain update repositories'

Assert-IfwContract `
    ($styleSheet -match 'QWizard,\s*QWizard QWidget,\s*QWizard QWizardPage\s*\{[^}]*color:\s*#E0E0E0;[^}]*background-color:\s*#111111') `
    'the root, Modern header descendants, and pages must use the website palette'
Assert-IfwContract `
    ($styleSheet -match '#PageListWidget::item:disabled\s*\{[^}]*color:\s*#999999') `
    'unvisited page-list text must remain visible on the dark sidebar'
Assert-IfwContract `
    ($styleSheet -match '#LicenseTextBrowser,[^\{]*\{[^}]*color:\s*#E0E0E0;[^}]*background-color:\s*#1F1F1F') `
    'license text must retain a high-contrast foreground and background'
Assert-IfwContract `
    ($styleSheet -match 'QCheckBox::indicator,\s*QWizard QRadioButton::indicator\s*\{[^}]*background-color:\s*#111111;[^}]*border:\s*2px solid #8AB4C7') `
    'unchecked selection indicators must have an explicit high-contrast border'
Assert-IfwContract `
    ($styleSheet -match 'QCheckBox::indicator:checked\s*\{[^}]*image:\s*url\(:/metadata/installer-theme/checkbox_check\.svg\);[^}]*background-color:\s*#8AB4C7') `
    'checked checkboxes must use the embedded check glyph and explicit fill'
Assert-IfwContract `
    ($styleSheet -match 'QRadioButton::indicator:checked\s*\{[^}]*image:\s*url\(:/metadata/installer-theme/radio_dot\.svg\);[^}]*background-color:\s*#8AB4C7') `
    'checked radio buttons must use the embedded dot glyph and explicit fill'
Assert-IfwContract `
    ($styleSheet -match 'QCheckBox::indicator:unchecked:disabled,[^\{]*\{[^}]*background-color:\s*#1F1F1F;[^}]*border-color:\s*#3A3A3A') `
    'disabled unchecked indicators must remain distinguishable'
Assert-IfwContract `
    ($styleSheet -match 'QPushButton:disabled\s*\{[^}]*color:\s*#999999;[^}]*background-color:\s*#1F1F1F') `
    'disabled wizard buttons must remain legible'
Assert-IfwContract `
    ($styleSheet -match 'QWizard QWizardPage#IntroductionPage\s*\{[^}]*background-image:\s*url\(:/metadata/installer-theme/varinomics_geometry\.png\);[^}]*background-position:\s*right bottom;[^}]*background-repeat:\s*no-repeat' -and
        $styleSheet -match 'QWizard QWizardPage#IntroductionPage QWidget\s*\{[^}]*background-color:\s*transparent') `
    'the stock Introduction page must resolve its nonessential artwork through a Qt stylesheet image without an obscuring child surface'
Assert-IfwContract `
    ($styleSheet -match 'QMessageBox\s*\{[^}]*color:\s*#E0E0E0;[^}]*background-color:\s*#111111' -and
        $styleSheet -match 'QMessageBox QLabel#qt_msgbox_label,[\s\S]*?\{[^}]*color:\s*#E0E0E0;[^}]*background-color:\s*transparent' -and
        $styleSheet -match 'QMessageBox QPushButton\s*\{[^}]*color:\s*#E0E0E0;[^}]*background-color:\s*#1F1F1F;[^}]*border:\s*1px solid #3A3A3A' -and
        $styleSheet -match 'QMessageBox QPushButton:hover\s*\{[^}]*border-color:\s*#8AB4C7' -and
        $styleSheet -match 'QMessageBox QPushButton:focus\s*\{[^}]*border:\s*2px solid #8EC4DF' -and
        $styleSheet -match 'QMessageBox QPushButton:default\s*\{[^}]*color:\s*#111111;[^}]*background-color:\s*#8AB4C7' -and
        $styleSheet -match 'QMessageBox QPushButton:disabled\s*\{[^}]*color:\s*#999999;[^}]*background-color:\s*#1F1F1F') `
    'installer-owned message boxes must use coherent website text, icon-area, and button states'
Assert-IfwContract `
    ($checkboxCheck -match '<path[^>]*stroke="#111111"') `
    'the checkbox glyph must remain visible on its selected fill'
Assert-IfwContract ($radioDot -match '<circle[^>]*fill="#111111"') `
    'the radio glyph must remain visible on its selected fill'
Assert-IfwContract ($comboArrow -match '<path[^>]*stroke="#E0E0E0"') `
    'the combo-box arrow must remain visible on dark controls'
Assert-IfwContract `
    (@($themeResources.RCC.qresource |
        Where-Object { $_.prefix -eq '/installer-theme' }).Count -eq 1) `
    'the indicator resource collection must use the stylesheet resource prefix'
$themeResourceFiles = @($themeResources.RCC.qresource |
    Where-Object { $_.prefix -eq '/installer-theme' } |
    ForEach-Object { @($_.file) })
Assert-IfwContract ($themeResourceFiles.Count -eq 6) `
    'the resource collection must contain every theme glyph, geometry density, and helper'
Assert-IfwContract ($themeResourceFiles -contains 'checkbox_check.svg') `
    'the checkbox glyph must be addressable through the resource collection'
Assert-IfwContract ($themeResourceFiles -contains 'radio_dot.svg') `
    'the radio glyph must be addressable through the resource collection'
Assert-IfwContract ($themeResourceFiles -contains 'combo_arrow.svg') `
    'the combo-box arrow must be addressable through the resource collection'
Assert-IfwContract ($themeResourceFiles -contains 'varinomics_geometry.png') `
    'the Qt-decodable website geometry must be addressable through the stylesheet resource collection'
Assert-IfwContract ($themeResourceFiles -contains 'varinomics_geometry@2x.png') `
    'the high-DPI website geometry must be addressable through the stylesheet resource collection'
Assert-IfwContract `
    ($themeResourceFiles -notcontains 'varinomics_logo.png' -and
        $themeResourceFiles -notcontains 'varinomics_geometry.svg') `
    'the QTextDocument-incompatible brand image paths must not remain in runtime resources'
Assert-IfwContract ($themeResourceFiles -contains 'log_path_probe.ps1') `
    'the writable log-path probe must be embedded with the controller resources'
Assert-IfwContract `
    (@($themeResources.RCC.qresource).Count -eq 2) `
    'the custom resource manifest must contain only the theme collection and proven high-DPI Banner sibling'
$highDpiBannerResource = @($themeResources.RCC.qresource |
    Where-Object { $_.prefix -eq '/installer-config' } |
    ForEach-Object { @($_.file) })
Assert-IfwContract `
    ($highDpiBannerResource.Count -eq 1 -and
        $highDpiBannerResource[0].alias -eq 'varinomics_banner_png@2x.' -and
        $highDpiBannerResource[0].'compression-algorithm' -eq 'none' -and
        $highDpiBannerResource[0].'#text' -eq 'varinomics_banner@2x.png') `
    'the XML QRC must expose the exact high-DPI sibling path proven against IFW 4.11'
Assert-IfwContract `
    ($styleSheet -notmatch ':/installer-theme/' -and
        $controllerScript -notmatch ':/installer-theme/') `
    'custom resources must be consumed below the IFW runtime metadata mount root'

$brandLogoHash =
    (Get-FileHash -LiteralPath $brandLogoPath -Algorithm SHA256).Hash.ToLowerInvariant()
Assert-IfwContract `
    ($brandLogoHash -eq '35d11678fa347e37f29b85bf0bbe4a0f21b89011f1406a61c7bca4098e6232c7') `
    'the wordmark must remain byte-exact with website public/logo-dark.png'
Assert-IfwContract ($brandProvenance.schema -eq 1) `
    'brand-asset provenance must use the supported schema'
Assert-IfwContract `
    ($brandProvenance.sourceRepository -eq 'https://github.com/Varinomics/website' -and
        $brandProvenance.sourceBranch -eq 'master') `
    'brand-asset provenance must identify the canonical website repository'
Assert-IfwContract `
    ($brandProvenance.assets.'varinomics_logo.png'.source -eq 'public/logo-dark.png' -and
        $brandProvenance.assets.'varinomics_logo.png'.sha256 -eq $brandLogoHash -and
        $brandProvenance.assets.'varinomics_logo.png'.transformation -eq 'none') `
    'the wordmark provenance must identify its exact authored source and lack of transformation'
Assert-IfwContract `
    ($brandProvenance.assets.'varinomics_geometry.svg'.geometrySourceSha256 -eq
        '9b7af692bb261943cf6a35987ec2f2fd929ed58b39eaf1fc7e7a147d2531d505' -and
        $brandProvenance.assets.'varinomics_geometry.svg'.paletteSourceSha256 -eq
        '87e5416cb1c2d456478cdff9e40e8da958d4ee9b8cb9b2b287c6376420a56a49' -and
        $brandProvenance.assets.'varinomics_geometry.svg'.shaderSourceSha256 -eq
        'd2d7f12c340c492cd1cf3d14ccfac27049ec17973b1a781e287bd8c154b8dff8') `
    'the derived artwork provenance must pin every authored website input'

$brandBannerHash =
    (Get-FileHash -LiteralPath $brandBannerPath -Algorithm SHA256).Hash.ToLowerInvariant()
$brandBannerHighDpiHash =
    (Get-FileHash -LiteralPath $brandBannerHighDpiPath -Algorithm SHA256).Hash.ToLowerInvariant()
$brandGeometryPngHash =
    (Get-FileHash -LiteralPath $brandGeometryPngPath -Algorithm SHA256).Hash.ToLowerInvariant()
$brandGeometryHighDpiHash =
    (Get-FileHash -LiteralPath $brandGeometryHighDpiPath -Algorithm SHA256).Hash.ToLowerInvariant()
Assert-IfwContract `
    ($brandBannerHash -eq 'ee98f36defaee87112f62b02e1dde6c26b1c3ec5fcf9d7fe50536e8b8bb79674' -and
        $brandBannerHighDpiHash -eq '23e02853788a3b547f3c4428e838829441ce759a6ec2d45e1fa690de82411334' -and
        $brandGeometryPngHash -eq '43543c0d3c20ced0acacc357b78ca9957b667cc5099c91dd5b34b57349759283' -and
        $brandGeometryHighDpiHash -eq '82ea293d949f7e10eeB77e632dd7abc3a4ea566e6705975f88ec8dcd4eb99987'.ToLowerInvariant()) `
    'the mechanically rendered standard and high-DPI brand PNGs must remain deterministic'
Assert-IfwContract `
    ($brandProvenance.assets.'varinomics_banner.png'.sha256 -eq $brandBannerHash -and
        $brandProvenance.assets.'varinomics_banner.png'.highDpiSha256 -eq $brandBannerHighDpiHash -and
        $brandProvenance.assets.'varinomics_geometry.png'.sha256 -eq $brandGeometryPngHash -and
        $brandProvenance.assets.'varinomics_geometry.png'.highDpiSha256 -eq $brandGeometryHighDpiHash -and
        $brandProvenance.assets.'varinomics_banner.png'.rendererQtVersion -eq '6.11.0' -and
        $brandProvenance.assets.'varinomics_geometry.png'.rendererQtVersion -eq '6.11.0') `
    'generated brand provenance must pin the renderer and every output hash'
$bannerDimensions = Get-IfwPngDimensions $brandBannerPath
$bannerHighDpiDimensions = Get-IfwPngDimensions $brandBannerHighDpiPath
$geometryDimensions = Get-IfwPngDimensions $brandGeometryPngPath
$geometryHighDpiDimensions = Get-IfwPngDimensions $brandGeometryHighDpiPath
Assert-IfwContract `
    (($bannerDimensions -join 'x') -eq '998x80' -and
        ($bannerHighDpiDimensions -join 'x') -eq '1996x160' -and
        ($geometryDimensions -join 'x') -eq '300x174' -and
        ($geometryHighDpiDimensions -join 'x') -eq '600x348') `
    'all generated PNGs must fully decode at the intended logical and high-DPI dimensions'
Assert-IfwContract `
    ($brandRenderer -match 'QImageReader\(str\(path\), b"PNG"\)' -and
        $brandRenderer -match 'if not reader\.canRead\(\)' -and
        $brandRenderer -match 'if reader\.read\(\)\.isNull\(\)' -and
        $brandRenderer -match 'x_position = width - BANNER_MARGIN \* scale - logo\.width\(\)' -and
        $brandRenderer -match 'y_position = BANNER_LOGO_TOP \* scale' -and
        $brandRenderer -match 'BANNER_TEXT_SAFE_RIGHT \+ BANNER_TEXT_LOGO_GUTTER > x_position // scale' -and
        $brandRenderer -match 'coordinate \* scale for coordinate in \(682, 11, 982, 53\)' -and
        $brandRenderer -match 'Qt\.AspectRatioMode\.KeepAspectRatio') `
    'the renderer must preserve wordmark geometry and reserve an explicit text-safe region at every density'

if ($ArtifactPath) {
    $resolvedArtifactPath = (Resolve-Path -LiteralPath $ArtifactPath).Path
    $artifactChecksumPath = "$resolvedArtifactPath.sha256"
    Assert-IfwContract (Test-Path -LiteralPath $artifactChecksumPath -PathType Leaf) `
        'the final executable must have a checksum written after final signing'
    $actualArtifactHash =
        (Get-FileHash -LiteralPath $resolvedArtifactPath -Algorithm SHA256).Hash.ToLowerInvariant()
    $checksumParts =
        (Get-Content -LiteralPath $artifactChecksumPath -Raw).Trim() -split '\s+', 2
    Assert-IfwContract `
        ($checksumParts.Count -eq 2 -and
            $checksumParts[0].ToLowerInvariant() -eq $actualArtifactHash -and
            $checksumParts[1] -eq (Split-Path -Leaf $resolvedArtifactPath)) `
        'the final signed artifact must match its adjacent checksum and exact filename'
}

if ($DumpPath) {
    Assert-IfwContract (-not [string]::IsNullOrWhiteSpace($ArtifactPath)) `
        'artifact-level resource checks require the dumped artifact path'
    $resolvedDumpPath = (Resolve-Path -LiteralPath $DumpPath).Path
    $dumpedConfigPath = Join-Path $resolvedDumpPath 'metadata\installer-config\config.xml'
    $dumpedBannerPath =
        Join-Path $resolvedDumpPath 'metadata\installer-config\varinomics_banner_png'
    $dumpedBannerHighDpiPath =
        Join-Path $resolvedDumpPath 'metadata\installer-config\varinomics_banner_png@2x'
    Assert-IfwContract `
        ((Test-Path -LiteralPath $dumpedConfigPath -PathType Leaf) -and
            (Test-Path -LiteralPath $dumpedBannerPath -PathType Leaf) -and
            (Test-Path -LiteralPath $dumpedBannerHighDpiPath -PathType Leaf)) `
        'devtool dump must materialize the IFW config and both exact Banner density paths'

    [xml]$dumpedConfig = Get-Content -LiteralPath $dumpedConfigPath -Raw
    $dumpedBannerHash =
        (Get-FileHash -LiteralPath $dumpedBannerPath -Algorithm SHA256).Hash.ToLowerInvariant()
    $dumpedBannerHighDpiHash =
        (Get-FileHash -LiteralPath $dumpedBannerHighDpiPath -Algorithm SHA256).Hash.ToLowerInvariant()
    Assert-IfwContract `
        ($dumpedConfig.Installer.Banner -eq 'varinomics_banner_png' -and
            $dumpedBannerHash -eq $brandBannerHash -and
            $dumpedBannerHighDpiHash -eq $brandBannerHighDpiHash) `
        'the dumped Banner setting and both embedded resources must match their exact source bodies'

    $dumpedBannerDimensions = Get-IfwPngDimensions $dumpedBannerPath
    $dumpedBannerHighDpiDimensions = Get-IfwPngDimensions $dumpedBannerHighDpiPath
    Assert-IfwContract `
        (($dumpedBannerDimensions -join 'x') -eq '998x80' -and
            ($dumpedBannerHighDpiDimensions -join 'x') -eq '1996x160') `
        'both extensionless dumped Banner resources must decode at their intended density dimensions'
}

Assert-IfwContract ($brandGeometryXml.svg.viewBox -eq '0 0 760 440') `
    'the geometry must retain its deliberate installer crop'
$geometryLines = @($brandGeometryXml.svg.g | ForEach-Object { @($_.line) }) |
    Where-Object { $null -ne $_ }
$geometryCircles = @($brandGeometryXml.svg.g | ForEach-Object { @($_.circle) }) |
    Where-Object { $null -ne $_ }
Assert-IfwContract ($geometryLines.Count -eq 30) `
    'the static geometry must retain all 30 authored icosahedron edges'
Assert-IfwContract ($geometryCircles.Count -eq 12) `
    'the static geometry must retain all 12 authored icosahedron vertices'

$expectedGeometryEdges = @(
    '319.5,399.76,521.66,411.9',
    '319.5,399.76,543.23,407.87',
    '319.5,399.76,335.68,261.83',
    '319.5,399.76,242.31,165.05',
    '319.5,399.76,370.58,255.3',
    '521.66,411.9,543.23,407.87',
    '521.66,411.9,335.68,261.83',
    '521.66,411.9,569.42,184.7',
    '521.66,411.9,697.69,274.95',
    '418.34,28.1,620.5,40.24',
    '418.34,28.1,604.32,178.17',
    '418.34,28.1,396.77,32.13',
    '418.34,28.1,242.31,165.05',
    '418.34,28.1,370.58,255.3',
    '620.5,40.24,604.32,178.17',
    '620.5,40.24,396.77,32.13',
    '620.5,40.24,569.42,184.7',
    '620.5,40.24,697.69,274.95',
    '604.32,178.17,543.23,407.87',
    '604.32,178.17,697.69,274.95',
    '604.32,178.17,370.58,255.3',
    '543.23,407.87,697.69,274.95',
    '543.23,407.87,370.58,255.3',
    '396.77,32.13,335.68,261.83',
    '396.77,32.13,569.42,184.7',
    '396.77,32.13,242.31,165.05',
    '335.68,261.83,569.42,184.7',
    '335.68,261.83,242.31,165.05',
    '569.42,184.7,697.69,274.95',
    '242.31,165.05,370.58,255.3'
)
$actualGeometryEdges = @($geometryLines | ForEach-Object {
    '{0},{1},{2},{3}' -f $_.x1, $_.y1, $_.x2, $_.y2
})
Assert-IfwContract `
    (($actualGeometryEdges -join '|') -eq ($expectedGeometryEdges -join '|')) `
    'the fixed projection must preserve the authored website edge order and coordinates'
Assert-IfwContract `
    ($brandGeometry -match 'stop-color="#2e1a0f"' -and
        $brandGeometry -match 'stop-color="#0f1424"' -and
        $brandGeometry -match 'fill="#111111"' -and
        $brandGeometry -match 'fill="#c44d28"' -and
        $brandGeometry -match 'fill="#a7acb4"') `
    'the artwork must use the website base, logo marks, and shader-derived warm/cool palette'
Assert-IfwContract `
    (($styleSheet + $controllerScript + $brandGeometry) -notmatch
        '#(?:1F6FEB|1858B6|12458F|75B7FF|79C0FF|B7D7FF)') `
    'the website-derived theme must not retain the superseded GitHub-blue palette'

Assert-IfwContrast '#E0E0E0' '#111111' 7.0 `
    'wizard text must meet enhanced contrast against the main surface'
Assert-IfwContrast '#E0E0E0' '#111111' 7.0 `
    'wizard titles must meet enhanced contrast against the Modern header'
Assert-IfwContrast '#999999' '#111111' 4.5 `
    'disabled navigation text must meet normal-text contrast'
Assert-IfwContrast '#8AB4C7' '#111111' 3.0 `
    'unchecked indicator borders must meet non-text control contrast'
Assert-IfwContrast '#111111' '#8AB4C7' 4.5 `
    'primary button and selected-item text must meet normal-text contrast'

Assert-IfwContract `
    ($package.Package.Name -eq 'com.varinomics.vnm_terminal') `
    'the package identifier must remain stable'
Assert-IfwContract ($package.Package.ForcedInstallation -eq 'true') `
    'the application package must not be deselectable'
Assert-IfwContract ($package.Package.RequiresAdminRights -eq 'true') `
    'installation under Program Files must require elevation'
Assert-IfwContract ($package.Package.Script -eq 'installscript.qs') `
    'the package must load its integration script'
Assert-IfwContract `
    ($package.Package.Licenses.License.file -eq 'LICENSE.txt') `
    'the package must present the project license'

Assert-IfwContract `
    ($installScript -match 'addElevatedOperation\s*\(\s*"CreateShortcut"') `
    'the Start Menu shortcut must be created as an elevated operation'
Assert-IfwContract `
    ($installScript -match '@AllUsersStartMenuProgramsPath@') `
    'the shortcut must be installed for all users'
Assert-IfwContract `
    ($installScript -match '@TargetDir@/vnm_terminal\.exe') `
    'the shortcut must target the portable launcher'
Assert-IfwContract `
    ($installScript -notmatch 'setDefaultPageVisible|ComponentSelection|ReadyForInstallationPage|hideColumn') `
    'component scripts must not mutate wizard pages after the first frame is visible'
Assert-IfwContract `
    ($controllerScript -match 'function\s+Controller\s*\(\s*\)\s*\{[\s\S]*?if\s*\(installer\.isInstaller\(\)\)[\s\S]*?setDefaultPageVisible\s*\(\s*QInstaller\.ComponentSelection\s*,\s*false\s*\)') `
    'the pre-display Controller constructor must skip the single forced component page during initial installation'
Assert-IfwContract `
    ($controllerScript -notmatch 'isUpdater\(\)[\s\S]*?setDefaultPageVisible\s*\(\s*QInstaller\.ComponentSelection' -and
        $controllerScript -notmatch 'isPackageManager\(\)[\s\S]*?setDefaultPageVisible\s*\(\s*QInstaller\.ComponentSelection' -and
        $controllerScript -notmatch 'isUninstaller\(\)[\s\S]*?setDefaultPageVisible\s*\(\s*QInstaller\.ComponentSelection') `
    'maintenance, updater, package-manager, and uninstaller page behavior must remain unchanged'
Assert-IfwContract `
    ($controllerScript -notmatch 'addWizardPage|addWizardPageItem|removeWizardPage' -and
        $controllerScript -notmatch 'IntroductionPageCallback[\s\S]*?setDefaultPageVisible' -and
        $controllerScript -notmatch 'FinishedPageCallback[\s\S]*?setDefaultPageVisible') `
    'branding must not add or mutate pages after the stable first-frame page list is built'
Assert-IfwContract `
    ($controllerScript -match 'Controller\.prototype\.IntroductionPageCallback\s*=\s*function\s*\(\s*\)\s*\{\s*if\s*\(\s*!installer\.isInstaller\(\)\s*\)\s*return\s*;\s*var\s+introductionPage\s*=\s*gui\.pageWidgetByObjectName\s*\(\s*"IntroductionPage"\s*\)' -and
        $controllerScript -match 'introductionPage\.MessageLabel\.setText' -and
        $controllerScript -notmatch '<img\s') `
    'initial-install branding must guard before touching the existing introduction page'
$installerPageSubtitles = @(
    @('IntroductionPageCallback', 'IntroductionPage', 'introductionPage',
        'Install vnm_terminal on this computer.'),
    @('TargetDirectoryPageCallback', 'TargetDirectoryPage', 'targetDirectoryPage',
        'Choose where vnm_terminal will be installed.'),
    @('LicenseAgreementPageCallback', 'LicenseAgreementPage', 'licenseAgreementPage',
        'Review and accept the license to continue.'),
    @('StartMenuDirectoryPageCallback', 'StartMenuDirectoryPage', 'startMenuDirectoryPage',
        'Choose where Start Menu shortcuts will appear.'),
    @('ReadyForInstallationPageCallback', 'ReadyForInstallationPage', 'summaryPage',
        'Review your choices before installation.'),
    @('PerformInstallationPageCallback', 'PerformInstallationPage', 'performInstallationPage',
        'Installing vnm_terminal. Please wait.')
)
foreach ($subtitleContract in $installerPageSubtitles) {
    $callbackName, $objectName, $variableName, $subtitle = $subtitleContract
    $callbackPattern =
        'Controller\.prototype\.' + [regex]::Escape($callbackName) +
        '\s*=\s*function\s*\(\s*\)\s*\{\s*' +
        'if\s*\(\s*!installer\.isInstaller\(\)\s*\)\s*return\s*;\s*' +
        'var\s+' + [regex]::Escape($variableName) +
        '\s*=\s*gui\.pageWidgetByObjectName\s*\(\s*"' +
        [regex]::Escape($objectName) + '"\s*\);[\s\S]*?' +
        [regex]::Escape($variableName) + '\.subTitle\s*=\s*"' +
        [regex]::Escape($subtitle) + '";'
    Assert-IfwContract ($controllerScript -match $callbackPattern) `
        "$objectName must receive its concise subtitle after an immediate installer-only guard"
}
Assert-IfwContract `
    ($controllerScript -notmatch 'Please read the following license agreement\. You must accept the terms') `
    'the controller must not preserve the overflowing framework License subtitle'
$userVisibleInstallerSources = @(
    $config.Installer.Name,
    $config.Installer.Title,
    $config.Installer.RunProgramDescription,
    $config.Installer.StartMenuDir,
    $package.Package.DisplayName,
    $maintenancePackage.Package.DisplayName,
    $controllerScript
)
foreach ($visibleSource in $userVisibleInstallerSources) {
    $productNameMatches = [regex]::Matches(
        [string]$visibleSource,
        'vnm_terminal',
        [Text.RegularExpressions.RegexOptions]::IgnoreCase)
    foreach ($productNameMatch in $productNameMatches) {
        Assert-IfwContract ($productNameMatch.Value -ceq 'vnm_terminal') `
            "user-visible installer product names must use exact lowercase vnm_terminal"
    }
}
Assert-IfwContract `
    ($controllerScript -notmatch 'varinomics_logo\.png|varinomics_geometry\.(?:svg|png)' -and
        $config.Installer.Banner -eq 'varinomics_banner.png') `
    'the canonical wordmark must appear only in the Modern banner and no image may rely on QTextDocument resource lookup'
Assert-IfwContract `
    ($controllerScript -match 'Controller\.prototype\.FinishedPageCallback\s*=\s*function\s*\(\s*\)\s*\{\s*if\s*\(\s*!installer\.isInstaller\(\)\s*\)\s*return\s*;\s*var\s+finishedPage\s*=\s*gui\.pageWidgetByObjectName\s*\(\s*"FinishedPage"\s*\)') `
    'initial-install branding must guard before reading or changing the finished page'
Assert-IfwContract `
    ($controllerScript -match 'if\s*\(\s*installer\.status\s*==\s*QInstaller\.Success\s*\)\s*\{[\s\S]*?subTitle\s*=\s*"Installation completed successfully\.";[\s\S]*?heading\s*=\s*"vnm_terminal is ready\.";[\s\S]*?\}\s*else\s*if\s*\(\s*installer\.status\s*==\s*QInstaller\.Canceled\s*\)\s*\{[\s\S]*?subTitle\s*=\s*"Setup stopped at your request\.";[\s\S]*?heading\s*=\s*"Installation was canceled\.";[\s\S]*?escapeHtml\s*\(\s*frameworkMessage\s*\)[\s\S]*?\}\s*else\s*if\s*\(\s*installer\.status\s*==\s*QInstaller\.Unfinished\s*\)\s*\{[\s\S]*?subTitle\s*=\s*"Setup ended before installation completed\.";[\s\S]*?heading\s*=\s*"Installation did not complete\.";[\s\S]*?Setup ended before installation could be completed\.[\s\S]*?escapeHtml\s*\(\s*frameworkMessage\s*\)[\s\S]*?\}\s*else\s*\{[\s\S]*?subTitle\s*=\s*"Setup could not complete the installation\.";[\s\S]*?heading\s*=\s*"Installation failed\.";[\s\S]*?escapeHtml\s*\(\s*frameworkMessage\s*\)') `
    'finished control flow must keep distinct success, canceled, unfinished, and failure copy while safely preserving every non-success diagnostic'
Assert-IfwContract `
    (($controllerScript | Select-String -AllMatches -Pattern 'vnm_terminal is ready\.' ).Matches.Count -eq 1 -and
        ($controllerScript | Select-String -AllMatches -Pattern 'Installation was canceled\.' ).Matches.Count -eq 1 -and
        ($controllerScript | Select-String -AllMatches -Pattern 'Installation did not complete\.' ).Matches.Count -eq 1) `
    'success, user cancellation, and unfinished work must each have unique truthful headings'
Assert-IfwContract `
    ($controllerScript -match 'installer\.status\s*!=\s*QInstaller\.Success[\s\S]*?RunItCheckBox\.hide\s*\(\s*\)') `
    'launch-after-install must be unavailable after cancellation, unfinished work, or failure'
Assert-IfwContract `
    ($controllerScript -match 'installer\.setValue\s*\(\s*"LogFileName"\s*,\s*"\\\\\\\\\.\\\\NUL"\s*\)') `
    'logging must start from an absolute device fallback that cannot resolve below TargetDir'
Assert-IfwContract `
    ($controllerScript -match 'readFile\s*\(\s*":/metadata/installer-theme/log_path_probe\.ps1"[\s\S]*?installer\.execute\s*\(') `
    'the controller must execute the exact embedded writable-path probe'
Assert-IfwContract `
    ($controllerScript -match 'result\.length\s*!=\s*2\s*\|\|\s*result\[1\]\s*!=\s*0[\s\S]*?\^\(\?:\[A-Za-z\][\s\S]*?installer\.setValue\s*\(\s*"LogFileName"') `
    'only a successful probe returning an absolute path may replace the safe fallback'
Assert-IfwContract `
    ($controllerScript -notmatch 'LogFileName[\s\S]{0,160}(?:TargetDir|ApplicationsDir)' -and
        $controllerScript -notmatch 'installer\.setCanceled\s*\(' -and
        $controllerScript -notmatch 'installer\.(?:gainAdminRights|runProgram)\s*\(' -and
        $controllerScript -notmatch 'finishButtonClicked\.connect') `
    'failure logging must preserve status and avoid privileged target writes, elevation, or launch actions'
Assert-IfwContract `
    ($logPathProbe -match '\[IO\.Path\]::IsPathRooted\(\$candidate\)' -and
        $logPathProbe -match '\[IO\.Directory\]::Exists\(\$candidate\)' -and
        $logPathProbe -match '''InstallationLog-\{0\}\.txt''' -and
        $logPathProbe -match '\[IO\.FileMode\]::CreateNew' -and
        $logPathProbe -match '\$logStream\.Flush\(\$true\)') `
    'the helper must exclusively create and flush the exact unique file it returns'
Assert-IfwContract `
    ($controllerScript -match 'Controller\.prototype\.ReadyForInstallationPageCallback\s*=\s*function\s*\(\s*\)[\s\S]*?if\s*\(!installer\.isInstaller\(\)\)\s*return') `
    'summary customization must run in the supported post-entry callback and remain initial-install-only'
Assert-IfwContract `
    ($controllerScript -match 'gui\.findChild\s*\(\s*summaryPage\s*,\s*"InstallComponentsTreeview"\s*\)[\s\S]*?installComponentsTreeview\.hideColumn\s*\(\s*5\s*\)' -and
        $controllerScript -notmatch 'summaryPage\.InstallComponentsTreeview') `
    'the ambiguous component subtotal must be hidden through IFW recursive child lookup'
Assert-IfwContract `
    ($controllerScript -notmatch 'pageWidgetByObjectName\s*\(\s*"(?:SpaceItem|SpaceWidget)"' -and
        $controllerScript -notmatch '\.(?:SpaceItem|SpaceWidget)\.') `
    'the labeled total required-space widget must remain visible'

Assert-IfwContract `
    ($maintenancePackage.Package.Name -eq 'com.varinomics.vnm_terminal.maintenance') `
    'the signed maintenance-tool component must have a stable identifier'
Assert-IfwContract ($maintenancePackage.Package.ForcedInstallation -eq 'true') `
    'the signed maintenance-tool component must always be installed'
Assert-IfwContract ($maintenancePackage.Package.Essential -eq 'true') `
    'the signed maintenance-tool component must be essential'
Assert-IfwContract ($maintenancePackage.Package.Virtual -eq 'true') `
    'the signed maintenance-tool component must stay hidden'
Assert-IfwContract (-not $maintenancePackage.Package.Default) `
    'virtual maintenance components must not declare the mutually exclusive Default element'
Assert-IfwContract `
    ($maintenanceInstallScript -match 'installer\.setInstallerBaseBinary') `
    'the maintenance component must select its signed installerbase'
Assert-IfwContract `
    ($maintenanceInstallScript -match '@TargetDir@/installerbase\.exe') `
    'the maintenance component must select its packaged installerbase'

Assert-IfwContract ($buildScript -match '\$ifwVersion\s*=\s*''4\.11\.0''') `
    'the IFW tool version must be pinned to 4.11.0'
Assert-IfwContract `
    ($buildScript -match 'c47201c4f6a82a8b607daa245237f40831d78425e904edd1514b71fd17efefc1') `
    'the official IFW archive checksum must remain pinned'
Assert-IfwContract ($buildScript -match '--offline-only') `
    'binarycreator must force offline-only behavior'
Assert-IfwContract `
    ($buildScript -match 'artifactSuffix\s*=\s*if\s*\(\$signingEnabled\)') `
    'unsigned artifacts must be distinguished in their filename'

$payloadSigningIndex = $buildScript.IndexOf(
    "Invoke-TrustedSigning (Join-Path `$packageDataRoot 'vnm_terminal.exe')")
$installerBaseSigningIndex = $buildScript.IndexOf(
    'Invoke-TrustedSigning $privateInstallerBasePath')
$configRenderIndex = $buildScript.IndexOf(
    "-Destination (Join-Path `$configRoot 'config.xml')")
$bannerCopyIndex = $buildScript.IndexOf(
    "ifwSourceRoot 'varinomics_banner.png'")
$bannerHighDpiCopyIndex = $buildScript.IndexOf(
    "ifwSourceRoot 'varinomics_banner@2x.png'")
$binaryCreatorIndex = $buildScript.IndexOf('& $binaryCreatorPath --offline-only')
$finalSigningIndex = $buildScript.LastIndexOf('Invoke-TrustedSigning $artifactPath')
Assert-IfwContract ($payloadSigningIndex -ge 0) `
    'the Varinomics launcher must be signed when release signing is enabled'
Assert-IfwContract ($installerBaseSigningIndex -gt $payloadSigningIndex) `
    'the private installerbase must be signed after the payload'
Assert-IfwContract ($binaryCreatorIndex -gt $installerBaseSigningIndex) `
    'binarycreator must run after the packaged maintenance-tool base is signed'
Assert-IfwContract `
    ($configRenderIndex -ge 0 -and
        $bannerCopyIndex -gt $configRenderIndex -and
        $bannerHighDpiCopyIndex -gt $bannerCopyIndex -and
        $binaryCreatorIndex -gt $bannerHighDpiCopyIndex) `
    'IFW must see the standard and @2x Banner siblings beside the rendered config before resource collection'
Assert-IfwContract ($finalSigningIndex -gt $binaryCreatorIndex) `
    'the final installer must be signed after binarycreator finishes'
Assert-IfwContract `
    ($buildScript -match '--template \$installerBaseSourcePath') `
    'binarycreator must use the unsigned IFW template so the final PE remains signable'
Assert-IfwContract `
    ($buildScript -notmatch '--template \$privateInstallerBasePath') `
    'the signed maintenance-tool component must not be used as the append-only installer template'
Assert-IfwContract `
    ($buildScript -match "repositoryRoot 'THIRD_PARTY_NOTICES\.md'") `
    'the current durable third-party notice must replace the release-payload copy'
Assert-IfwContract `
    ($buildScript -match 'ifwSourceRoot ''style\.qss''[^\r\n]*\r?\n\s*-Destination \$configRoot') `
    'the configured stylesheet must be embedded in the installer'
Assert-IfwContract `
    ($buildScript -match 'ifwSourceRoot ''controller\.qs''[^\r\n]*\r?\n\s*-Destination \$configRoot') `
    'the pre-display controller must be embedded beside the installer config'
Assert-IfwContract `
    ($buildScript -match 'ifwSourceRoot ''log_path_probe\.ps1''[^\r\n]*\r?\n\s*-Destination \$configRoot') `
    'the writable log-path probe must be staged beside its resource collection'
Assert-IfwContract `
    ($buildScript -match '--resources \(Join-Path \$configRoot ''theme_resources\.qrc''\)' -and
        $buildScript -notmatch '--resources \(Join-Path \$configRoot ''theme_resources\.rcc''\)') `
    'binarycreator must receive the XML resource manifest it compiles internally'
Assert-IfwContract `
    ($buildScript -match 'ifwSourceRoot ''varinomics_banner\.png''[^\r\n]*\r?\n\s*-Destination \$configRoot' -and
        $buildScript -match 'ifwSourceRoot ''varinomics_banner@2x\.png''[^\r\n]*\r?\n\s*-Destination \$configRoot' -and
        $buildScript -match 'ifwSourceRoot ''varinomics_geometry\.png''[^\r\n]*\r?\n\s*-Destination \$configRoot' -and
        $buildScript -match 'ifwSourceRoot ''varinomics_geometry@2x\.png''[^\r\n]*\r?\n\s*-Destination \$configRoot') `
    'the supported Modern banner and Qt stylesheet geometry at both densities must be staged beside the config'

$probeTestRoot = Join-Path ([IO.Path]::GetTempPath()) (
    'vnm-ifw-log-probe-test-' + [Guid]::NewGuid().ToString('N'))
$validCandidate = Join-Path $probeTestRoot 'valid'
$nonWritableCandidate = Join-Path $probeTestRoot 'non-writable'
$missingCandidate = Join-Path $probeTestRoot 'missing'
$lockedFixedLog = $null
$originalAcl = $null
try {
    [void](New-Item -ItemType Directory -Path $validCandidate, $nonWritableCandidate)

    function Invoke-LogPathProbe {
        param([string]$Candidate)

        $output = & powershell.exe -NoLogo -NoProfile -NonInteractive `
            -ExecutionPolicy Bypass -File $logPathProbePath `
            -CandidatePath $Candidate
        return [pscustomobject]@{
            ExitCode = $LASTEXITCODE
            Output = ($output | Out-String).Trim()
        }
    }

    $relativeResult = Invoke-LogPathProbe '.'
    Assert-IfwContract `
        ($relativeResult.ExitCode -ne 0 -and $relativeResult.Output -eq '') `
        'the helper must reject a hostile relative LOCALAPPDATA candidate'

    $missingResult = Invoke-LogPathProbe $missingCandidate
    Assert-IfwContract `
        ($missingResult.ExitCode -ne 0 -and $missingResult.Output -eq '' -and
            -not (Test-Path -LiteralPath $missingCandidate)) `
        'the helper must reject rather than create a nonexistent candidate root'

    $originalAcl = Get-Acl -LiteralPath $nonWritableCandidate
    $denyRule = [Security.AccessControl.FileSystemAccessRule]::new(
        [Security.Principal.WindowsIdentity]::GetCurrent().User,
        [Security.AccessControl.FileSystemRights]::CreateDirectories -bor
            [Security.AccessControl.FileSystemRights]::CreateFiles -bor
            [Security.AccessControl.FileSystemRights]::WriteData -bor
            [Security.AccessControl.FileSystemRights]::AppendData,
        [Security.AccessControl.InheritanceFlags]::ContainerInherit -bor
            [Security.AccessControl.InheritanceFlags]::ObjectInherit,
        [Security.AccessControl.PropagationFlags]::None,
        [Security.AccessControl.AccessControlType]::Deny)
    $deniedAcl = Get-Acl -LiteralPath $nonWritableCandidate
    [void]$deniedAcl.AddAccessRule($denyRule)
    Set-Acl -LiteralPath $nonWritableCandidate -AclObject $deniedAcl

    $nonWritableResult = Invoke-LogPathProbe $nonWritableCandidate
    Assert-IfwContract `
        ($nonWritableResult.ExitCode -ne 0 -and
            $nonWritableResult.Output -eq '') `
        'the helper must reject an existing candidate that fails its write probe'

    Set-Acl -LiteralPath $nonWritableCandidate -AclObject $originalAcl
    $originalAcl = $null

    $validLogDirectory = Join-Path $validCandidate 'Varinomics\vnm_terminal'
    [void](New-Item -ItemType Directory -Path $validLogDirectory)
    $fixedLogPath = Join-Path $validLogDirectory 'InstallationLog.txt'
    [IO.File]::WriteAllText($fixedLogPath, 'existing log')
    [IO.File]::SetAttributes($fixedLogPath, [IO.FileAttributes]::ReadOnly)
    $lockedFixedLog = [IO.File]::Open(
        $fixedLogPath,
        [IO.FileMode]::Open,
        [IO.FileAccess]::Read,
        [IO.FileShare]::None)

    $validResult = Invoke-LogPathProbe $validCandidate
    Assert-IfwContract `
        ($validResult.ExitCode -eq 0 -and
            [IO.Path]::IsPathRooted($validResult.Output) -and
            $validResult.Output -ne $fixedLogPath -and
            (Split-Path $validResult.Output) -eq $validLogDirectory -and
            (Split-Path $validResult.Output -Leaf) -match
                '^InstallationLog-[0-9a-f]{32}\.txt$') `
        'the helper must bypass a locked read-only fixed name with a unique absolute filename'
    Assert-IfwContract `
        (Test-Path -LiteralPath $validResult.Output -PathType Leaf) `
        'the exact returned log file must remain in place for IFW shutdown'

    $appendStream = [IO.File]::Open(
        $validResult.Output,
        [IO.FileMode]::Append,
        [IO.FileAccess]::Write,
        [IO.FileShare]::Read)
    $appendStream.WriteByte(0x56)
    $appendStream.Dispose()
    Assert-IfwContract `
        ((Get-Item -LiteralPath $validResult.Output).Length -eq 1) `
        'the exact returned file must be reopenable for IFW-compatible append'

    $lockedFixedLog.Dispose()
    $lockedFixedLog = $null
    [IO.File]::SetAttributes($fixedLogPath, [IO.FileAttributes]::Normal)
}
finally {
    if ($null -ne $lockedFixedLog) {
        $lockedFixedLog.Dispose()
    }
    if ($null -ne $originalAcl -and
        (Test-Path -LiteralPath $nonWritableCandidate)) {
        Set-Acl -LiteralPath $nonWritableCandidate -AclObject $originalAcl
    }
    if (Test-Path -LiteralPath $probeTestRoot) {
        Remove-Item -LiteralPath $probeTestRoot -Recurse -Force
    }
}
Assert-IfwContract `
    ($buildScript -match 'ifwSourceRoot ''checkbox_check\.svg''[^\r\n]*\r?\n\s*-Destination \$configRoot') `
    'the checkbox glyph must be embedded in the installer'
Assert-IfwContract `
    ($buildScript -match 'ifwSourceRoot ''radio_dot\.svg''[^\r\n]*\r?\n\s*-Destination \$configRoot') `
    'the radio glyph must be embedded in the installer'
Assert-IfwContract `
    ($buildScript -match 'ifwSourceRoot ''combo_arrow\.svg''[^\r\n]*\r?\n\s*-Destination \$configRoot') `
    'the combo-box arrow must be embedded in the installer'
Assert-IfwContract `
    ($buildScript -match 'ifwSourceRoot ''theme_resources\.qrc''[^\r\n]*\r?\n\s*-Destination \$configRoot') `
    'the source theme resource manifest must be staged for auditability'
Assert-IfwContract `
    ($buildScript -match '--resources \(Join-Path \$configRoot ''theme_resources\.qrc''\)') `
    'binarycreator must compile and embed the theme resource manifest'

Assert-IfwContract ($notices -match 'libarchive 3\.8\.5') `
    'the bundled libarchive version must be identified'
Assert-IfwContract `
    ($notices -match 'Copyright \(c\) 2003-2018 Tim Kientzle') `
    'the libarchive copyright notice must be retained'
Assert-IfwContract `
    ($notices -match 'Redistributions? in binary form must reproduce') `
    'the libarchive binary redistribution condition must be retained'
Assert-IfwContract `
    ($notices -match 'THIS SOFTWARE IS PROVIDED BY THE AUTHOR\(S\) ``AS IS''') `
    'the libarchive warranty disclaimer must be retained'

Write-Host "Qt IFW packaging contract passed: $resolvedSourceRoot"
