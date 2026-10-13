const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");

const source = fs.readFileSync(
    path.join(__dirname, "../packaging/windows/ifw/controller.qs"), "utf8");

function fixture(running, commandLine = false) {
    const events = [];
    const callbacks = {};
    const state = {target: "C:\\Program Files\\vnm_terminal\\"};
    function signal(name) {
        return {connect(owner, callback) { callbacks[name] = () => callback.call(owner); }};
    }
    const context = vm.createContext({
        installer: {
            value(name) { return name === "RootDir" ? "C:/" : state.target; },
            toNativeSeparators(value) { return value.replace(/\//g, "\\"); },
            setValue() {}, fileExists() { return false; }, readFile() { return ""; },
            isOfflineGenerator() { return false; },
            isPackageViewer() { return false; },
            isInstaller() { return true; },
            isCommandLineInstance() { return commandLine; },
            isProcessRunning(image) {
                events.push(["query", image]);
                const normalized = image.replace(/\\/g, "/").toLowerCase();
                return running.some(item => {
                    const actual = item.replace(/\\/g, "/").toLowerCase();
                    const basename = path.posix.basename(actual);
                    return actual === normalized || basename === normalized
                        || basename.replace(/\.[^.]*$/, "") === normalized;
                });
            },
            setCanceled() { events.push(["cancel"]); },
            setDefaultPageVisible() {},
            aboutCalculateComponentsToInstall: signal("install"),
            aboutCalculateComponentsToUninstall: signal("uninstall")
        },
        QInstaller: {ComponentSelection: 0},
        QMessageBox: {
            Ok: 1,
            critical(identifier, title, message, button) {
                assert.equal(identifier, "vnm_terminal.runningSessions");
                assert.equal(title, "vnm_terminal setup");
                assert.match(message, /installation folder/);
                assert.match(message, /has not stopped any process/);
                assert.equal(button, 1);
                events.push(["message"]);
            }
        }
    });
    vm.runInContext(source, context);
    return {events, callbacks, state, construct() { vm.runInContext("new Controller()", context); }};
}

const unrelated = fixture([
    "C:/Program Files/Soter/soter_runtime/x64/OpenConsole.exe",
    "C:/Program Files/WindowsApps/WindowsTerminal/OpenConsole.exe",
    "C:/Other/vnm_terminal/vnm_terminal.exe",
    "C:/Other/vnm_terminal/vnm_terminal_host.exe",
    "C:/Other/Logonomic/vnm_hosted_worker_host.exe"
]);
unrelated.construct();
assert.equal(unrelated.events.length, 4);
assert.ok(unrelated.events.every(event => event[0] === "query"));

for (const relative of ["vnm_terminal.exe", "vnm_terminal_runtime/vnm_terminal.exe",
    "vnm_terminal_runtime/x64/OpenConsole.exe", "vnm_terminal_runtime/arm64/OpenConsole.exe"])
{
    for (const commandLine of [false, true]) {
        const owned = fixture(["C:/Program Files/vnm_terminal/" + relative], commandLine);
        assert.throws(() => owned.construct(), /vnm_terminal setup cannot continue/);
        const actions = owned.events.filter(event => event[0] !== "query").map(event => event[0]);
        assert.deepEqual(actions, commandLine ? ["cancel"] : ["message", "cancel"]);
    }
}

for (const operation of ["install", "uninstall"]) {
    const running = [];
    const changed = fixture(running);
    changed.construct();
    changed.state.target = "D:\\Selected vnm_terminal\\";
    running.push("D:/Selected vnm_terminal/vnm_terminal_runtime/vnm_terminal.exe");
    assert.throws(() => changed.callbacks[operation](), /vnm_terminal setup cannot continue/);
    assert.equal(changed.events.at(-2)[0], "message");
    assert.equal(changed.events.at(-1)[0], "cancel");
}

console.log("PASS: unrelated images admitted; four owned images refused visibly; CLI and changed target guarded.");
