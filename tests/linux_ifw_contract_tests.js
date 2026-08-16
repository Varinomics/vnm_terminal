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

assert(controllerScript.indexOf(
    "Controller.prototype.maintenanceToolFileName = \"" +
    maintenanceToolName + "\";") >= 0,
    "the existing-installation probe must name the configured maintenance tool");
assert(/TargetDirectoryPageCallback[\s\S]*?Controller\.prototype\.offerToRemoveExistingInstallation\s*\(\s*\)/
    .test(controllerScript),
    "choosing a directory that already holds an installation must offer to remove it");
assert(/QMessageBox\.question\s*\(\s*"RemoveExistingInstallation"[\s\S]*?if\s*\(\s*answer\s*!=\s*QMessageBox\.Yes\s*\)\s*return\s*;/
    .test(controllerScript),
    "no installation may be removed without an explicit confirmation");
assert(controllerScript.indexOf(
    "[\"purge\", \"--accept-messages\", \"--confirm-command\"]") >= 0,
    "the removal must be owned by a non-interactive framework maintenance-tool purge");
assert(controllerScript.indexOf("RemoveTargetDir") < 0 &&
    !/performOperation\s*\(/.test(controllerScript),
    "the installer must not weaken target-directory validation or delete the previous installation itself");

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
    value(name) {
        if (name === "TargetDir") return state.directory;
        fail("unexpected installer value: " + name);
    },
    fileExists(candidate) {
        if (candidate === state.maintenanceToolPath)
            return state.installationPresent;
        if (candidate === state.directory) return state.directoryPresent;
        return false;
    },
    execute(program, args) {
        state.executions.push({ program, args });
        if (program !== state.maintenanceToolPath)
            fail("unexpected execution: " + program);
        if (!state.purgeStarts) return [];
        if (state.purgeExitCode === 0 && state.directoryRemovedByPurge)
            state.directoryPresent = false;
        return ["", state.purgeExitCode];
    },
};
global.gui = {
    pageWidgetByObjectName(name) {
        if (name !== "TargetDirectoryPage")
            fail("unexpected page lookup: " + name);
        return { subTitle: "" };
    },
};

eval(controllerScript);

function run(overrides) {
    const directory = overrides.directory || "/opt/vnm_terminal";
    state = Object.assign({
        directory,
        maintenanceToolPath: directory + "/" + maintenanceToolName,
        installationPresent: true,
        directoryPresent: true,
        directoryRemovedByPurge: true,
        purgeStarts: true,
        purgeExitCode: 0,
        questions: [],
        errors: [],
        executions: [],
    }, overrides);
    new Controller();
    Controller.prototype.TargetDirectoryPageCallback();
    return state;
}

let result = run({
    installationPresent: false,
    directoryPresent: false,
    answer: QMessageBox.No,
});
assert(result.questions.length === 0 && result.executions.length === 0 &&
    result.errors.length === 0,
    "a free target directory must not offer a removal");

result = run({ answer: QMessageBox.No });
assert(result.questions.length === 1 && result.executions.length === 0 &&
    result.errors.length === 0,
    "a declined removal must leave the installation in place");
assert(result.questions[0].buttons === (QMessageBox.Yes | QMessageBox.No) &&
    result.questions[0].text.indexOf(result.directory) >= 0,
    "the offer must be a Yes/No choice naming its directory");

result = run({ answer: QMessageBox.Yes });
assert(result.executions.length === 1 &&
    result.executions[0].program === result.maintenanceToolPath &&
    result.executions[0].args.join(" ") ===
        "purge --accept-messages --confirm-command",
    "acceptance must remove the installation with one confirmed maintenance-tool purge");
assert(result.errors.length === 0,
    "a completed removal must report no failure");

result = run({ answer: QMessageBox.Yes, purgeExitCode: 1 });
assert(result.errors.length === 1 &&
    result.errors[0].text.indexOf(result.directory) >= 0,
    "a failed purge must be reported and must name the retained directory");

result = run({ answer: QMessageBox.Yes, purgeStarts: false });
assert(result.errors.length === 1,
    "an unstartable maintenance tool must be reported as a failed removal");

result = run({ answer: QMessageBox.Yes, directoryRemovedByPurge: false });
assert(result.errors.length === 1,
    "a directory that survives the purge must be reported, not accepted");

process.stdout.write(
    "Linux Qt IFW controller contract passed: " + controllerScriptPath + "\n");
