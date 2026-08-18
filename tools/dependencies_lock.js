// Dependency identity gate. release/dependencies.lock.json is the single
// declaration of what a release was built from: the owned repositories, the
// branch each tracks, the commit a release was cut from, and the job output
// each is carried by. release/manifest.json answers what the release *is*;
// this file answers what it was built *from*, and the two never overlap.
//
// Ordinary CI still tracks master. What changes is that a workflow run
// resolves each branch once, in a small unprivileged job, and every build,
// package and test job in that run checks out the resolved commit. A surface
// break therefore still fails app CI on the next run, but two jobs of one run
// can no longer build different source.
//
// The lock's commits bind only a release. A `release: published` run resolves
// from the lock and additionally re-resolves master, failing when the two
// disagree, so a release cut from a stale lock fails loudly at the moment it
// is cut. A dispatched recovery run resolves from the lock without that check,
// so an old tag can still be rebuilt after master has moved on.
//
// Modes:
//   check <root>            offline; report every drift, exit non-zero if any
//   resolve <root>          emit the resolved commits as job outputs
//   verify-checkout <root>  compare the checked-out commits with the resolved
//   refresh <root>          rewrite the lock from the current branch heads
//
// check is the ctest gate and never touches the network. resolve and refresh
// call `gh api`, which is already how ci-windows.yml resolves a release tag.
// Workflow structure comes from tools/github_workflow_structure.js, which the
// release-manifest gate reads through as well.

const child_process = require("child_process");
const fs = require("fs");
const path = require("path");

const structure = require("./github_workflow_structure.js");

const LOCK_RELATIVE_PATH = "release/dependencies.lock.json";
const SUPPORTED_SCHEMA = 1;
const COMMIT_PATTERN = /^[0-9a-f]{40}$/;
const WORKFLOWS = [
    ".github/workflows/ci-windows.yml",
    ".github/workflows/ci-linux.yml",
    ".github/workflows/ci-macos.yml"
];
const RESOLVE_JOB = "resolve-dependencies";

const violations = [];

function violation(message)
{
    violations.push("Dependency lock violation: " + message);
}

function check(condition, message)
{
    if (!condition)
        violation(message);
}

function readFile(root, relativePath)
{
    return fs.readFileSync(path.join(root, relativePath), "utf8");
}

function exists(root, relativePath)
{
    return fs.existsSync(path.join(root, relativePath));
}

function loadLock(root)
{
    const lock = JSON.parse(readFile(root, LOCK_RELATIVE_PATH));
    if (lock.schema !== SUPPORTED_SCHEMA) {
        throw new Error("Dependency lock violation: " + LOCK_RELATIVE_PATH +
            " declares schema " + lock.schema + ", but this contract" +
            " understands schema " + SUPPORTED_SCHEMA + ".");
    }
    return lock;
}

function ownedEntries(lock)
{
    return Object.keys(lock.owned).map(
        name => Object.assign({ name: name }, lock.owned[name]));
}

function escapeRegExp(text)
{
    return text.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
}

// Every actions/checkout step that names another repository, with the ref it
// asks for. The application's own checkout takes no `repository:` and is not a
// dependency, so it is not reported.
function checkoutSites(lines)
{
    const sites = [];
    for (const block of structure.stepBlocks(lines)) {
        let isCheckout = false;
        let repository = null;
        let ref = null;
        let refLine = 0;
        for (let index = block.firstLine; index < block.lastLine; ++index) {
            const line = lines[index];
            if (/^\s*uses:\s*actions\/checkout@/.test(line)) {
                isCheckout = true;
                continue;
            }

            const entry = /^\s*(repository|ref):\s*(.+?)\s*$/.exec(line);
            if (!entry)
                continue;
            if (entry[1] === "repository")
                repository = entry[2].replace(/^['"]|['"]$/g, "");
            else {
                ref = entry[2].replace(/^['"]|['"]$/g, "");
                refLine = index + 1;
            }
        }
        if (!isCheckout || repository === null)
            continue;

        sites.push({
            stepLine: block.firstLine + 1,
            job: structure.jobNameAt(lines, block.firstLine),
            repository: repository,
            ref: ref,
            refLine: refLine || block.firstLine + 1
        });
    }
    return sites;
}

function resolveOutputExpression(output)
{
    return "${{ needs." + RESOLVE_JOB + ".outputs." + output + " }}";
}

// --- check ------------------------------------------------------------------

function checkLockSelfConsistency(lock)
{
    const outputs = new Set();
    const paths = new Set();
    for (const entry of ownedEntries(lock)) {
        check(/^[a-z][a-z0-9_]*$/.test(entry.name),
            LOCK_RELATIVE_PATH + " owned key \"" + entry.name + "\" is not" +
            " snake_case. Windows PowerShell 5.1 ConvertFrom-Json exposes lock" +
            " keys as object properties, and the signing job reads them as" +
            " $lock.owned.<key>.commit.");
        check(COMMIT_PATTERN.test(entry.commit),
            LOCK_RELATIVE_PATH + " owned." + entry.name + ".commit \"" +
            entry.commit + "\" is not a 40-character lowercase hex commit SHA.");

        // Owned dependencies track master. The lock records what a release was
        // cut from; it is not the dependency policy, and it has to stay
        // structurally unable to read as one.
        check(entry.branch === "master",
            LOCK_RELATIVE_PATH + " owned." + entry.name + ".branch is \"" +
            entry.branch + "\". An owned dependency must keep branch" +
            " \"master\": the lock records what a release was cut from, it is" +
            " not the dependency policy.");

        check(!outputs.has(entry.output),
            LOCK_RELATIVE_PATH + " owned." + entry.name + " shares the job" +
            " output \"" + entry.output + "\" with another entry. One" +
            " dependency, one output.");
        outputs.add(entry.output);

        check(!paths.has(entry.checkout_path),
            LOCK_RELATIVE_PATH + " owned." + entry.name + " shares the" +
            " checkout path \"" + entry.checkout_path + "\" with another" +
            " entry. One dependency, one checkout.");
        paths.add(entry.checkout_path);
    }
}

function checkResolveJob(lock, workflowText)
{
    for (const [workflow, text] of workflowText) {
        const job = new RegExp("(?:^|\\n)  " + RESOLVE_JOB +
            ":\\n([\\s\\S]*?)(?=\\n  [A-Za-z0-9_-]+:\\n|$)").exec(text);
        if (!job) {
            violation(workflow + " declares no " + RESOLVE_JOB + " job. A" +
                " workflow that checks out an owned dependency must resolve" +
                " its branches once per run, or two jobs of one run can build" +
                " different source.");
            continue;
        }

        for (const entry of ownedEntries(lock)) {
            check(job[1].indexOf("      " + entry.output + ":") >= 0,
                LOCK_RELATIVE_PATH + " owned." + entry.name + ".output \"" +
                entry.output + "\" is not declared as an output of the " +
                RESOLVE_JOB + " job in " + workflow + ".");
        }
        check(/node tools\/dependencies_lock\.js resolve/.test(job[1]),
            workflow + " " + RESOLVE_JOB + " must resolve through \"node" +
            " tools/dependencies_lock.js resolve\"; that invocation was not" +
            " found.");
    }
}

function checkCheckoutRefs(lock, workflowLines)
{
    const byRepository = new Map();
    for (const entry of ownedEntries(lock))
        byRepository.set(entry.repository, entry);

    const seen = new Set();
    for (const [workflow, lines] of workflowLines) {
        const needs = structure.jobNeeds(lines);
        const regions = structure.jobRegions(lines);
        const jobsWithDependencies = new Set();
        for (const site of checkoutSites(lines)) {
            const entry = byRepository.get(site.repository);
            if (!entry) {
                violation(workflow + ":" + site.stepLine + " checks out " +
                    site.repository + ", which " + LOCK_RELATIVE_PATH +
                    " does not declare in owned. A dependency outside the lock" +
                    " is a dependency no release can reproduce.");
                continue;
            }
            seen.add(entry.name);
            jobsWithDependencies.add(site.job);

            const expected = resolveOutputExpression(entry.output);
            if (site.ref !== expected) {
                violation(workflow + ":" + site.refLine + " checks out " +
                    site.repository + " at the ref \"" + site.ref + "\"." +
                    " Every dependency checkout must take its ref from the " +
                    RESOLVE_JOB + " job (" + expected + "), so that one" +
                    " workflow run builds one source.");
                continue;
            }

            // An expression naming a job absent from `needs:` evaluates to the
            // empty string, and actions/checkout with an empty ref silently
            // takes the repository's default branch. That is the same class of
            // quiet failure this change exists to remove, so it is a violation
            // rather than a footnote.
            check((needs.get(site.job) || []).indexOf(RESOLVE_JOB) >= 0,
                workflow + " job " + site.job + " references " + expected +
                " but does not list " + RESOLVE_JOB + " in needs:. The" +
                " expression evaluates to the empty string, and" +
                " actions/checkout with an empty ref silently falls back to" +
                " the repository's default branch.");
        }

        // A static reader of YAML can be defeated by a step it cannot parse,
        // which would make this whole contract pass vacuously. Every job that
        // checks out a dependency therefore also proves at run time that it
        // built from the commits the run resolved.
        for (const job of jobsWithDependencies) {
            const region = regions.get(job);
            const body = lines
                .slice(region.firstLine, region.lastLine)
                .join("\n");
            check(/dependencies_lock\.js verify-checkout/.test(body),
                workflow + " job " + job + " checks out a locked dependency" +
                " but never runs \"dependencies_lock.js verify-checkout\"." +
                " A checkout this contract fails to parse would otherwise" +
                " build from an unresolved commit without any signal.");
        }
    }

    for (const entry of ownedEntries(lock)) {
        check(seen.has(entry.name),
            LOCK_RELATIVE_PATH + " declares owned \"" + entry.name +
            "\", which no workflow checks out. A locked dependency nothing" +
            " checks out is a commit nothing honours.");
    }
}

function checkThirdPartyTags(root, lock)
{
    for (const name of Object.keys(lock.third_party)) {
        const entry = lock.third_party[name];
        const clonePattern = new RegExp("--branch\\s+(\\S+)[\\s\\S]{0,120}?" +
            escapeRegExp(entry.repository_url));

        for (const site of entry.clone_sites) {
            if (!exists(root, site)) {
                violation(LOCK_RELATIVE_PATH + " third_party." + name +
                    " declares clone site \"" + site + "\", which does not" +
                    " exist.");
                continue;
            }

            // The clones are wrapped across lines in bash and in batch alike,
            // so the URL is located first and the branch is read from the text
            // that precedes it.
            const lines = readFile(root, site).split(/\r?\n/);
            let cloned = false;
            lines.forEach((line, index) => {
                if (line.indexOf(entry.repository_url) < 0)
                    return;

                const statement = lines
                    .slice(Math.max(0, index - 2), index + 1)
                    .join("\n");
                const match = clonePattern.exec(statement);
                if (!match) {
                    violation(site + ":" + (index + 1) + " clones " +
                        entry.repository_url + " without a --branch, so " +
                        LOCK_RELATIVE_PATH + " third_party." + name +
                        ".tag cannot pin it.");
                    return;
                }
                cloned = true;
                check(match[1] === entry.tag,
                    site + ":" + (index + 1) + " clones " +
                    entry.repository_url + " at --branch " + match[1] +
                    ", but " + LOCK_RELATIVE_PATH + " third_party." + name +
                    ".tag is " + entry.tag + ".");
            });
            check(cloned,
                LOCK_RELATIVE_PATH + " third_party." + name + " declares clone" +
                " site \"" + site + "\", which contains no clone of " +
                entry.repository_url + ".");
        }
    }
}

function runCheck(root)
{
    const lock = loadLock(root);

    const workflowText = new Map();
    const workflowLines = new Map();
    for (const workflow of WORKFLOWS) {
        if (!exists(root, workflow)) {
            violation("workflow " + workflow + " does not exist, so this" +
                " contract cannot see how it resolves its dependencies.");
            continue;
        }
        const text = readFile(root, workflow);
        workflowText.set(workflow, text);
        workflowLines.set(workflow, text.split(/\r?\n/));
    }

    checkLockSelfConsistency(lock);
    checkResolveJob(lock, workflowText);
    checkCheckoutRefs(lock, workflowLines);
    checkThirdPartyTags(root, lock);

    if (violations.length > 0) {
        for (const message of violations)
            process.stderr.write(message + "\n");
        process.exit(1);
    }

    process.stdout.write("Dependency lock contract passed: " +
        path.join(root, LOCK_RELATIVE_PATH) + "\n");
}

// --- resolve ----------------------------------------------------------------

function resolveBranchHead(entry)
{
    const result = child_process.spawnSync("gh", [
        "api",
        "repos/" + entry.repository + "/commits/" + entry.branch,
        "--jq",
        ".sha"
    ], { encoding: "utf8" });

    if (result.status !== 0) {
        throw new Error("Dependency lock violation: could not resolve " +
            entry.repository + "@" + entry.branch + ": " +
            String(result.stderr || result.error || "").trim());
    }

    const commit = result.stdout.trim();
    if (!COMMIT_PATTERN.test(commit)) {
        throw new Error("Dependency lock violation: resolving " +
            entry.repository + "@" + entry.branch + " produced \"" + commit +
            "\", which is not a 40-character lowercase hex commit SHA.");
    }
    return commit;
}

function appendLine(variableName, line)
{
    const target = process.env[variableName];
    if (!target)
        return;
    fs.appendFileSync(target, line + "\n");
}

function resolutionTable(entries, commitByName)
{
    const width = entries.reduce(
        (widest, entry) => Math.max(widest, entry.name.length), 0);
    return entries
        .map(entry =>
            "  " + entry.name.padEnd(width) + "  " + commitByName[entry.name])
        .join("\n");
}

function runResolve(root)
{
    const lock = loadLock(root);
    const entries = ownedEntries(lock);
    const resolveFrom = process.env.RESOLVE_FROM === "lock" ? "lock" : "master";

    if (resolveFrom === "lock") {
        for (const entry of entries) {
            if (COMMIT_PATTERN.test(entry.commit))
                continue;
            throw new Error("Dependency lock violation: " + LOCK_RELATIVE_PATH +
                " owned." + entry.name + ".commit \"" + entry.commit +
                "\" is not a 40-character lowercase hex commit SHA, so this" +
                " release cannot be resolved from the lock.");
        }
    }

    const resolved = {};
    for (const entry of entries) {
        resolved[entry.name] = resolveFrom === "lock"
            ? entry.commit
            : resolveBranchHead(entry);
    }

    // A release is the one trigger that must not be allowed to ship from a
    // stale lock, so it resolves master as well and refuses to differ. A
    // dispatched recovery run deliberately skips this: rebuilding an old tag
    // has to keep working after master has moved on.
    if (process.env.STALE_LOCK_IS_FATAL === "1") {
        const head = {};
        for (const entry of entries)
            head[entry.name] = resolveBranchHead(entry);

        const stale = entries.filter(entry => head[entry.name] !== entry.commit);
        if (stale.length > 0) {
            throw new Error("Dependency lock violation: this release resolves " +
                stale[0].name + " to " + head[stale[0].name] + ", but " +
                LOCK_RELATIVE_PATH + " records " + stale[0].commit + ".\n" +
                "Run \"node tools/dependencies_lock.js refresh .\" in the" +
                " release commit, commit the result, and re-cut the tag." +
                " Current resolution:\n" + resolutionTable(entries, head));
        }
    }

    for (const entry of entries)
        appendLine("GITHUB_OUTPUT", entry.output + "=" + resolved[entry.name]);
    appendLine("GITHUB_OUTPUT", "resolved_from=" + resolveFrom);

    const report = "Resolved dependency commits from `" + resolveFrom + "`:\n\n" +
        "```\n" + resolutionTable(entries, resolved) + "\n```";
    appendLine("GITHUB_STEP_SUMMARY", report);
    process.stdout.write(report + "\n");
}

// --- verify-checkout --------------------------------------------------------

// Catches a wrong ref, a rewritten history, and a checkout that silently fell
// back to a default branch, none of which the resolve job can see.
function runVerifyCheckout(root, workspace)
{
    const lock = loadLock(root);
    const resolved = JSON.parse(process.env.DEPENDENCY_COMMITS || "{}");

    let verified = 0;
    for (const entry of ownedEntries(lock)) {
        const checkoutPath = path.join(workspace, entry.checkout_path);
        if (!fs.existsSync(checkoutPath))
            continue;
        ++verified;

        const expected = resolved[entry.output];
        check(COMMIT_PATTERN.test(expected),
            "this run resolved no commit for " + entry.name + ", so the " +
            entry.checkout_path + " checkout cannot be verified. The " +
            RESOLVE_JOB + " outputs must reach every job that checks out a" +
            " dependency.");

        const result = child_process.spawnSync(
            "git", ["-C", checkoutPath, "rev-parse", "HEAD"],
            { encoding: "utf8" });
        if (result.status !== 0) {
            violation("could not read a commit from the " + entry.name +
                " checkout at " + checkoutPath + ": " +
                String(result.stderr || result.error || "").trim());
            continue;
        }

        const actual = result.stdout.trim();
        check(actual === expected,
            entry.name + " checked out " + actual + ", but this run resolved " +
            expected + ".");
    }

    // Nothing to compare means the workspace argument is wrong, not that the
    // job is clean. A verification that can pass without looking at anything
    // is the failure this step exists to prevent.
    check(verified > 0,
        "no locked dependency checkout was found under " + workspace + "," +
        " so this job verified nothing.");

    if (violations.length > 0) {
        for (const message of violations)
            process.stderr.write(message + "\n");
        process.exit(1);
    }

    process.stdout.write("Dependency checkouts match the resolved commits.\n");
}

// --- refresh ----------------------------------------------------------------

// JSON.stringify would collapse the blank lines that separate the sections, so
// the lock is rendered rather than dumped. This is a file people read and
// review, and the only writer of it must not reformat everything it touches.
function renderLock(lock)
{
    const owned = ownedEntries(lock).map(entry =>
        "    \"" + entry.name + "\": {\n" +
        "      \"repository\": " + JSON.stringify(entry.repository) + ",\n" +
        "      \"branch\": " + JSON.stringify(entry.branch) + ",\n" +
        "      \"checkout_path\": " + JSON.stringify(entry.checkout_path) + ",\n" +
        "      \"output\": " + JSON.stringify(entry.output) + ",\n" +
        "      \"commit\": " + JSON.stringify(entry.commit) + "\n" +
        "    }");

    const thirdParty = Object.keys(lock.third_party).map(name => {
        const entry = lock.third_party[name];
        return "    \"" + name + "\": {\n" +
            "      \"repository_url\": " +
                JSON.stringify(entry.repository_url) + ",\n" +
            "      \"tag\": " + JSON.stringify(entry.tag) + ",\n" +
            "      \"clone_sites\": [\n" +
            entry.clone_sites
                .map(site => "        " + JSON.stringify(site))
                .join(",\n") + "\n" +
            "      ]\n" +
            "    }";
    });

    return "{\n" +
        "  \"schema\": " + lock.schema + ",\n\n" +
        "  \"owned\": {\n" + owned.join(",\n") + "\n  },\n\n" +
        "  \"third_party\": {\n" + thirdParty.join(",\n") + "\n  }\n" +
        "}\n";
}

function runRefresh(root)
{
    const lock = loadLock(root);
    for (const entry of ownedEntries(lock))
        lock.owned[entry.name].commit = resolveBranchHead(entry);

    fs.writeFileSync(path.join(root, LOCK_RELATIVE_PATH), renderLock(lock), "utf8");

    const entries = ownedEntries(lock);
    const commitByName = {};
    for (const entry of entries)
        commitByName[entry.name] = entry.commit;
    process.stdout.write("Refreshed " + LOCK_RELATIVE_PATH + ":\n" +
        resolutionTable(entries, commitByName) + "\n");
}

// --- Entry point ------------------------------------------------------------

const MODES = ["check", "resolve", "verify-checkout", "refresh"];
const mode = process.argv[2];
const root = process.argv[3];
if (!root || MODES.indexOf(mode) < 0) {
    process.stderr.write("usage: dependencies_lock.js " + MODES.join("|") +
        " <source-root> [workspace]\n");
    process.exit(1);
}

// A resolve failure exists to be read and pasted into the lock, so it is
// reported as the message it is rather than as a node stack trace.
try {
    if (mode === "check")
        runCheck(root);
    else if (mode === "resolve")
        runResolve(root);
    else if (mode === "verify-checkout")
        runVerifyCheckout(root, process.argv[4] || root);
    else
        runRefresh(root);
}
catch (error) {
    process.stderr.write(error.message + "\n");
    process.exit(1);
}
