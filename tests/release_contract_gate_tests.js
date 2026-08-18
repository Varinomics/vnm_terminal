// Drift drills for the two release gates. release/manifest.json and
// release/dependencies.lock.json declare the release surface once, and
// tools/release_manifest.js and tools/dependencies_lock.js are what make that
// declaration authoritative. Those programs are only worth their contract if a
// broken tree actually fails them, so each case here reintroduces one concrete
// defect into a throwaway copy of the source tree and requires the gate to
// report it by name.
//
// Every case is a defect that once passed. The gates used to skip a check whose
// subject they could not find - an absent manifest field, a producer that does
// not exist, a locked checkout missing from the workspace - which turns a
// deleted declaration into a silently narrower contract. A gate that cannot
// find what it was told to check must fail, and these drills are how that stays
// true.
//
// The copy is assembled from the declarations themselves rather than from a
// hardcoded file list, so a file that becomes a consumer, a reader or a
// producer is drilled without editing this program. git is required: the
// checkout verification compares real commits.

const child_process = require("child_process");
const fs = require("fs");
const os = require("os");
const path = require("path");

const MANIFEST_RELATIVE_PATH = "release/manifest.json";
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

// Everything either gate opens, derived from the two declarations so that the
// copy cannot silently lose a file a rule depends on.
function contractFiles(sourceRoot)
{
    const manifest = readJson(sourceRoot, MANIFEST_RELATIVE_PATH);
    const lock = readJson(sourceRoot, LOCK_RELATIVE_PATH);
    const files = [MANIFEST_RELATIVE_PATH, LOCK_RELATIVE_PATH]
        .concat(directoryEntries(sourceRoot, "tools"))
        .concat(directoryEntries(sourceRoot, WORKFLOW_DIRECTORY))
        .concat(manifest.consumers)
        .concat(manifest.release_assets.map(asset => asset.produced_by))
        .concat(manifest.signing.publisher_declared_in)
        .concat(manifest.signing.publisher_pattern_readers)
        .concat(manifest.qt_ifw.readers)
        .concat([manifest.signing.builder]);

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
    fs.rmSync(root, { recursive: true, force: true });
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
        name: "a deleted signing.publisher_subject_pattern",
        tool: "release_manifest.js",
        mutate: root => {
            const manifest = readJson(root, MANIFEST_RELATIVE_PATH);
            delete manifest.signing.publisher_subject_pattern;
            writeJson(root, MANIFEST_RELATIVE_PATH, manifest);
        },
        expect: "declares no signing.publisher_subject_pattern"
    },
    {
        name: "a release asset whose producer does not exist",
        tool: "release_manifest.js",
        mutate: root => {
            const manifest = readJson(root, MANIFEST_RELATIVE_PATH);
            const asset = manifest.release_assets.find(
                entry => entry.id === "windows_portable_zip");
            asset.produced_by = "build_portable.sh";
            writeJson(root, MANIFEST_RELATIVE_PATH, manifest);
        },
        expect: "as its producer, but that file does not exist"
    },
    {
        name: "a declared Qt IFW reader that does not exist",
        tool: "release_manifest.js",
        mutate: root => {
            const manifest = readJson(root, MANIFEST_RELATIVE_PATH);
            manifest.qt_ifw.readers.push("tools/provision_windows_ifw.sh");
            writeJson(root, MANIFEST_RELATIVE_PATH, manifest);
        },
        expect: "as a reader of qt_ifw, but it does not exist"
    },
    {
        name: "a retention family whose pattern would match every artifact",
        tool: "release_manifest.js",
        mutate: root => {
            const manifest = readJson(root, MANIFEST_RELATIVE_PATH);
            const artifact = manifest.actions_artifacts.find(
                entry => entry.retention.mode === "pattern");
            artifact.retention.pattern = "";
            writeJson(root, MANIFEST_RELATIVE_PATH, manifest);
            return { arguments: ["retention-families", root] };
        },
        expect: "selects every artifact in the repository"
    },
    {
        name: "a workflow the manifest does not declare",
        tool: "release_manifest.js",
        mutate: addProbeWorkflow,
        expect: "is not listed in release/manifest.json consumers"
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
        for (const tool of ["release_manifest.js", "dependencies_lock.js"]) {
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
