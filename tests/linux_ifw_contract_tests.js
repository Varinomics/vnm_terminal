// Causal contract gate for the Linux Qt IFW control script. The control
// script is evaluated in this module's scope, which requires sloppy mode.

const fs = require("fs");
const path = require("path");

const sourceRoot = process.argv[2];
if (!sourceRoot)
    throw new Error("usage: linux_ifw_contract_tests.js <source-root>");

const ifwSourceRoot = path.join(sourceRoot, "packaging", "linux", "ifw");
const controllerScript = fs.readFileSync(
    path.join(ifwSourceRoot, "controller.qs"), "utf8");
const configTemplate = fs.readFileSync(
    path.join(ifwSourceRoot, "config.xml.in"), "utf8");

function assert(condition, message) {
    if (!condition)
        throw new Error("Linux Qt IFW contract violation: " + message);
}

const maintenanceToolName =
    /<MaintenanceToolName>([^<]+)<\/MaintenanceToolName>/
        .exec(configTemplate)[1];
assert(controllerScript.indexOf(
    "Controller.prototype.maintenanceToolFileName = \"" +
        maintenanceToolName + "\";") >= 0,
    "selected-folder detection must name the configured maintenance tool");
const obsoleteDiscoveryOrReplacement = new RegExp([
    "readlink",
    "launcherInstallationDirectory",
    "detectExistingInstallations",
    "ambiguousInstallation",
    "\\bpurge\\b",
    "installationStarted",
    "setCanceled",
    "gainAdminRights",
].join("|"));
assert(!obsoleteDiscoveryOrReplacement.test(controllerScript),
    "startup discovery and automatic replacement machinery must stay removed");
assert(/TargetDirectoryPageCallback[\s\S]*?subTitle[\s\S]*?offerSelectedInstallationUninstaller/
    .test(controllerScript),
    "the selected folder must be inspected only after its page is initialized");
assert(!/TargetDirectoryLineEdit[\s\S]{0,80}?\.connect/.test(controllerScript),
    "later target edits must remain owned by IFW validation");
assert(/closeRequested\s*=\s*true[\s\S]*?rejectWithoutPrompt/
    .test(controllerScript),
    "a successful handoff must latch its close before requesting it");

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
        state.questions.push({ identifier, title, text, buttons, defaultButton });
        return state.answers.length ? state.answers.shift() : state.answer;
    },
    critical(identifier, title, text) {
        state.errors.push({ identifier, title, text });
        return 0x00000400;
    },
};
global.installer = {
    status: QInstaller.Success,
    isInstaller() { return state.installerMode; },
    hasAdminRights() { return state.isAdmin; },
    setDefaultPageVisible(page) { state.hiddenPages.push(page); },
    value(name) {
        if (name === "TargetDir") return state.targetDirectory;
        throw new Error("unexpected installer value: " + name);
    },
    fileExists(candidate) {
        state.fileChecks.push(candidate);
        return state.maintenanceToolPaths.indexOf(candidate) >= 0;
    },
    execute() { throw new Error("startup discovery must not execute programs"); },
    executeDetached(program, args, workingDirectory) {
        state.detachedExecutions.push({ program, args, workingDirectory });
        return state.detachedLaunchSucceeds;
    },
};
global.gui = {
    rejectWithoutPrompt() { state.rejections += 1; },
    pageWidgetByObjectName(name) { return state.pages[name]; },
};

function labelStub() {
    return { text: "framework", setText(value) { this.text = value; } };
}

eval(controllerScript);

function createState(overrides) {
    state = Object.assign({
        installerMode: true,
        isAdmin: false,
        targetDirectory: "/opt/vnm_terminal",
        maintenanceToolPaths: ["/opt/vnm_terminal/" + maintenanceToolName],
        detachedLaunchSucceeds: true,
        answer: QMessageBox.Yes,
        answers: [],
        questions: [],
        errors: [],
        fileChecks: [],
        detachedExecutions: [],
        hiddenPages: [],
        rejections: 0,
        pages: {
            IntroductionPage: {
                title: "", subTitle: "", MessageLabel: labelStub(),
            },
            TargetDirectoryPage: {
                subTitle: "",
                TargetDirectoryLineEdit: { text: "/opt/vnm_terminal" },
            },
            LicenseAgreementPage: { subTitle: "" },
            ReadyForInstallationPage: { subTitle: "" },
            PerformInstallationPage: { subTitle: "" },
            FinishedPage: {
                title: "", subTitle: "", MessageLabel: labelStub(),
                RunItCheckBox: { hide() {} },
            },
        },
    }, overrides || {});
    new Controller();
    return state;
}

let result = createState();
Controller.prototype.IntroductionPageCallback();
assert(result.questions.length === 0 &&
    result.fileChecks.length === 0 &&
    result.detachedExecutions.length === 0 &&
    result.rejections === 0 &&
    result.pages.IntroductionPage.subTitle.indexOf("Install") >= 0,
"startup and Welcome must remain normal even when the default folder exists");

Controller.prototype.TargetDirectoryPageCallback();
assert(result.pages.TargetDirectoryPage.subTitle.indexOf("Choose") >= 0 &&
    result.questions.length === 1 &&
    result.questions[0].text.indexOf("Select No") >= 0 &&
    result.questions[0].text.indexOf("global launcher") >= 0 &&
    result.questions[0].text.indexOf("not independent") >= 0,
"the existing default folder must present the side-by-side tradeoff");
assert(result.detachedExecutions.length === 1 &&
    result.detachedExecutions[0].program ===
        "/opt/vnm_terminal/" + maintenanceToolName &&
    result.detachedExecutions[0].args.join(" ") === "--start-uninstaller" &&
    result.detachedExecutions[0].workingDirectory === "/opt/vnm_terminal" &&
    result.rejections === 1,
"Yes must open the exact graphical uninstaller and request setup close");
Controller.prototype.TargetDirectoryPageCallback();
Controller.prototype.IntroductionPageCallback();
assert(result.questions.length === 1 &&
    result.detachedExecutions.length === 1 &&
    result.rejections === 3,
"every close re-entry must reject again without another prompt or launch");

result = createState({ answer: QMessageBox.No });
Controller.prototype.IntroductionPageCallback();
Controller.prototype.TargetDirectoryPageCallback();
assert(result.questions.length === 1 &&
    result.detachedExecutions.length === 0 &&
    result.rejections === 0 &&
    result.pages.TargetDirectoryPage.subTitle.indexOf("Choose") >= 0,
"No must stay on the folder page and remain latched for an unchanged path");
result.targetDirectory = "/srv/free";
result.pages.TargetDirectoryPage.TargetDirectoryLineEdit.text = "/srv/free";
Controller.prototype.LicenseAgreementPageCallback();
Controller.prototype.ReadyForInstallationPageCallback();
assert(result.questions.length === 1 &&
    result.rejections === 0 &&
    result.pages.LicenseAgreementPage.subTitle.indexOf("Review") >= 0 &&
    result.pages.ReadyForInstallationPage.subTitle.indexOf("Review") >= 0,
"choosing a free folder after No must proceed through ordinary callbacks");

result = createState({ isAdmin: true });
Controller.prototype.IntroductionPageCallback();
Controller.prototype.TargetDirectoryPageCallback();
assert(result.questions.length === 1 &&
    result.errors.length === 1 &&
    result.errors[0].text.indexOf("inherit those rights") >= 0 &&
    result.errors[0].text.indexOf("/opt/vnm_terminal/" + maintenanceToolName) >= 0 &&
    result.detachedExecutions.length === 0 &&
    result.rejections === 0,
"elevated handoff must be blocked once while leaving the folder page usable");

result = createState({ detachedLaunchSucceeds: false });
Controller.prototype.IntroductionPageCallback();
Controller.prototype.TargetDirectoryPageCallback();
assert(result.questions.length === 1 &&
    result.errors.length === 1 &&
    result.errors[0].text.indexOf("No removal was started") >= 0 &&
    result.errors[0].text.indexOf("choose another folder") >= 0 &&
    result.detachedExecutions.length === 1 &&
    result.rejections === 0,
"launch failure must remain safe, truthful, latched, and usable");

result = createState({
    targetDirectory: "/srv/fresh",
    maintenanceToolPaths: [],
});
Controller.prototype.IntroductionPageCallback();
Controller.prototype.TargetDirectoryPageCallback();
Controller.prototype.ReadyForInstallationPageCallback();
assert(result.questions.length === 0 &&
    result.detachedExecutions.length === 0 &&
    result.rejections === 0,
"fresh graphical installation must retain its normal flow");

console.log("Linux Qt IFW controller contract passed: " +
    path.relative(sourceRoot, path.join(ifwSourceRoot, "controller.qs")));
