// Drift drills for the two release gates. release/artifacts.json declares the
// artifact families a release produces and release/dependencies.lock.json
// declares what it was built from; tools/release_artifacts.js and
// tools/dependencies_lock.js are what make those declarations authoritative.
// Those programs are only worth their contract if a broken tree actually fails
// them, so each case here reintroduces one concrete defect into a throwaway
// copy of the source tree and requires the gate to report it by name.
//
// The gates used to skip a check whose subject they could not find: an absent
// declaration field, a locked checkout missing from the workspace. Skipping
// turns a deleted declaration into a silently narrower contract. A gate that
// cannot find what it was told to check must fail, and these drills are how
// that stays true.
//
// The copy is assembled from the declarations and the directories the gates
// enumerate rather than from a hardcoded file list, so a file that becomes a
// clone site or a workflow is drilled without editing this program. git is
// required: the checkout verification compares real commits.

const child_process = require("child_process");
const fs = require("fs");
const os = require("os");
const path = require("path");

const LOCK_RELATIVE_PATH = "release/dependencies.lock.json";
const WORKFLOW_DIRECTORY = ".github/workflows";

const failures = [];

function fail(caseName, detail)
{
    failures.push("Release contract drill failure: " + caseName + ": " + detail);
}

function readJson(root, relativePath)
{
    return JSON.parse(fs.readFileSync(path.join(root, relativePath), "utf8"));
}

function writeJson(root, relativePath, value)
{
    fs.writeFileSync(path.join(root, relativePath),
        JSON.stringify(value, null, 2) + "\n", "utf8");
}

function readText(root, relativePath)
{
    return fs.readFileSync(path.join(root, relativePath), "utf8");
}

function writeText(root, relativePath, text)
{
    fs.writeFileSync(path.join(root, relativePath), text, "utf8");
}

function directoryEntries(root, relativePath)
{
    const directory = path.join(root, relativePath);
    if (!fs.existsSync(directory))
        return [];

    return fs.readdirSync(directory)
        .filter(name => fs.statSync(path.join(directory, name)).isFile())
        .map(name => relativePath + "/" + name);
}

// Everything either gate opens: the declarations and the programs themselves,
// the workflow directory both enumerate, and the clone sites the lock names.
function contractFiles(sourceRoot)
{
    const lock = readJson(sourceRoot, LOCK_RELATIVE_PATH);
    const files = directoryEntries(sourceRoot, "release")
        .concat(directoryEntries(sourceRoot, "tools"))
        .concat(directoryEntries(sourceRoot, WORKFLOW_DIRECTORY));

    for (const name of Object.keys(lock.third_party))
        files.push(...lock.third_party[name].clone_sites);

    return Array.from(new Set(files))
        .filter(relativePath => fs.existsSync(path.join(sourceRoot, relativePath)));
}

function copyTree(sourceRoot, files)
{
    const root = fs.mkdtempSync(path.join(os.tmpdir(), "vnm-release-drill-"));
    for (const relativePath of files) {
        const target = path.join(root, relativePath);
        fs.mkdirSync(path.dirname(target), { recursive: true });
        fs.copyFileSync(path.join(sourceRoot, relativePath), target);
    }
    return root;
}

function removeTree(root)
{
    try {
        // git leaves its object files read-only, which Windows reports as a
        // permission error on the first attempt to unlink them.
        fs.rmSync(root, { recursive: true, force: true, maxRetries: 3 });
    }
    catch (error) {
        process.stdout.write("Could not remove the drill copy at " + root +
            ": " + error.message + "\n");
    }
}

function git(argumentList)
{
    const result = child_process.spawnSync("git", argumentList,
        { encoding: "utf8" });
    if (result.status !== 0) {
        throw new Error("git " + argumentList.join(" ") + " failed: " +
            String(result.stderr || result.error || "").trim());
    }
    return result.stdout.trim();
}

function initializeRepository(directory)
{
    fs.mkdirSync(directory, { recursive: true });
    git(["init", "--quiet", directory]);
    git(["-C", directory,
        "-c", "user.name=release contract drill",
        "-c", "user.email=drill@varinomics.invalid",
        "commit", "--quiet", "--allow-empty", "-m", "drill"]);
    return git(["-C", directory, "rev-parse", "HEAD"]);
}

function runGate(root, tool, argumentList, environment)
{
    return child_process.spawnSync(
        process.execPath,
        [path.join(root, "tools", tool)].concat(argumentList),
        {
            encoding: "utf8",
            env: Object.assign({}, process.env, environment || {})
        });
}

// A workflow neither gate reads. It resolves a dependency branch on its own and
// uploads an artifact under a name nothing prunes.
const PROBE_WORKFLOW = [
    "name: Probe",
    "",
    "on:",
    "  workflow_dispatch:",
    "",
    "jobs:",
    "  probe:",
    "    runs-on: ubuntu-24.04",
    "    steps:",
    "      - name: Checkout surface",
    "        uses: actions/checkout@11d5960a326750d5838078e36cf38b85af677262 # v4",
    "        with:",
    "          repository: Varinomics/vnm_terminal_surface",
    "          path: vnm_terminal_surface",
    "          ref: master",
    "",
    "      - name: Upload probe artifact",
    "        uses: actions/upload-artifact@ea165f8d65b6e75b540449e92b4886f43607fa02 # v4",
    "        with:",
    "          name: vnm-terminal-probe-artifact",
    "          path: dist/probe.txt",
    ""
].join("\n");

function addProbeWorkflow(root)
{
    writeText(root, WORKFLOW_DIRECTORY + "/zz-probe.yml", PROBE_WORKFLOW);
}

// --- Cases ------------------------------------------------------------------

const CASES = [
    {
        name: "an artifact renamed in a workflow but not in the declaration",
        tool: "release_artifacts.js",
        mutate: root => {
            const relativePath = WORKFLOW_DIRECTORY + "/ci-linux.yml";
            const text = readText(root, relativePath);
            if (text.indexOf("name: vnm-terminal-linux-x64\n") < 0)
                throw new Error("ci-linux.yml no longer names that artifact");
            writeText(root, relativePath, text.split("name: vnm-terminal-linux-x64\n")
                .join("name: vnm-terminal-linux-x64-v2\n"));
        },
        expect: "\"vnm-terminal-linux-x64-v2\", which release/artifacts.json" +
            " does not declare"
    },
    {
        name: "a workflow that uploads an artifact no family declares",
        tool: "release_artifacts.js",
        mutate: addProbeWorkflow,
        expect: "\"vnm-terminal-probe-artifact\", which release/artifacts.json" +
            " does not declare"
    },
    {
        name: "a retention family whose pattern would match every artifact",
        tool: "release_artifacts.js",
        mutate: root => {
            const declaration = readJson(root, "release/artifacts.json");
            const artifact = declaration.artifacts.find(
                entry => entry.prune_pattern !== undefined);
            artifact.prune_pattern = "";
            writeJson(root, "release/artifacts.json", declaration);
            return { arguments: ["retention-families", root] };
        },
        expect: "selects every artifact in the repository"
    },
    {
        name: "a dependency checked out beside the directory the lock names",
        tool: "dependencies_lock.js",
        mutate: root => {
            const relativePath = WORKFLOW_DIRECTORY + "/ci-windows.yml";
            const text = readText(root, relativePath);
            writeText(root, relativePath,
                text.replace("          path: vnm_msdf_text\n",
                    "          path: deps/vnm_msdf_text\n"));
        },
        expect: "checkout_path is \"vnm_msdf_text\""
    },
    {
        name: "a job that checks out only some of the locked dependencies",
        tool: "dependencies_lock.js",
        mutate: root => {
            const relativePath = WORKFLOW_DIRECTORY + "/ci-windows.yml";
            const text = readText(root, relativePath);
            const step = text.slice(text.indexOf("      - name: Checkout Qt dispatch"));
            writeText(root, relativePath,
                text.replace(step.slice(0, step.indexOf("\n\n") + 2), ""));
        },
        expect: "checks out a locked dependency but not vnm_qt_dispatch"
    },
    {
        name: "a locked dependency absent from the verified workspace",
        tool: "dependencies_lock.js",
        mutate: root => {
            const lock = readJson(root, LOCK_RELATIVE_PATH);
            const entries = Object.keys(lock.owned).map(
                name => Object.assign({ name }, lock.owned[name]));
            const workspace = path.join(root, "workspace");
            const commits = {};
            for (const entry of entries.slice(0, entries.length - 1)) {
                commits[entry.output] = initializeRepository(
                    path.join(workspace, entry.checkout_path));
            }
            // Resolved by the run, so the only thing wrong is that no job
            // checked it out.
            commits[entries[entries.length - 1].output] = "0".repeat(40);
            return {
                arguments: ["verify-checkout", root, workspace],
                environment: { DEPENDENCY_COMMITS: JSON.stringify(commits) }
            };
        },
        expect: "checkout is missing from"
    },
    {
        name: "a resolve job that no longer stops a release on a stale lock",
        tool: "dependencies_lock.js",
        mutate: root => {
            const relativePath = WORKFLOW_DIRECTORY + "/ci-windows.yml";
            const text = readText(root, relativePath);
            writeText(root, relativePath, text.replace(
                /^ +STALE_LOCK_IS_FATAL:.*\n/m, ""));
        },
        expect: "must set STALE_LOCK_IS_FATAL"
    },
    {
        name: "a resolve job pinned to master for every trigger",
        tool: "dependencies_lock.js",
        mutate: root => {
            const relativePath = WORKFLOW_DIRECTORY + "/ci-linux.yml";
            const text = readText(root, relativePath);
            writeText(root, relativePath, text.replace(
                /^( +)RESOLVE_FROM:.*$/m, "$1RESOLVE_FROM: master"));
        },
        expect: "must set RESOLVE_FROM"
    },
    {
        name: "a third-party tag pinned to no commit",
        tool: "dependencies_lock.js",
        mutate: root => {
            const lock = readJson(root, LOCK_RELATIVE_PATH);
            delete lock.third_party.freetype.commit;
            writeJson(root, LOCK_RELATIVE_PATH, lock);
        },
        expect: "third_party.freetype.commit"
    },
    {
        name: "a workflow that resolves a dependency branch of its own",
        tool: "dependencies_lock.js",
        mutate: addProbeWorkflow,
        expect: "declares no resolve-dependencies job"
    }
];

// --- Runner -----------------------------------------------------------------

function runBaseline(sourceRoot, files)
{
    const root = copyTree(sourceRoot, files);
    try {
        for (const tool of ["release_artifacts.js", "dependencies_lock.js"]) {
            const result = runGate(root, tool, ["check", root]);
            if (result.status !== 0) {
                fail("baseline", tool + " does not pass on an unmodified copy" +
                    " of the source tree, so no drill below proves anything: " +
                    String(result.stderr || result.error || "").trim());
            }
        }
    }
    finally {
        removeTree(root);
    }
}

function runCase(sourceRoot, files, testCase)
{
    const root = copyTree(sourceRoot, files);
    try {
        const outcome = testCase.mutate(root) || {};
        const result = runGate(root,
            testCase.tool,
            outcome.arguments || ["check", root],
            outcome.environment);
        const output = String(result.stdout || "") + String(result.stderr || "");

        if (result.status === 0) {
            fail(testCase.name, testCase.tool + " reported success. The gate" +
                " must fail and name the drift. Output: " + output.trim());
            return;
        }
        if (output.indexOf(testCase.expect) < 0) {
            fail(testCase.name, testCase.tool + " failed without naming the" +
                " drift. Expected to read \"" + testCase.expect +
                "\". Output: " + output.trim());
        }
    }
    finally {
        removeTree(root);
    }
}

const sourceRoot = process.argv[2];
if (!sourceRoot) {
    process.stderr.write("usage: release_contract_gate_tests.js <source-root>\n");
    process.exit(1);
}

const files = contractFiles(sourceRoot);
runBaseline(sourceRoot, files);
for (const testCase of CASES)
    runCase(sourceRoot, files, testCase);

if (failures.length > 0) {
    for (const message of failures)
        process.stderr.write(message + "\n");
    process.exit(1);
}

process.stdout.write("Release contract drift drills passed: " +
    CASES.length + " cases\n");
