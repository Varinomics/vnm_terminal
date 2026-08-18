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
const RESOLVE_JOB = "resolve-dependencies";

// What the resolve job decides through its environment: whether a release is
// built from the lock, and whether a lock master has moved past stops the
// release. Nothing else in either contract can see these two lines, and a
// release resolved from master would ship a lock that describes a different
// source, so the wiring is compared here and the program refuses to guess it at
// run time.
const RESOLVE_FROM_PATTERN = new RegExp(
    "^\\s+RESOLVE_FROM:\\s*\\$\\{\\{[^}]*github\\.event_name == 'release'" +
    "[^}]*'lock'[^}]*'master'[^}]*\\}\\}\\s*$", "m");
const STALE_LOCK_EXPRESSION =
    "${{ github.event_name == 'release' && '1' || '0' }}";

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
// asks for and the directory it lands in. The application's own checkout takes
// no `repository:` and is not a dependency, so it is not reported.
function checkoutSites(lines)
{
    const sites = [];
    for (const block of structure.stepBlocks(lines)) {
        let isCheckout = false;
        const values = {};
        const valueLines = {};
        for (let index = block.firstLine; index < block.lastLine; ++index) {
            const line = lines[index];
            if (/^\s*uses:\s*actions\/checkout@/.test(line)) {
                isCheckout = true;
                continue;
            }

            const entry = /^\s*(repository|ref|path):\s*(.+?)\s*$/.exec(line);
            if (!entry)
                continue;
            values[entry[1]] = entry[2].replace(/^['"]|['"]$/g, "");
            valueLines[entry[1]] = index + 1;
        }
        if (!isCheckout || values.repository === undefined)
            continue;

        sites.push({
            stepLine: block.firstLine + 1,
            job: structure.jobNameAt(lines, block.firstLine),
            repository: values.repository,
            ref: values.ref === undefined ? null : values.ref,
            refLine: valueLines.ref || block.firstLine + 1,
            path: values.path === undefined ? null : values.path,
            pathLine: valueLines.path || block.firstLine + 1
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

// Only a workflow that checks out an owned dependency needs the resolve job.
// The rule is keyed on that rather than on a list of workflow names, so a new
// workflow that grows a dependency checkout inherits the obligation.
function checkResolveJob(lock, workflowText, dependencyWorkflows)
{
    for (const [workflow, text] of workflowText) {
        if (!dependencyWorkflows.has(workflow))
            continue;

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

        check(RESOLVE_FROM_PATTERN.test(job[1]),
            workflow + " " + RESOLVE_JOB + " must set RESOLVE_FROM to an" +
            " expression that selects 'lock' when the run is a release and" +
            " 'master' otherwise. Without it a release resolves whatever" +
            " master holds at the moment it is cut, and " +
            LOCK_RELATIVE_PATH + " then describes source the release does not" +
            " contain.");

        check(job[1].indexOf(
                "STALE_LOCK_IS_FATAL: " + STALE_LOCK_EXPRESSION) >= 0,
            workflow + " " + RESOLVE_JOB + " must set STALE_LOCK_IS_FATAL to" +
            " \"" + STALE_LOCK_EXPRESSION + "\". That is what makes a release" +
            " cut from a lock master has moved past fail at the moment it is" +
            " cut, rather than ship a lock nobody refreshed.");
    }
}

function checkCheckoutRefs(lock, workflowLines, sitesByWorkflow)
{
    const byRepository = new Map();
    for (const entry of ownedEntries(lock))
        byRepository.set(entry.repository, entry);

    const seen = new Set();
    for (const [workflow, lines] of workflowLines) {
        const needs = structure.jobNeeds(lines);
        const regions = structure.jobRegions(lines);
        const jobsWithDependencies = new Set();
        const namesByJob = new Map();
        for (const site of sitesByWorkflow.get(workflow)) {
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
            if (!namesByJob.has(site.job))
                namesByJob.set(site.job, new Set());
            namesByJob.get(site.job).add(entry.name);

            // The lock names the directory because the build flags, the
            // provenance record and the run-time checkout verification all
            // address the dependency by it. A checkout that lands somewhere
            // else leaves the verification looking at an empty path.
            check(site.path === entry.checkout_path,
                workflow + ":" + site.pathLine + " checks " +
                site.repository + " out at path \"" + site.path + "\", but " +
                LOCK_RELATIVE_PATH + " owned." + entry.name +
                ".checkout_path is \"" + entry.checkout_path + "\". One" +
                " dependency, one directory, named once.");

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

            // Partial is the dangerous shape: the dependencies the job omits
            // are resolved by whatever the build falls back to, which is the
            // provider's own default branch, while the run-time verification
            // reports on the ones that are present.
            const names = namesByJob.get(job);
            for (const entry of ownedEntries(lock)) {
                check(names.has(entry.name),
                    workflow + " job " + job + " checks out a locked" +
                    " dependency but not " + entry.name + ". A job that takes" +
                    " some of the resolved commits and lets the build find the" +
                    " rest builds source no release can reproduce.");
            }
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
    const sitesByWorkflow = new Map();
    const dependencyWorkflows = new Set();
    for (const workflow of structure.workflowFiles(root)) {
        const text = readFile(root, workflow);
        const lines = text.split(/\r?\n/);
        const sites = checkoutSites(lines);
        workflowText.set(workflow, text);
        workflowLines.set(workflow, lines);
        sitesByWorkflow.set(workflow, sites);
        if (sites.length > 0)
            dependencyWorkflows.add(workflow);
    }

    check(workflowText.size > 0,
        "found no workflow under " + structure.WORKFLOW_DIRECTORY + "/, so" +
        " this contract cannot see how any dependency is resolved.");

    checkLockSelfConsistency(lock);
    checkResolveJob(lock, workflowText, dependencyWorkflows);
    checkCheckoutRefs(lock, workflowLines, sitesByWorkflow);
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

// Both settings are read from the environment because a workflow expression is
// the only thing that knows what triggered the run. Neither has a default: a
// missing or misspelled RESOLVE_FROM used to mean "master", which turns a
// release into an unpinned build, and the run that would notice is the one that
// no longer happens.
function resolveSource()
{
    const value = process.env.RESOLVE_FROM;
    if (value === "lock" || value === "master")
        return value;

    throw new Error("Dependency lock violation: RESOLVE_FROM is \"" +
        String(value) + "\". It must be \"lock\" for a release or a" +
        " dispatched rebuild and \"master\" for ordinary CI. This program" +
        " does not choose one for you: resolving a release from master would" +
        " build source " + LOCK_RELATIVE_PATH + " does not describe.");
}

function staleLockIsFatal()
{
    const value = process.env.STALE_LOCK_IS_FATAL;
    if (value === "0")
        return false;
    if (value === "1")
        return true;

    throw new Error("Dependency lock violation: STALE_LOCK_IS_FATAL is \"" +
        String(value) + "\". It must be \"1\" for a release, so that a lock" +
        " master has moved past stops it, and \"0\" for every other run, so" +
        " that an old tag can still be rebuilt.");
}

function runResolve(root)
{
    const lock = loadLock(root);
    const entries = ownedEntries(lock);
    const resolveFrom = resolveSource();
    const stopOnStaleLock = staleLockIsFatal();

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
    if (stopOnStaleLock) {
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

    // DEPENDENCY_COMMITS carries toJSON(needs.resolve-dependencies.outputs),
    // which is the literal null when the job forgot the needs: edge. That is
    // the same hazard the static rule catches, and this step exists precisely
    // for the case where the static reader could not see the job, so it names
    // the cause rather than failing on a missing property.
    const resolved = JSON.parse(process.env.DEPENDENCY_COMMITS || "{}");
    if (resolved === null || typeof resolved !== "object") {
        throw new Error("Dependency lock violation: this job received no " +
            RESOLVE_JOB + " outputs. Add \"needs: " + RESOLVE_JOB + "\" to it:" +
            " without that edge the ref expressions on its checkouts evaluate" +
            " to the empty string and actions/checkout silently takes each" +
            " repository's default branch.");
    }

    for (const entry of ownedEntries(lock)) {
        const checkoutPath = path.join(workspace, entry.checkout_path);
        if (!fs.existsSync(checkoutPath)) {
            violation("the " + entry.name + " checkout is missing from " +
                checkoutPath + ". " + LOCK_RELATIVE_PATH + " declares it, and" +
                " a job that builds without it builds whatever its provider" +
                " resolves on its own. A verification that skips what it" +
                " cannot find is a verification that can pass without looking" +
                " at anything.");
            continue;
        }

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
