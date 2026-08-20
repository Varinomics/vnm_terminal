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

// GitHub Actions does not require a step name. The structural reader must see
// a step whose sequence item starts directly with uses:, or a valid workflow
// can place both a checkout and an artifact outside the release contracts.
const UNNAMED_PROBE_WORKFLOW = [
    "name: Unnamed probe",
    "",
    "on:",
    "  workflow_dispatch:",
    "",
    "jobs:",
    "  probe:",
    "    runs-on: ubuntu-24.04",
    "    steps:",
    "      - uses: actions/checkout@11d5960a326750d5838078e36cf38b85af677262 # v4",
    "        with:",
    "          repository: Varinomics/vnm_terminal_surface",
    "          path: vnm_terminal_surface",
    "          ref: master",
    "",
    "      - uses: actions/upload-artifact@ea165f8d65b6e75b540449e92b4886f43607fa02 # v4",
    "        with:",
    "          name: vnm-terminal-unnamed-probe",
    "          path: dist/probe.txt",
    ""
].join("\n");

// Quoting a scalar is semantically invisible to YAML. The normalized workflow
// IR must therefore expose quoted action identifiers exactly as it exposes
// their plain-scalar form.
const QUOTED_CHECKOUT_PROBE_WORKFLOW = PROBE_WORKFLOW.replace(
    "uses: actions/checkout@",
    "uses: 'actions/checkout@").replace(
        " # v4\n        with:", "' # v4\n        with:");

const QUOTED_ARTIFACT_PROBE_WORKFLOW = [
    "name: Quoted artifact probe",
    "",
    "on:",
    "  workflow_dispatch:",
    "",
    "jobs:",
    "  probe:",
    "    runs-on: ubuntu-24.04",
    "    steps:",
    "      - run: echo normalized unnamed run",
    "",
    "      - uses: \"actions/upload-artifact@ea165f8d65b6e75b540449e92b4886f43607fa02\" # v4",
    "        with:",
    "          name: vnm-terminal-quoted-probe",
    "          path: dist/probe.txt",
    ""
].join("\n");

const FLOW_STEP_PROBE_WORKFLOW = [
    "name: Flow step probe",
    "on: workflow_dispatch",
    "jobs:",
    "  probe:",
    "    runs-on: ubuntu-24.04",
    "    steps:",
    "      - { uses: actions/upload-artifact@ea165f8d65b6e75b540449e92b4886f43607fa02 }",
    ""
].join("\n");

const ANCHORED_STEP_PROBE_WORKFLOW = [
    "name: Anchored step probe",
    "on: workflow_dispatch",
    "jobs:",
    "  probe:",
    "    runs-on: ubuntu-24.04",
    "    steps:",
    "      - &upload",
    "        uses: actions/upload-artifact@ea165f8d65b6e75b540449e92b4886f43607fa02",
    "        with:",
    "          name: vnm-terminal-anchor-probe",
    "          path: dist/probe.txt",
    ""
].join("\n");

const ALIASED_STEP_PROBE_WORKFLOW = [
    "name: Aliased step probe",
    "x-upload: &upload",
    "  uses: actions/upload-artifact@ea165f8d65b6e75b540449e92b4886f43607fa02",
    "  with:",
    "    name: vnm-terminal-alias-probe",
    "    path: dist/probe.txt",
    "on: workflow_dispatch",
    "jobs:",
    "  probe:",
    "    runs-on: ubuntu-24.04",
    "    steps:",
    "      - *upload",
    ""
].join("\n");

function addProbeWorkflow(root)
{
    writeText(root, WORKFLOW_DIRECTORY + "/zz-probe.yml", PROBE_WORKFLOW);
}

function addUnnamedProbeWorkflow(root)
{
    writeText(root, WORKFLOW_DIRECTORY + "/zz-unnamed-probe.yml",
        UNNAMED_PROBE_WORKFLOW);
}

function addWorkflow(root, name, text)
{
    writeText(root, WORKFLOW_DIRECTORY + "/" + name, text);
}

function sourceTupleFixture(root, lock)
{
    const owned = {};
    for (const name of Object.keys(lock.owned))
        owned[name] = lock.owned[name].commit;
    const thirdParty = {};
    for (const name of Object.keys(lock.third_party))
        thirdParty[name] = lock.third_party[name].commit;
    return {
        schema: 1,
        application: {
            repository: "Varinomics/vnm_terminal",
            commit: initializeRepository(root),
            tag: "v1.4.5"
        },
        owned,
        third_party: thirdParty
    };
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
        name: "an unnamed uses step that uploads an undeclared artifact",
        tool: "release_artifacts.js",
        mutate: addUnnamedProbeWorkflow,
        expect: "\"vnm-terminal-unnamed-probe\", which release/artifacts.json" +
            " does not declare"
    },
    {
        name: "a quoted uses scalar on a dependency checkout",
        tool: "dependencies_lock.js",
        mutate: root => addWorkflow(root, "zz-quoted-checkout.yml",
            QUOTED_CHECKOUT_PROBE_WORKFLOW),
        expect: "declares no resolve-dependencies job"
    },
    {
        name: "a quoted uses scalar on an undeclared artifact upload",
        tool: "release_artifacts.js",
        mutate: root => addWorkflow(root, "zz-quoted-artifact.yml",
            QUOTED_ARTIFACT_PROBE_WORKFLOW),
        expect: "\"vnm-terminal-quoted-probe\", which release/artifacts.json" +
            " does not declare"
    },
    {
        name: "a flow-style workflow step the IR cannot normalize",
        tool: "release_artifacts.js",
        mutate: root => addWorkflow(root, "zz-flow-step.yml",
            FLOW_STEP_PROBE_WORKFLOW),
        expect: "uses a flow-style, anchored, or aliased step item"
    },
    {
        name: "an anchored workflow step the IR cannot normalize",
        tool: "release_artifacts.js",
        mutate: root => addWorkflow(root, "zz-anchor-step.yml",
            ANCHORED_STEP_PROBE_WORKFLOW),
        expect: "uses a flow-style, anchored, or aliased step item"
    },
    {
        name: "an aliased workflow step the IR cannot normalize",
        tool: "release_artifacts.js",
        mutate: root => addWorkflow(root, "zz-alias-step.yml",
            ALIASED_STEP_PROBE_WORKFLOW),
        expect: "uses a flow-style, anchored, or aliased step item"
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
            const owned = {};
            for (const entry of entries.slice(0, entries.length - 1)) {
                owned[entry.name] = initializeRepository(
                    path.join(workspace, entry.checkout_path));
            }
            // Resolved by the run, so the only thing wrong is that no job
            // checked it out.
            owned[entries[entries.length - 1].name] = "0".repeat(40);
            const thirdParty = {};
            for (const name of Object.keys(lock.third_party))
                thirdParty[name] = lock.third_party[name].commit;
            const tuple = {
                schema: 1,
                application: {
                    repository: "Varinomics/vnm_terminal",
                    commit: initializeRepository(root),
                    tag: null
                },
                owned,
                third_party: thirdParty
            };
            return {
                arguments: ["verify-checkout", root, workspace],
                environment: { SOURCE_TUPLE: JSON.stringify(tuple) }
            };
        },
        expect: "checkout is missing from"
    },
    {
        name: "a resolve job that restates stale-lock event policy",
        tool: "dependencies_lock.js",
        mutate: root => {
            const relativePath = WORKFLOW_DIRECTORY + "/ci-windows.yml";
            const text = readText(root, relativePath);
            writeText(root, relativePath, text.replace(
                /^( +GH_TOKEN:.*\n)/m,
                "$1          STALE_LOCK_IS_FATAL: 0\n"));
        },
        expect: "still wires STALE_LOCK_IS_FATAL"
    },
    {
        name: "a resolve job that restates source selection policy",
        tool: "dependencies_lock.js",
        mutate: root => {
            const relativePath = WORKFLOW_DIRECTORY + "/ci-linux.yml";
            const text = readText(root, relativePath);
            writeText(root, relativePath, text.replace(
                /^( +GH_TOKEN:.*\n)/m,
                "$1          RESOLVE_FROM: master\n"));
        },
        expect: "still wires RESOLVE_FROM"
    },
    {
        name: "a branch and release tag with the same short name",
        tool: "dependencies_lock.js",
        mutate: root => {
            const branchCommit = initializeRepository(root);
            git(["-C", root, "branch", "collision", branchCommit]);
            git(["-C", root,
                "-c", "user.name=release contract drill",
                "-c", "user.email=drill@varinomics.invalid",
                "commit", "--quiet", "--allow-empty", "-m", "tag target"]);
            git(["-C", root, "tag", "collision"]);
            git(["-C", root, "checkout", "--quiet", "--detach", branchCommit]);
            writeJson(root, "event.json",
                { inputs: { release_tag: "collision" } });
            return {
                arguments: ["resolve", root],
                environment: {
                    GITHUB_EVENT_NAME: "workflow_dispatch",
                    GITHUB_EVENT_PATH: path.join(root, "event.json")
                }
            };
        },
        expect: "A branch/tag name collision"
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
        name: "a third-party commit that clone sites do not consume",
        tool: "dependencies_lock.js",
        mutate: root => {
            const lock = readJson(root, LOCK_RELATIVE_PATH);
            lock.third_party.freetype.commit = "0".repeat(40);
            writeJson(root, LOCK_RELATIVE_PATH, lock);
        },
        expect: "does not consume the resolved freetype commit"
    },
    {
        name: "test and package jobs with divergent application commits",
        tool: "dependencies_lock.js",
        mutate: root => {
            const relativePath = WORKFLOW_DIRECTORY + "/ci-windows.yml";
            const text = readText(root, relativePath);
            const packageStart = text.indexOf("\n  package:\n");
            const checkoutStart = text.indexOf("      - name: Checkout app\n",
                packageStart);
            const expectedRef =
                "          ref: ${{ needs.resolve-dependencies.outputs." +
                "application }}\n";
            const refStart = text.indexOf(expectedRef, checkoutStart);
            if (packageStart < 0 || checkoutStart < 0 || refStart < 0)
                throw new Error("ci-windows.yml package app checkout moved");
            writeText(root, relativePath,
                text.slice(0, refStart) +
                "          ref: ${{ github.sha }}\n" +
                text.slice(refStart + expectedRef.length));
        },
        expect: "package checks the application out at"
    },
    {
        name: "a signer wired to provenance for a different source tuple",
        tool: "dependencies_lock.js",
        mutate: root => {
            const relativePath = WORKFLOW_DIRECTORY + "/ci-windows.yml";
            const text = readText(root, relativePath);
            const signerStart = text.indexOf("  sign-windows-package:\n");
            const expectedTuple =
                "          SOURCE_TUPLE: ${{ needs.resolve-dependencies." +
                "outputs.source_tuple }}\n";
            const envStart = text.indexOf(expectedTuple, signerStart);
            if (signerStart < 0 || envStart < 0)
                throw new Error("ci-windows.yml signer verification step moved");
            writeText(root, relativePath,
                text.slice(0, envStart) +
                "          SOURCE_TUPLE: ${{ needs.resolve-dependencies.outputs.chrome }}\n" +
                text.slice(envStart + expectedTuple.length));
        },
        expect: "signer must verify the resolved source_tuple output"
    },
    {
        name: "a signer statically omitting its payload argument",
        tool: "dependencies_lock.js",
        mutate: root => {
            const relativePath = WORKFLOW_DIRECTORY + "/ci-windows.yml";
            const text = readText(root, relativePath);
            const signerStart = text.indexOf("  sign-windows-package:\n");
            const payloadLine = "            dist/portable_candidate\n";
            const payloadStart = text.indexOf(payloadLine, signerStart);
            if (signerStart < 0 || payloadStart < 0)
                throw new Error("ci-windows.yml signing payload command moved");
            writeText(root, relativePath,
                text.slice(0, payloadStart) +
                text.slice(payloadStart + payloadLine.length));
        },
        expect: "must run exactly the foreground command"
    },
    {
        name: "a signer verifier guarded by if false",
        tool: "dependencies_lock.js",
        mutate: root => {
            const relativePath = WORKFLOW_DIRECTORY + "/ci-windows.yml";
            const text = readText(root, relativePath);
            const signerStart = text.indexOf("  sign-windows-package:\n");
            const stepLine = "      - name: Verify the signing payload\n";
            const stepStart = text.indexOf(stepLine, signerStart);
            if (signerStart < 0 || stepStart < 0)
                throw new Error("ci-windows.yml signer verification step moved");
            writeText(root, relativePath,
                text.slice(0, stepStart + stepLine.length) +
                "        if: false\n" +
                text.slice(stepStart + stepLine.length));
        },
        expect: "verify-signing-input step must be unconditional"
    },
    {
        name: "a signer verifier allowed to continue on failure",
        tool: "dependencies_lock.js",
        mutate: root => {
            const relativePath = WORKFLOW_DIRECTORY + "/ci-windows.yml";
            const text = readText(root, relativePath);
            const signerStart = text.indexOf("  sign-windows-package:\n");
            const stepLine = "      - name: Verify the signing payload\n";
            const stepStart = text.indexOf(stepLine, signerStart);
            if (signerStart < 0 || stepStart < 0)
                throw new Error("ci-windows.yml signer verification step moved");
            writeText(root, relativePath,
                text.slice(0, stepStart + stepLine.length) +
                "        continue-on-error: true\n" +
                text.slice(stepStart + stepLine.length));
        },
        expect: "verify-signing-input step must be blocking"
    },
    {
        name: "a signer no-op comment containing the expected command",
        tool: "dependencies_lock.js",
        mutate: root => {
            const relativePath = WORKFLOW_DIRECTORY + "/ci-windows.yml";
            const text = readText(root, relativePath);
            const signerStart = text.indexOf("  sign-windows-package:\n");
            const stepStart = text.indexOf(
                "      - name: Verify the signing payload\n", signerStart);
            const runStart = text.indexOf("        run: |\n", stepStart);
            const runEnd = text.indexOf("\n\n", runStart);
            if (signerStart < 0 || stepStart < 0 || runStart < 0 || runEnd < 0)
                throw new Error("ci-windows.yml signer verification run moved");
            const noOp = [
                "        run: |",
                "          # node tools/dependencies_lock.js " +
                    "verify-signing-input . " +
                    "dist/vnm_terminal_windows_x64_build_provenance.json " +
                    "dist/portable_candidate",
                ""
            ].join("\n");
            writeText(root, relativePath,
                text.slice(0, runStart) + noOp + text.slice(runEnd + 1));
        },
        expect: "must run exactly the foreground command"
    },
    {
        name: "a signing-input verifier invoked without a payload",
        tool: "dependencies_lock.js",
        mutate: root => ({
            arguments: ["verify-signing-input", root,
                path.join(root, "provenance.json")]
        }),
        expect: "requires a payload directory"
    },
    {
        name: "a signer receiving provenance for a different source tuple",
        tool: "dependencies_lock.js",
        mutate: root => {
            const lock = readJson(root, LOCK_RELATIVE_PATH);
            const tuple = sourceTupleFixture(root, lock);
            const provenance = {
                schema_version: 2,
                source_tuple: JSON.parse(JSON.stringify(tuple)),
                source_date_epoch: 1,
                build_environment: {}
            };
            provenance.source_tuple.third_party.freetype = "0".repeat(40);
            writeJson(root, "provenance.json", provenance);
            return {
                arguments: ["verify-signing-input", root,
                    path.join(root, "provenance.json"),
                    path.join(root, "payload")],
                environment: { SOURCE_TUPLE: JSON.stringify(tuple) }
            };
        },
        expect: "package provenance does not match the resolved source tuple"
    },
    {
        name: "a signer receiving a payload changed after provenance",
        tool: "dependencies_lock.js",
        mutate: root => {
            const lock = readJson(root, LOCK_RELATIVE_PATH);
            const tuple = sourceTupleFixture(root, lock);
            const provenance = {
                schema_version: 2,
                source_tuple: tuple,
                source_date_epoch: 1,
                build_environment: {}
            };
            const provenancePath = path.join(root, "signing", "provenance.json");
            const payloadPath = path.join(root, "payload");
            fs.mkdirSync(path.dirname(provenancePath), { recursive: true });
            fs.mkdirSync(payloadPath, { recursive: true });
            writeJson(root, "signing/provenance.json", provenance);
            writeText(root, "signing/payload.sha256",
                "0".repeat(64) + "  vnm_terminal.exe\n");
            writeText(root, "payload/vnm_terminal.exe", "changed payload\n");
            return {
                arguments: ["verify-signing-input", root,
                    provenancePath, payloadPath],
                environment: { SOURCE_TUPLE: JSON.stringify(tuple) }
            };
        },
        expect: "payload file vnm_terminal.exe does not match its recorded hash"
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

function runEventStateContracts(sourceRoot, files)
{
    const root = copyTree(sourceRoot, files);
    const cases = [
        {
            name: "push",
            eventName: "push",
            payload: {},
            expected: {
                resolveFrom: "master",
                tag: null,
                staleLockIsFatal: false
            }
        },
        {
            name: "pull request",
            eventName: "pull_request",
            payload: {},
            expected: {
                resolveFrom: "master",
                tag: null,
                staleLockIsFatal: false
            }
        },
        {
            name: "empty dispatch",
            eventName: "workflow_dispatch",
            payload: { inputs: { release_tag: "" } },
            expected: {
                resolveFrom: "master",
                tag: null,
                staleLockIsFatal: false
            }
        },
        {
            name: "release",
            eventName: "release",
            payload: { release: { tag_name: "v1.4.5" } },
            expected: {
                resolveFrom: "lock",
                tag: "v1.4.5",
                staleLockIsFatal: true
            }
        },
        {
            name: "tagged dispatch",
            eventName: "workflow_dispatch",
            payload: { inputs: { release_tag: "v1.4.5" } },
            expected: {
                resolveFrom: "lock",
                tag: "v1.4.5",
                staleLockIsFatal: false
            }
        }
    ];

    try {
        for (const testCase of cases) {
            writeJson(root, "event.json", testCase.payload);
            const result = runGate(root, "dependencies_lock.js",
                ["event-state", root], {
                    GITHUB_EVENT_NAME: testCase.eventName,
                    GITHUB_EVENT_PATH: path.join(root, "event.json")
                });
            if (result.status !== 0) {
                fail("event state " + testCase.name,
                    String(result.stderr || result.error || "").trim());
                continue;
            }
            let actual = null;
            try {
                actual = JSON.parse(result.stdout);
            }
            catch (error) {
                fail("event state " + testCase.name,
                    "did not emit JSON: " + result.stdout.trim());
                continue;
            }
            if (JSON.stringify(actual) !== JSON.stringify(testCase.expected)) {
                fail("event state " + testCase.name,
                    "expected " + JSON.stringify(testCase.expected) +
                    ", received " + JSON.stringify(actual));
            }
        }
    }
    finally {
        removeTree(root);
    }
}

function runTaggedResolveContract(sourceRoot, files)
{
    const root = copyTree(sourceRoot, files);
    try {
        const commit = initializeRepository(root);
        git(["-C", root, "tag", "v1.4.5"]);
        writeJson(root, "event.json",
            { inputs: { release_tag: "v1.4.5" } });
        const outputPath = path.join(root, "github-output.txt");
        const summaryPath = path.join(root, "github-summary.txt");
        const result = runGate(root, "dependencies_lock.js",
            ["resolve", root], {
                GITHUB_EVENT_NAME: "workflow_dispatch",
                GITHUB_EVENT_PATH: path.join(root, "event.json"),
                GITHUB_OUTPUT: outputPath,
                GITHUB_STEP_SUMMARY: summaryPath
            });
        if (result.status !== 0) {
            fail("tagged resolve",
                String(result.stderr || result.error || "").trim());
            return;
        }
        const tupleLine = fs.readFileSync(outputPath, "utf8")
            .split(/\r?\n/)
            .find(line => line.startsWith("source_tuple="));
        if (!tupleLine) {
            fail("tagged resolve", "emitted no source_tuple output");
            return;
        }
        const tuple = JSON.parse(tupleLine.slice("source_tuple=".length));
        if (tuple.application.commit !== commit ||
            tuple.application.tag !== "v1.4.5") {
            fail("tagged resolve",
                "published the wrong application/tag identity: " +
                JSON.stringify(tuple.application));
        }
    }
    finally {
        removeTree(root);
    }
}

function runProvenanceRoundTrip(sourceRoot, files)
{
    const root = copyTree(sourceRoot, files);
    try {
        const lock = readJson(root, LOCK_RELATIVE_PATH);
        const workspace = path.join(root, "workspace");
        const tuple = sourceTupleFixture(root, lock);
        const thirdPartyCheckouts = {};
        for (const name of Object.keys(lock.owned)) {
            tuple.owned[name] = initializeRepository(
                path.join(workspace, lock.owned[name].checkout_path));
        }
        for (const name of Object.keys(lock.third_party)) {
            const checkoutPath = path.join("_deps", name);
            thirdPartyCheckouts[name] = checkoutPath;
            tuple.third_party[name] = initializeRepository(
                path.join(workspace, checkoutPath));
        }

        const provenancePath = path.join(root, "signing", "provenance.json");
        const payloadPath = path.join(root, "payload");
        fs.mkdirSync(payloadPath, { recursive: true });
        writeText(root, "payload/vnm_terminal.exe", "signed payload fixture\n");
        const environment = {
            SOURCE_TUPLE: JSON.stringify(tuple),
            THIRD_PARTY_CHECKOUTS: JSON.stringify(thirdPartyCheckouts),
            RELEASE_TAG: tuple.application.tag
        };

        const record = runGate(root, "dependencies_lock.js", [
            "record-provenance",
            root,
            workspace,
            provenancePath,
            payloadPath
        ], environment);
        if (record.status !== 0) {
            fail("provenance round trip",
                "record-provenance rejected exact checkouts: " +
                String(record.stderr || record.error || "").trim());
            return;
        }

        const metadata = runGate(root, "dependencies_lock.js", [
            "verify-provenance",
            root,
            provenancePath
        ], environment);
        if (metadata.status !== 0) {
            fail("provenance round trip",
                "verify-provenance rejected the recorded metadata: " +
                String(metadata.stderr || metadata.error || "").trim());
            return;
        }

        const signingInput = runGate(root, "dependencies_lock.js", [
            "verify-signing-input",
            root,
            provenancePath,
            payloadPath
        ], environment);
        if (signingInput.status !== 0) {
            fail("provenance round trip",
                "verify-signing-input rejected the tuple or payload: " +
                String(signingInput.stderr || signingInput.error || "").trim());
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
runEventStateContracts(sourceRoot, files);
runTaggedResolveContract(sourceRoot, files);
runProvenanceRoundTrip(sourceRoot, files);
for (const testCase of CASES)
    runCase(sourceRoot, files, testCase);

if (failures.length > 0) {
    for (const message of failures)
        process.stderr.write(message + "\n");
    process.exit(1);
}

process.stdout.write("Release contract drift drills passed: " +
    CASES.length + " cases\n");
