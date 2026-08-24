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
    "existing-installation discovery must name the configured maintenance tool");
assert(/function\s+Controller\s*\(\s*\)\s*\{[\s\S]*?Controller\.prototype\.detectExistingInstallations\s*\(\s*\)/
    .test(controllerScript),
    "an installed copy must be recognized before the wizard presents its pages");
assert(/executeDetached\s*\([\s\S]*?\["--start-uninstaller"\]/
    .test(controllerScript),
    "the handoff must open the installed maintenance tool in graphical uninstaller mode");
assert(!/\bpurge\b|installationStarted|setCanceled|gainAdminRights|performOperation\s*\(/
    .test(controllerScript),
    "the new setup must not remove, elevate, or begin installing over the existing copy");

const hiddenPages = [];
const hiddenPagePattern =
    /setDefaultPageVisible\s*\(\s*QInstaller\.(\w+)\s*,\s*(\w+)\s*\)/g;
let hiddenPageMatch;
while ((hiddenPageMatch = hiddenPagePattern.exec(controllerScript)) !== null) {
    assert(hiddenPageMatch[2] === "false",
        "setDefaultPageVisible is only used here to hide a page");
    hiddenPages.push(hiddenPageMatch[1]);
}
assert(hiddenPages.join(",") === "ComponentSelection",
    "only the forced component page may be hidden");

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
    hasAdminRights() { return state.isAdmin; },
    setDefaultPageVisible(page) {
        state.hiddenPages.push(page);
    },
    value(name) {
        if (name === "TargetDir") return state.currentTargetDirectory;
        fail("unexpected installer value: " + name);
    },
    fileExists(candidate) {
        if (state.maintenanceToolPaths.indexOf(candidate) >= 0) return true;
        if (candidate === launcherLinkPath) return state.launcherPresent;
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
        if (program !== readlinkPath)
            fail("unexpected synchronous execution: " + program);
        if (!state.readlinkStarts)
            return [];
        const outputEncoding = stdOutCodec === "UTF-8"
            ? "utf8"
            : "latin1";
        const decodedOutput = Buffer.from(
            state.launcherTarget + "\n", "utf8").toString(outputEncoding);
        return [decodedOutput, state.readlinkExitCode];
    },
    executeDetached(program, args, workingDirectory) {
        state.detachedExecutions.push({ program, args, workingDirectory });
        return state.detachedLaunchSucceeds;
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
    overrides = overrides || {};
    const directory = overrides.directory || "/opt/vnm_terminal";
    const installationPresent = overrides.installationPresent !== false;
    state = Object.assign({
        directory,
        currentTargetDirectory: directory,
        maintenanceToolPath: directory + "/" + maintenanceToolName,
        maintenanceToolPaths: installationPresent
            ? [directory + "/" + maintenanceToolName]
            : [],
        launcherPresent: installationPresent,
        launcherTarget: directory + "/bin/vnm_terminal",
        readlinkStarts: true,
        readlinkExitCode: 0,
        detachedLaunchSucceeds: true,
        isAdmin: false,
        answer: QMessageBox.Yes,
        questions: [],
        errors: [],
        executions: [],
        detachedExecutions: [],
        hiddenPages: [],
        rejections: 0,
        pages: {
            IntroductionPage: {
                title: "",
                subTitle: "",
                MessageLabel: labelStub(),
            },
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

    Controller.prototype.existingInstallationDirectory = "";
    Controller.prototype.ambiguousInstallationDirectories = [];
    Controller.prototype.reportedAmbiguousInstallations = false;
    Controller.prototype.existingInstallationHandoffHandled = false;

    new Controller();
    Controller.prototype.IntroductionPageCallback();
    return state;
}

let result = run({ installationPresent: false });
Controller.prototype.TargetDirectoryPageCallback();
Controller.prototype.ReadyForInstallationPageCallback();
assert(result.hiddenPages.length === 1 &&
    result.hiddenPages[0] === QInstaller.ComponentSelection,
    "a fresh install must hide only the forced component page");
assert(result.questions.length === 0 &&
    result.detachedExecutions.length === 0 &&
    result.rejections === 0,
    "a fresh install must not enter the existing-installation handoff");
assert(result.pages.IntroductionPage.subTitle.indexOf("Install") >= 0 &&
    result.pages.TargetDirectoryPage.subTitle.indexOf("Choose") >= 0 &&
    result.pages.ReadyForInstallationPage.subTitle.indexOf("Review") >= 0,
    "fresh GUI installation pages must remain usable");

result = run({});
assert(result.questions.length === 1 &&
    result.questions[0].text.indexOf(result.directory) >= 0 &&
    result.questions[0].text.indexOf("does not replace") >= 0 &&
    result.questions[0].text.indexOf("Nothing is removed until") >= 0 &&
    result.questions[0].text.indexOf("run this setup again") >= 0,
    "one installed copy must offer a clear, non-destructive two-step handoff");
assert(result.detachedExecutions.length === 1 &&
    result.detachedExecutions[0].program === result.maintenanceToolPath &&
    result.detachedExecutions[0].args.join(" ") === "--start-uninstaller" &&
    result.detachedExecutions[0].workingDirectory === result.directory &&
    result.rejections === 1,
    "accepting the handoff must open the installed graphical uninstaller and close setup");
Controller.prototype.IntroductionPageCallback();
assert(result.questions.length === 1 &&
    result.detachedExecutions.length === 1,
    "re-entering Introduction must not repeat the handoff prompt or launch");

result = run({ isAdmin: true });
assert(result.questions.length === 1 &&
    result.detachedExecutions.length === 0 &&
    result.errors.length === 1 &&
    result.errors[0].text.indexOf("inherit those rights") >= 0 &&
    result.errors[0].text.indexOf(result.maintenanceToolPath) >= 0 &&
    result.rejections === 1,
    "elevated setup must not detach the discovered maintenance tool");
Controller.prototype.IntroductionPageCallback();
assert(result.questions.length === 1 &&
    result.detachedExecutions.length === 0 &&
    result.errors.length === 1,
    "elevated handoff rejection must remain latched during shutdown");

const customDirectory = "/srv/varinomics/\u0130mak terminal";
result = run({
    installationPresent: false,
    launcherPresent: true,
    launcherTarget: customDirectory + "/bin/vnm_terminal",
    maintenanceToolPath: customDirectory + "/" + maintenanceToolName,
    maintenanceToolPaths: [customDirectory + "/" + maintenanceToolName],
});
assert(result.questions[0].text.indexOf(customDirectory) >= 0 &&
    result.detachedExecutions[0].program ===
        customDirectory + "/" + maintenanceToolName,
    "launcher discovery must hand a custom installation to its own uninstaller");
const linkProbe = result.executions.find(
    (execution) => execution.program === readlinkPath);
assert(linkProbe.argumentCount === 5 &&
    linkProbe.stdIn === "" &&
    linkProbe.stdInCodec === "UTF-8" &&
    linkProbe.stdOutCodec === "UTF-8",
    "launcher discovery must preserve a non-ASCII custom location");

result = run({ answer: QMessageBox.No });
assert(result.questions.length === 1 &&
    result.detachedExecutions.length === 0 &&
    result.rejections === 1,
    "declining the handoff must close setup without opening or removing anything");
Controller.prototype.IntroductionPageCallback();
assert(result.questions.length === 1 &&
    result.detachedExecutions.length === 0,
    "declining must remain latched during setup shutdown");

result = run({ detachedLaunchSucceeds: false });
assert(result.questions.length === 1 &&
    result.detachedExecutions.length === 1 &&
    result.errors.length === 1 &&
    result.rejections === 0,
    "a launch failure must leave setup open with one explanation");
assert(result.errors[0].text.indexOf(result.maintenanceToolPath) >= 0 &&
    result.errors[0].text.indexOf("No removal was started") >= 0 &&
    result.errors[0].text.indexOf("manually") >= 0 &&
    result.errors[0].text.indexOf("run setup again") >= 0,
    "launch-failure recovery must be truthful and name the maintenance tool");
Controller.prototype.IntroductionPageCallback();
assert(result.questions.length === 1 &&
    result.detachedExecutions.length === 1 &&
    result.errors.length === 1,
    "a failed launch must not duplicate prompts, launches, or errors");
Controller.prototype.TargetDirectoryPageCallback();
assert(result.rejections === 1,
    "trying to advance after launch failure must close instead of installing");

result = run({
    installationPresent: false,
    launcherPresent: true,
    launcherTarget: "/srv/stale/vnm_terminal/bin/vnm_terminal",
});
assert(result.questions.length === 0 &&
    result.detachedExecutions.length === 0 &&
    result.rejections === 0,
    "a stale launcher link without a maintenance tool must remain a fresh install");

result = run({
    launcherTarget: customDirectory + "/bin/vnm_terminal",
    maintenanceToolPaths: [
        "/opt/vnm_terminal/" + maintenanceToolName,
        customDirectory + "/" + maintenanceToolName,
    ],
});
Controller.prototype.IntroductionPageCallback();
assert(result.questions.length === 0 &&
    result.detachedExecutions.length === 0 &&
    result.errors.length === 1 &&
    result.rejections === 2,
    "ambiguous discovery must explain once, never launch, and close on every entry");
assert(result.errors[0].text.indexOf("/opt/vnm_terminal") >= 0 &&
    result.errors[0].text.indexOf(customDirectory) >= 0 &&
    result.errors[0].text.indexOf("cannot choose which uninstaller") >= 0 &&
    result.errors[0].text.indexOf("Remove the extra installations manually") >= 0,
    "the ambiguity explanation must list the locations and give bounded recovery");

process.stdout.write(
    "Linux Qt IFW controller contract passed: " + controllerScriptPath + "\n");
