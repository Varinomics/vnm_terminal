// Release artifact gate. release/artifacts.json declares the Actions artifact
// families a release produces, and artifact-retention.yml derives its prune
// families from it. That workflow used to carry the five artifact names itself,
// under a comment admitting that a rename there without a rename in
// ci-windows.yml silently stops pruning rather than failing. Moving the names
// here only relocates that risk unless something compares the two sides, and
// this program is that comparison.
//
// A GitHub Actions `name:` accepts only a literal or an expression, and an
// expression whose `needs:` edge is missing silently evaluates to the empty
// string, so the workflows still spell most artifact names out. This program
// extracts them and compares them with the declaration in both directions, so a
// rename on either side fails loudly and names the file and line on the other.
//
// Modes:
//   check <root>               report every drift, exit non-zero if any
//   retention-families <root>  emit the prune families artifact-retention.yml
//                              reads, one TSV row per family
//
// Workflow structure comes from tools/github_workflow_structure.js, which the
// dependency-lock gate reads through as well. An extraction that finds nothing
// where the declaration says something exists is itself a violation, so a
// workflow this program can no longer read fails rather than passing vacuously.

const fs = require("fs");
const path = require("path");

const structure = require("./github_workflow_structure.js");

const DECLARATION_RELATIVE_PATH = "release/artifacts.json";
const RETENTION_RELATIVE_PATH = ".github/workflows/artifact-retention.yml";
const SUPPORTED_SCHEMA = 1;
const VERSION_PROBE = "0.0.0";

const violations = [];

function violation(message)
{
    violations.push("Release artifact violation: " + message);
}

function check(condition, message)
{
    if (!condition)
        violation(message);
}

function reportViolations()
{
    for (const message of violations)
        process.stderr.write(message + "\n");
    process.exit(1);
}

function readFile(root, relativePath)
{
    return fs.readFileSync(path.join(root, relativePath), "utf8");
}

function loadDeclaration(root)
{
    const declaration = JSON.parse(readFile(root, DECLARATION_RELATIVE_PATH));
    if (declaration.schema !== SUPPORTED_SCHEMA) {
        throw new Error("Release artifact violation: " +
            DECLARATION_RELATIVE_PATH + " declares schema " +
            declaration.schema + ", but this program understands schema " +
            SUPPORTED_SCHEMA + ".");
    }
    return declaration;
}

// The macOS artifact name is assembled at run time from the package version, so
// it is declared as a template and compared against a rendered probe.
function renderName(template)
{
    return template.split("{version}").join(VERSION_PROBE);
}

function containsInOrder(line, parts)
{
    let position = 0;
    for (const part of parts) {
        const index = line.indexOf(part, position);
        if (index < 0)
            return false;
        position = index + part.length;
    }
    return true;
}

function retentionFamilies(declaration)
{
    return declaration.artifacts.map(artifact => ({
        id: artifact.id,
        mode: artifact.name !== undefined ? "exact" : "pattern",
        value: artifact.name !== undefined
            ? artifact.name
            : artifact.prune_pattern
    }));
}

// Every upload-artifact and download-artifact step, with the name it carries.
// A step that names no artifact is reported rather than skipped: an artifact
// this program cannot see is an artifact it cannot keep in sync.
function artifactSites(workflow)
{
    const sites = [];
    for (const job of workflow.jobs.values()) {
        for (const step of job.steps) {
            const uses = step.uses &&
                /^actions\/(upload|download)-artifact@/.exec(step.uses.value);
            if (!uses)
                continue;

            const kind = uses[1];
            if (!step.with.name) {
                violation(workflow.path + ":" + step.line +
                    " uses actions/" + kind + "-artifact but no name: was" +
                    " found in its with: block.");
                continue;
            }
            sites.push({
                workflow: workflow.path,
                nameLine: step.with.name.line,
                kind,
                name: step.with.name.value
            });
        }
    }
    return sites;
}

// This runs before every other rule and stops the run when it reports, because
// the rules that follow read these fields directly: an absent one would raise a
// stack trace instead of naming the field that is missing.
function checkDeclarationShape(declaration)
{
    check(Array.isArray(declaration.artifacts) &&
            declaration.artifacts.length > 0,
        DECLARATION_RELATIVE_PATH + " declares no artifacts.");
    if (violations.length > 0)
        return;

    for (const artifact of declaration.artifacts) {
        check(typeof artifact.id === "string" && artifact.id !== "",
            DECLARATION_RELATIVE_PATH + " declares an artifact with no id." +
            " artifact-retention.yml prints the id as the family label of" +
            " every artifact it keeps or deletes.");

        // The prune mode is derived rather than declared: a family pruned by
        // exact name has a name, and a family whose artifact name embeds the
        // version has a pattern. A separate field saying which would be a
        // second statement of the same fact, able only to disagree with it.
        const named     = artifact.name !== undefined;
        const patterned = artifact.prune_pattern !== undefined;
        check(named !== patterned,
            DECLARATION_RELATIVE_PATH + " artifact \"" + artifact.id +
            "\" must declare exactly one of name and prune_pattern. " +
            (named
                ? "Declaring both leaves the prune mode ambiguous."
                : "Declaring neither leaves it in no prune family."));
        check(!patterned || typeof artifact.name_template === "string",
            DECLARATION_RELATIVE_PATH + " artifact \"" + artifact.id +
            "\" prunes by pattern but declares no name_template, so the" +
            " pattern has no artifact name to be validated against.");
    }
}

// artifact-retention.yml holds actions: write and interpolates the value of
// every row into a gh api --jq expression, where an empty value selects every
// artifact in the repository. Both modes check this, because the emitting mode
// must not be able to produce a row the checking mode would have rejected.
function checkFamilyValues(declaration)
{
    for (const family of retentionFamilies(declaration)) {
        check(typeof family.value === "string" && family.value !== "",
            "retention family \"" + family.id + "\" would be pruned with an" +
            " empty " + family.mode + " value, which selects every artifact" +
            " in the repository.");
    }
}

// An overbroad pattern reaches artifacts of another family, and one that
// matches nothing prunes nothing. Both are silent in the retention job, which
// only ever sees the rows this program hands it.
function checkPrunePattern(declaration)
{
    for (const artifact of declaration.artifacts) {
        // An empty pattern is reported by checkFamilyValues, and the character
        // rule below would describe it as the wrong defect.
        if (!artifact.prune_pattern)
            continue;

        const pattern = artifact.prune_pattern;
        check(/^[A-Za-z0-9_^.*+?()[\]{}|-]+$/.test(pattern),
            "prune_pattern \"" + pattern + "\" for artifact \"" + artifact.id +
            "\" contains a character that cannot survive interpolation into" +
            " the gh api --jq expression in " + RETENTION_RELATIVE_PATH + ".");

        let expression = null;
        try {
            expression = new RegExp(pattern);
        }
        catch (error) {
            violation("prune_pattern \"" + pattern + "\" for artifact \"" +
                artifact.id + "\" is not a valid regular expression.");
            continue;
        }

        const probe = renderName(artifact.name_template);
        check(expression.test(probe),
            "prune_pattern \"" + pattern + "\" for artifact \"" + artifact.id +
            "\" does not match \"" + probe + "\", rendered from its own" +
            " name_template. A pattern that cannot match its own artifact" +
            " prunes nothing.");

        for (const other of declaration.artifacts) {
            if (other.id === artifact.id)
                continue;
            const otherProbe = other.name || renderName(other.name_template);
            check(!expression.test(otherProbe),
                "prune_pattern \"" + pattern + "\" for artifact \"" +
                artifact.id + "\" also matches artifact \"" + other.id +
                "\" name \"" + otherProbe + "\". One artifact must belong to" +
                " exactly one retention family.");
        }
    }
}

// A templated name is built at run time out of a shell variable, so it cannot
// be compared as a literal. Its fixed halves can be: they are what a rename of
// the artifact changes, and finding them is what keeps the prune pattern
// pointed at an artifact that is still produced under that name.
function checkNameTemplate(declaration, workflows)
{
    for (const artifact of declaration.artifacts) {
        if (!artifact.name_template)
            continue;

        const halves = artifact.name_template.split("{version}");
        let found = false;
        for (const workflow of workflows.values()) {
            for (const job of workflow.jobs.values()) {
                if (job.steps.some(step => structure.stepScalars(step)
                    .some(value => containsInOrder(value, halves)))) {
                    found = true;
                    break;
                }
            }
            if (found)
                break;
        }
        check(found,
            DECLARATION_RELATIVE_PATH + " declares artifact \"" + artifact.id +
            "\" as \"" + artifact.name_template + "\", but no normalized" +
            " workflow step builds a name out of " +
            halves.map(half => "\"" + half + "\"").join(" followed by ") + ".");
    }
}

function checkArtifactNames(declaration, sitesByWorkflow)
{
    const declaredNames = declaration.artifacts
        .filter(artifact => artifact.name)
        .map(artifact => artifact.name);
    const declaredExpressions = new Set();
    for (const artifact of declaration.artifacts) {
        for (const expression of artifact.name_expressions || [])
            declaredExpressions.add(expression);
    }

    for (const [workflow, sites] of sitesByWorkflow) {
        for (const site of sites) {
            if (site.name.startsWith("${{")) {
                check(declaredExpressions.has(site.name),
                    workflow + ":" + site.nameLine + " names an artifact with" +
                    " the expression \"" + site.name + "\", which " +
                    DECLARATION_RELATIVE_PATH + " does not list in any" +
                    " name_expressions.");
                continue;
            }
            check(declaredNames.indexOf(site.name) >= 0,
                workflow + ":" + site.nameLine + " " + site.kind + "s" +
                " artifact \"" + site.name + "\", which " +
                DECLARATION_RELATIVE_PATH + " does not declare. An artifact" +
                " outside this declaration belongs to no retention family," +
                " so nothing ever prunes it.");
        }
    }

    const everySite = [];
    for (const sites of sitesByWorkflow.values())
        everySite.push(...sites);

    for (const artifact of declaration.artifacts) {
        if (artifact.name) {
            check(everySite.some(site => site.name === artifact.name),
                DECLARATION_RELATIVE_PATH + " declares artifact \"" +
                artifact.id + "\" as \"" + artifact.name + "\", but no" +
                " workflow uploads or downloads an artifact by that name." +
                " A family that matches nothing prunes nothing.");
        }
        for (const expression of artifact.name_expressions || []) {
            check(everySite.some(site => site.name === expression),
                DECLARATION_RELATIVE_PATH + " declares artifact \"" +
                artifact.id + "\" name_expressions entry \"" + expression +
                "\", but no workflow names an artifact with it.");
        }
    }
}

function checkRetentionWorkflow(workflows, declaration)
{
    const workflow = workflows.get(RETENTION_RELATIVE_PATH);
    if (!workflow) {
        violation(RETENTION_RELATIVE_PATH + " was not found in the normalized" +
            " workflow IR.");
        return;
    }
    const families = retentionFamilies(declaration);
    const steps = [];
    for (const job of workflow.jobs.values())
        steps.push(...job.steps);

    for (const step of steps) {
        const values = structure.stepScalars(step);
        for (const family of families) {
            check(!values.some(value => value.indexOf(family.value) >= 0),
                RETENTION_RELATIVE_PATH + ":" + step.line + " names" +
                " artifact family \"" + family.value + "\" literally. The" +
                " families come from " + DECLARATION_RELATIVE_PATH + " so" +
                " that a rename can never silently stop pruning.");
        }
    }

    check(steps.some(step => step.run && step.run.value.indexOf(
        "node tools/release_artifacts.js retention-families") >= 0),
        RETENTION_RELATIVE_PATH + " must obtain its prune families by running" +
        " \"node tools/release_artifacts.js retention-families\"; that" +
        " invocation was not found.");
}

function runCheck(root)
{
    const declaration = loadDeclaration(root);

    checkDeclarationShape(declaration);
    if (violations.length > 0)
        reportViolations();

    const workflows = structure.readWorkflows(root);
    check(workflows.size > 0,
        "found no workflow under " + structure.WORKFLOW_DIRECTORY + "/, which" +
        " is where every artifact this declaration names is produced.");

    const sitesByWorkflow = new Map();
    for (const [workflowPath, workflow] of workflows)
        sitesByWorkflow.set(workflowPath, artifactSites(workflow));

    checkFamilyValues(declaration);
    checkPrunePattern(declaration);
    checkNameTemplate(declaration, workflows);
    checkArtifactNames(declaration, sitesByWorkflow);
    checkRetentionWorkflow(workflows, declaration);

    if (violations.length > 0)
        reportViolations();

    process.stdout.write("Release artifact contract passed: " +
        path.join(root, DECLARATION_RELATIVE_PATH) + "\n");
}

// The families are validated here and not only in check mode: this program must
// not be able to emit a row that selects more than the family it names.
function runRetentionFamilies(root)
{
    const declaration = loadDeclaration(root);

    checkDeclarationShape(declaration);
    if (violations.length > 0)
        reportViolations();

    checkFamilyValues(declaration);
    checkPrunePattern(declaration);
    if (violations.length > 0)
        reportViolations();

    for (const family of retentionFamilies(declaration)) {
        process.stdout.write(
            [family.mode, family.id, family.value].join("\t") + "\n");
    }
}

const mode = process.argv[2];
const root = process.argv[3];
if (!root || (mode !== "check" && mode !== "retention-families")) {
    process.stderr.write(
        "usage: release_artifacts.js check|retention-families <source-root>\n");
    process.exit(1);
}

try {
    if (mode === "check")
        runCheck(root);
    else
        runRetentionFamilies(root);
}
catch (error) {
    process.stderr.write(error.message + "\n");
    process.exit(1);
}
