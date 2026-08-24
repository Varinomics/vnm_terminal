// Contract gate for the Linux Qt IFW control script. IFW loads a control
// script only in GUI mode, so the command-line installation lifecycle in
// Linux CI can never reach this code. The control script is evaluated in this
// module's scope, which requires sloppy mode.

const fs = require("fs");
const path = require("path");

const sourceRoot = process.argv[2];
if (!sourceRoot)
    throw new Error("usage: linux_ifw_contract_tests.js <source-root>");

const ifwSourceRoot = path.join(sourceRoot, "packaging", "linux", "ifw");
const controllerScriptPath = path.join(ifwSourceRoot, "controller.qs");
const controllerScript = fs.readFileSync(controllerScriptPath, "utf8");
const configTemplate = fs.readFileSync(
    path.join(ifwSourceRoot, "config.xml.in"), "utf8");

function fail(message) {
    throw new Error("Linux Qt IFW contract violation: " + message);
}

function assert(condition, message) {
    if (!condition)
        fail(message);
}

const maintenanceToolNameMatch =
    /<MaintenanceToolName>([^<]+)<\/MaintenanceToolName>/.exec(configTemplate);
assert(maintenanceToolNameMatch !== null,
    "the installer configuration must declare a maintenance tool name");
const maintenanceToolName = maintenanceToolNameMatch[1];
const launcherLinkPath = "/usr/local/bin/vnm_terminal";
const readlinkPath = "/usr/bin/readlink";

assert(controllerScript.indexOf(
    "Controller.prototype.maintenanceToolFileName = \"" +
    maintenanceToolName + "\";") >= 0,
    "the existing-installation probe must name the configured maintenance tool");
assert(/function\s+Controller\s*\(\s*\)\s*\{[\s\S]*?Controller\.prototype\.adoptExistingInstallation\s*\(\s*\)/
    .test(controllerScript),
    "an installed copy must be recognized before the wizard presents its pages");
assert(/installer\.installationStarted\.connect\s*\(\s*Controller\.prototype\.replaceExistingInstallation\s*\)/
    .test(controllerScript),
    "the removal must be bound to the start of the installation, not to a page");
assert(/replaceExistingInstallation\s*=\s*function[\s\S]*?installer\.setValue\s*\(\s*"TargetDir"[\s\S]*?installer\.setCanceled\s*\(\s*\)/
    .test(controllerScript),
    "a stopped run must move its target directory before it is abandoned");
assert(controllerScript.indexOf(
    "[\"purge\", \"--accept-messages\", \"--confirm-command\"]") >= 0,
    "the removal must be owned by a non-interactive framework maintenance-tool purge");
assert(controllerScript.indexOf("RemoveTargetDir") < 0 &&
    !/performOperation\s*\(/.test(controllerScript),
    "the installer must not weaken target-directory validation or delete the previous installation itself");

const hiddenPages = [];
const hiddenPagePattern = /setDefaultPageVisible\s*\(\s*QInstaller\.(\w+)\s*,\s*(\w+)\s*\)/g;
let hiddenPageMatch;
while ((hiddenPageMatch = hiddenPagePattern.exec(controllerScript)) !== null) {
    assert(hiddenPageMatch[2] === "false",
        "setDefaultPageVisible is only used here to hide a page");
    hiddenPages.push(hiddenPageMatch[1]);
}
assert(hiddenPages.length ===
    (controllerScript.match(/setDefaultPageVisible\s*\(/g) || []).length,
    "every page visibility change must name a framework page");
assert(hiddenPages.join(",") === "ComponentSelection,TargetDirectory",
    "the controller may hide the forced component page and, on an upgrade, " +
    "the installation folder page, and no other built-in wizard page");

let state = null;

global.QInstaller = {
    ComponentSelection: 1,
    TargetDirectory: 2,
    Success: 0,
    Failure: 1,
    Canceled: 3,
    Unfinished: 4,
};
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
    status: QInstaller.Success,
    isInstaller() { return true; },
    setDefaultPageVisible(page, visible) {
        state.hiddenPages.push(page);
    },
    setCanceled() { state.cancellations += 1; },
    setValue(name, value) {
        state.assignedValues.push({ name, value });
        if (name === "TargetDir") state.currentTargetDirectory = value;
    },
    installationStarted: {
        connect(handler) { state.installationStartedHandlers.push(handler); },
    },
    value(name) {
        if (name === "TargetDir") return state.currentTargetDirectory;
        if (name === "HomeDir") return "/home/tester";
        fail("unexpected installer value: " + name);
    },
    fileExists(candidate) {
        if (state.maintenanceToolPaths.indexOf(candidate) >= 0) return true;
        if (candidate === launcherLinkPath) return state.launcherPresent;
        if (candidate === state.activeInstallationDirectory)
            return state.directoryPresent;
        return false;
    },
    execute(program, args, stdIn, stdInCodec, stdOutCodec) {
        state.executions.push({
            program,
            args,
            stdIn,
            stdInCodec,
            stdOutCodec,
            argumentCount: arguments.length,
        });
        if (program === readlinkPath) {
            if (!state.readlinkStarts) return [];
            const outputEncoding = stdOutCodec === "UTF-8"
                ? "utf8"
                : "latin1";
            const decodedOutput = Buffer.from(
                state.launcherTarget + "\n", "utf8").toString(outputEncoding);
            return [decodedOutput, state.readlinkExitCode];
        }
        if (state.maintenanceToolPaths.indexOf(program) < 0)
            fail("unexpected execution: " + program);
        if (!state.purgeStarts) return [];
        if (state.purgeExitCode === 0 && state.directoryRemovedByPurge)
            state.directoryPresent = false;
        return ["", state.purgeExitCode];
    },
};
global.gui = {
    rejectWithoutPrompt() { state.rejections += 1; },
    pageWidgetByObjectName(name) {
        if (!Object.prototype.hasOwnProperty.call(state.pages, name))
            fail("unexpected page lookup: " + name);
        return state.pages[name];
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
    const directory = overrides.directory || "/opt/vnm_terminal";
    const installationPresent = overrides.installationPresent !== false;
    state = Object.assign({
        directory,
        currentTargetDirectory: directory,
        activeInstallationDirectory: directory,
        maintenanceToolPath: directory + "/" + maintenanceToolName,
        maintenanceToolPaths: installationPresent
            ? [directory + "/" + maintenanceToolName]
            : [],
        launcherPresent: installationPresent,
        launcherTarget: directory + "/bin/vnm_terminal",
        readlinkStarts: true,
        readlinkExitCode: 0,
        directoryPresent: true,
        directoryRemovedByPurge: true,
        purgeStarts: true,
        purgeExitCode: 0,
        questions: [],
        errors: [],
        executions: [],
        assignedValues: [],
        hiddenPages: [],
        installationStartedHandlers: [],
        cancellations: 0,
        rejections: 0,
        pages: {
            IntroductionPage: { title: "", subTitle: "", MessageLabel: labelStub() },
            TargetDirectoryPage: { subTitle: "" },
            ReadyForInstallationPage: { subTitle: "" },
            FinishedPage: {
                title: "",
                subTitle: "",
                MessageLabel: labelStub(),
                RunItCheckBox: { hide() { this.hidden = true; } },
            },
        },
    }, overrides);

    // The framework evaluates the control script once per process, so the run
    // that owns the state is the one that sets it.
    Controller.prototype.replacedInstallationDirectory = "";
    Controller.prototype.replacementFailed = false;
    Controller.prototype.ambiguousInstallationDirectories = [];
    Controller.prototype.reportedAmbiguousInstallations = false;

    new Controller();
    Controller.prototype.IntroductionPageCallback();
    Controller.prototype.TargetDirectoryPageCallback();
    Controller.prototype.ReadyForInstallationPageCallback();
    return state;
}

function startInstallation(runState) {
    runState.installationStartedHandlers.forEach((handler) => handler());
    runState.purges = runState.executions.filter(
        (execution) => execution.args[0] === "purge");
    runState.linkProbes = runState.executions.filter(
        (execution) => execution.program === readlinkPath);
    return runState;
}

let result = run({ installationPresent: false, directoryPresent: false });
assert(result.hiddenPages.length === 1 &&
    result.hiddenPages[0] === QInstaller.ComponentSelection,
    "a free target directory must leave the installation folder page in place");
assert(result.installationStartedHandlers.length === 0,
    "a free target directory must not arm a removal");
assert(result.pages.ReadyForInstallationPage.subTitle
        .indexOf("replace") < 0 &&
    result.pages.IntroductionPage.MessageLabel.text
        .indexOf("already installed") < 0,
    "a first installation must not announce a replacement");
assert(startInstallation(result).executions.length === 0,
    "a first installation must remove nothing");

result = run({});
assert(result.hiddenPages.length === 2 &&
    result.hiddenPages[1] === QInstaller.TargetDirectory,
    "an installed copy must take the installation folder page out of the wizard");
assert(result.installationStartedHandlers.length === 1,
    "an installed copy must arm exactly one removal");
assert(result.executions.filter(
        (execution) => execution.args[0] === "purge").length === 0 &&
    result.errors.length === 0,
    "nothing may be removed before the installation starts");
assert(result.pages.IntroductionPage.MessageLabel.text
        .indexOf(result.directory) >= 0 &&
    result.pages.IntroductionPage.MessageLabel.text
        .indexOf("remove that installation") >= 0,
    "the first page must name the installation this run replaces");
assert(result.pages.ReadyForInstallationPage.subTitle
        .indexOf(result.directory) >= 0 &&
    result.pages.ReadyForInstallationPage.subTitle.indexOf("replace") >= 0,
    "the page that accepts the installation must state the replacement");

startInstallation(result);
assert(result.purges.length === 1 &&
    result.purges[0].program === result.maintenanceToolPath &&
    result.purges[0].args.join(" ") ===
        "purge --accept-messages --confirm-command",
    "starting the installation must remove the installed copy with one confirmed purge");
assert(result.errors.length === 0 && result.cancellations === 0 &&
    Controller.prototype.replacementFailed === false,
    "a completed removal must report no failure and must not stop the run");

const customDirectory = "/srv/varinomics/\u0130mak terminal";
result = run({
    installationPresent: false,
    launcherPresent: true,
    launcherTarget: customDirectory + "/bin/vnm_terminal",
    activeInstallationDirectory: customDirectory,
    maintenanceToolPath: customDirectory + "/" + maintenanceToolName,
    maintenanceToolPaths: [customDirectory + "/" + maintenanceToolName],
});
assert(result.currentTargetDirectory === customDirectory &&
    result.hiddenPages[1] === QInstaller.TargetDirectory &&
    result.installationStartedHandlers.length === 1,
    "the package-owned launcher link must transition a custom installation " +
    "into the one-run replacement flow");
startInstallation(result);
assert(result.purges.length === 1 &&
    result.purges[0].program === customDirectory + "/" + maintenanceToolName,
    "a discovered custom installation must be purged through its own maintenance tool");
assert(result.linkProbes.length === 1 &&
    result.linkProbes[0].argumentCount === 5 &&
    result.linkProbes[0].stdIn === "" &&
    result.linkProbes[0].stdInCodec === "UTF-8" &&
    result.linkProbes[0].stdOutCodec === "UTF-8",
    "the launcher probe must decode UTF-8 without corrupting a non-ASCII custom location");

result = run({
    installationPresent: false,
    launcherPresent: true,
    launcherTarget: "/srv/stale/vnm_terminal/bin/vnm_terminal",
});
assert(result.hiddenPages.length === 1 &&
    result.installationStartedHandlers.length === 0,
    "a stale launcher link without a maintenance tool must not arm a replacement");

result = run({
    launcherTarget: customDirectory + "/bin/vnm_terminal",
    maintenanceToolPaths: [
        "/opt/vnm_terminal/" + maintenanceToolName,
        customDirectory + "/" + maintenanceToolName,
    ],
});
assert(result.hiddenPages.length === 1 &&
    result.installationStartedHandlers.length === 0,
    "ambiguous evidence for two live installations must not select or arm a replacement");
Controller.prototype.IntroductionPageCallback();
assert(Controller.prototype.ambiguousInstallationDirectories.length === 2 &&
    result.errors.length === 1 && result.rejections === 2,
    "repeated ambiguous entry must show one blocking explanation while explicitly closing setup every time");
assert(result.errors[0].text.indexOf("/opt/vnm_terminal") >= 0 &&
    result.errors[0].text.indexOf(customDirectory) >= 0 &&
    result.errors[0].text.indexOf(
        "Only one managed vnm_terminal installation is supported") >= 0 &&
    result.errors[0].text.indexOf(
        "cannot safely repair or remove this ambiguous state") >= 0 &&
    result.errors[0].text.indexOf("Remove all listed installations") >= 0 &&
    result.errors[0].text.indexOf("remaining shared launcher state") >= 0 &&
    result.errors[0].text.indexOf("fresh installation") >= 0 &&
    result.errors[0].text.indexOf("cleanup or removal fails") >= 0 &&
    result.errors[0].text.indexOf("Varinomics support") >= 0 &&
    result.errors[0].text.indexOf("unwanted copies") < 0 &&
    result.errors[0].text.indexOf("keep one") < 0 &&
    result.errors[0].text.indexOf("run every uninstaller") < 0,
    "the ambiguity explanation must list the locations, require full cleanup, " +
    "and give a support path without promising automatic recovery");
assert(startInstallation(result).purges.length === 0,
    "an ambiguous run must never purge an installation");

result = startInstallation(run({ purgeExitCode: 1 }));
assert(result.errors.length === 1 &&
    result.errors[0].text.indexOf(result.directory) >= 0,
    "a failed purge must be reported and must name its installation directory");
assert(result.errors[0].text.indexOf("may now be incomplete") >= 0,
    "a failed purge must not promise that the previous installation stayed pristine");
assert(result.cancellations === 1 &&
    Controller.prototype.replacementFailed === true,
    "a failed removal must stop the installation before it writes anything");

assert(result.assignedValues.length === 1 &&
    result.assignedValues[0].name === "TargetDir" &&
    result.assignedValues[0].value.indexOf(result.directory) !== 0,
    "a stopped run must take its target directory off the installation it kept, " +
    "so that the framework's own cleanup cannot reach that installation");

result = startInstallation(run({ purgeStarts: false }));
assert(result.errors.length === 1 && result.cancellations === 1,
    "an unstartable maintenance tool must be reported as a failed removal");

result = startInstallation(run({ directoryRemovedByPurge: false }));
assert(result.errors.length === 1 && result.cancellations === 1,
    "a directory that survives the purge must be reported, not accepted");

installer.status = QInstaller.Canceled;
Controller.prototype.FinishedPageCallback();
assert(result.pages.FinishedPage.MessageLabel.text
        .indexOf("removal of the installation") >= 0 &&
    result.pages.FinishedPage.MessageLabel.text
        .indexOf(result.directory) >= 0 &&
    result.pages.FinishedPage.MessageLabel.text
        .indexOf("may now be incomplete") >= 0,
    "a run stopped by a failed removal must report that, not a cancellation");
installer.status = QInstaller.Success;

process.stdout.write(
    "Linux Qt IFW controller contract passed: " + controllerScriptPath + "\n");
