[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$SourceRoot,

    [string]$ArtifactPath,

    [string]$DumpPath,

    [string]$IfwArchivePath
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

    $harnessPath = [IO.Path]::Combine(
        [IO.Path]::GetTempPath(),
        "vnm-terminal-ifw-ready-$([Guid]::NewGuid().ToString('N')).js")
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

function Assert-IfwExistingInstallationRuntime {
    param(
        [Parameter(Mandatory = $true)]
        [string]$ControllerScriptPath
    )

    $node = Get-Command node.exe -ErrorAction SilentlyContinue
    Assert-IfwContract ($null -ne $node) `
        'the existing-installation contract requires node.exe'

    $harnessPath = [IO.Path]::Combine(
        [IO.Path]::GetTempPath(),
        "vnm-terminal-ifw-existing-$([Guid]::NewGuid().ToString('N')).js")
    try {
        $harness = @'
const fs = require("fs");
const controllerScript = fs.readFileSync(process.argv[2], "utf8");
const powershellPath =
    "C:\\Windows\\System32\\WindowsPowerShell\\v1.0\\powershell.exe";

let state = null;

global.QInstaller = { ComponentSelection: 1 };
global.QMessageBox = {
    Yes: 0x00004000,
    No: 0x00010000,
    question(identifier, title, text, buttons) {
        state.questions.push({ identifier, title, text, buttons });
        return state.answer;
    },
    critical(identifier, title, text) {
        state.errors.push({ identifier, title, text });
        return 0x00000400;
    },
};
global.installer = {
    isInstaller() { return true; },
    setDefaultPageVisible() {},
    setValue() {},
    readFile() { return ""; },
    toNativeSeparators(value) { return value.replace(/\//g, "\\"); },
    value(name) {
        if (name === "TargetDir") return state.directory;
        if (name === "RootDir") return "C:/";
        throw new Error("unexpected installer value: " + name);
    },
    fileExists(path) {
        if (path === state.maintenanceToolPath) return state.installationPresent;
        if (path === state.nativeDirectory) return state.directoryPresent;
        return false;
    },
    execute(program, args, stdIn) {
        state.executions.push({ program, args, stdIn });
        if (program === state.maintenanceToolPath) {
            return state.purgeStarts ? ["", state.purgeExitCode] : [];
        }
        if (program === powershellPath) {
            if (state.directoryRemovedByWait) state.directoryPresent = false;
            return ["", 0];
        }
        throw new Error("unexpected execution: " + program);
    },
};
global.gui = {
    pageWidgetByObjectName(name) {
        if (name !== "TargetDirectoryPage")
            throw new Error("unexpected page lookup: " + name);
        return { subTitle: "" };
    },
};

eval(controllerScript);

function run(overrides) {
    const directory = overrides.directory || "C:/Program Files/vnm_terminal";
    const nativeDirectory = directory.replace(/\//g, "\\");
    state = Object.assign({
        directory,
        nativeDirectory,
        maintenanceToolPath:
            nativeDirectory + "\\vnm_terminal_maintenance.exe",
        installationPresent: true,
        directoryPresent: true,
        directoryRemovedByWait: true,
        purgeStarts: true,
        purgeExitCode: 0,
        questions: [],
        errors: [],
        executions: [],
    }, overrides);
    new Controller();
    Controller.prototype.TargetDirectoryPageCallback();
    state.purges = state.executions.filter(
        (execution) => execution.program === state.maintenanceToolPath);
    state.waits = state.executions.filter(
        (execution) => execution.program === powershellPath);
    return state;
}

let result = run({
    installationPresent: false,
    directoryPresent: false,
    answer: QMessageBox.No,
});
if (result.questions.length !== 0 || result.executions.length !== 0 ||
    result.errors.length !== 0)
{
    throw new Error("a free target directory must not offer a removal");
}

result = run({ answer: QMessageBox.No });
if (result.questions.length !== 1 || result.executions.length !== 0 ||
    result.errors.length !== 0)
{
    throw new Error("a declined removal must leave the installation in place");
}
if (result.questions[0].buttons !== (QMessageBox.Yes | QMessageBox.No) ||
    result.questions[0].text.indexOf(result.nativeDirectory) < 0)
{
    throw new Error("the offer must be a Yes/No choice naming its directory");
}

result = run({ answer: QMessageBox.Yes });
if (result.purges.length !== 1 ||
    result.purges[0].args.join(" ") !==
        "purge --accept-messages --confirm-command")
{
    throw new Error(
        "acceptance must run the existing maintenance tool once with a confirmed purge");
}
if (result.waits.length !== 1 || result.errors.length !== 0)
{
    throw new Error(
        "a completed removal must wait for the detached deletion and report no failure");
}

result = run({ answer: QMessageBox.Yes, directory: "D:/Tools/O'Brien/terminal" });
if (result.waits[0].stdIn.indexOf(
        "-LiteralPath 'D:\\Tools\\O''Brien\\terminal'") < 0)
{
    throw new Error("the wait must quote the directory as a PowerShell literal");
}

result = run({ answer: QMessageBox.Yes, purgeExitCode: 1 });
if (result.waits.length !== 0 || result.errors.length !== 1 ||
    result.errors[0].text.indexOf(result.nativeDirectory) < 0)
{
    throw new Error(
        "a failed purge must be reported without waiting for a removal that cannot happen");
}

result = run({ answer: QMessageBox.Yes, purgeStarts: false });
if (result.waits.length !== 0 || result.errors.length !== 1)
    throw new Error("an unstartable maintenance tool must be reported");

result = run({ answer: QMessageBox.Yes, directoryRemovedByWait: false });
if (result.errors.length !== 1)
    throw new Error("a surviving directory must be reported, not accepted");
'@
        [IO.File]::WriteAllText(
            $harnessPath,
            $harness,
            [Text.UTF8Encoding]::new($false))
        $runtimeOutput = & $node.Source $harnessPath $ControllerScriptPath 2>&1 |
            Out-String
        Assert-IfwContract ($LASTEXITCODE -eq 0) `
            "the existing-installation offer must satisfy its runtime contract: $runtimeOutput"
    }
    finally {
        Remove-Item -LiteralPath $harnessPath -Force -ErrorAction SilentlyContinue
    }
}

function Assert-IfwStartMenuShortcutRuntime {
    param(
        [Parameter(Mandatory = $true)]
        [string]$InstallScriptPath
    )

    $node = Get-Command node.exe -ErrorAction SilentlyContinue
    Assert-IfwContract ($null -ne $node) `
        'the Start Menu mapping contract requires node.exe'

    $harnessPath = [IO.Path]::Combine(
        [IO.Path]::GetTempPath(),
        "vnm-terminal-ifw-start-menu-$([Guid]::NewGuid().ToString('N')).js")
    try {
        $harness = @'
const fs = require("fs");
const childProcess = require("child_process");
const path = require("path");
const installScript = fs.readFileSync(process.argv[2], "utf8");
const userPrograms = "C:\\Users\\runner\\AppData\\Roaming\\Microsoft\\Windows\\Start Menu\\Programs";
const allUsersPrograms = "C:\\ProgramData\\Microsoft\\Windows\\Start Menu\\Programs";
const values = {
    StartMenuDir: "",
    UserStartMenuProgramsPath: userPrograms,
    AllUsersStartMenuProgramsPath: allUsersPrograms,
    RootDir: path.parse(process.env.SystemRoot).root,
};
let defaultOperations = 0;
let elevatedOperations = [];

global.installer = {
    value(name) { return values[name] || ""; },
    fileExists(filePath) { return fs.existsSync(filePath); },
    toNativeSeparators(filePath) { return filePath.replace(/\//g, "\\"); },
    execute(executable, args, stdin) {
        const result = childProcess.spawnSync(executable, args, {
            encoding: "utf8",
            input: stdin || "",
            windowsHide: true,
        });
        return [result.stdout || "", result.status];
    },
};
global.component = {
    createOperations() { ++defaultOperations; },
    addElevatedOperation() {
        elevatedOperations.push(Array.from(arguments));
    },
};

eval(installScript);

function reset(selection) {
    values.StartMenuDir = selection;
    defaultOperations = 0;
    elevatedOperations = [];
}

function expectMapped(selection, expectedShortcut) {
    reset(selection);
    Component.prototype.createOperations();
    if (defaultOperations !== 1 || elevatedOperations.length !== 1)
        throw new Error("the component did not add exactly one shortcut operation");

    const operation = elevatedOperations[0];
    if (operation.length !== 3 || operation[0] !== "CreateShortcut" ||
        operation[1] !== "@TargetDir@/vnm_terminal.exe" ||
        operation[2] !== expectedShortcut)
    {
        throw new Error("the component did not preserve the selected all-users group");
    }
}

function expectRejected(selection) {
    reset(selection);
    let rejected = false;
    try {
        Component.prototype.createOperations();
    }
    catch (error) {
        rejected = true;
    }
    if (!rejected || defaultOperations !== 0 || elevatedOperations.length !== 0)
        throw new Error("an out-of-scope Start Menu selection was not rejected before operation creation");
}

expectMapped(
    userPrograms + "\\Varinomics\\Chosen Group",
    allUsersPrograms.replace(/\\/g, "/") + "/Varinomics/Chosen Group/vnm_terminal.lnk");
expectMapped(
    allUsersPrograms + "\\Another Group",
    allUsersPrograms.replace(/\\/g, "/") + "/Another Group/vnm_terminal.lnk");
expectMapped(
    "c:/USERS/RUNNER/AppData/Roaming/Microsoft/Windows/Start Menu/Programs/Mixed Case Group",
    allUsersPrograms.replace(/\\/g, "/") + "/Mixed Case Group/vnm_terminal.lnk");
expectMapped(
    "c:\\\\Users//runner\\AppData/Roaming/Microsoft/Windows/Start Menu/Programs\\Separator Group\\",
    allUsersPrograms.replace(/\\/g, "/") + "/Separator Group/vnm_terminal.lnk");

values.UserStartMenuProgramsPath =
    "C:\\Users\\\u0130mak\\AppData\\Roaming\\Microsoft\\Windows\\Start Menu\\Programs";
expectMapped(
    values.UserStartMenuProgramsPath + "\\Chosen Group",
    allUsersPrograms.replace(/\\/g, "/") + "/Chosen Group/vnm_terminal.lnk");
expectRejected(
    "C:\\Users\\I\u0307mak\\AppData\\Roaming\\Microsoft\\Windows\\Start Menu\\Programs\\Lookalike Group");

values.UserStartMenuProgramsPath =
    "C:\\Users\\I\u0307mak\\AppData\\Roaming\\Microsoft\\Windows\\Start Menu\\Programs";
expectMapped(
    values.UserStartMenuProgramsPath + "\\Decomposed Group",
    allUsersPrograms.replace(/\\/g, "/") + "/Decomposed Group/vnm_terminal.lnk");
expectRejected(
    "C:\\Users\\\u0130mak\\AppData\\Roaming\\Microsoft\\Windows\\Start Menu\\Programs\\Composed Lookalike Group");

values.UserStartMenuProgramsPath =
    "C:\\Users\\O'Brien\\AppData\\Roaming\\Microsoft\\Windows\\Start Menu\\Programs";
expectMapped(
    values.UserStartMenuProgramsPath + "\\Chosen O'Brien Group",
    allUsersPrograms.replace(/\\/g, "/") + "/Chosen O'Brien Group/vnm_terminal.lnk");

values.UserStartMenuProgramsPath = userPrograms;
expectRejected("D:\\Unrelated\\Programs\\Unexpected Group");
expectRejected(userPrograms + " Unexpected\\Group");
expectRejected(userPrograms + "\\..\\Escaped Group");
'@
        [IO.File]::WriteAllText(
            $harnessPath,
            $harness,
            [Text.UTF8Encoding]::new($false))
        $runtimeOutput = & $node.Source $harnessPath $InstallScriptPath 2>&1 |
            Out-String
        Assert-IfwContract ($LASTEXITCODE -eq 0) `
            "the Start Menu group mapping must preserve scope at runtime: $runtimeOutput"
    }
    finally {
        Remove-Item -LiteralPath $harnessPath -Force -ErrorAction SilentlyContinue
    }
}

function Assert-IfwHashRuntime {
    param(
        [Parameter(Mandatory = $true)]
        [string]$ScriptPath,

        [Parameter(Mandatory = $true)]
        [string]$Description
    )

    $tokens = $null
    $parseErrors = $null
    $scriptAst = [Management.Automation.Language.Parser]::ParseFile(
        $ScriptPath,
        [ref]$tokens,
        [ref]$parseErrors)
    Assert-IfwContract ($parseErrors.Count -eq 0) `
        "$Description must parse in Windows PowerShell"

    $hashFunctions = @($scriptAst.FindAll(
        {
            param($node)
            return $node -is [Management.Automation.Language.FunctionDefinitionAst] -and
                $node.Name -eq 'Get-Sha256FileHash'
        },
        $true))
    Assert-IfwContract ($hashFunctions.Count -eq 1) `
        "$Description must define one SHA-256 file helper"

    $probePath = [System.IO.Path]::GetTempFileName()
    try {
        [System.IO.File]::WriteAllText(
            $probePath,
            'Varinomics IFW hash probe',
            [System.Text.UTF8Encoding]::new($false))
        Invoke-Expression $hashFunctions[0].Extent.Text
        function Get-FileHash {
            throw 'Get-FileHash is unavailable in the hosted packaging shell.'
        }

        $hash = Get-Sha256FileHash $probePath
        Assert-IfwContract `
            ($hash -eq 'fe133befaa3576ebe217cdfcb5a1a1c55263a311e9dfeb3c697085cbf0554cf4') `
            "$Description hashing must work when Get-FileHash is unavailable"
    }
    finally {
        Remove-Item -LiteralPath $probePath -Force -ErrorAction SilentlyContinue
    }
}

function Assert-IfwPowerShellParses {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,

        [Parameter(Mandatory = $true)]
        [string]$Description
    )

    $tokens = $null
    $parseErrors = $null
    [void][Management.Automation.Language.Parser]::ParseFile(
        $Path,
        [ref]$tokens,
        [ref]$parseErrors)
    Assert-IfwContract ($parseErrors.Count -eq 0) `
        "$Description must parse in Windows PowerShell"
}

function Assert-IfwCertificateTableRuntime {
    param(
        [Parameter(Mandatory = $true)]
        [string]$BuildScriptPath
    )

    $tokens = $null
    $parseErrors = $null
    $scriptAst = [Management.Automation.Language.Parser]::ParseFile(
        $BuildScriptPath,
        [ref]$tokens,
        [ref]$parseErrors)
    Assert-IfwContract ($parseErrors.Count -eq 0) `
        'the IFW build script must parse before certificate-table testing'

    $certificateFunctions = @($scriptAst.FindAll(
        {
            param($node)
            return $node -is [Management.Automation.Language.FunctionDefinitionAst] -and
                $node.Name -eq 'Test-PeHasCertificateTable'
        },
        $true))
    Assert-IfwContract ($certificateFunctions.Count -eq 1) `
        'the IFW build script must define one PE certificate-table helper'

    Invoke-Expression $certificateFunctions[0].Extent.Text
    function Get-AuthenticodeSignature {
        throw 'Get-AuthenticodeSignature is unavailable in the hosted packaging shell.'
    }

    $testRoot = Join-Path ([IO.Path]::GetTempPath()) (
        'vnm-ifw-pe-test-' + [Guid]::NewGuid().ToString('N'))
    $pe64Path = Join-Path $testRoot 'pe64.exe'
    $pe32Path = Join-Path $testRoot 'pe32.exe'
    try {
        [void](New-Item -ItemType Directory -Path $testRoot)
        $peBytes = [byte[]]::new(520)
        [BitConverter]::GetBytes([uint16]0x5a4d).CopyTo($peBytes, 0)
        [BitConverter]::GetBytes([uint32]0x80).CopyTo($peBytes, 0x3c)
        [BitConverter]::GetBytes([uint32]0x00004550).CopyTo($peBytes, 0x80)
        [BitConverter]::GetBytes([uint16]0x00f0).CopyTo($peBytes, 0x94)
        [BitConverter]::GetBytes([uint16]0x020b).CopyTo($peBytes, 0x98)
        [BitConverter]::GetBytes([uint32]16).CopyTo($peBytes, 0x104)
        [IO.File]::WriteAllBytes($pe64Path, $peBytes)

        Assert-IfwContract (-not (Test-PeHasCertificateTable $pe64Path)) `
            'unsigned PE32+ detection must not depend on Get-AuthenticodeSignature'

        [BitConverter]::GetBytes([uint32]4).CopyTo($peBytes, 0x104)
        [IO.File]::WriteAllBytes($pe64Path, $peBytes)
        Assert-IfwContract (-not (Test-PeHasCertificateTable $pe64Path)) `
            'a PE with no declared security-directory entry must be unsigned'
        [BitConverter]::GetBytes([uint32]16).CopyTo($peBytes, 0x104)

        [BitConverter]::GetBytes([uint32]0x200).CopyTo($peBytes, 0x128)
        [IO.File]::WriteAllBytes($pe64Path, $peBytes)
        $inconsistentMessage = $null
        try {
            [void](Test-PeHasCertificateTable $pe64Path)
        }
        catch {
            $inconsistentMessage = $_.Exception.Message
        }
        Assert-IfwContract ($inconsistentMessage -match 'inconsistent security directory') `
            'a half-empty PE security-directory entry must be rejected as malformed'

        [BitConverter]::GetBytes([uint32]8).CopyTo($peBytes, 0x12c)
        [IO.File]::WriteAllBytes($pe64Path, $peBytes)
        Assert-IfwContract (Test-PeHasCertificateTable $pe64Path) `
            'a non-empty PE32+ certificate-table entry must be rejected as signed'

        [BitConverter]::GetBytes([uint32]0).CopyTo($peBytes, 0x128)
        [BitConverter]::GetBytes([uint32]0).CopyTo($peBytes, 0x12c)
        [BitConverter]::GetBytes([uint16]0x00e0).CopyTo($peBytes, 0x94)
        [BitConverter]::GetBytes([uint16]0x010b).CopyTo($peBytes, 0x98)
        [BitConverter]::GetBytes([uint32]16).CopyTo($peBytes, 0xf4)
        [IO.File]::WriteAllBytes($pe32Path, $peBytes)
        Assert-IfwContract (-not (Test-PeHasCertificateTable $pe32Path)) `
            'an empty PE32 security-directory entry must be unsigned'

        [BitConverter]::GetBytes([uint32]0x200).CopyTo($peBytes, 0x118)
        [BitConverter]::GetBytes([uint32]8).CopyTo($peBytes, 0x11c)
        [IO.File]::WriteAllBytes($pe32Path, $peBytes)
        Assert-IfwContract (Test-PeHasCertificateTable $pe32Path) `
            'a non-empty PE32 certificate-table entry must be rejected as signed'
    }
    finally {
        Remove-Item -LiteralPath $testRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
}

function Assert-IfwGuiSubsystemRuntime {
    param(
        [Parameter(Mandatory = $true)]
        [string]$BuildScriptPath
    )

    $tokens = $null
    $parseErrors = $null
    $scriptAst = [Management.Automation.Language.Parser]::ParseFile(
        $BuildScriptPath,
        [ref]$tokens,
        [ref]$parseErrors)
    Assert-IfwContract ($parseErrors.Count -eq 0) `
        'the IFW build script must parse before subsystem testing'

    $subsystemFunctions = @($scriptAst.FindAll(
        {
            param($node)
            return $node -is [Management.Automation.Language.FunctionDefinitionAst] -and
                $node.Name -eq 'Set-PeGuiSubsystem'
        },
        $true))
    Assert-IfwContract ($subsystemFunctions.Count -eq 1) `
        'the IFW build script must define one PE subsystem helper'

    Invoke-Expression $subsystemFunctions[0].Extent.Text

    # The fixtures below place the PE header at 0x80, so their optional header
    # starts at 0x98 and its CheckSum and Subsystem fields sit at 0xd8 and 0xdc.
    $checkSumFieldPosition = 0xd8
    $subsystemFieldPosition = 0xdc

    function Get-FixtureSubsystem {
        param(
            [Parameter(Mandatory = $true)]
            [string]$Path
        )

        $stream = [IO.File]::OpenRead($Path)
        $reader = [IO.BinaryReader]::new($stream)
        try {
            $stream.Position = $subsystemFieldPosition
            return $reader.ReadUInt16()
        }
        finally {
            $reader.Dispose()
            $stream.Dispose()
        }
    }

    function Assert-SubsystemFixtureThrows {
        param(
            [Parameter(Mandatory = $true)]
            [string]$Path,

            [Parameter(Mandatory = $true)]
            [string]$ExpectedMessage
        )

        $message = $null
        try {
            Set-PeGuiSubsystem $Path
        }
        catch {
            $message = $_.Exception.Message
        }
        Assert-IfwContract ($message -match $ExpectedMessage) `
            "the subsystem rewrite must reject the fixture with $ExpectedMessage"
    }

    $testRoot = Join-Path ([IO.Path]::GetTempPath()) (
        'vnm-ifw-subsystem-test-' + [Guid]::NewGuid().ToString('N'))
    $pe64Path = Join-Path $testRoot 'pe64.exe'
    $pe32Path = Join-Path $testRoot 'pe32.exe'
    try {
        [void](New-Item -ItemType Directory -Path $testRoot)
        $peBytes = [byte[]]::new(520)
        [BitConverter]::GetBytes([uint16]0x5a4d).CopyTo($peBytes, 0)
        [BitConverter]::GetBytes([uint32]0x80).CopyTo($peBytes, 0x3c)
        [BitConverter]::GetBytes([uint32]0x00004550).CopyTo($peBytes, 0x80)
        [BitConverter]::GetBytes([uint16]0x00f0).CopyTo($peBytes, 0x94)
        [BitConverter]::GetBytes([uint16]0x020b).CopyTo($peBytes, 0x98)
        [BitConverter]::GetBytes([uint16]3).CopyTo($peBytes, $subsystemFieldPosition)
        [IO.File]::WriteAllBytes($pe64Path, $peBytes)

        Set-PeGuiSubsystem $pe64Path
        Assert-IfwContract ((Get-FixtureSubsystem $pe64Path) -eq 2) `
            'a console-subsystem PE32+ image must be republished as a graphical image'
        Set-PeGuiSubsystem $pe64Path
        Assert-IfwContract ((Get-FixtureSubsystem $pe64Path) -eq 2) `
            'rewriting an already graphical image must leave it unchanged'

        [BitConverter]::GetBytes([uint16]0x00e0).CopyTo($peBytes, 0x94)
        [BitConverter]::GetBytes([uint16]0x010b).CopyTo($peBytes, 0x98)
        [IO.File]::WriteAllBytes($pe32Path, $peBytes)
        Set-PeGuiSubsystem $pe32Path
        Assert-IfwContract ((Get-FixtureSubsystem $pe32Path) -eq 2) `
            'a console-subsystem PE32 image must be republished as a graphical image'
        [BitConverter]::GetBytes([uint16]0x00f0).CopyTo($peBytes, 0x94)
        [BitConverter]::GetBytes([uint16]0x020b).CopyTo($peBytes, 0x98)

        [BitConverter]::GetBytes([uint32]0x12345678).CopyTo(
            $peBytes, $checkSumFieldPosition)
        [IO.File]::WriteAllBytes($pe64Path, $peBytes)
        Assert-SubsystemFixtureThrows $pe64Path 'header checksum'
        Assert-IfwContract ((Get-FixtureSubsystem $pe64Path) -eq 3) `
            'a rejected checksummed image must keep its authored subsystem'
        [BitConverter]::GetBytes([uint32]0).CopyTo($peBytes, $checkSumFieldPosition)

        [BitConverter]::GetBytes([uint16]9).CopyTo($peBytes, $subsystemFieldPosition)
        [IO.File]::WriteAllBytes($pe64Path, $peBytes)
        Assert-SubsystemFixtureThrows $pe64Path 'unsupported subsystem'
        [BitConverter]::GetBytes([uint16]3).CopyTo($peBytes, $subsystemFieldPosition)

        [BitConverter]::GetBytes([uint16]69).CopyTo($peBytes, 0x94)
        [IO.File]::WriteAllBytes($pe64Path, $peBytes)
        Assert-SubsystemFixtureThrows $pe64Path 'truncated optional header'
        [BitConverter]::GetBytes([uint16]0x00f0).CopyTo($peBytes, 0x94)

        [BitConverter]::GetBytes([uint32]0).CopyTo($peBytes, 0x80)
        [IO.File]::WriteAllBytes($pe64Path, $peBytes)
        Assert-SubsystemFixtureThrows $pe64Path 'does not have a PE header'
    }
    finally {
        Remove-Item -LiteralPath $testRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
}

function Assert-IfwSignedValidationRuntime {
    param(
        [Parameter(Mandatory = $true)]
        [string]$BuildScriptPath
    )

    $tokens = $null
    $parseErrors = $null
    $scriptAst = [Management.Automation.Language.Parser]::ParseFile(
        $BuildScriptPath,
        [ref]$tokens,
        [ref]$parseErrors)
    Assert-IfwContract ($parseErrors.Count -eq 0) `
        'the IFW build script must parse before signed-artifact testing'

    $signingFunctions = @($scriptAst.FindAll(
        {
            param($node)
            return $node -is [Management.Automation.Language.FunctionDefinitionAst] -and
                $node.Name -eq 'Invoke-TrustedSigning'
        },
        $true))
    Assert-IfwContract ($signingFunctions.Count -eq 1) `
        'the IFW build script must define one trusted-signing helper'

    Invoke-Expression $signingFunctions[0].Extent.Text

    $testRoot = Join-Path ([IO.Path]::GetTempPath()) (
        'vnm-ifw-signing-test-' + [Guid]::NewGuid().ToString('N'))
    $artifactPath = Join-Path $testRoot 'artifact.exe'
    $SignToolPath = Join-Path $testRoot 'signtool.cmd'
    $TrustedSigningDlibPath = 'unused-dlib'
    $TrustedSigningMetadataPath = 'unused-metadata'
    $timestampUrl = 'https://timestamp.example.test'
    $expectedPublisher = 'Varinomics Ltd'
    $signatureFixture = $null
    function Get-AuthenticodeSignature {
        param([string]$LiteralPath)

        if ($LiteralPath -ne $artifactPath) {
            throw "Unexpected signature fixture path: $LiteralPath"
        }
        return $signatureFixture
    }

    function Assert-SigningFixtureThrows {
        param(
            [Parameter(Mandatory = $true)]
            [string]$ExpectedMessage
        )

        $message = $null
        try {
            Invoke-TrustedSigning $artifactPath
        }
        catch {
            $message = $_.Exception.Message
        }
        Assert-IfwContract ($message -match $ExpectedMessage) `
            "signed validation must reject the fixture with $ExpectedMessage"
    }

    try {
        [void](New-Item -ItemType Directory -Path $testRoot)
        [IO.File]::WriteAllBytes($artifactPath, [byte[]]::new(1))
        [IO.File]::WriteAllText(
            $SignToolPath,
            "@echo off`r`nexit /b 0`r`n",
            [Text.Encoding]::ASCII)

        # signtool is invoked with /tr, so a release signature always carries a
        # timestamp countersignature. The fixture models one for the same reason
        # it models the signer: the validation rejects a signature without it.
        $signatureFixture = [pscustomobject]@{
            Status = 'Valid'
            StatusMessage = 'Signature is valid.'
            SignerCertificate = [pscustomobject]@{
                Subject = 'CN=Varinomics Ltd, O=Varinomics Ltd'
            }
            TimeStamperCertificate = [pscustomobject]@{
                Subject = 'CN=Microsoft Public RSA Timestamping CA 2020'
            }
        }
        Invoke-TrustedSigning $artifactPath

        $signatureFixture.Status = 'HashMismatch'
        $signatureFixture.StatusMessage = 'The file hash does not match.'
        Assert-SigningFixtureThrows 'is not valid'

        $signatureFixture.Status = 'Valid'
        $signatureFixture.SignerCertificate.Subject = 'CN=Unexpected Publisher'
        Assert-SigningFixtureThrows 'does not identify Varinomics Ltd'

        # A subject that merely contains the publisher somewhere is not the
        # publisher: only a whole CN component is.
        $signatureFixture.SignerCertificate.Subject = 'CN=Not Varinomics Ltd Either'
        Assert-SigningFixtureThrows 'does not identify Varinomics Ltd'

        $signatureFixture.SignerCertificate.Subject = 'CN=Varinomics Ltd, O=Varinomics Ltd'
        $signatureFixture.TimeStamperCertificate = $null
        Assert-SigningFixtureThrows 'is not timestamped'

        $signatureFixture.TimeStamperCertificate = [pscustomobject]@{
            Subject = 'CN=Microsoft Public RSA Timestamping CA 2020'
        }
        $signatureFixture.SignerCertificate = $null
        Assert-SigningFixtureThrows 'does not identify Varinomics Ltd'
    }
    finally {
        Remove-Item -LiteralPath $testRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
}

function Assert-IfwProvisionerRuntime {
    param(
        [Parameter(Mandatory = $true)]
        [string]$ProvisionScriptPath,

        [Parameter(Mandatory = $true)]
        [string]$ArchivePath
    )

    $resolvedArchivePath = (Resolve-Path -LiteralPath $ArchivePath).Path
    $testRoot = Join-Path ([IO.Path]::GetTempPath()) (
        'vnm-ifw-provision-test-' + [Guid]::NewGuid().ToString('N'))
    $destinationPath = Join-Path $testRoot 'root'
    $corruptArchivePath = Join-Path $testRoot 'corrupt.7z'
    $corruptDestinationPath = Join-Path $testRoot 'corrupt-root'
    $extractionFailureDestinationPath = Join-Path $testRoot 'extraction-failure-root'
    $fakeToolsPath = Join-Path $testRoot 'fake-tools'

    function Invoke-Provisioner {
        param(
            [Parameter(Mandatory = $true)]
            [string]$InputArchivePath,

            [Parameter(Mandatory = $true)]
            [string]$OutputRootPath,

            [string]$PathPrefix
        )

        $originalPath = $env:PATH
        $originalErrorActionPreference = $ErrorActionPreference
        try {
            if ($PathPrefix) {
                $env:PATH = "$PathPrefix;$originalPath"
            }
            $ErrorActionPreference = 'Continue'
            $output = & powershell.exe -NoLogo -NoProfile -NonInteractive `
                -ExecutionPolicy Bypass `
                -File $ProvisionScriptPath `
                -ArchivePath $InputArchivePath `
                -DestinationPath $OutputRootPath 2>&1 |
                    Out-String
            return [pscustomobject]@{
                ExitCode = $LASTEXITCODE
                Output = $output
            }
        }
        finally {
            $env:PATH = $originalPath
            $ErrorActionPreference = $originalErrorActionPreference
        }
    }

    try {
        [void](New-Item -ItemType Directory -Path $testRoot)

        $freshResult = Invoke-Provisioner $resolvedArchivePath $destinationPath
        Assert-IfwContract ($freshResult.ExitCode -eq 0) `
            "fresh explicit-archive provisioning must succeed: $($freshResult.Output)"
        Assert-IfwContract `
            (Test-Path -LiteralPath (Join-Path $destinationPath 'bin\binarycreator.exe') -PathType Leaf) `
            'fresh provisioning must publish binarycreator in the requested root'
        Assert-IfwContract `
            (Test-Path -LiteralPath (Join-Path $destinationPath 'bin\installerbase.exe') -PathType Leaf) `
            'fresh provisioning must publish installerbase in the requested root'
        $markerPath = Join-Path $destinationPath '.vnm-ifw-provisioned.json'
        Assert-IfwContract (Test-Path -LiteralPath $markerPath -PathType Leaf) `
            'fresh provisioning must atomically publish its verification marker with the root'
        $marker = Get-Content -LiteralPath $markerPath -Raw | ConvertFrom-Json
        Assert-IfwContract ($marker.version -eq '4.11.0') `
            'the provisioned root marker must identify IFW 4.11.0'
        Assert-IfwContract `
            ($marker.archive_sha256 -eq 'c47201c4f6a82a8b607daa245237f40831d78425e904edd1514b71fd17efefc1') `
            'the provisioned root marker must record the verified official archive hash'

        $staleSentinelPath = Join-Path $destinationPath 'must-not-survive-reprovision.txt'
        [IO.File]::WriteAllText($staleSentinelPath, 'stale extraction')
        $secondResult = Invoke-Provisioner $resolvedArchivePath $destinationPath
        Assert-IfwContract ($secondResult.ExitCode -eq 0) `
            "a second extraction from the same cache input must succeed: $($secondResult.Output)"
        Assert-IfwContract (-not (Test-Path -LiteralPath $staleSentinelPath)) `
            'a cache-input hit must still rehash and extract a complete fresh root'

        [IO.File]::WriteAllText($corruptArchivePath, 'not the official IFW archive')
        $corruptResult = Invoke-Provisioner $corruptArchivePath $corruptDestinationPath
        Assert-IfwContract ($corruptResult.ExitCode -ne 0) `
            'a corrupt explicit archive must fail provisioning'
        Assert-IfwContract (-not (Test-Path -LiteralPath $corruptDestinationPath)) `
            'a corrupt archive must not leave a committed IFW root'

        [void](New-Item -ItemType Directory -Path $fakeToolsPath)
        Copy-Item -LiteralPath "$env:SystemRoot\System32\where.exe" `
            -Destination (Join-Path $fakeToolsPath '7z.exe')
        $extractionFailureResult = Invoke-Provisioner `
            $resolvedArchivePath `
            $extractionFailureDestinationPath `
            $fakeToolsPath
        Assert-IfwContract ($extractionFailureResult.ExitCode -ne 0) `
            'an extraction-tool failure must fail provisioning'
        Assert-IfwContract `
            (-not (Test-Path -LiteralPath $extractionFailureDestinationPath)) `
            'an extraction failure must not leave a committed IFW root'
    }
    finally {
        Remove-Item -LiteralPath $testRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
}

$resolvedSourceRoot = (Resolve-Path -LiteralPath $SourceRoot).Path
$ifwSourceRoot = Join-Path $resolvedSourceRoot 'packaging\windows\ifw'
$configPath = Join-Path $ifwSourceRoot 'config.xml.in'
$styleSheetPath = Join-Path $ifwSourceRoot 'style.qss'
$controllerScriptPath = Join-Path $ifwSourceRoot 'controller.qs'
$checkboxCheckPath = Join-Path $ifwSourceRoot 'checkbox_check.svg'
$radioDotPath = Join-Path $ifwSourceRoot 'radio_dot.svg'
$comboArrowPath = Join-Path $ifwSourceRoot 'combo_arrow.svg'
$brandLogoPath = Join-Path $ifwSourceRoot 'varinomics_logo.png'
$brandGeometryPath = Join-Path $ifwSourceRoot 'varinomics_geometry.svg'
$brandBannerPath = Join-Path $ifwSourceRoot 'varinomics_banner.png'
$brandBannerHighDpiPath = Join-Path $ifwSourceRoot 'varinomics_banner@2x.png'
$brandGeometryPngPath = Join-Path $ifwSourceRoot 'varinomics_geometry.png'
$brandGeometryHighDpiPath = Join-Path $ifwSourceRoot 'varinomics_geometry@2x.png'
$brandProvenancePath = Join-Path $ifwSourceRoot 'brand_assets.provenance.json'
$logPathProbePath = Join-Path $ifwSourceRoot 'log_path_probe.ps1'
$themeResourcesPath = Join-Path $ifwSourceRoot 'theme_resources.qrc'
$packagePath = Join-Path $ifwSourceRoot 'package.xml.in'
$installScriptPath = Join-Path $ifwSourceRoot 'installscript.qs'
$maintenancePackagePath = Join-Path $ifwSourceRoot 'maintenance_package.xml.in'
$maintenanceInstallScriptPath = Join-Path $ifwSourceRoot 'maintenance_installscript.qs'
$buildScriptPath = Join-Path $resolvedSourceRoot 'tools\build_windows_ifw_installer.ps1'
$provisionScriptPath = Join-Path $resolvedSourceRoot 'tools\provision_windows_ifw.ps1'
$windowsWorkflowPath = Join-Path $resolvedSourceRoot '.github\workflows\ci-windows.yml'
$installationTestsPath = Join-Path `
    $resolvedSourceRoot 'tests\windows_ifw_installation_tests.ps1'
$windowsPackagesPath = Join-Path $resolvedSourceRoot 'build_windows_packages.bat'
$buildConfigExamplePath = Join-Path $resolvedSourceRoot 'build_config.bat.example'
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
$provisionScript = Get-Content -Raw -LiteralPath $provisionScriptPath
$windowsWorkflow = Get-Content -Raw -LiteralPath $windowsWorkflowPath
$windowsPackages = Get-Content -Raw -LiteralPath $windowsPackagesPath
$buildConfigExample = Get-Content -Raw -LiteralPath $buildConfigExamplePath
$brandRenderer = Get-Content -Raw -LiteralPath $brandRendererPath
$notices = Get-Content -Raw -LiteralPath $noticesPath
$installationTests = Get-Content -Raw -LiteralPath $installationTestsPath

Assert-IfwReadyPageRuntime $controllerScriptPath
Assert-IfwExistingInstallationRuntime $controllerScriptPath
Assert-IfwHashRuntime $buildScriptPath 'the IFW build script'
Assert-IfwHashRuntime $provisionScriptPath 'the IFW provisioner'
Assert-IfwCertificateTableRuntime $buildScriptPath
Assert-IfwGuiSubsystemRuntime $buildScriptPath
Assert-IfwSignedValidationRuntime $buildScriptPath
Assert-IfwPowerShellParses $provisionScriptPath 'the IFW provisioner'
Assert-IfwPowerShellParses `
    $installationTestsPath 'the generated-installer lifecycle test'
if ($IfwArchivePath) {
    Assert-IfwProvisionerRuntime $provisionScriptPath $IfwArchivePath
}
Assert-IfwStartMenuShortcutRuntime $installScriptPath

Assert-IfwContract ($config.Installer.Name -eq 'vnm_terminal') `
    'the product name must match the application'
Assert-IfwContract ($config.Installer.Title -eq 'vnm_terminal') `
    'the window title must stay the bare product name because IFW appends its own wizard wording'
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
    $dumpedInstallScriptPath = Join-Path `
        $resolvedDumpPath 'metadata\com.varinomics.vnm_terminal\installscript.qs'
    Assert-IfwContract `
        ((Test-Path -LiteralPath $dumpedConfigPath -PathType Leaf) -and
            (Test-Path -LiteralPath $dumpedBannerPath -PathType Leaf) -and
            (Test-Path -LiteralPath $dumpedBannerHighDpiPath -PathType Leaf) -and
            (Test-Path -LiteralPath $dumpedInstallScriptPath -PathType Leaf)) `
        'devtool dump must materialize the IFW config, component script, and both Banner density paths'

    [xml]$dumpedConfig = Get-Content -LiteralPath $dumpedConfigPath -Raw
    $dumpedBannerHash =
        (Get-FileHash -LiteralPath $dumpedBannerPath -Algorithm SHA256).Hash.ToLowerInvariant()
    $dumpedBannerHighDpiHash =
        (Get-FileHash -LiteralPath $dumpedBannerHighDpiPath -Algorithm SHA256).Hash.ToLowerInvariant()
    Assert-IfwContract `
        ($dumpedConfig.Installer.Banner -eq 'varinomics_banner_png' -and
            $dumpedBannerHash -eq $brandBannerHash -and
            $dumpedBannerHighDpiHash -eq $brandBannerHighDpiHash -and
            (Get-Content -LiteralPath $dumpedInstallScriptPath -Raw) -eq
                $installScript) `
        'the dumped component script, Banner setting, and embedded resources must match their exact source bodies'

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
    ([regex]::Matches(
        $installScript,
        'addElevatedOperation\s*\(\s*"CreateShortcut"').Count -eq 1) `
    'the Start Menu shortcut must use one elevated component operation'
Assert-IfwContract `
    ($installScript -match '@TargetDir@/vnm_terminal\.exe') `
    'the shortcut must target the portable launcher'
Assert-IfwContract `
    ($installScript -match 'installer\.value\s*\(\s*"StartMenuDir"\s*\)' -and
        $installScript -match `
            'installer\.value\s*\(\s*"UserStartMenuProgramsPath"\s*\)' -and
        $installScript -match `
            'installer\.value\s*\(\s*"AllUsersStartMenuProgramsPath"\s*\)' -and
        $installScript -notmatch `
            '@AllUsersStartMenuProgramsPath@\s*/\s*@StartMenuDir@') `
    'the final StartMenuDir group must be mapped once from an expected Programs root to the all-users root'
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
    ($logPathProbe -match '\[IO\.Directory\]::GetFiles\([\s\S]{0,40}?''InstallationLog-\*\.txt''\)' -and
        $logPathProbe -match '\$staleIsEmpty\s*=\s*\$staleStream\.Length\s*-eq\s*0' -and
        $logPathProbe -match '\[IO\.File\]::Delete\(\$staleLog\)') `
    'the helper must reclaim the empty files that cancelled runs leave behind'
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
    ($controllerScript -match ('Controller\.prototype\.maintenanceToolFileName\s*=\s*"' +
        [regex]::Escape($config.Installer.MaintenanceToolName) + '\.exe"')) `
    'the existing-installation probe must name the configured maintenance tool'
Assert-IfwContract `
    ($controllerScript -match 'TargetDirectoryPageCallback[\s\S]*?Controller\.prototype\.offerToRemoveExistingInstallation\s*\(\s*\)') `
    'choosing a directory that already holds an installation must offer to remove it'
Assert-IfwContract `
    ($controllerScript -match 'QMessageBox\.question\s*\(\s*"RemoveExistingInstallation"[\s\S]*?if\s*\(\s*answer\s*!=\s*QMessageBox\.Yes\s*\)\s*return\s*;') `
    'no installation may be removed without an explicit confirmation'
Assert-IfwContract `
    ($controllerScript -match '\[\s*"purge"\s*,\s*"--accept-messages"\s*,\s*"--confirm-command"\s*\]') `
    'the removal must be owned by a non-interactive framework maintenance-tool purge'
Assert-IfwContract `
    ($controllerScript -notmatch 'RemoveTargetDir' -and
        $controllerScript -notmatch 'performOperation\s*\(') `
    'the installer must not weaken target-directory validation or delete the previous installation itself'

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

Assert-IfwContract ($provisionScript -match '\$ifwVersion\s*=\s*''4\.11\.0''') `
    'the IFW tool version must be pinned to 4.11.0'
Assert-IfwContract `
    ($provisionScript -match 'c47201c4f6a82a8b607daa245237f40831d78425e904edd1514b71fd17efefc1') `
    'the official IFW archive checksum must remain pinned'
Assert-IfwContract `
    ($provisionScript -match 'https://download\.qt\.io/online/qtsdkrepository/windows_x86/ifw/' -and
        $provisionScript -match '4\.11\.0-0-202603231357ifw-win-x64\.7z') `
    'the provisioner must own the exact official IFW 4.11.0 archive URL'
Assert-IfwContract `
    ($provisionScript -match 'Get-Sha256FileHash\s+\$ArchivePath' -and
        $provisionScript -match '--fail' -and
        $provisionScript -match '--location' -and
        $provisionScript -match '--retry\s+4' -and
        $provisionScript -match '--max-time\s+600' -and
        $provisionScript -match '\$partPath\s*=\s*"\$Path\.part"') `
    'the provisioner must use BCL hashing and bounded curl retries through a part file'
Assert-IfwContract `
    ($provisionScript -match '\.staging-' -and
        $provisionScript -match '\.vnm-ifw-provisioned\.json' -and
        $provisionScript -notmatch 'RedirectStandardOutput' -and
        $provisionScript -match '\*>\s*\$extractionOutputPath') `
    'the provisioner must isolate extraction output and publish a marked staged root'
Assert-IfwContract `
    ($buildScript -notmatch 'Invoke-WebRequest|curl\.exe|ifwArchiveUrl|ifwArchiveSha256|Resolve-IfwRoot|Get-SevenZipPath|\.7z') `
    'the installer builder must not own IFW network, archive, cache, or extraction behavior'
Assert-IfwContract `
    ($buildScript -match '(?s)\[Parameter\(Mandatory\s*=\s*\$true\)\]\s*\[ValidateNotNullOrEmpty\(\)\]\s*\[string\]\$IfwRoot') `
    'the installer builder must require an explicit non-empty IFW root'
Assert-IfwContract `
    ($buildScript -match `
            '(?s)else\s*\{\s*if \(Test-PeHasCertificateTable \$artifactPath\)\s*\{\s*throw ''Unsigned artifact unexpectedly contains an Authenticode certificate table\.''' -and
        [regex]::Matches($buildScript, 'Get-AuthenticodeSignature').Count -eq 1 -and
        $buildScript -match '\$signature\.Status\s+-ne\s+''Valid''' -and
        $buildScript -match `
            '\$signature\.SignerCertificate\.Subject\s+-notmatch' -and
        $buildScript -match 'CN=Varinomics Ltd' -and
        $buildScript -match '\$signature\.TimeStamperCertificate') `
    'unsigned validation must inspect the PE certificate table while signed validation remains strict and timestamped'
Assert-IfwContract `
    ($windowsPackages -match 'if\s+"%IFW_ROOT%"==""' -and
        $windowsPackages -match '-IfwRoot\s+"%IFW_ROOT%"') `
    'the Windows package entry point must require and pass IFW_ROOT explicitly'
Assert-IfwContract ($buildConfigExample -match 'set IFW_ROOT=') `
    'the local build configuration example must document IFW_ROOT'
$jobLevelEnvironmentBlocks = [regex]::Matches(
    $windowsWorkflow,
    '(?m)^ {4}env:\s*\r?\n(?:(?:^ {6,}[^\r\n]*\r?\n)|(?:^\s*\r?\n))*')
Assert-IfwContract `
    (-not ($jobLevelEnvironmentBlocks.Value -match '\$\{\{\s*runner\.')) `
    'job-level workflow environment must not reference runner context'
$ifwPathInitializationIndex = $windowsWorkflow.IndexOf(
    '- name: Initialize Qt IFW paths')
$ifwCacheRestoreIndex = $windowsWorkflow.IndexOf(
    '- name: Restore verified Qt IFW archive')
Assert-IfwContract `
    ($ifwPathInitializationIndex -ge 0 -and
        $ifwCacheRestoreIndex -gt $ifwPathInitializationIndex -and
        $windowsWorkflow -match '\$ifwArchivePath\s*=\s*Join-Path\s+\$env:RUNNER_TEMP' -and
        $windowsWorkflow -match '\$ifwRoot\s*=\s*Join-Path\s+\$env:RUNNER_TEMP' -and
        $windowsWorkflow -match '"IFW_ARCHIVE_PATH=\$ifwArchivePath"[\s\S]*?\$env:GITHUB_ENV' -and
        $windowsWorkflow -match '"IFW_ROOT=\$ifwRoot"[\s\S]*?\$env:GITHUB_ENV') `
    'Windows CI must derive IFW paths on the runner before the cache step and export them'
$ifwCacheKeyPattern =
    '\$\{\{ runner\.os \}\}-qt-ifw-4\.11\.0-' +
    'c47201c4f6a82a8b607daa245237f40831d78425e904edd1514b71fd17efefc1'
Assert-IfwContract `
    ([regex]::Matches($windowsWorkflow, $ifwCacheKeyPattern).Count -eq 2 -and
        [regex]::Matches(
            $windowsWorkflow,
            'path:\s*\$\{\{\s*env\.IFW_ARCHIVE_PATH\s*\}\}').Count -eq 2) `
    'Windows CI restore and save steps must share the exact archive path and cache key'
Assert-IfwContract `
    ($windowsWorkflow -match 'actions/cache/restore@[0-9a-f]{40}' -and
        $windowsWorkflow -match 'actions/cache/save@[0-9a-f]{40}' -and
        $windowsWorkflow -match 'tools/provision_windows_ifw\.ps1' -and
        $windowsWorkflow -match 'set IFW_ROOT=\$env:IFW_ROOT') `
    'Windows CI must cache only the verified archive, reprovision IFW, and pass IFW_ROOT to packaging'
Assert-IfwContract `
    ($windowsWorkflow -match `
            'nuget\.exe install Microsoft\.ArtifactSigning\.Client' -and
        $windowsWorkflow -match `
            '-Version \$env:ARTIFACT_SIGNING_CLIENT_VERSION' -and
        $windowsWorkflow -match `
            '-Source https://api\.nuget\.org/v3/index\.json' -and
        $windowsWorkflow -match '-NoCache' -and
        $windowsWorkflow -match `
            'Microsoft\.ArtifactSigning\.Client\\bin\\x64\\Azure\.CodeSigning\.Dlib\.dll' -and
        $windowsWorkflow -notmatch 'Microsoft\.Trusted\.Signing\.Client') `
    'release signing must acquire the pinned current Artifact Signing client from nuget.org'
Assert-IfwContract `
    ($windowsWorkflow -match "'EnvironmentCredential'" -and
        $windowsWorkflow -match "'WorkloadIdentityCredential'" -and
        $windowsWorkflow -match "'AzurePowerShellCredential'" -and
        $windowsWorkflow -match "'AzureDeveloperCliCredential'" -and
        $windowsWorkflow -notmatch "'AzureCliCredential'") `
    'the signing dlib must be restricted to the Azure CLI credential established by azure/login'
Assert-IfwContract `
    ($windowsWorkflow -match `
        '-File tests/windows_ifw_installation_tests\.ps1[\s\S]*?-InstallerPath \$installer\.FullName' -and
        $installationTests -match `
            '\$env:GITHUB_ACTIONS\s+-eq\s+''true''' -and
        $installationTests -match `
            'WindowsBuiltInRole\]::Administrator' -and
        $installationTests -match `
            'StartMenuDir=\$startMenuGroup' -and
        $installationTests -match `
            '''AllUsers=true''' -and
        $installationTests -match `
            '\[Environment\+SpecialFolder\]::CommonPrograms' -and
        $installationTests -match `
            'vnm_terminal_runtime\\platforms\\qwindows\.dll' -and
        $installationTests -match `
            'Get-ShortcutTarget\s+\$shortcutPath' -and
        $installationTests -match `
            'Get-InstallationRegistrations\s+\$installRoot' -and
        $installationTests -match `
            '''purge''[\s\S]*?--confirm-command' -and
        $installationTests -match `
            'Test-Path -LiteralPath \$installRoot' -and
        $installationTests -match `
            'Test-Path -LiteralPath \$startMenuRoot') `
    'hosted Windows CI must commit an all-users installation, verify its shortcut and registration, run it, purge it, and check residue'
Assert-IfwContract `
    ($windowsWorkflow -match `
            '- name: Build signed portable archive[\s\S]*?Compress-Archive' -and
        $windowsWorkflow -match `
            'name: vnm-terminal-windows-x64-signed[\s\S]*?vnm_terminal_v\*_w64\.zip[\s\S]*?vnm_terminal_v\*_w64\.zip\.sha256' -and
        $windowsWorkflow -notmatch `
            '(?s)attach-release-packages:.*?Download unsigned Windows package artifacts' -and
        $windowsWorkflow -match `
            'dist/vnm_terminal_v\*_w64\.zip\.sha256') `
    'release attachment must publish the portable ZIP rebuilt from the signed payload'
Assert-IfwContract `
    ($windowsWorkflow -match `
            '-File tests/windows_ifw_installation_tests\.ps1[\s\S]*?-InstallerPath \$installer\.FullName[\s\S]*?-RequireSigned' -and
        $installationTests -match '\[switch\]\$RequireSigned' -and
        $installationTests -match `
            '\$acceptedStatuses\s*=\s*if \(\$RequireSigned\)\s*\{\s*@\(''Valid''\)') `
    'the release lifecycle must reject unsigned installed executables'
Assert-IfwContract `
    ($installationTests -match 'Assert-PeGuiSubsystem \$resolvedInstallerPath' -and
        $installationTests -match 'Assert-PeGuiSubsystem \$maintenancePath') `
    'the lifecycle gate must prove that neither delivered binary makes Windows allocate a console'
Assert-IfwContract ($buildScript -match '--offline-only') `
    'binarycreator must force offline-only behavior'
Assert-IfwContract `
    ($buildScript -match 'artifactSuffix\s*=\s*if\s*\(\$signingEnabled\)') `
    'unsigned artifacts must be distinguished in their filename'

$payloadSigningIndex = $buildScript.IndexOf(
    "Invoke-TrustedSigning (Join-Path `$PayloadPath 'vnm_terminal.exe')")
$payloadCopyIndex = $buildScript.IndexOf(
    "Copy-Item -Path (Join-Path `$PayloadPath '*')")
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
    'the public portable payload must be signed when release signing is enabled'
Assert-IfwContract ($payloadCopyIndex -gt $payloadSigningIndex) `
    'the signed portable payload must be the source copied into the installer'
Assert-IfwContract ($installerBaseSigningIndex -gt $payloadCopyIndex) `
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
$privateBaseSubsystemIndex = $buildScript.IndexOf(
    'Set-PeGuiSubsystem $privateInstallerBasePath')
$artifactSubsystemIndex = $buildScript.IndexOf('Set-PeGuiSubsystem $artifactPath')
Assert-IfwContract `
    ($privateBaseSubsystemIndex -ge 0 -and
        $privateBaseSubsystemIndex -lt $installerBaseSigningIndex -and
        $artifactSubsystemIndex -gt $binaryCreatorIndex -and
        $artifactSubsystemIndex -lt $finalSigningIndex) `
    'both delivered binaries must be republished as graphical images before they are signed'
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
$lockedEmptyLog = $null
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

    $staleEmptyLog = Join-Path $validLogDirectory (
        'InstallationLog-{0}.txt' -f [Guid]::NewGuid().ToString('N'))
    $staleContentLog = Join-Path $validLogDirectory (
        'InstallationLog-{0}.txt' -f [Guid]::NewGuid().ToString('N'))
    $runningEmptyLog = Join-Path $validLogDirectory (
        'InstallationLog-{0}.txt' -f [Guid]::NewGuid().ToString('N'))
    [IO.File]::WriteAllBytes($staleEmptyLog, [byte[]]::new(0))
    [IO.File]::WriteAllText($staleContentLog, 'diagnostic log content')
    [IO.File]::WriteAllBytes($runningEmptyLog, [byte[]]::new(0))
    $lockedEmptyLog = [IO.File]::Open(
        $runningEmptyLog,
        [IO.FileMode]::Open,
        [IO.FileAccess]::ReadWrite,
        [IO.FileShare]::None)

    $validResult = Invoke-LogPathProbe $validCandidate
    Assert-IfwContract (-not (Test-Path -LiteralPath $staleEmptyLog)) `
        'the helper must reclaim an unlocked empty log left by a cancelled run'
    Assert-IfwContract `
        ((Test-Path -LiteralPath $staleContentLog -PathType Leaf) -and
            (Get-Item -LiteralPath $staleContentLog).Length -gt 0) `
        'the helper must keep a log that already carries diagnostic content'
    Assert-IfwContract (Test-Path -LiteralPath $runningEmptyLog -PathType Leaf) `
        'the helper must keep an empty log that a running installer still holds'
    $lockedEmptyLog.Dispose()
    $lockedEmptyLog = $null
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
    if ($null -ne $lockedEmptyLog) {
        $lockedEmptyLog.Dispose()
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
