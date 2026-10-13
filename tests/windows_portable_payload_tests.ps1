[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$PayloadPath,

    [Parameter(Mandatory = $true)]
    [string]$EmbeddedLauncherPath
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Assert-SymbolFreeExecutable {
    param([string]$Path)

    $bytes = [IO.File]::ReadAllBytes($Path)
    if ($bytes.Length -lt 64 -or [Text.Encoding]::ASCII.GetString($bytes, 0, 2) -ne 'MZ') {
        throw "Not a Windows executable: $Path"
    }
    $peOffset = [BitConverter]::ToInt32($bytes, 60)
    if ([BitConverter]::ToUInt32($bytes, $peOffset) -ne 0x00004550) {
        throw "Invalid PE signature: $Path"
    }
    $symbolOffset = [BitConverter]::ToUInt32($bytes, $peOffset + 12)
    $symbolCount  = [BitConverter]::ToUInt32($bytes, $peOffset + 16)
    if ($symbolOffset -ne 0 -or $symbolCount -ne 0) {
        throw "Release executable retains a COFF symbol/string table: $Path"
    }

    $optionalHeader = $peOffset + 24
    $magic = [BitConverter]::ToUInt16($bytes, $optionalHeader)
    if ($magic -eq 0x20b) {
        $directories = $optionalHeader + 112
    } elseif ($magic -eq 0x10b) {
        $directories = $optionalHeader + 96
    } else {
        throw "Unknown PE optional header: $Path"
    }
    $debugSize = [BitConverter]::ToUInt32($bytes, $directories + 6 * 8 + 4)
    if ($debugSize -ne 0) {
        throw "Release executable retains a PE debug directory: $Path"
    }

    $sectionCount = [BitConverter]::ToUInt16($bytes, $peOffset + 6)
    $headerSize   = [BitConverter]::ToUInt16($bytes, $peOffset + 20)
    for ($index = 0; $index -lt $sectionCount; ++$index) {
        $offset = $optionalHeader + $headerSize + $index * 40
        $name = [Text.Encoding]::ASCII.GetString($bytes, $offset, 8).TrimEnd([char]0)
        if ($name -match '^\.(debug|zdebug|stab)' -or $name.StartsWith('/')) {
            throw "Release executable retains a debugging section $name`: $Path"
        }
    }
}

$payloadRoot = (Resolve-Path -LiteralPath $PayloadPath).Path
$runtimeRoot = Join-Path $payloadRoot 'vnm_terminal_runtime'
Assert-SymbolFreeExecutable (Join-Path $payloadRoot 'vnm_terminal.exe')
Assert-SymbolFreeExecutable (Join-Path $runtimeRoot 'vnm_terminal.exe')
# This helper is embedded by rcc. Check its exact build input before packaging.
Assert-SymbolFreeExecutable (Resolve-Path -LiteralPath $EmbeddedLauncherPath).Path

foreach ($file in (Get-ChildItem -LiteralPath $payloadRoot -Recurse -File)) {
    if ($file.Extension -in @('.pdb', '.qmltypes', '.debug')) {
        throw "Development-only file in the application payload: $($file.FullName)"
    }
}
foreach ($requiredFile in @(
    'Qt6Core.dll', 'Qt6Gui.dll', 'Qt6Quick.dll', 'Qt6QuickControls2Basic.dll',
    'qml\QtQuick\Controls\Basic\qmldir', 'conpty.dll',
    'x64\OpenConsole.exe', 'arm64\OpenConsole.exe'
)) {
    if (-not (Test-Path -LiteralPath (Join-Path $runtimeRoot $requiredFile) -PathType Leaf)) {
        throw "Required runtime file missing: $requiredFile"
    }
}
$controlsRoot = Join-Path $runtimeRoot 'qml\QtQuick\Controls'
$otherStyles = Get-ChildItem -LiteralPath $controlsRoot -Directory |
    Where-Object { $_.Name -notin @('Basic', 'impl') }
if ($otherStyles) {
    throw "Unselected Controls styles remain: $($otherStyles.Name -join ', ')"
}
foreach ($unusedRuntime in @(
    'qml\QtQuick\NativeStyle', 'opengl32sw.dll',
    'D3Dcompiler_47.dll', 'dxcompiler.dll', 'dxil.dll'
)) {
    if (Test-Path -LiteralPath (Join-Path $runtimeRoot $unusedRuntime)) {
        throw "Unselected runtime deployment remains: $unusedRuntime"
    }
}
Write-Output 'Windows portable payload has symbol-free application executables and runtime-only Qt deployment.'
