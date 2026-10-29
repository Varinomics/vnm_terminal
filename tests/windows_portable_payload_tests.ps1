[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$PayloadPath,

    [string]$ArchivePath,

    [string]$InstallerPath
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
    $debugRva  = [BitConverter]::ToUInt32($bytes, $directories + 6 * 8)
    $debugSize = [BitConverter]::ToUInt32($bytes, $directories + 6 * 8 + 4)

    $sectionCount = [BitConverter]::ToUInt16($bytes, $peOffset + 6)
    $headerSize   = [BitConverter]::ToUInt16($bytes, $peOffset + 20)
    $debugOffset = $null
    for ($index = 0; $index -lt $sectionCount; ++$index) {
        $offset = $optionalHeader + $headerSize + $index * 40
        $name = [Text.Encoding]::ASCII.GetString($bytes, $offset, 8).TrimEnd([char]0)
        if ($name -match '^\.(debug|zdebug|stab)' -or $name.StartsWith('/')) {
            throw "Release executable retains a debugging section $name`: $Path"
        }
        $rva  = [BitConverter]::ToUInt32($bytes, $offset + 12)
        $size = [BitConverter]::ToUInt32($bytes, $offset + 16)
        if ($debugSize -gt 0 -and $debugRva -ge $rva -and
            $debugRva + $debugSize -le $rva + $size)
        {
            $debugOffset = [BitConverter]::ToUInt32($bytes, $offset + 20) + $debugRva - $rva
        }
    }
    if ($debugSize % 28 -ne 0 -or ($debugSize -gt 0 -and $null -eq $debugOffset)) {
        throw "Invalid PE debug directory: $Path"
    }
    # Match the ConPTY producer: REPRO and extended DLL characteristics carry
    # build identity and CET security metadata, rather than debugging symbols.
    for ($index = 0; $index -lt $debugSize; $index += 28) {
        $type = [BitConverter]::ToUInt32($bytes, $debugOffset + $index + 12)
        if ($type -notin @(16, 20)) {
            throw "Release executable retains debugging metadata type ${type}: $Path"
        }
    }
}

$payloadRoot = (Resolve-Path -LiteralPath $PayloadPath).Path
$runtimeRoot = Join-Path $payloadRoot 'vnm_terminal_runtime'
Assert-SymbolFreeExecutable (Join-Path $payloadRoot 'vnm_terminal.exe')
Assert-SymbolFreeExecutable (Join-Path $runtimeRoot 'vnm_terminal.exe')
if ($InstallerPath) {
    Assert-SymbolFreeExecutable (Resolve-Path -LiteralPath $InstallerPath).Path
}

foreach ($file in (Get-ChildItem -LiteralPath $payloadRoot -Recurse -File -Force)) {
    if ($file.Extension -eq '.qmltypes') {
        throw "Development-only Qt tooling metadata in the runtime deployment: $($file.FullName)"
    }
    if ($file.Extension -in @('.exe', '.dll')) {
        if ($file.Extension -eq '.exe' -or $file.Name -eq 'conpty.dll' -or
            $file.Name -like 'vnm_*.dll')
        {
            Assert-SymbolFreeExecutable $file.FullName
        }
        else {
            Write-Output "Supplied vendor runtime (debug metadata permitted): $($file.FullName)"
        }
    }
}
$inspector = Join-Path $PSScriptRoot '..\tools\inspect_runtime_contents.py'
& python $inspector $payloadRoot
if ($LASTEXITCODE -ne 0) { throw 'Runtime contents inspection failed.' }
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
if ($ArchivePath) {
    $archiveRoot = Join-Path ([IO.Path]::GetTempPath()) (
        'vnm-terminal-packed-' + [Guid]::NewGuid().ToString('N'))
    $temporaryPrefix = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
    $archiveRoot = [IO.Path]::GetFullPath($archiveRoot)
    if (-not $archiveRoot.StartsWith($temporaryPrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Packed inspection target is outside the temporary directory: $archiveRoot"
    }
    try {
        Expand-Archive -LiteralPath $ArchivePath -DestinationPath $archiveRoot
        & $PSCommandPath -PayloadPath $archiveRoot
        & python $inspector $archiveRoot --reference $payloadRoot
        if ($LASTEXITCODE -ne 0) { throw 'Packed ZIP differs from its final staging tree.' }
    }
    finally {
        if (Test-Path -LiteralPath $archiveRoot) {
            Remove-Item -LiteralPath $archiveRoot -Recurse -Force
        }
    }
}
Write-Output 'Windows portable payload has symbol-free application executables and runtime-only Qt deployment.'
