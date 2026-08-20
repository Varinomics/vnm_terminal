// Source identity gate. release/dependencies.lock.json records the owned and
// third-party inputs of a release. The resolve job combines those inputs with
// the exact application commit and release tag into one immutable source tuple
// for the run. Every build, test and package job consumes that tuple, and
// package provenance records the commits it actually saw before it is signed.
//
// Ordinary CI still tracks master. What changes is that a workflow run
// resolves each branch and third-party tag once, in a small unprivileged job,
// and every build, package and test job in that run checks out the resolved
// application and dependency commits. A surface break therefore still fails
// app CI on the next run, but two jobs of one run can no longer build different
// source.
//
// The lock's commits bind only a release. A `release: published` run resolves
// from the lock and additionally re-resolves master, failing when the two
// disagree, so a release cut from a stale lock fails loudly at the moment it
// is cut. A dispatched recovery run resolves from the lock without that check,
// so an old tag can still be rebuilt after master has moved on.
//
// Modes:
//   check <root>            offline; report every drift, exit non-zero if any
//   event-state <root>      report the event-derived resolution mode
//   resolve <root>          emit the resolved commits as job outputs
//   verify-checkout <root>  compare the checked-out commits with the tuple
//   record-provenance       record the actual package tuple and payload hashes
//   verify-provenance       verify package provenance metadata
//   verify-signing-input    verify metadata and a mandatory signing payload
//   refresh <root>          rewrite the lock from the current branch heads
//
// check is the ctest gate and never touches the network. resolve and refresh
// call `gh api`, which is already how ci-windows.yml resolves a release tag.
// Workflow structure comes from tools/github_workflow_structure.js, which the
// release-manifest gate reads through as well.

const child_process = require("child_process");
const crypto = require("crypto");
const fs = require("fs");
const path = require("path");

const structure = require("./github_workflow_structure.js");

const LOCK_RELATIVE_PATH = "release/dependencies.lock.json";
const SUPPORTED_SCHEMA = 2;
const SOURCE_TUPLE_SCHEMA = 1;
const PROVENANCE_SCHEMA = 2;
const COMMIT_PATTERN = /^[0-9a-f]{40}$/;
const RESOLVE_JOB = "resolve-dependencies";
const APPLICATION_OUTPUT = "application";
const SOURCE_TUPLE_OUTPUT = "source_tuple";
const APPLICATION_REPOSITORY = "Varinomics/vnm_terminal";

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

function thirdPartyEntries(lock)
{
    return Object.keys(lock.third_party).map(
        name => Object.assign({ name: name }, lock.third_party[name]));
}

function allDependencyEntries(lock)
{
    return ownedEntries(lock).concat(thirdPartyEntries(lock));
}

// Every actions/checkout step that names another repository, with the ref it
// asks for and the directory it lands in. The application's own checkout takes
// no `repository:` and is not a dependency, so it is not reported.
function checkoutSites(workflow)
{
    const sites = [];
    for (const job of workflow.jobs.values()) {
        for (const step of job.steps) {
            if (!step.uses ||
                !/^actions\/checkout@/.test(step.uses.value)) {
                continue;
            }
            sites.push({
                stepLine: step.line,
                job: job.id,
                repository: step.with.repository
                    ? step.with.repository.value
                    : null,
                ref: step.with.ref ? step.with.ref.value : null,
                refLine: step.with.ref ? step.with.ref.line : step.line,
                path: step.with.path ? step.with.path.value : null,
                pathLine: step.with.path ? step.with.path.line : step.line
            });
        }
    }
    return sites;
}

function stepsRunning(job, phrase)
{
    return job.steps.filter(step =>
        step.run && step.run.value.indexOf(phrase) >= 0);
}

function hasResolvedTuple(step)
{
    return step.env.SOURCE_TUPLE &&
        step.env.SOURCE_TUPLE.value ===
            resolveOutputExpression(SOURCE_TUPLE_OUTPUT);
}

function resolveOutputExpression(output)
{
    return "${{ needs." + RESOLVE_JOB + ".outputs." + output + " }}";
}

// --- check ------------------------------------------------------------------

function checkLockSelfConsistency(lock)
{
    const outputs = new Set([APPLICATION_OUTPUT, SOURCE_TUPLE_OUTPUT]);
    const paths = new Set();
    for (const entry of ownedEntries(lock)) {
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

    for (const entry of thirdPartyEntries(lock)) {
        check(COMMIT_PATTERN.test(entry.commit),
            LOCK_RELATIVE_PATH + " third_party." + entry.name + ".commit \"" +
            entry.commit + "\" is not a 40-character lowercase hex commit SHA.");
        check(typeof entry.output === "string" && entry.output !== "",
            LOCK_RELATIVE_PATH + " third_party." + entry.name +
            " declares no job output.");
        check(!outputs.has(entry.output),
            LOCK_RELATIVE_PATH + " third_party." + entry.name +
            " shares the job output \"" + entry.output +
            "\" with another source. One source, one output.");
        outputs.add(entry.output);
    }
}

// Only a workflow that checks out an owned dependency needs the resolve job.
// The rule is keyed on that rather than on a list of workflow names, so a new
// workflow that grows a dependency checkout inherits the obligation.
function checkResolveJob(lock, workflows, dependencyWorkflows)
{
    for (const [workflowPath, workflow] of workflows) {
        if (!dependencyWorkflows.has(workflowPath))
            continue;

        const job = workflow.jobs.get(RESOLVE_JOB);
        if (!job) {
            violation(workflowPath + " declares no " + RESOLVE_JOB + " job. A" +
                " workflow that checks out an owned dependency must resolve" +
                " its branches once per run, or two jobs of one run can build" +
                " different source.");
            continue;
        }

        const outputs = [
            { name: "application", output: APPLICATION_OUTPUT },
            { name: "source tuple", output: SOURCE_TUPLE_OUTPUT }
        ].concat(allDependencyEntries(lock));
        for (const entry of outputs) {
            check(Object.prototype.hasOwnProperty.call(job.outputs,
                    entry.output),
                "The " + entry.name + " output \"" + entry.output +
                "\" is not declared by the " + RESOLVE_JOB + " job in " +
                workflowPath + ".");
        }
        const resolvers = stepsRunning(job,
            "node tools/dependencies_lock.js resolve");
        check(resolvers.length === 1,
            workflowPath + " " + RESOLVE_JOB + " must resolve exactly once" +
            " through \"node" +
            " tools/dependencies_lock.js resolve\"; that invocation was not" +
            " found exactly once.");
        if (resolvers.length === 1) {
            for (const obsolete of [
                "RESOLVE_FROM",
                "STALE_LOCK_IS_FATAL",
                "SOURCE_TAG"
            ]) {
                check(!Object.prototype.hasOwnProperty.call(
                        resolvers[0].env, obsolete),
                    workflowPath + " " + RESOLVE_JOB + " still wires " +
                    obsolete + ". Event/tag state belongs to" +
                    " dependencies_lock.js and must not be restated by a" +
                    " caller expression.");
            }
        }
    }
}

function checkCheckoutRefs(lock, workflows, sitesByWorkflow)
{
    const byRepository = new Map();
    for (const entry of ownedEntries(lock))
        byRepository.set(entry.repository, entry);

    const seen = new Set();
    for (const [workflowPath, workflow] of workflows) {
        const jobsWithDependencies = new Set();
        const namesByJob = new Map();
        for (const site of sitesByWorkflow.get(workflowPath)) {
            const entry = byRepository.get(site.repository);
            if (!entry) {
                violation(workflowPath + ":" + site.stepLine + " checks out " +
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
                workflowPath + ":" + site.pathLine + " checks " +
                site.repository + " out at path \"" + site.path + "\", but " +
                LOCK_RELATIVE_PATH + " owned." + entry.name +
                ".checkout_path is \"" + entry.checkout_path + "\". One" +
                " dependency, one directory, named once.");

            const expected = resolveOutputExpression(entry.output);
            if (site.ref !== expected) {
                violation(workflowPath + ":" + site.refLine + " checks out " +
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
            const job = workflow.jobs.get(site.job);
            check(job.needs.indexOf(RESOLVE_JOB) >= 0,
                workflowPath + " job " + site.job + " references " + expected +
                " but does not list " + RESOLVE_JOB + " in needs:. The" +
                " expression evaluates to the empty string, and" +
                " actions/checkout with an empty ref silently falls back to" +
                " the repository's default branch.");
        }

        // A static reader of YAML can be defeated by a step it cannot parse,
        // which would make this whole contract pass vacuously. Every job that
        // checks out a dependency therefore also proves at run time that it
        // built from the commits the run resolved.
        for (const jobId of jobsWithDependencies) {
            const job = workflow.jobs.get(jobId);
            const verifiers = stepsRunning(job,
                "dependencies_lock.js verify-checkout");
            check(verifiers.length > 0,
                workflowPath + " job " + jobId +
                " checks out a locked dependency" +
                " but never runs \"dependencies_lock.js verify-checkout\"." +
                " A checkout this contract fails to parse would otherwise" +
                " build from an unresolved commit without any signal.");
            check(verifiers.length > 0 && verifiers.every(hasResolvedTuple),
                workflowPath + " job " + jobId +
                " verifies checkouts without the" +
                " resolved source_tuple output. Every build and test job must" +
                " consume the same application, owned, and third-party" +
                " identity even when it does not clone every source itself.");

            // Partial is the dangerous shape: the dependencies the job omits
            // are resolved by whatever the build falls back to, which is the
            // provider's own default branch, while the run-time verification
            // reports on the ones that are present.
            const names = namesByJob.get(jobId);
            for (const entry of ownedEntries(lock)) {
                check(names.has(entry.name),
                    workflowPath + " job " + jobId + " checks out a locked" +
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

// The application is part of the tuple too. The resolver checkout selects the
// triggering commit or release tag; every downstream checkout must use the
// exact commit it emitted, including package and signing jobs that otherwise
// have no owned dependency checkout to expose a divergent ref.
function checkApplicationCheckouts(workflows, allSitesByWorkflow,
    dependencyWorkflows)
{
    const expected = resolveOutputExpression(APPLICATION_OUTPUT);
    for (const [workflowPath, workflow] of workflows) {
        if (!dependencyWorkflows.has(workflowPath))
            continue;

        const applicationSites = allSitesByWorkflow.get(workflowPath)
            .filter(site => site.repository === null);
        for (const site of applicationSites) {
            if (site.job === RESOLVE_JOB) {
                check(site.ref !== null &&
                        site.ref.indexOf("github.sha") >= 0 &&
                        site.ref.indexOf("refs/tags/") >= 0,
                    workflowPath + " " + RESOLVE_JOB + " must select" +
                    " github.sha for ordinary CI and a full refs/tags/<tag>" +
                    " ref for a release or tagged dispatch. A short tag can" +
                    " collide with a branch of the same name.");
                continue;
            }

            check(site.ref === expected,
                workflowPath + " job " + site.job +
                " checks the application out at \"" + site.ref + "\". Every" +
                " downstream application checkout must use " + expected +
                ", or test and package jobs can consume different commits.");
            const job = workflow.jobs.get(site.job);
            check(job.needs.indexOf(RESOLVE_JOB) >= 0,
                workflowPath + " job " + site.job +
                " checks the application out" +
                " from " + expected + " but does not list " + RESOLVE_JOB +
                " in needs:.");

            const verifiers = job.steps.filter(step => step.run &&
                /dependencies_lock\.js (?:verify-checkout|verify-provenance|verify-signing-input)/
                    .test(step.run.value));
            check(verifiers.length > 0 && verifiers.every(hasResolvedTuple),
                workflowPath + " job " + site.job +
                " checks out application" +
                " source but does not consume the resolved source_tuple." +
                " Every build, test, package, and signing job must use one" +
                " source identity.");
            check(verifiers.length > 0,
                workflowPath + " job " + site.job +
                " checks out application" +
                " source but never verifies it against the source tuple.");
        }
    }
}

function containsInOrder(text, parts)
{
    let position = 0;
    for (const part of parts) {
        const found = text.indexOf(part, position);
        if (found < 0)
            return false;
        position = found + part.length;
    }
    return true;
}

function checkThirdPartySources(root, lock, workflows)
{
    for (const entry of thirdPartyEntries(lock)) {
        for (const site of entry.clone_sites) {
            if (!exists(root, site)) {
                violation(LOCK_RELATIVE_PATH + " third_party." + entry.name +
                    " declares clone site \"" + site + "\", which does not" +
                    " exist.");
                continue;
            }

            if (/\.ya?ml$/.test(site)) {
                const workflow = workflows.get(site);
                if (!workflow) {
                    violation(site + " is absent from the normalized workflow" +
                        " IR.");
                    continue;
                }
                const expression = resolveOutputExpression(entry.output);
                const runs = [];
                for (const job of workflow.jobs.values()) {
                    for (const step of job.steps) {
                        if (step.run)
                            runs.push(step.run.value);
                    }
                }
                check(runs.some(run => containsInOrder(run, [
                    entry.repository_url,
                    "checkout --detach",
                    expression
                ])),
                    site + " does not consume the resolved " + entry.name +
                    " commit " + expression + " after cloning " +
                    entry.repository_url + ". A mutable tag is not the source" +
                    " tuple.");
                continue;
            }

            const text = readFile(root, site);
            check(text.indexOf(entry.repository_url) >= 0,
                LOCK_RELATIVE_PATH + " third_party." + entry.name +
                " declares clone site \"" + site + "\", which contains no" +
                " clone of " + entry.repository_url + ".");
            const variable = entry.name.toUpperCase().replace(/-/g, "_") +
                "_COMMIT";
            check(text.indexOf("set " + variable + "=" + entry.commit) >= 0 &&
                    text.indexOf("checkout --detach \"%" + variable + "%\"") >= 0,
                site + " does not consume the resolved " + entry.name +
                " commit " + entry.commit + " from " + LOCK_RELATIVE_PATH +
                ". Local packaging must checkout the exact commit, not only" +
                " the mutable tag that once named it.");
        }
    }
}

// Provenance is written from actual checkouts in the unprivileged package job
// and verified against the resolver output in the credential-bearing signer.
// Keeping both operations in this tool gives the static workflow gate one
// stable semantic boundary instead of parsing two copies of inline PowerShell.
function normalizedCommand(run)
{
    return run.replace(/[\\`]\s*\n/g, " ").replace(/\s+/g, " ").trim();
}

function checkProvenanceWiring(workflows)
{
    const workflowPath = ".github/workflows/ci-windows.yml";
    const workflow = workflows.get(workflowPath);
    if (!workflow)
        return;

    const recorders = [];
    const signerVerifiers = [];
    for (const job of workflow.jobs.values()) {
        for (const step of job.steps) {
            if (!step.run)
                continue;
            if (step.run.value.indexOf(
                    "dependencies_lock.js record-provenance") >= 0) {
                recorders.push({ job, step });
            }
            if (job.id === "sign-windows-package" &&
                step.run.value.indexOf(
                    "dependencies_lock.js verify-signing-input") >= 0) {
                signerVerifiers.push({ job, step });
            }
        }
    }

    check(recorders.length === 1,
        workflowPath + " must contain exactly one record-provenance step in the" +
        " package job; found " + recorders.length + ".");
    check(signerVerifiers.length === 1,
        workflowPath + " signer must contain exactly one" +
        " verify-signing-input step; found " + signerVerifiers.length + ".");

    const expected = resolveOutputExpression(SOURCE_TUPLE_OUTPUT);
    if (recorders.length === 1) {
        check(recorders[0].job.id === "package",
            workflowPath + " record-provenance must run in the package job.");
        check(hasResolvedTuple(recorders[0].step),
            workflowPath + " package provenance must record the resolved " +
            SOURCE_TUPLE_OUTPUT + " output " + expected + " exactly once.");
    }
    if (signerVerifiers.length === 1) {
        const verifier = signerVerifiers[0];
        check(hasResolvedTuple(verifier.step),
            workflowPath + " signer must verify the resolved " +
            SOURCE_TUPLE_OUTPUT + " output " + expected + " exactly once.");
        check(verifier.job.needs.indexOf(RESOLVE_JOB) >= 0,
            workflowPath + " signer verifies " + expected +
            " but does not list " +
            RESOLVE_JOB + " in needs:.");
        check(verifier.step.if === null,
            workflowPath + " signer verify-signing-input step must be" +
            " unconditional. A step-level if can skip the signing boundary.");
        const blocking = verifier.step.continueOnError === null ||
            verifier.step.continueOnError.value === "false";
        check(blocking,
            workflowPath + " signer verify-signing-input step must be" +
            " blocking; continue-on-error may not suppress a failed payload" +
            " identity check.");
        check(verifier.step.shell !== null &&
                verifier.step.shell.value === "pwsh",
            workflowPath + " signer verify-signing-input step must execute" +
            " directly in the pwsh shell. A custom shell wrapper can turn an" +
            " exact-looking command into a no-op.");
        const expectedCommand =
            "node tools/dependencies_lock.js verify-signing-input . " +
            "dist/vnm_terminal_windows_x64_build_provenance.json " +
            "dist/portable_candidate";
        check(normalizedCommand(verifier.step.run.value)
                === expectedCommand,
            workflowPath + " signer must run exactly the foreground command" +
            " \"" + expectedCommand + "\". Comments, wrappers, background" +
            " execution, or extra commands do not establish a blocking" +
            " signing-input boundary.");
    }
}

function runCheck(root)
{
    const lock = loadLock(root);

    const workflows = structure.readWorkflows(root);
    const allSitesByWorkflow = new Map();
    const sitesByWorkflow = new Map();
    const dependencyWorkflows = new Set();
    for (const [workflowPath, workflow] of workflows) {
        const allSites = checkoutSites(workflow);
        const sites = allSites.filter(site => site.repository !== null);
        allSitesByWorkflow.set(workflowPath, allSites);
        sitesByWorkflow.set(workflowPath, sites);
        if (sites.length > 0)
            dependencyWorkflows.add(workflowPath);
    }

    check(workflows.size > 0,
        "found no workflow under " + structure.WORKFLOW_DIRECTORY + "/, so" +
        " this contract cannot see how any dependency is resolved.");

    checkLockSelfConsistency(lock);
    checkResolveJob(lock, workflows, dependencyWorkflows);
    checkCheckoutRefs(lock, workflows, sitesByWorkflow);
    checkApplicationCheckouts(workflows, allSitesByWorkflow,
        dependencyWorkflows);
    checkThirdPartySources(root, lock, workflows);
    checkProvenanceWiring(workflows);

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

// git rather than the GitHub API: one of these two lives on GitLab, and a tag
// is a ref every git remote answers for. An annotated tag answers twice, and
// the peeled entry is the commit the clone checks out.
function resolveTagCommit(entry)
{
    const reference = "refs/tags/" + entry.tag;
    const result = child_process.spawnSync("git", [
        "ls-remote",
        entry.repository_url,
        reference,
        reference + "^{}"
    ], { encoding: "utf8" });

    if (result.status !== 0) {
        throw new Error("Dependency lock violation: could not resolve " +
            entry.repository_url + " tag " + entry.tag + ": " +
            String(result.stderr || result.error || "").trim());
    }

    const lines = result.stdout.split(/\r?\n/).filter(Boolean);
    const peeled = lines.filter(line => line.trim().endsWith("^{}"));
    const chosen = (peeled.length > 0 ? peeled : lines)[0];
    const commit = chosen === undefined ? "" : chosen.split(/\s/)[0];
    if (!COMMIT_PATTERN.test(commit)) {
        throw new Error("Dependency lock violation: " + entry.repository_url +
            " names no tag " + entry.tag + ", so the commit it pins cannot be" +
            " recorded.");
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

function repositoryCommit(directory)
{
    const result = child_process.spawnSync(
        "git", ["-C", directory, "rev-parse", "HEAD"],
        { encoding: "utf8" });
    const commit = result.status === 0 ? result.stdout.trim() : "";
    if (!COMMIT_PATTERN.test(commit)) {
        throw new Error("Dependency lock violation: could not read an exact" +
            " commit from " + directory + ": " +
            String(result.stderr || result.error || "").trim());
    }
    return commit;
}

function eventPayload()
{
    const eventPath = process.env.GITHUB_EVENT_PATH;
    if (!eventPath) {
        throw new Error("Dependency lock violation: GITHUB_EVENT_PATH is" +
            " empty, so the resolver cannot derive release state.");
    }
    try {
        return JSON.parse(fs.readFileSync(eventPath, "utf8"));
    }
    catch (error) {
        throw new Error("Dependency lock violation: could not read the GitHub" +
            " event payload at " + eventPath + ": " + error.message);
    }
}

// Event and tag policy has one owner. Callers provide only GitHub's immutable
// event name and payload; workflow expressions do not restate release state.
function eventState()
{
    const name = process.env.GITHUB_EVENT_NAME;
    const payload = eventPayload();
    if (name === "push" || name === "pull_request") {
        return { resolveFrom: "master", tag: null, staleLockIsFatal: false };
    }
    if (name === "workflow_dispatch") {
        const tag = payload.inputs && payload.inputs.release_tag;
        if (tag === undefined || tag === null || tag === "") {
            return {
                resolveFrom: "master",
                tag: null,
                staleLockIsFatal: false
            };
        }
        if (typeof tag !== "string") {
            throw new Error("Dependency lock violation: workflow_dispatch" +
                " release_tag is not a string.");
        }
        return { resolveFrom: "lock", tag, staleLockIsFatal: false };
    }
    if (name === "release") {
        const tag = payload.release && payload.release.tag_name;
        if (typeof tag !== "string" || tag === "") {
            throw new Error("Dependency lock violation: release event carries" +
                " no release.tag_name.");
        }
        return { resolveFrom: "lock", tag, staleLockIsFatal: true };
    }
    throw new Error("Dependency lock violation: unsupported GitHub event \"" +
        String(name) + "\". The resolver only accepts push, pull_request," +
        " workflow_dispatch, or release.");
}

function verifyTaggedApplication(root, tag, applicationCommit)
{
    const reference = "refs/tags/" + tag + "^{commit}";
    const result = child_process.spawnSync(
        "git", ["-C", root, "rev-parse", "--verify", reference],
        { encoding: "utf8" });
    const commit = result.status === 0 ? result.stdout.trim() : "";
    if (!COMMIT_PATTERN.test(commit)) {
        throw new Error("Dependency lock violation: full tag ref refs/tags/" +
            tag + " cannot be peeled to a commit in the resolver checkout: " +
            String(result.stderr || result.error || "").trim());
    }
    if (commit !== applicationCommit) {
        throw new Error("Dependency lock violation: checked-out application" +
            " commit " + applicationCommit + ", but full tag ref refs/tags/" +
            tag + " peels to " + commit + ". A branch/tag name collision or" +
            " wrong checkout must not publish a source tuple.");
    }
}

function makeSourceTuple(lock, applicationCommit, tag, resolved)
{
    const tuple = {
        schema: SOURCE_TUPLE_SCHEMA,
        application: {
            repository: process.env.GITHUB_REPOSITORY ||
                APPLICATION_REPOSITORY,
            commit: applicationCommit,
            tag: tag
        },
        owned: {},
        third_party: {}
    };
    for (const entry of ownedEntries(lock))
        tuple.owned[entry.name] = resolved[entry.name];
    for (const entry of thirdPartyEntries(lock))
        tuple.third_party[entry.name] = resolved[entry.name];
    return tuple;
}

function parseSourceTuple(lock)
{
    let tuple = null;
    try {
        tuple = JSON.parse(process.env.SOURCE_TUPLE || "null");
    }
    catch (error) {
        throw new Error("Dependency lock violation: SOURCE_TUPLE is not valid" +
            " JSON: " + error.message);
    }

    if (!tuple || tuple.schema !== SOURCE_TUPLE_SCHEMA ||
        !tuple.application || !tuple.owned || !tuple.third_party) {
        throw new Error("Dependency lock violation: SOURCE_TUPLE does not" +
            " carry schema " + SOURCE_TUPLE_SCHEMA + " application, owned," +
            " and third_party records.");
    }
    if (typeof tuple.application.repository !== "string" ||
        tuple.application.repository === "" ||
        !COMMIT_PATTERN.test(tuple.application.commit) ||
        !(tuple.application.tag === null ||
            (typeof tuple.application.tag === "string" &&
                tuple.application.tag !== ""))) {
        throw new Error("Dependency lock violation: SOURCE_TUPLE carries an" +
            " invalid application repository, commit, or tag identity.");
    }

    const sections = [
        { name: "owned", entries: ownedEntries(lock) },
        { name: "third_party", entries: thirdPartyEntries(lock) }
    ];
    for (const section of sections) {
        const expectedNames = section.entries.map(entry => entry.name).sort();
        const actualNames = Object.keys(tuple[section.name]).sort();
        if (JSON.stringify(actualNames) !== JSON.stringify(expectedNames)) {
            throw new Error("Dependency lock violation: SOURCE_TUPLE " +
                section.name + " names [" + actualNames.join(", ") +
                "], but " + LOCK_RELATIVE_PATH + " declares [" +
                expectedNames.join(", ") + "].");
        }
        for (const name of expectedNames) {
            if (!COMMIT_PATTERN.test(tuple[section.name][name])) {
                throw new Error("Dependency lock violation: SOURCE_TUPLE " +
                    section.name + "." + name + " is not a full commit SHA.");
            }
        }
    }
    return tuple;
}

function runResolve(root)
{
    const lock = loadLock(root);
    const owned = ownedEntries(lock);
    const thirdParty = thirdPartyEntries(lock);
    const entries = owned.concat(thirdParty);
    const state = eventState();
    const applicationCommit = repositoryCommit(root);
    if (state.tag !== null)
        verifyTaggedApplication(root, state.tag, applicationCommit);

    if (state.resolveFrom === "lock") {
        for (const entry of entries) {
            if (COMMIT_PATTERN.test(entry.commit))
                continue;
            throw new Error("Dependency lock violation: " + LOCK_RELATIVE_PATH +
                " source." + entry.name + ".commit \"" + entry.commit +
                "\" is not a 40-character lowercase hex commit SHA, so this" +
                " release cannot be resolved from the lock.");
        }
    }

    const resolved = {};
    for (const entry of owned) {
        resolved[entry.name] = state.resolveFrom === "lock"
            ? entry.commit
            : resolveBranchHead(entry);
    }
    for (const entry of thirdParty) {
        resolved[entry.name] = state.resolveFrom === "lock"
            ? entry.commit
            : resolveTagCommit(entry);
    }

    // A release is the one trigger that must not be allowed to ship from a
    // stale lock, so it resolves master as well and refuses to differ. A
    // dispatched recovery run deliberately skips this: rebuilding an old tag
    // has to keep working after master has moved on.
    if (state.staleLockIsFatal) {
        const head = {};
        for (const entry of owned)
            head[entry.name] = resolveBranchHead(entry);
        for (const entry of thirdParty)
            head[entry.name] = resolveTagCommit(entry);

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

    const tuple = makeSourceTuple(
        lock, applicationCommit, state.tag, resolved);
    appendLine("GITHUB_OUTPUT", APPLICATION_OUTPUT + "=" + applicationCommit);
    for (const entry of entries)
        appendLine("GITHUB_OUTPUT", entry.output + "=" + resolved[entry.name]);
    appendLine("GITHUB_OUTPUT", SOURCE_TUPLE_OUTPUT + "=" +
        JSON.stringify(tuple));

    const reported = [{ name: "application" }].concat(entries);
    const reportValues = Object.assign(
        { application: applicationCommit }, resolved);
    const report = "Resolved source tuple from `" + state.resolveFrom +
        "`:\n\n" +
        "```\n" + resolutionTable(reported, reportValues) + "\n```";
    appendLine("GITHUB_STEP_SUMMARY", report);
    process.stdout.write(report + "\n");
}

// --- verify-checkout --------------------------------------------------------

// Catches a wrong ref, a rewritten history, and a checkout that silently fell
// back to a default branch, none of which the resolve job can see.
function parseThirdPartyCheckouts(lock, required)
{
    const serialized = process.env.THIRD_PARTY_CHECKOUTS;
    if (!serialized && !required)
        return null;

    let checkouts = null;
    try {
        checkouts = JSON.parse(serialized || "null");
    }
    catch (error) {
        throw new Error("Dependency lock violation: THIRD_PARTY_CHECKOUTS is" +
            " not valid JSON: " + error.message);
    }

    const expected = thirdPartyEntries(lock).map(entry => entry.name).sort();
    const actual = checkouts && typeof checkouts === "object"
        ? Object.keys(checkouts).sort()
        : [];
    if (JSON.stringify(actual) !== JSON.stringify(expected)) {
        throw new Error("Dependency lock violation: THIRD_PARTY_CHECKOUTS" +
            " names [" + actual.join(", ") + "], but the source tuple" +
            " requires [" + expected.join(", ") + "].");
    }
    return checkouts;
}

function verifyCommitAt(name, directory, expected)
{
    if (!fs.existsSync(directory)) {
        throw new Error("Dependency lock violation: the " + name +
            " checkout is missing from " + directory + ".");
    }
    const actual = repositoryCommit(directory);
    if (actual !== expected) {
        throw new Error("Dependency lock violation: " + name + " checked out " +
            actual + ", but the source tuple resolved " + expected + ".");
    }
}

function verifyTupleCheckouts(root, workspace, lock, tuple,
    thirdPartyCheckouts)
{
    verifyCommitAt("application", root, tuple.application.commit);
    for (const entry of ownedEntries(lock)) {
        verifyCommitAt(entry.name,
            path.join(workspace, entry.checkout_path),
            tuple.owned[entry.name]);
    }
    if (thirdPartyCheckouts) {
        for (const entry of thirdPartyEntries(lock)) {
            verifyCommitAt(entry.name,
                path.resolve(workspace, thirdPartyCheckouts[entry.name]),
                tuple.third_party[entry.name]);
        }
    }
}

function runVerifyCheckout(root, workspace)
{
    const lock = loadLock(root);
    const tuple = parseSourceTuple(lock);
    const thirdPartyCheckouts = parseThirdPartyCheckouts(lock, false);
    verifyTupleCheckouts(root, workspace, lock, tuple,
        thirdPartyCheckouts);
    process.stdout.write("Checkouts match the resolved source tuple.\n");
}

function sourceEpoch(root)
{
    const result = child_process.spawnSync(
        "git", ["-C", root, "show", "-s", "--format=%ct", "HEAD"],
        { encoding: "utf8" });
    const epoch = result.status === 0 ? result.stdout.trim() : "";
    if (!/^[0-9]+$/.test(epoch)) {
        throw new Error("Dependency lock violation: could not read the" +
            " application commit timestamp from " + root + ".");
    }
    return Number(epoch);
}

function payloadFiles(root, relativeDirectory)
{
    const files = [];
    const directory = path.join(root, relativeDirectory);
    for (const entry of fs.readdirSync(directory,
        { withFileTypes: true }).sort((left, right) =>
            left.name.localeCompare(right.name))) {
        const relativePath = path.join(relativeDirectory, entry.name);
        if (entry.isDirectory())
            files.push(...payloadFiles(root, relativePath));
        else
        if (entry.isFile())
            files.push(relativePath.split(path.sep).join("/"));
        else {
            throw new Error("Dependency lock violation: payload entry " +
                relativePath + " is neither a regular file nor a directory.");
        }
    }
    return files;
}

function sha256(filePath)
{
    return crypto.createHash("sha256")
        .update(fs.readFileSync(filePath))
        .digest("hex");
}

function writePayloadManifest(payloadRoot, manifestPath)
{
    const files = payloadFiles(payloadRoot, "");
    if (files.length === 0)
        throw new Error("Dependency lock violation: package payload is empty.");
    const lines = files.map(relativePath =>
        sha256(path.join(payloadRoot, relativePath)) + "  " + relativePath);
    fs.writeFileSync(manifestPath, lines.join("\n") + "\n", "utf8");
}

function verifyPayloadManifest(payloadRoot, manifestPath)
{
    const expected = new Map();
    const lines = fs.readFileSync(manifestPath, "utf8")
        .split(/\r?\n/).filter(Boolean);
    for (const line of lines) {
        const match = /^([0-9a-f]{64})  (.+)$/.exec(line);
        if (!match)
            throw new Error("Dependency lock violation: malformed payload" +
                " manifest line: " + line);
        if (expected.has(match[2]))
            throw new Error("Dependency lock violation: duplicate payload" +
                " manifest path: " + match[2]);
        expected.set(match[2], match[1]);
    }
    if (expected.size === 0)
        throw new Error("Dependency lock violation: payload manifest is empty.");

    const actualFiles = payloadFiles(payloadRoot, "");
    if (actualFiles.length !== expected.size) {
        throw new Error("Dependency lock violation: payload contains " +
            actualFiles.length + " files, but its manifest contains " +
            expected.size + ".");
    }
    for (const relativePath of actualFiles) {
        if (!expected.has(relativePath)) {
            throw new Error("Dependency lock violation: payload contains " +
                relativePath + ", which its manifest does not list.");
        }
        const actual = sha256(path.join(payloadRoot, relativePath));
        if (actual !== expected.get(relativePath)) {
            throw new Error("Dependency lock violation: payload file " +
                relativePath + " does not match its recorded hash.");
        }
    }
}

function sameTuple(left, right)
{
    return JSON.stringify(left) === JSON.stringify(right);
}

function runRecordProvenance(root, workspace, provenancePath, payloadRoot)
{
    if (!provenancePath) {
        throw new Error("Dependency lock violation: record-provenance requires" +
            " an output JSON path.");
    }
    const lock = loadLock(root);
    const tuple = parseSourceTuple(lock);
    const thirdPartyCheckouts = parseThirdPartyCheckouts(lock, true);
    verifyTupleCheckouts(root, workspace, lock, tuple,
        thirdPartyCheckouts);

    const releaseTag = process.env.RELEASE_TAG || null;
    if (releaseTag !== tuple.application.tag) {
        throw new Error("Dependency lock violation: package RELEASE_TAG \"" +
            releaseTag + "\" does not match source tuple tag \"" +
            tuple.application.tag + "\".");
    }

    const provenance = {
        schema_version: PROVENANCE_SCHEMA,
        source_tuple: tuple,
        source_date_epoch: sourceEpoch(root),
        build_environment: {
            qt_version: process.env.QT_VERSION || null,
            qt_ifw_version: process.env.QT_IFW_VERSION || null,
            runner_image: process.env.ImageOS || null,
            runner_version: process.env.ImageVersion || null,
            workflow_run_id: process.env.GITHUB_RUN_ID || null,
            workflow_run_attempt: process.env.GITHUB_RUN_ATTEMPT || null
        }
    };
    fs.mkdirSync(path.dirname(provenancePath), { recursive: true });
    fs.writeFileSync(provenancePath,
        JSON.stringify(provenance, null, 2) + "\n", "utf8");

    if (payloadRoot) {
        writePayloadManifest(payloadRoot,
            path.join(path.dirname(provenancePath), "payload.sha256"));
    }
    process.stdout.write("Recorded package provenance for source tuple " +
        tuple.application.commit + ".\n");
}

function verifyProvenanceMetadata(root, provenancePath)
{
    if (!provenancePath) {
        throw new Error("Dependency lock violation: provenance verification" +
            " requires a provenance JSON path.");
    }
    const lock = loadLock(root);
    const expectedTuple = parseSourceTuple(lock);
    const provenance = JSON.parse(fs.readFileSync(provenancePath, "utf8"));
    if (provenance.schema_version !== PROVENANCE_SCHEMA) {
        throw new Error("Dependency lock violation: unsupported build" +
            " provenance schema " + provenance.schema_version + ".");
    }
    if (!sameTuple(provenance.source_tuple, expectedTuple)) {
        throw new Error("Dependency lock violation: package provenance does" +
            " not match the resolved source tuple.");
    }
    verifyCommitAt("application", root, expectedTuple.application.commit);

    return expectedTuple;
}

function runVerifyProvenance(root, provenancePath, unexpectedPayload)
{
    if (unexpectedPayload) {
        throw new Error("Dependency lock violation: verify-provenance checks" +
            " metadata only. Use verify-signing-input when a payload must be" +
            " identified.");
    }
    verifyProvenanceMetadata(root, provenancePath);
    process.stdout.write("Verified package provenance metadata.\n");
}

function runVerifySigningInput(root, provenancePath, payloadRoot)
{
    if (!payloadRoot) {
        throw new Error("Dependency lock violation: verify-signing-input" +
            " requires a payload directory. A signer must identify both" +
            " provenance metadata and the bytes it will sign.");
    }
    verifyProvenanceMetadata(root, provenancePath);
    verifyPayloadManifest(payloadRoot,
        path.join(path.dirname(provenancePath), "payload.sha256"));
    process.stdout.write("Verified signing provenance and payload identity.\n");
}

// --- refresh ----------------------------------------------------------------

function runRefresh(root)
{
    const lock = loadLock(root);
    for (const entry of ownedEntries(lock))
        lock.owned[entry.name].commit = resolveBranchHead(entry);
    for (const name of Object.keys(lock.third_party)) {
        lock.third_party[name].commit =
            resolveTagCommit(lock.third_party[name]);
    }

    // JSON.stringify carries every key through in the order it was found,
    // including keys this program has no rule for, so a refresh is a diff of
    // commit values and nothing else. A writer that silently dropped what it
    // did not recognise would turn a reviewed file into a file only it may
    // edit.
    fs.writeFileSync(path.join(root, LOCK_RELATIVE_PATH),
        JSON.stringify(lock, null, 2) + "\n", "utf8");

    const entries = allDependencyEntries(lock);
    const commitByName = {};
    for (const entry of entries)
        commitByName[entry.name] = entry.commit;
    process.stdout.write("Refreshed " + LOCK_RELATIVE_PATH + ":\n" +
        resolutionTable(entries, commitByName) + "\n");
}

// --- Entry point ------------------------------------------------------------

const MODES = [
    "check",
    "event-state",
    "resolve",
    "verify-checkout",
    "record-provenance",
    "verify-provenance",
    "verify-signing-input",
    "refresh"
];
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
    else if (mode === "event-state")
        process.stdout.write(JSON.stringify(eventState()) + "\n");
    else if (mode === "resolve")
        runResolve(root);
    else if (mode === "verify-checkout")
        runVerifyCheckout(root, process.argv[4] || root);
    else if (mode === "record-provenance")
        runRecordProvenance(
            root,
            process.argv[4] || root,
            process.argv[5],
            process.argv[6]);
    else if (mode === "verify-provenance")
        runVerifyProvenance(root, process.argv[4], process.argv[5]);
    else if (mode === "verify-signing-input")
        runVerifySigningInput(root, process.argv[4], process.argv[5]);
    else
        runRefresh(root);
}
catch (error) {
    process.stderr.write(error.message + "\n");
    process.exit(1);
}
