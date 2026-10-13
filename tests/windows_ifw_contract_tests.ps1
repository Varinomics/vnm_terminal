[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$SourceRoot,

    [string]$ArtifactPath,

    [string]$DumpPath,

    [string]$IfwRoot
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

function Assert-IfwControllerRuntime {
    param(
        [Parameter(Mandatory = $true)]
        [string]$ControllerScriptPath
    )

    $node = Get-Command node.exe -ErrorAction SilentlyContinue
    Assert-IfwContract ($null -ne $node) 'the controller contract requires node.exe'

    $harnessPath = [IO.Path]::Combine(
        [IO.Path]::GetTempPath(),
        "vnm-terminal-ifw-controller-$([Guid]::NewGuid().ToString('N')).js")
    try {
        $harness = @'
const fs = require("fs");
const controllerScript = fs.readFileSync(process.argv[2], "utf8");
const powershellPath =
    "C:\\Windows\\System32\\WindowsPowerShell\\v1.0\\powershell.exe";
let state = null;

global.QInstaller = {
    ComponentSelection: 1,
    Success: 0,
    Failure: 1,
    Canceled: 3,
    Unfinished: 4,
};
global.QMessageBox = {
    Yes: 0x00004000,
    No: 0x00010000,
    question(identifier, title, text, buttons, defaultButton) {
        state.questions.push({
            identifier,
            title,
            text,
            buttons,
            defaultButton,
        });
        return state.answer;
    },
    critical(identifier, title, text) {
        state.errors.push({ identifier, title, text });
        return 0x00000400;
    },
};
global.installer = {
    status: QInstaller.Success,
    isInstaller() { return true; },
    isOfflineGenerator() { return false; },
    isPackageViewer() { return false; },
    isProcessRunning() { return false; },
    aboutCalculateComponentsToInstall: { connect() {} },
    aboutCalculateComponentsToUninstall: { connect() {} },
    hasAdminRights() { return state.isAdmin; },
    setDefaultPageVisible(page) {
        state.hiddenPages.push(page);
    },
    setValue(name, value) {
        state.assignedValues.push({ name, value });
    },
    readFile() { return state.logProbeScript; },
    toNativeSeparators(value) { return value.replace(/\//g, "\\"); },
    value(name) {
        if (Object.prototype.hasOwnProperty.call(state.values, name))
            return state.values[name];
        throw new Error("unexpected installer value: " + name);
    },
    fileExists(candidate) {
        return candidate === powershellPath ||
            state.maintenanceToolPaths.indexOf(candidate) >= 0;
    },
    execute(program, args, stdIn, stdInCodec, stdOutCodec) {
        const execution = {
            program,
            args,
            stdIn,
            stdInCodec,
            stdOutCodec,
            argumentCount: arguments.length,
        };
        state.executions.push(execution);
        if (program !== powershellPath)
            throw new Error("unexpected synchronous execution: " + program);
        if (args[4] === "-") {
            if (!state.logProbeStarts) return [];
            const outputEncoding = stdOutCodec === "UTF-8"
                ? "utf8"
                : "latin1";
            const output = Buffer.from(
                state.logProbeOutput, "utf8").toString(outputEncoding);
            return [output, state.logProbeExitCode];
        }
        throw new Error("unexpected PowerShell command");
    },
};
global.gui = {
    rejectWithoutPrompt() { state.rejections += 1; },
    pageWidgetByObjectName(name) {
        if (!Object.prototype.hasOwnProperty.call(state.pages, name))
            throw new Error("unexpected page lookup: " + name);
        return state.pages[name];
    },
    findChild(parent, name) {
        if (parent !== state.pages.ReadyForInstallationPage ||
            name !== "InstallComponentsTreeview")
        {
            throw new Error("unexpected recursive child lookup");
        }
        return {
            hideColumn(index) { state.hiddenSummaryColumns.push(index); },
        };
    },
};

function labelStub() {
    return {
        text: "framework message",
        setText(value) { this.text = value; },
    };
}

eval(controllerScript);

function run(overrides) {
    overrides = overrides || {};
    const directory = overrides.directory ||
        "C:/Program Files/vnm_terminal";
    const nativeDirectory = directory.replace(/\//g, "\\");
    const installationPresent = overrides.installationPresent !== false;
    const maintenanceToolPath =
        nativeDirectory + "\\vnm_terminal_maintenance.exe";
    state = Object.assign({
        directory,
        nativeDirectory,
        maintenanceToolPath,
        maintenanceToolPaths: installationPresent
            ? [maintenanceToolPath]
            : [],
        logProbeScript: "",
        logProbeOutput: "",
        logProbeStarts: true,
        logProbeExitCode: 0,
        isAdmin: false,
        answer: QMessageBox.Yes,
        questions: [],
        errors: [],
        executions: [],
        assignedValues: [],
        hiddenPages: [],
        hiddenSummaryColumns: [],
        rejections: 0,
        pages: {
            IntroductionPage: {
                title: "",
                subTitle: "",
                MessageLabel: labelStub(),
            },
            TargetDirectoryPage: {
                subTitle: "",
                TargetDirectoryLineEdit: { text: nativeDirectory },
                MessageLabel: labelStub(),
            },
            LicenseAgreementPage: { subTitle: "" },
            ReadyForInstallationPage: { subTitle: "" },
            FinishedPage: {
                title: "",
                subTitle: "",
                MessageLabel: labelStub(),
                RunItCheckBox: { hide() {} },
            },
        },
    }, overrides);
    state.values = Object.assign({
        TargetDir: directory,
        RootDir: "C:\\",
    }, overrides.values || {});

    new Controller();
    Controller.prototype.IntroductionPageCallback();
    return state;
}

let result = run({ installationPresent: false });
Controller.prototype.TargetDirectoryPageCallback();
Controller.prototype.ReadyForInstallationPageCallback();
if (result.hiddenPages.join(",") !== String(QInstaller.ComponentSelection) ||
    result.questions.length !== 0 ||
    result.rejections !== 0 ||
    result.pages.ReadyForInstallationPage.subTitle.indexOf("Review") < 0 ||
    result.hiddenSummaryColumns.join(",") !== "5")
{
    throw new Error("a fresh install must retain its ordinary flow");
}

const uncLogPath =
    "\\\\server\\share\\InstallationLog-c8d20e8ad6754f65b69ac93b02c39b48.txt";
result = run({
    installationPresent: false,
    logProbeScript: "writable-path probe",
    logProbeOutput: uncLogPath + "\r\n",
});
const logAssignments = result.assignedValues.filter(
    (assignment) => assignment.name === "LogFileName");
if (logAssignments.length !== 2 || logAssignments[1].value !== uncLogPath)
    throw new Error("the UTF-8 log probe and UNC result must remain intact");
'@
        [IO.File]::WriteAllText(
            $harnessPath,
            $harness,
            [Text.UTF8Encoding]::new($false))
        & $node.Source $harnessPath $ControllerScriptPath
        Assert-IfwContract ($LASTEXITCODE -eq 0) 'the controller runtime contract must pass'
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

$resolvedSourceRoot = (Resolve-Path -LiteralPath $SourceRoot).Path
$ifwSourceRoot = Join-Path $resolvedSourceRoot 'packaging\windows\ifw'
$configPath = Join-Path $ifwSourceRoot 'config.xml.in'
$styleSheetPath = Join-Path $ifwSourceRoot 'style.qss'
$controllerScriptPath = Join-Path $ifwSourceRoot 'controller.qs'
$brandLogoPath = Join-Path $ifwSourceRoot 'varinomics_logo.png'
$brandLogoHighDpiPath = Join-Path $ifwSourceRoot 'varinomics_logo@2x.png'
$brandProvenancePath = Join-Path $ifwSourceRoot 'brand_assets.provenance.json'
$logPathProbePath = Join-Path $ifwSourceRoot 'log_path_probe.ps1'
$themeResourcesPath = Join-Path $ifwSourceRoot 'theme_resources.qrc'
$packagePath = Join-Path $ifwSourceRoot 'package.xml.in'
$installScriptPath = Join-Path $ifwSourceRoot 'installscript.qs'
$maintenancePackagePath = Join-Path $ifwSourceRoot 'maintenance_package.xml.in'
$maintenanceInstallScriptPath = Join-Path $ifwSourceRoot 'maintenance_installscript.qs'
$buildScriptPath = Join-Path $resolvedSourceRoot 'tools\build_windows_ifw_installer.ps1'
$nativeBuildScriptPath = Join-Path $resolvedSourceRoot 'tools\build_windows_private_ifw.ps1'
$windowsWorkflowPath = Join-Path $resolvedSourceRoot '.github\workflows\ci-windows.yml'
$installationTestsPath = Join-Path `
    $resolvedSourceRoot 'tests\windows_ifw_installation_tests.ps1'
$windowsPackagesPath = Join-Path $resolvedSourceRoot 'build_windows_packages.bat'
$buildConfigExamplePath = Join-Path $resolvedSourceRoot 'build_config.bat.example'
$noticesPath = Join-Path $resolvedSourceRoot 'THIRD_PARTY_NOTICES.md'

[xml]$config = Get-Content -Raw -LiteralPath $configPath
$styleSheet = Get-Content -Raw -LiteralPath $styleSheetPath
$controllerScript = Get-Content -Raw -LiteralPath $controllerScriptPath
$brandProvenance = Get-Content -Raw -LiteralPath $brandProvenancePath |
    ConvertFrom-Json
$logPathProbe = Get-Content -Raw -LiteralPath $logPathProbePath
[xml]$themeResources = Get-Content -Raw -LiteralPath $themeResourcesPath
[xml]$package = Get-Content -Raw -LiteralPath $packagePath
[xml]$maintenancePackage = Get-Content -Raw -LiteralPath $maintenancePackagePath
$installScript = Get-Content -Raw -LiteralPath $installScriptPath
$maintenanceInstallScript = Get-Content -Raw -LiteralPath $maintenanceInstallScriptPath
$buildScript = Get-Content -Raw -LiteralPath $buildScriptPath
$windowsWorkflow = Get-Content -Raw -LiteralPath $windowsWorkflowPath
$windowsPackages = Get-Content -Raw -LiteralPath $windowsPackagesPath
$buildConfigExample = Get-Content -Raw -LiteralPath $buildConfigExamplePath
$notices = Get-Content -Raw -LiteralPath $noticesPath
$installationTests = Get-Content -Raw -LiteralPath $installationTestsPath

Assert-IfwControllerRuntime $controllerScriptPath
& node.exe (Join-Path $PSScriptRoot 'windows_ifw_maintenance_tests.js')
Assert-IfwContract ($LASTEXITCODE -eq 0) 'the maintenance base must be selected before the disk-space estimate'
& (Join-Path $PSScriptRoot 'windows_ifw_replacement_tests.ps1')
Assert-IfwHashRuntime $buildScriptPath 'the IFW build script'
Assert-IfwCertificateTableRuntime $buildScriptPath
Assert-IfwGuiSubsystemRuntime $buildScriptPath
Assert-IfwSignedValidationRuntime $buildScriptPath
Assert-IfwPowerShellParses $nativeBuildScriptPath 'the private IFW build script'
Assert-IfwPowerShellParses `
    $installationTestsPath 'the generated-installer lifecycle test'
if ($IfwRoot) {
    & (Join-Path $resolvedSourceRoot 'tools\assert_windows_private_ifw.ps1') -IfwRoot $IfwRoot
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
Assert-IfwContract ($config.Installer.Logo -eq 'varinomics_logo.png') `
    'the Modern header must use the adopted IFW logo pixmap hook'
Assert-IfwContract `
    ($config.Installer.WizardDefaultWidth -eq '800' -and
        $config.Installer.WizardMinimumWidth -eq '800' -and
        $config.Installer.WizardDefaultHeight -eq '500' -and
        $config.Installer.WizardMinimumHeight -eq '500') `
    'the wizard must use the adopted 800 by 500 default and minimum size'
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
    ($styleSheet -match 'QCheckBox::indicator:checked\s*\{[^}]*image:\s*url\(:/metadata/installer-theme/checkbox_check\.png\);[^}]*background-color:\s*#8AB4C7') `
    'checked checkboxes must use the embedded check glyph and explicit fill'
Assert-IfwContract `
    ($styleSheet -match 'QRadioButton::indicator:checked\s*\{[^}]*image:\s*url\(:/metadata/installer-theme/radio_dot\.png\);[^}]*background-color:\s*#8AB4C7') `
    'checked radio buttons must use the embedded dot glyph and explicit fill'
Assert-IfwContract `
    ($styleSheet -match 'QCheckBox::indicator:unchecked:disabled,[^\{]*\{[^}]*background-color:\s*#1F1F1F;[^}]*border-color:\s*#3A3A3A') `
    'disabled unchecked indicators must remain distinguishable'
Assert-IfwContract `
    ($styleSheet -match 'QPushButton:disabled\s*\{[^}]*color:\s*#828282') `
    'disabled wizard buttons must remain legible'
Assert-IfwContract `
    ($styleSheet -match 'QMessageBox\s*\{[^}]*color:\s*#E0E0E0;[^}]*background-color:\s*#111111' -and
        $styleSheet -match 'QMessageBox QLabel#qt_msgbox_label,[\s\S]*?\{[^}]*color:\s*#E0E0E0;[^}]*background-color:\s*transparent' -and
        $styleSheet -match 'QMessageBox QPushButton\s*\{[^}]*color:\s*#F0F0F0;[^}]*background-color:\s*#363636;[^}]*border:\s*1px solid #666666' -and
        $styleSheet -match 'QMessageBox QPushButton:hover\s*\{[^}]*background-color:\s*#444444' -and
        $styleSheet -match 'QMessageBox QPushButton:pressed\s*\{[^}]*background-color:\s*#2D2D2D' -and
        $styleSheet -match 'QMessageBox QPushButton:disabled\s*\{[^}]*color:\s*#828282') `
    'installer-owned message boxes must use the adopted Soter text and button states'
Assert-IfwContract `
    (@($themeResources.RCC.qresource |
        Where-Object { $_.prefix -eq '/installer-theme' }).Count -eq 1) `
    'the indicator resource collection must use the stylesheet resource prefix'
$themeResourceFiles = @($themeResources.RCC.qresource |
    Where-Object { $_.prefix -eq '/installer-theme' } |
    ForEach-Object { @($_.file) })
$expectedThemeFiles = @(
    'checkbox_check.png', 'checkbox_check@2x.png',
    'radio_dot.png', 'radio_dot@2x.png',
    'combo_arrow.png', 'combo_arrow@2x.png',
    'log_path_probe.ps1', 'replacement_support.ps1'
)
Assert-IfwContract ($themeResourceFiles.Count -eq $expectedThemeFiles.Count) `
    'the resource collection must contain the adopted control densities and controller helpers'
foreach ($resource in $expectedThemeFiles) {
    Assert-IfwContract ($themeResourceFiles -contains $resource) `
        "the resource collection must expose $resource"
}
Assert-IfwContract `
    (@($themeResources.RCC.qresource).Count -eq 2) `
    'the custom resource manifest must contain only the theme collection and proven high-DPI logo sibling'
$highDpiLogoResource = @($themeResources.RCC.qresource |
    Where-Object { $_.prefix -eq '/installer-config' } |
    ForEach-Object { @($_.file) })
Assert-IfwContract `
    ($highDpiLogoResource.Count -eq 1 -and
        $highDpiLogoResource[0].alias -eq 'varinomics_logo_png@2x.' -and
        $highDpiLogoResource[0].'compression-algorithm' -eq 'none' -and
        $highDpiLogoResource[0].'#text' -eq 'varinomics_logo@2x.png') `
    'the XML QRC must expose the exact high-DPI sibling path proven against IFW 4.11'
Assert-IfwContract `
    ($styleSheet -notmatch ':/installer-theme/' -and
        $controllerScript -notmatch ':/installer-theme/') `
    'custom resources must be consumed below the IFW runtime metadata mount root'

$brandLogoHash =
    (Get-FileHash -LiteralPath $brandLogoPath -Algorithm SHA256).Hash.ToLowerInvariant()
$brandLogoHighDpiHash =
    (Get-FileHash -LiteralPath $brandLogoHighDpiPath -Algorithm SHA256).Hash.ToLowerInvariant()
Assert-IfwContract `
    ($brandProvenance.schema_version -eq 1 -and
        $brandProvenance.source_repository -eq 'https://github.com/Varinomics/website' -and
        $brandProvenance.source_path -eq 'public/logo-dark.png' -and
        $brandProvenance.source_sha256 -eq '35d11678fa347e37f29b85bf0bbe4a0f21b89011f1406a61c7bca4098e6232c7') `
    'logo provenance must identify the canonical website artwork'
Assert-IfwContract `
    ($brandLogoHash -eq 'c2bad1ff7612bb35996fcdf3a5a943ecd275f4c6bf9dd99031f22797d48013e7' -and
        $brandLogoHighDpiHash -eq 'f6926e2d6fd95ef8636a9f990bf6b7a6d1245d738be387eb4b384387fae736db' -and
        $brandProvenance.outputs_sha256.'varinomics_logo.png' -eq $brandLogoHash -and
        $brandProvenance.outputs_sha256.'varinomics_logo@2x.png' -eq $brandLogoHighDpiHash) `
    'both logo densities must match the adopted Soter artwork and provenance'
foreach ($asset in @(
    @('varinomics_logo.png', '246x34'), @('varinomics_logo@2x.png', '492x68'),
    @('checkbox_check.png', '16x16'), @('checkbox_check@2x.png', '32x32'),
    @('radio_dot.png', '10x10'), @('radio_dot@2x.png', '20x20'),
    @('combo_arrow.png', '10x6'), @('combo_arrow@2x.png', '20x12')
)) {
    $dimensions = Get-IfwPngDimensions (Join-Path $ifwSourceRoot $asset[0])
    Assert-IfwContract (($dimensions -join 'x') -eq $asset[1]) `
        "$($asset[0]) must decode at its adopted density dimensions"
}

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

    # The SDK's recursive dump omits some separately mounted resource segments.
    # CLI search loads a control script without a GUI or installation, so it can
    # read those exact resources without devtool's privileged logging fallback.
    $resourceTestRoot = Join-Path ([IO.Path]::GetTempPath()) (
        'vnm-ifw-resources-' + [Guid]::NewGuid().ToString('N'))
    $inspectionTarget = Join-Path $resourceTestRoot 'uncreated-target'
    $inspectionScriptPath = Join-Path $resourceTestRoot 'inspect-resources.qs'
    $inspectionCache = Join-Path $resourceTestRoot 'cache'
    [void](New-Item -ItemType Directory -Path $resourceTestRoot)
    $inspectionProcess = $null
    try {
        $inspectionScript = @'
function Controller()
{
    installer.setValue("LogFileName", "\\\\.\\NUL");
    installer.setValue("TargetDir", @TARGET@);
    installer.autoRejectMessageBoxes();
    var resources = @RESOURCES@;
    for (var index = 0; index < resources.length; ++index) {
        var name = resources[index];
        if (!installer.performOperation("Copy", [
                ":/metadata/installer-theme/" + name, @DESTINATION@ + "/" + name]))
        {
            throw new Error("Cannot extract installer resource: " + name);
        }
    }
    console.log("VNM_IFW_RESOURCES_COMPLETE");
}
'@
        $inspectionScript = $inspectionScript.Replace(
            '@TARGET@', (ConvertTo-Json -InputObject $inspectionTarget -Compress)).Replace(
            '@RESOURCES@', (ConvertTo-Json -InputObject $expectedThemeFiles -Compress)).Replace(
            '@DESTINATION@', (ConvertTo-Json -InputObject $resourceTestRoot -Compress))
        [IO.File]::WriteAllText($inspectionScriptPath, $inspectionScript, [Text.UTF8Encoding]::new($false))
        $inspectionStart = [Diagnostics.ProcessStartInfo]::new()
        $inspectionStart.FileName = $resolvedArtifactPath
        $inspectionStart.Arguments = 'search --verbose --script "' + $inspectionScriptPath +
            '" --cache-path "' + $inspectionCache + '" "TargetDir=' + $inspectionTarget +
            '" "LogFileName=\\.\NUL"'
        $inspectionStart.UseShellExecute = $false
        $inspectionStart.CreateNoWindow = $true
        $inspectionStart.RedirectStandardOutput = $true
        $inspectionStart.RedirectStandardError = $true
        $inspectionStart.EnvironmentVariables['QT_FORCE_STDERR_LOGGING'] = '1'
        $inspectionProcess = [Diagnostics.Process]::Start($inspectionStart)
        $inspectionOutput = $inspectionProcess.StandardOutput.ReadToEndAsync()
        $inspectionErrors = $inspectionProcess.StandardError.ReadToEndAsync()
        if (-not $inspectionProcess.WaitForExit(45000)) {
            $inspectionProcess.Kill()
            $inspectionProcess.WaitForExit()
            throw 'The read-only installer resource inspector did not exit within 45 seconds.'
        }
        $inspectionDiagnostics = $inspectionOutput.Result + $inspectionErrors.Result
        Assert-IfwContract `
            ($inspectionProcess.ExitCode -eq 0 -and
                $inspectionDiagnostics -match 'VNM_IFW_RESOURCES_COMPLETE') `
            "the read-only resource inspector must finish successfully: $inspectionDiagnostics"
        Assert-IfwContract (-not (Test-Path -LiteralPath $inspectionTarget)) `
            'resource inspection must not create an installation target'
        foreach ($resource in $expectedThemeFiles) {
            $extractedResource = Join-Path $resourceTestRoot $resource
            Assert-IfwContract (Test-Path -LiteralPath $extractedResource -PathType Leaf) `
                "the installer must embed $resource"
            Assert-IfwContract `
                ((Get-FileHash -LiteralPath $extractedResource -Algorithm SHA256).Hash -eq
                    (Get-FileHash -LiteralPath (Join-Path $ifwSourceRoot $resource) -Algorithm SHA256).Hash) `
                "the embedded $resource must match its source"
        }
    }
    finally {
        if ($null -ne $inspectionProcess) {
            $inspectionProcess.Dispose()
        }
        $resolvedResourceTestRoot = [IO.Path]::GetFullPath($resourceTestRoot)
        $temporaryRootPrefix = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
        if (-not $resolvedResourceTestRoot.StartsWith($temporaryRootPrefix, [StringComparison]::OrdinalIgnoreCase)) {
            throw 'Refusing cleanup outside the owned resource-inspection directory.'
        }
        Remove-Item -LiteralPath $resolvedResourceTestRoot -Recurse -Force
    }
}

if ($DumpPath) {
    Assert-IfwContract (-not [string]::IsNullOrWhiteSpace($ArtifactPath)) `
        'artifact-level resource checks require the dumped artifact path'
    $resolvedDumpPath = (Resolve-Path -LiteralPath $DumpPath).Path
    $dumpedConfigPath = Join-Path $resolvedDumpPath 'metadata\installer-config\config.xml'
    $dumpedLogoPath =
        Join-Path $resolvedDumpPath 'metadata\installer-config\varinomics_logo_png'
    $dumpedLogoHighDpiPath =
        Join-Path $resolvedDumpPath 'metadata\installer-config\varinomics_logo_png@2x'
    $dumpedInstallScriptPath = Join-Path `
        $resolvedDumpPath 'metadata\com.varinomics.vnm_terminal\installscript.qs'
    Assert-IfwContract `
        ((Test-Path -LiteralPath $dumpedConfigPath -PathType Leaf) -and
            (Test-Path -LiteralPath $dumpedLogoPath -PathType Leaf) -and
            (Test-Path -LiteralPath $dumpedLogoHighDpiPath -PathType Leaf) -and
            (Test-Path -LiteralPath $dumpedInstallScriptPath -PathType Leaf)) `
        'devtool dump must materialize the IFW config, component script, and both logo density paths'

    [xml]$dumpedConfig = Get-Content -LiteralPath $dumpedConfigPath -Raw
    $dumpedLogoHash =
        (Get-FileHash -LiteralPath $dumpedLogoPath -Algorithm SHA256).Hash.ToLowerInvariant()
    $dumpedLogoHighDpiHash =
        (Get-FileHash -LiteralPath $dumpedLogoHighDpiPath -Algorithm SHA256).Hash.ToLowerInvariant()
    Assert-IfwContract `
        ($dumpedConfig.Installer.Logo -eq 'varinomics_logo_png' -and
            $dumpedLogoHash -eq $brandLogoHash -and
            $dumpedLogoHighDpiHash -eq $brandLogoHighDpiHash -and
            (Get-Content -LiteralPath $dumpedInstallScriptPath -Raw) -eq
                $installScript) `
        'the dumped component script, logo setting, and embedded resources must match their exact source bodies'

    $dumpedLogoDimensions = Get-IfwPngDimensions $dumpedLogoPath
    $dumpedLogoHighDpiDimensions = Get-IfwPngDimensions $dumpedLogoHighDpiPath
    Assert-IfwContract `
        (($dumpedLogoDimensions -join 'x') -eq '246x34' -and
            ($dumpedLogoHighDpiDimensions -join 'x') -eq '492x68') `
        'both extensionless dumped logo resources must decode at their intended density dimensions'
}

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
    ($null -eq $package.Package.UserInterfaces -and
        $buildScript -notmatch 'replacement_commit\.ui') `
    'the package must not retain the superseded replacement page'

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
    ($installScript -notmatch 'setDefaultPageVisible|ComponentSelection|hideColumn|isCommandLineInstance|addWizardPage|setValidatorForCustomPage') `
    'the component script must leave fresh GUI and headless page lifecycles to IFW'
Assert-IfwContract `
    ($controllerScript -match 'function\s+Controller\s*\(\s*\)\s*\{[\s\S]*?if\s*\(installer\.isInstaller\(\)\)[\s\S]*?setDefaultPageVisible\s*\(\s*QInstaller\.ComponentSelection\s*,\s*false\s*\)') `
    'the pre-display Controller constructor must skip the single forced component page during initial installation'
Assert-IfwContract `
    ((([regex]::Matches(
            $controllerScript,
            'setDefaultPageVisible\s*\(\s*QInstaller\.(?<page>\w+)\s*,\s*false\s*\)') |
        ForEach-Object { $_.Groups['page'].Value }) -join ',') -eq
        'ComponentSelection' -and
        [regex]::Matches($controllerScript, 'setDefaultPageVisible\s*\(').Count -eq 1) `
    'the controller must hide only the forced component page'
Assert-IfwContract `
    ($controllerScript -notmatch 'isUpdater\(\)[\s\S]*?setDefaultPageVisible\s*\(\s*QInstaller\.ComponentSelection' -and
        $controllerScript -notmatch 'isPackageManager\(\)[\s\S]*?setDefaultPageVisible\s*\(\s*QInstaller\.ComponentSelection' -and
        $controllerScript -notmatch 'isUninstaller\(\)[\s\S]*?setDefaultPageVisible\s*\(\s*QInstaller\.ComponentSelection') `
    'maintenance, updater, package-manager, and uninstaller page behavior must remain unchanged'
Assert-IfwContract `
    ($controllerScript -notmatch 'addWizardPage|addWizardPageItem|removeWizardPage' -and
        $controllerScript -notmatch 'IntroductionPageCallback[\s\S]*?setDefaultPageVisible' -and
        $controllerScript -notmatch 'FinishedPageCallback[\s\S]*?setDefaultPageVisible') `
    'the controller must not add pages or mutate built-in visibility after the stable first-frame page list is built'
Assert-IfwContract `
    ($controllerScript -match 'Controller\.prototype\.IntroductionPageCallback\s*=\s*function\s*\(\s*\)\s*\{\s*if\s*\(\s*!installer\.isInstaller\(\)\s*\)\s*return\s*;[\s\S]{0,1200}?var\s+introductionPage\s*=\s*gui\.pageWidgetByObjectName\s*\(\s*"IntroductionPage"\s*\)' -and
        $controllerScript -match 'introductionPage\.MessageLabel\.setText' -and
        $controllerScript -notmatch '<img\s') `
    'initial-install branding must guard before touching the existing introduction page'
$installerPageSubtitles = @(
    @('IntroductionPageCallback', 'IntroductionPage', 'introductionPage',
        'vnm_terminal installation.'),
    @('TargetDirectoryPageCallback', 'TargetDirectoryPage', 'targetDirectoryPage',
        'Choose where vnm_terminal will be installed.'),
    @('LicenseAgreementPageCallback', 'LicenseAgreementPage', 'licenseAgreementPage',
        'Review and accept the license to continue.'),
    @('StartMenuDirectoryPageCallback', 'StartMenuDirectoryPage', 'startMenuDirectoryPage',
        'Choose where Start Menu shortcuts will appear.'),
    @('ReadyForInstallationPageCallback', 'ReadyForInstallationPage', 'summaryPage',
        'Review your choices before installation.')
)
foreach ($subtitleContract in $installerPageSubtitles) {
    $callbackName, $objectName, $variableName, $subtitle = $subtitleContract
    $callbackPattern =
        'Controller\.prototype\.' + [regex]::Escape($callbackName) +
        '\s*=\s*function\s*\(\s*\)\s*\{\s*' +
        'if\s*\(\s*!installer\.isInstaller\(\)\s*\)\s*return\s*;\s*' +
        '[\s\S]{0,1200}?' +
        'var\s+' + [regex]::Escape($variableName) +
        '\s*=\s*gui\.pageWidgetByObjectName\s*\(\s*"' +
        [regex]::Escape($objectName) + '"\s*\);[\s\S]*?' +
        [regex]::Escape($variableName) +
        '\.subTitle\s*=\s*(?:[\s\S]{0,240}?\?[\s\S]{0,240}?:\s*)?"' +
        [regex]::Escape($subtitle) + '";'
    Assert-IfwContract ($controllerScript -match $callbackPattern) `
        "$objectName must receive its concise subtitle after an immediate installer-only guard"
}
Assert-IfwContract `
    ($controllerScript -match 'Controller\.prototype\.PerformInstallationPageCallback\s*=\s*function\s*\(\s*\)\s*\{[\s\S]{0,800}?if\s*\(\s*!installer\.isInstaller\(\)\s*\)\s*return\s*;[\s\S]{0,240}?performInstallationPage\.subTitle\s*=\s*"Installing vnm_terminal\. Please wait\.";') `
    'the shared progress page must limit its installation subtitle to installer mode'
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
    $controllerScript,
    $installScript
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
    ($controllerScript -notmatch '<img\s' -and
        $config.Installer.Logo -eq 'varinomics_logo.png') `
    'the canonical wordmark must appear only in the Modern logo and no image may rely on QTextDocument resource lookup'
Assert-IfwContract `
    ($controllerScript -match 'Controller\.prototype\.FinishedPageCallback\s*=\s*function\s*\(\s*\)\s*\{\s*if\s*\(\s*!installer\.isInstaller\(\)\s*\)\s*return\s*;[\s\S]{0,300}?var\s+finishedPage\s*=\s*gui\.pageWidgetByObjectName\s*\(\s*"FinishedPage"\s*\)') `
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
    ($controllerScript -notmatch 'TargetDirectoryLineEdit[\s\S]{0,80}?\.connect') `
    'later target edits must remain owned by IFW validation'
Assert-IfwContract `
    ($controllerScript -match 'gui\.findChild\s*\(\s*summaryPage\s*,\s*"InstallComponentsTreeview"\s*\)[\s\S]*?installComponentsTreeview\.hideColumn\s*\(\s*5\s*\)' -and
        $controllerScript -notmatch 'summaryPage\.InstallComponentsTreeview' -and
        $installScript -notmatch 'DynamicReplacementCommitPageCallback') `
    'the restored Ready page must hide only the ambiguous subtotal through recursive child lookup'
Assert-IfwContract `
    ($controllerScript -notmatch 'pageWidgetByObjectName\s*\(\s*"(?:SpaceItem|SpaceWidget)"' -and
        $controllerScript -notmatch '\.(?:SpaceItem|SpaceWidget)\.') `
    'the labeled total required-space widget must remain visible'

Assert-IfwContract `
    ($controllerScript -match ('Controller\.prototype\.maintenanceToolFileName\s*=\s*"' +
        [regex]::Escape($config.Installer.MaintenanceToolName) + '\.exe"')) `
    'the existing-installation probe must name the configured maintenance tool'
Assert-IfwContract `
    ($controllerScript -notmatch 'registeredInstallationDirectories|detectExistingInstallations|VNM_INSTALL:|CurrentVersion\\\\Uninstall' -and
        $controllerScript -match 'TargetDirectoryPageCallback[\s\S]{0,1600}?offerSelectedInstallationReplacement\s*\(\s*\)') `
    'existing-installation detection must inspect only the current folder after its page is initialized'
Assert-IfwContract `
    ($installScript -notmatch 'runningTargetPaths|validateReplacementCommitPage|runMaintenancePurge|waitForDirectoryRemoval|gainAdminRights|executeDetached' -and
        $controllerScript -notmatch 'setDefaultPageVisible\s*\(\s*QInstaller\.(?:TargetDirectory|ReadyForInstallation)\s*,\s*false\s*\)') `
    'the package script must contain no replacement validator, elevation, purge, or page-suppression machinery'
Assert-IfwContract `
    (($controllerScript + $installScript) -notmatch 'RemoveTargetDir' -and
        ($controllerScript + $installScript) -notmatch 'performOperation\s*\(') `
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
        $windowsPackages -match 'vnm-terminal-ifw-provenance\.json' -and
        $windowsPackages -match '-IfwRoot\s+"%IFW_ROOT%"') `
    'the Windows package entry point must require a custom IFW root and pass it explicitly'
Assert-IfwContract `
    ($windowsPackages -match 'if\s+"%VNM_TERMINAL_CONPTY_PACKAGE_DIR%"==""' -and
        $windowsPackages -match 'conpty-source-build\.json' -and
        $windowsWorkflow -match 'tools/provision_windows_conpty\.ps1' -and
        $windowsWorkflow -match 'set VNM_TERMINAL_CONPTY_PACKAGE_DIR=\$env:VNM_CONPTY_ROOT\\package') `
    'Windows packages must provision and require the receipt-bearing source-built ConPTY runtime'
Assert-IfwContract `
    ($buildConfigExample -match 'build_windows_private_ifw\.ps1' -and
        $buildConfigExample -match 'set IFW_ROOT=.*\\build') `
    'the local build configuration example must point to the custom engine build directory'
$jobLevelEnvironmentBlocks = [regex]::Matches(
    $windowsWorkflow,
    '(?m)^ {4}env:\s*\r?\n(?:(?:^ {6,}[^\r\n]*\r?\n)|(?:^\s*\r?\n))*')
Assert-IfwContract `
    (-not ($jobLevelEnvironmentBlocks.Value -match '\$\{\{\s*runner\.')) `
    'job-level workflow environment must not reference runner context'
$ifwPathInitializationIndex = $windowsWorkflow.IndexOf(
    '- name: Initialize private Qt IFW paths')
$ifwCacheRestoreIndex = $windowsWorkflow.IndexOf(
    '- name: Restore private Qt IFW inputs')
Assert-IfwContract `
    ($ifwPathInitializationIndex -ge 0 -and
        $ifwCacheRestoreIndex -gt $ifwPathInitializationIndex -and
        $windowsWorkflow -match '\$ifwInputRoot\s*=\s*Join-Path\s+\$env:RUNNER_TEMP' -and
        $windowsWorkflow -match '\$ifwEngineRoot\s*=\s*Join-Path\s+\$env:RUNNER_TEMP' -and
        $windowsWorkflow -match '"IFW_INPUT_ROOT=\$ifwInputRoot"[\s\S]*?\$env:GITHUB_ENV' -and
        $windowsWorkflow -match '"IFW_ENGINE_ROOT=\$ifwEngineRoot"[\s\S]*?\$env:GITHUB_ENV' -and
        $windowsWorkflow -match '"IFW_ROOT=\$ifwRoot"[\s\S]*?\$env:GITHUB_ENV') `
    'Windows CI must derive IFW paths on the runner before the cache step and export them'
foreach ($cacheKind in @('inputs', 'engine')) {
    $cacheKeys = [regex]::Matches($windowsWorkflow,
        "(?m)^\s*key: (.+-vs2026-ifw-$cacheKind-.+)$")
    Assert-IfwContract `
        ($cacheKeys.Count -eq 2 -and
            $cacheKeys[0].Groups[1].Value -ceq $cacheKeys[1].Groups[1].Value -and
            $cacheKeys[0].Value -match 'hashFiles\(') `
        "Windows CI must restore and save the $cacheKind cache with the same source-derived key"
}
Assert-IfwContract `
    ($windowsWorkflow -match 'actions/cache/restore@[0-9a-f]{40}' -and
        $windowsWorkflow -match 'actions/cache/save@[0-9a-f]{40}' -and
        $windowsWorkflow -match 'tools/provision_windows_private_ifw_inputs\.ps1' -and
        $windowsWorkflow -match 'tools/build_windows_private_ifw\.ps1' -and
        $windowsWorkflow -match 'tools/assert_windows_private_ifw\.ps1' -and
        $windowsWorkflow -match 'set IFW_ROOT=\$env:IFW_ROOT') `
    'Windows CI must provision, build, verify, and pass the custom engine to packaging'
$signingJob = [regex]::Match($windowsWorkflow,
    '(?ms)^  sign-windows-package:\r?\n.*?(?=^  [a-z][a-z-]+:|\z)').Value
Assert-IfwContract `
    ($windowsWorkflow -match '(?s)Upload the release installer engine.*?name: vnm-terminal-windows-installer-engine' -and
        $signingJob -match '(?s)Download the release installer engine.*?name: vnm-terminal-windows-installer-engine' -and
        $signingJob.IndexOf('-File tools/assert_windows_private_ifw.ps1') -ge 0 -and
        $signingJob.IndexOf('-File tools/assert_windows_private_ifw.ps1') -lt
            $signingJob.IndexOf('- name: Authenticate to Azure for signing')) `
    'the signing job must verify the exact engine artifact from the package job before authenticating'
Assert-IfwContract `
    ($windowsWorkflow -match '(?s)Archive installer corresponding source.*?IFW_ENGINE_ROOT\\source-material' -and
        $windowsWorkflow -match '(?s)Upload installer corresponding source.*?name: vnm-terminal-windows-installer-source' -and
        $windowsWorkflow -match '(?s)Download installer corresponding source.*?name: vnm-terminal-windows-installer-source' -and
        $windowsWorkflow -match '(?s)gh release upload.*?dist/vnm_terminal_v\*_windows_installer_source\.zip') `
    'release publication must provide the matching installer source packet as a separate download'
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
$resourceCopyIndex = $buildScript.IndexOf('foreach ($resource in @(')
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
    ($resourceCopyIndex -gt $configRenderIndex -and $binaryCreatorIndex -gt $resourceCopyIndex) `
    'IFW must see the adopted resource files beside the rendered config before resource collection'
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
foreach ($resource in ($expectedThemeFiles + @('varinomics_logo.png', 'varinomics_logo@2x.png', 'theme_resources.qrc'))) {
    Assert-IfwContract ($buildScript.Contains("'$resource'")) `
        "the builder must stage $resource"
}
Assert-IfwContract `
    ($buildScript -match 'Copy-Item -LiteralPath \(Join-Path \$ifwSourceRoot \$resource\) -Destination \$configRoot') `
    'the resource inventory must be copied beside the configuration'
Assert-IfwContract `
    ($buildScript -match '--resources \(Join-Path \$configRoot ''theme_resources\.qrc''\)' -and
        $buildScript -notmatch '--resources \(Join-Path \$configRoot ''theme_resources\.rcc''\)') `
    'binarycreator must receive the XML resource manifest it compiles internally'
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
