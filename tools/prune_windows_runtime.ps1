[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$RuntimePath
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$runtimeRoot = (Resolve-Path -LiteralPath $RuntimePath).Path
foreach ($requiredFile in @('vnm_terminal.exe', 'Qt6Core.dll', 'qml\QtQuick\Controls\Basic\qmldir')) {
    if (-not (Test-Path -LiteralPath (Join-Path $runtimeRoot $requiredFile) -PathType Leaf)) {
        throw "Incomplete application runtime: $requiredFile"
    }
}

# Refuse redirected content before removing anything from a deployment tree.
$entries = @((Get-Item -LiteralPath $runtimeRoot)) + @(Get-ChildItem -LiteralPath $runtimeRoot -Recurse -Force)
foreach ($entry in $entries) {
    if ($entry.Attributes -band [IO.FileAttributes]::ReparsePoint) {
        throw "Runtime pruning does not follow reparse points: $($entry.FullName)"
    }
}

$removals = [Collections.Generic.List[string]]::new()
foreach ($entry in $entries) {
    if (-not $entry.PSIsContainer -and $entry.Extension -eq '.qmltypes') {
        $removals.Add($entry.FullName)
    }
}

# Production selects Basic before loading QML. The import scanner also follows
# Controls' optional styles; those styles and Qt Creator's files are not used.
$relativePaths = @(
    'qmltooling',
    'qml\QtQuick\tooling',
    'qml\QtQuick\designer',
    'qml\QtQuick\NativeStyle',
    'qml\QtQuick\Controls\Native',
    'qml\QtQuick\Controls\Fusion',
    'qml\QtQuick\Controls\Windows',
    'qml\QtQuick\Controls\FluentWinUI3',
    'qml\QtQuick\Controls\Imagine',
    'qml\QtQuick\Controls\Material',
    'qml\QtQuick\Controls\Universal',
    'Qt6QuickControls2Fusion.dll',
    'Qt6QuickControls2FusionStyleImpl.dll',
    'Qt6QuickControls2Windows.dll',
    'Qt6QuickControls2WindowsStyleImpl.dll',
    'Qt6QuickControls2FluentWinUI3.dll',
    'Qt6QuickControls2FluentWinUI3StyleImpl.dll',
    'Qt6QuickControls2Imagine.dll',
    'Qt6QuickControls2ImagineStyleImpl.dll',
    'Qt6QuickControls2Material.dll',
    'Qt6QuickControls2MaterialStyleImpl.dll',
    'Qt6QuickControls2Universal.dll',
    'Qt6QuickControls2UniversalStyleImpl.dll',
    'Qt6QuickControls2Native.dll',
    'Qt6QuickNativeStyle.dll',
    # Match the Windows distribution's system D3D11 runtime. Local Qt kits
    # otherwise add optional software OpenGL and D3D12 compiler deployments.
    'opengl32sw.dll',
    'D3Dcompiler_47.dll',
    'dxcompiler.dll',
    'dxil.dll'
)
foreach ($relativePath in $relativePaths) {
    $path = [IO.Path]::GetFullPath((Join-Path $runtimeRoot $relativePath))
    if (-not $path.StartsWith($runtimeRoot.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw "Pruning target is outside the runtime: $path"
    }
    if (Test-Path -LiteralPath $path) {
        $removals.Add($path)
    }
}
foreach ($path in $removals) {
    if (Test-Path -LiteralPath $path) {
        Remove-Item -LiteralPath $path -Recurse -Force
    }
}
