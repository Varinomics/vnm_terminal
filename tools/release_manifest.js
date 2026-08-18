// Release identity gate. release/manifest.json is the single declaration of
// what a release is: the Actions artifacts it produces, the assets those
// artifacts carry, the checksum convention, and the retention families the
// pruning workflow acts on. Workflows still spell most of those names
// literally, because a GitHub Actions `name:` accepts only a literal or an
// expression, and an expression whose `needs:` edge is missing silently
// evaluates to the empty string. This program makes the literals
// authoritative-by-check instead: it extracts them from the workflows and
// scripts and compares them with the manifest in both directions, so a rename
// on either side fails loudly and names the other side's file and line.
//
// Modes:
//   check <root>               report every drift, exit non-zero if any
//   retention-families <root>  emit the prune families artifact-retention.yml
//                              reads, one TSV row per family
//
// Workflow structure comes from tools/github_workflow_structure.js, which the
// dependency-lock gate reads through as well. An extraction that finds nothing
// where the manifest says something exists is itself a violation, so a workflow
// this program can no longer read fails rather than passing vacuously.

const fs = require("fs");
const path = require("path");

const structure = require("./github_workflow_structure.js");

const MANIFEST_RELATIVE_PATH = "release/manifest.json";
const WORKFLOW_DIRECTORY = structure.WORKFLOW_DIRECTORY;
const SUPPORTED_SCHEMA = 1;
const VERSION_PROBE = "0.0.0";
const CHECKSUM_EXTENSION_PATTERN = /\.(sha1|sha224|sha256|sha384|sha512|md5)$/;

// One opaque stand-in for every substitution syntax the tree uses, so that a
// glob, a shell variable, a PowerShell subexpression, a batch variable and a
// manifest placeholder all compare equal. Displayed as {}.
const SUBSTITUTION = "\u0001";
const SHAPE_PATTERN = new RegExp(
    "vnm[_-]terminal[_-]v[A-Za-z0-9_.@" + SUBSTITUTION + "-]*", "g");

// Every fact the manifest owns on its own. A rule that compares a file against
// an absent field compares it against nothing: JavaScript renders the missing
// value as "undefined", no file contains that, and the rule reports success.
// Absence is therefore a violation of the manifest rather than a reason to
// skip, and it is reported before the rules that read these fields run.
const REQUIRED_TEXT_FIELDS = [
    "checksum.algorithm",
    "checksum.suffix",
    "qt.version",
    "qt_ifw.version",
    "qt_ifw.archive_url",
    "qt_ifw.archive_sha256",
    "qt_ifw.cache_key_template",
    "signing.builder",
    "signing.publisher",
    "signing.publisher_subject_pattern",
    "signing.final_artifact"
];

const REQUIRED_LISTS = [
    "checksum.readers",
    "actions_artifacts",
    "release_assets",
    "release_attachments",
    "consumers",
    "qt_ifw.directory_prefixes",
    "qt_ifw.readers",
    "signing.publisher_declared_in",
    "signing.publisher_pattern_readers",
    "signing.payload_binaries",
    "signing.payload_binary_readers",
    "signing.stage_binaries"
];

const violations = [];

function violation(message)
{
    violations.push("Release manifest violation: " + message);
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

function isWorkflow(relativePath)
{
    return relativePath.startsWith(WORKFLOW_DIRECTORY + "/");
}

function fieldValue(manifest, dottedPath)
{
    let value = manifest;
    for (const key of dottedPath.split(".")) {
        if (value === undefined || value === null)
            return undefined;
        value = value[key];
    }
    return value;
}

function sortedList(values)
{
    return Array.from(new Set(values)).sort().join(", ");
}

// Substitution collapse. The order matters: the outer syntaxes are collapsed
// before the bare-variable rule can reach their contents.
function normalizeShape(text)
{
    return text
        .replace(/\\([*.])/g, "$1")
        .replace(/\$\{\{[^}]*\}\}/g, SUBSTITUTION)
        .replace(/\$\([^)]*\)/g, SUBSTITUTION)
        .replace(/\$\{[^}]*\}/g, SUBSTITUTION)
        .replace(/%[A-Za-z_][A-Za-z0-9_]*%/g, SUBSTITUTION)
        .replace(/\{[A-Za-z_][A-Za-z0-9_]*\}/g, SUBSTITUTION)
        .replace(/<[A-Za-z_][A-Za-z0-9_]*>/g, SUBSTITUTION)
        .replace(/\$[A-Za-z_][A-Za-z0-9_]*/g, SUBSTITUTION)
        .replace(/\*/g, SUBSTITUTION)
        .replace(new RegExp(SUBSTITUTION + "+", "g"), SUBSTITUTION);
}

function displayShape(shape)
{
    return shape.split(SUBSTITUTION).join("{}");
}

function displayShapes(shapes)
{
    return sortedList(shapes.map(displayShape));
}

function renderTemplate(template, variant)
{
    return template
        .split("{version}").join(VERSION_PROBE)
        .split("{variant}").join(variant === undefined ? "" : variant);
}

function sweepLine(line, patternsToIgnore)
{
    let text = line;
    for (const pattern of patternsToIgnore)
        text = text.split(pattern).join(" ");

    const shapes = [];
    const normalized = normalizeShape(text);
    let match;
    SHAPE_PATTERN.lastIndex = 0;
    while ((match = SHAPE_PATTERN.exec(normalized)) !== null)
        shapes.push(match[0].replace(/[.,:;'"-]+$/, ""));
    return shapes;
}

function artifactSites(workflowPath, lines)
{
    const sites = [];
    for (const block of structure.stepBlocks(lines)) {
        let kind = null;
        for (let index = block.firstLine; index < block.lastLine; ++index) {
            const uses = /^\s*uses:\s*actions\/(upload|download)-artifact@/
                .exec(lines[index]);
            if (uses) {
                kind = uses[1];
                break;
            }
        }
        if (!kind)
            continue;

        const mapping = structure.withMapping(lines, block) || {};
        if (!mapping.name) {
            violation(workflowPath + ":" + (block.firstLine + 1) +
                " uses actions/" + kind + "-artifact but no name: was found in" +
                " its with: block. An artifact this contract cannot see is an" +
                " artifact it cannot keep in sync.");
            continue;
        }

        const paths = mapping.path
            ? (mapping.path.blockLines.length > 0
                ? mapping.path.blockLines
                : [mapping.path.value])
            : [];
        sites.push({
            workflow: workflowPath,
            stepLine: block.firstLine + 1,
            nameLine: mapping.name.line,
            job: structure.jobNameAt(lines, block.firstLine),
            kind,
            name: mapping.name.value,
            paths
        });
    }
    return sites;
}

function releaseUploadSites(workflowPath, lines)
{
    const sites = [];
    for (let index = 0; index < lines.length; ++index) {
        if (!/gh release upload/.test(lines[index]))
            continue;

        let joined = lines[index];
        let last = index;
        while (/\\\s*$/.test(lines[last]) && last + 1 < lines.length) {
            ++last;
            joined = joined.replace(/\\\s*$/, " ") + lines[last];
        }
        sites.push({
            workflow: workflowPath,
            line: index + 1,
            job: structure.jobNameAt(lines, index),
            arguments: joined.trim().split(/\s+/)
        });
        index = last;
    }
    return sites;
}

// --- Manifest ---------------------------------------------------------------

function assetShape(manifest, assetId)
{
    const asset = manifest.release_assets.find(entry => entry.id === assetId);
    if (!asset)
        return null;
    return normalizeShape(asset.template || asset.path);
}

function declaredShapes(manifest)
{
    const shapes = [];
    const sidecar = manifest.checksum.suffix;
    for (const asset of manifest.release_assets) {
        if (!asset.template)
            continue;
        shapes.push(normalizeShape(asset.template));
        shapes.push(normalizeShape(asset.template + sidecar));
    }
    for (const template of Object.values(manifest.asset_templates)) {
        shapes.push(normalizeShape(template.template));
        shapes.push(normalizeShape(template.template + sidecar));
    }
    for (const artifact of manifest.actions_artifacts) {
        if (artifact.name_template)
            shapes.push(normalizeShape(artifact.name_template));
    }
    return new Set(shapes);
}

function retentionFamilies(manifest)
{
    return manifest.actions_artifacts.map(artifact => ({
        id: artifact.id,
        mode: artifact.retention.mode,
        value: artifact.retention.mode === "exact"
            ? artifact.name
            : artifact.retention.pattern
    }));
}

function artifactProbeName(artifact)
{
    return artifact.name || renderTemplate(artifact.name_template);
}

// --- Rules ------------------------------------------------------------------

function checkIdentifier(kind, id)
{
    check(/^[a-z][a-z0-9_]*$/.test(id),
        kind + " id \"" + id + "\" is not snake_case. Windows PowerShell 5.1" +
        " ConvertFrom-Json exposes manifest keys as object properties, and the" +
        " release scripts read them as $releaseManifest.<section>.<key>.");
}

// This runs before every other rule and stops the run when it reports, because
// the rules that follow read these fields directly: an absent one would raise a
// stack trace instead of naming the manifest field that is missing.
function checkRequiredDeclarations(manifest)
{
    for (const field of REQUIRED_TEXT_FIELDS) {
        const value = fieldValue(manifest, field);
        check(typeof value === "string" && value !== "",
            MANIFEST_RELATIVE_PATH + " declares no " + field + ". A fact this" +
            " contract compares files against must be present and non-empty:" +
            " an absent one is compared as \"undefined\", which no file" +
            " contains, so every copy of it would pass.");
    }

    for (const field of REQUIRED_LISTS) {
        const value = fieldValue(manifest, field);
        check(Array.isArray(value) && value.length > 0,
            MANIFEST_RELATIVE_PATH + " declares no " + field + " entries. A" +
            " list this contract iterates must not be absent or empty: an" +
            " empty one is a rule that inspects nothing.");
    }

    check(manifest.asset_templates !== null &&
            typeof manifest.asset_templates === "object",
        MANIFEST_RELATIVE_PATH + " declares no asset_templates object.");

    for (const artifact of manifest.actions_artifacts || []) {
        const retention = artifact.retention;
        check(retention !== undefined && retention !== null &&
                (retention.mode === "exact" || retention.mode === "pattern"),
            "actions_artifacts[\"" + artifact.id + "\"] declares no" +
            " retention mode of \"exact\" or \"pattern\"." +
            " artifact-retention.yml prunes by family, and an artifact in no" +
            " family is an artifact nothing prunes.");
    }
}

function checkManifestSelfConsistency(root, manifest)
{
    const names = new Map();
    for (const artifact of manifest.actions_artifacts) {
        checkIdentifier("artifact", artifact.id);

        if (artifact.name) {
            const previous = names.get(artifact.name);
            if (previous !== undefined) {
                violation("artifacts \"" + previous + "\" and \"" +
                    artifact.id + "\" both declare the name \"" +
                    artifact.name + "\". Two ids with one name make retention" +
                    " ambiguous.");
            }
            names.set(artifact.name, artifact.id);
        }

        check(Boolean(artifact.contents) || Boolean(artifact.selection),
            "actions_artifacts[\"" + artifact.id + "\"] declares neither" +
            " contents nor selection. An artifact whose upload paths this" +
            " contract cannot see is an artifact it cannot keep in sync.");

        for (const content of artifact.contents || []) {
            check(assetShape(manifest, content.asset) !== null,
                "actions_artifacts[\"" + artifact.id + "\"].contents" +
                " references asset \"" + content.asset + "\", which" +
                " release_assets does not declare.");
        }
    }

    for (const attachment of manifest.release_attachments) {
        check(Array.isArray(attachment.source_artifacts) &&
                attachment.source_artifacts.length > 0,
            "release_attachments for job \"" + attachment.job + "\" in " +
            attachment.workflow + " declares no source_artifacts. The job" +
            " uploads whatever its download directory holds, so which" +
            " artifacts it may download is part of what a release is.");

        for (const id of attachment.source_artifacts || []) {
            check(manifest.actions_artifacts.some(entry => entry.id === id),
                "release_attachments for job \"" + attachment.job +
                "\" names source artifact \"" + id + "\", which" +
                " actions_artifacts does not declare.");
        }

        for (const entry of attachment.assets || []) {
            check(assetShape(manifest, entry.asset) !== null,
                "release_attachments for job \"" + attachment.job +
                "\" references asset \"" + entry.asset + "\", which" +
                " release_assets does not declare.");
        }
    }

    for (const asset of manifest.release_assets) {
        checkIdentifier("release asset", asset.id);
        if (asset.variant_of) {
            check(manifest.asset_templates[asset.variant_of] !== undefined,
                "release asset \"" + asset.id + "\" is a variant of \"" +
                asset.variant_of + "\", which asset_templates does not" +
                " declare.");
        }
    }

    check(manifest.checksum.suffix === "." + manifest.checksum.algorithm,
        "checksum.suffix \"" + manifest.checksum.suffix + "\" does not match" +
        " checksum.algorithm \"" + manifest.checksum.algorithm + "\".");

    for (const consumer of manifest.consumers) {
        check(exists(root, consumer),
            MANIFEST_RELATIVE_PATH + " lists consumer \"" + consumer +
            "\", which does not exist. A renamed consumer silently narrows the" +
            " checks that guard the release surface.");
    }
}

// A template exists because more than one release asset is rendered from it,
// and the variants are what say which. Without this comparison the map is a
// comment: each asset is checked against its producer through its own shape, so
// a variant could name a suffix nothing produces and an asset could be a
// variant of a template that never renders it.
function checkAssetTemplates(manifest)
{
    for (const id of Object.keys(manifest.asset_templates)) {
        const template = manifest.asset_templates[id];
        checkIdentifier("asset template", id);

        const variants = template.variants;
        if (variants === null || typeof variants !== "object" ||
                Object.keys(variants).length === 0) {
            violation("asset_templates[\"" + id + "\"] declares no variants." +
                " A shape with one rendering is a release asset, not a" +
                " template.");
            continue;
        }

        const rendered = new Map();
        for (const variant of Object.keys(variants)) {
            rendered.set(
                template.template.split("{variant}").join(variants[variant]),
                variant);
        }

        const assets = manifest.release_assets
            .filter(asset => asset.variant_of === id);
        for (const asset of assets) {
            check(rendered.has(asset.template),
                "release asset \"" + asset.id + "\" is a variant of \"" + id +
                "\", but no variant of that template renders \"" +
                asset.template + "\". The template renders " +
                sortedList(Array.from(rendered.keys())) + ".");
        }
        for (const [shape, variant] of rendered) {
            check(assets.some(asset => asset.template === shape),
                "asset_templates[\"" + id + "\"].variants." + variant +
                " renders \"" + shape + "\", which no release asset declares." +
                " A variant nothing is rendered from is a suffix nobody" +
                " produces.");
        }
    }
}

// A workflow the manifest does not list is a workflow no rule below reads. The
// artifact names, the upload paths and the release attachments of such a file
// are outside this contract entirely, which is the one outcome a contract that
// exists to make names authoritative cannot allow.
function checkWorkflowCoverage(root, manifest)
{
    const declared = new Set(manifest.consumers);
    const workflows = structure.workflowFiles(root);

    check(workflows.length > 0,
        "found no workflow under " + WORKFLOW_DIRECTORY + "/, which " +
        MANIFEST_RELATIVE_PATH + " says produces every release artifact.");

    for (const workflow of workflows) {
        check(declared.has(workflow),
            workflow + " is not listed in " + MANIFEST_RELATIVE_PATH +
            " consumers, so no rule in this contract reads it. A workflow" +
            " outside the contract can upload an artifact no retention family" +
            " prunes and attach an asset no release declares.");
    }
}

function checkRetentionFamilies(manifest)
{
    for (const artifact of manifest.actions_artifacts) {
        const retention = artifact.retention;
        if (retention.mode === "exact") {
            check(Boolean(artifact.name),
                "actions_artifacts[\"" + artifact.id + "\"] prunes by exact" +
                " name but declares no name.");
            continue;
        }

        const pattern = retention.pattern;
        check(/^[A-Za-z0-9_^.*+?()[\]{}|-]+$/.test(pattern),
            "retention pattern \"" + pattern + "\" for artifact \"" +
            artifact.id + "\" contains a character that cannot survive" +
            " interpolation into the gh api --jq expression in" +
            " artifact-retention.yml.");

        let expression = null;
        try {
            expression = new RegExp(pattern);
        }
        catch (error) {
            violation("retention pattern \"" + pattern + "\" for artifact \"" +
                artifact.id + "\" is not a valid regular expression.");
            continue;
        }

        const probe = artifactProbeName(artifact);
        check(expression.test(probe),
            "retention pattern \"" + pattern + "\" for artifact \"" +
            artifact.id + "\" does not match \"" + probe + "\", rendered from" +
            " its own name_template. A pattern that cannot match its own" +
            " artifact prunes nothing.");

        for (const other of manifest.actions_artifacts) {
            if (other.id === artifact.id)
                continue;
            const otherProbe = artifactProbeName(other);
            check(!expression.test(otherProbe),
                "retention pattern \"" + pattern + "\" for artifact \"" +
                artifact.id + "\" also matches artifact \"" + other.id +
                "\" name \"" + otherProbe + "\". One artifact must belong to" +
                " exactly one retention family.");
        }
    }
}

function checkArtifactNames(manifest, sitesByWorkflow)
{
    const declaredNames = manifest.actions_artifacts
        .filter(artifact => artifact.name)
        .map(artifact => artifact.name);
    const declaredExpressions = new Set();
    for (const artifact of manifest.actions_artifacts) {
        for (const expression of artifact.name_expressions || [])
            declaredExpressions.add(expression);
    }

    for (const [workflow, sites] of sitesByWorkflow) {
        for (const site of sites) {
            if (site.name.startsWith("${{")) {
                check(declaredExpressions.has(site.name),
                    workflow + ":" + site.nameLine + " names an artifact with" +
                    " the expression \"" + site.name + "\", which " +
                    MANIFEST_RELATIVE_PATH + " does not list in any" +
                    " actions_artifacts[].name_expressions.");
                continue;
            }
            check(declaredNames.indexOf(site.name) >= 0,
                workflow + ":" + site.nameLine + " " + site.kind + "s" +
                " artifact \"" + site.name + "\", which " +
                MANIFEST_RELATIVE_PATH + " does not declare. Declared names: " +
                sortedList(declaredNames) + ".");
        }
    }

    for (const artifact of manifest.actions_artifacts) {
        const sites = sitesByWorkflow.get(artifact.workflow) || [];
        if (artifact.name) {
            const matching = sites.filter(site => site.name === artifact.name);
            if (matching.length === 0) {
                // The sites that name something the manifest does not declare
                // are the counterpart of this one, so they are what a reader
                // has to reconcile.
                const undeclared = sites.filter(site =>
                    !site.name.startsWith("${{") &&
                    declaredNames.indexOf(site.name) < 0);
                const hint = undeclared.length > 0
                    ? " The undeclared artifact names in that workflow are: " +
                        sortedList(undeclared.map(site =>
                            "\"" + site.name + "\" at " + site.workflow + ":" +
                            site.nameLine)) + "."
                    : " Every other artifact name in that workflow is" +
                        " declared, so nothing there produces it.";
                violation(MANIFEST_RELATIVE_PATH + " declares" +
                    " actions_artifacts[\"" + artifact.id + "\"] as \"" +
                    artifact.name + "\", but " + artifact.workflow +
                    " has no artifact step by that name." + hint);
            }

            const producers = matching.filter(site => site.kind === "upload");
            check(producers.length <= 1,
                MANIFEST_RELATIVE_PATH + " artifact \"" + artifact.id +
                "\" is uploaded by " + producers.length + " steps (" +
                producers
                    .map(site => site.workflow + ":" + site.stepLine)
                    .join(", ") +
                "). artifact-retention.yml keeps one artifact per family, so a" +
                " second producer is silently discarded.");
        }

        for (const expression of artifact.name_expressions || []) {
            check(sites.some(site => site.name === expression),
                MANIFEST_RELATIVE_PATH + " declares actions_artifacts[\"" +
                artifact.id + "\"].name_expressions entry \"" + expression +
                "\", but " + artifact.workflow + " names no artifact with it.");
        }
    }
}

function artifactNames(artifact)
{
    return (artifact.name ? [artifact.name] : [])
        .concat(artifact.name_expressions || []);
}

// An attachment job uploads by glob out of the directory its downloads land in,
// so what it attaches is decided by which artifacts it downloads and not by the
// upload arguments alone. Two artifacts can carry the same file name, as the
// unsigned portable archive and the archive rebuilt from the signed payload do,
// and the second download overwrites the first under the name the release
// publishes. The comparison is therefore on artifact identity: renaming a step
// cannot disarm it, and the argument-set comparison below stays about shapes.
function checkAttachmentSources(manifest, sitesByWorkflow)
{
    for (const declaration of manifest.release_attachments) {
        const sites = (sitesByWorkflow.get(declaration.workflow) || [])
            .filter(site => site.kind === "download" &&
                site.job === declaration.job);
        const sources = (declaration.source_artifacts || [])
            .map(id => manifest.actions_artifacts.find(
                entry => entry.id === id))
            .filter(Boolean);
        const permitted = new Set();
        for (const artifact of sources) {
            for (const name of artifactNames(artifact))
                permitted.add(name);
        }

        for (const site of sites) {
            check(permitted.has(site.name),
                site.workflow + ":" + site.nameLine + " downloads artifact \"" +
                site.name + "\" into job " + declaration.job + ", which " +
                MANIFEST_RELATIVE_PATH + " does not declare among its" +
                " source_artifacts (" + sortedList(Array.from(permitted)) +
                "). Every downloaded file lands in the directory this job" +
                " attaches from, so an artifact it may not consume can" +
                " overwrite one it must.");
        }

        for (const artifact of sources) {
            const names = artifactNames(artifact);
            check(sites.some(site => names.indexOf(site.name) >= 0),
                MANIFEST_RELATIVE_PATH + " declares that job " +
                declaration.job + " in " + declaration.workflow + " attaches" +
                " artifact \"" + artifact.id + "\", but that job downloads" +
                " no artifact named " + sortedList(names) + ".");
        }
    }
}

function checkArtifactUploadPaths(manifest, sitesByWorkflow)
{
    const sidecar = manifest.checksum.suffix;
    for (const artifact of manifest.actions_artifacts) {
        if (!artifact.contents)
            continue;

        const prefix = normalizeShape(artifact.path_prefix);
        const expected = new Set();
        for (const content of artifact.contents) {
            const shape = assetShape(manifest, content.asset);
            if (shape === null)
                continue;
            expected.add(prefix + (content.path_expression
                ? normalizeShape(content.path_expression)
                : shape));
            if (content.sidecar)
                expected.add(prefix + shape + sidecar);
        }

        const sites = (sitesByWorkflow.get(artifact.workflow) || [])
            .filter(site => site.kind === "upload" &&
                (site.name === artifact.name ||
                    (artifact.name_expressions || []).indexOf(site.name) >= 0));
        for (const site of sites) {
            const observed = new Set(site.paths.map(normalizeShape));
            const missing = Array.from(expected)
                .filter(entry => !observed.has(entry));
            const unexpected = Array.from(observed)
                .filter(entry => !expected.has(entry));
            if (missing.length === 0 && unexpected.length === 0)
                continue;

            violation(site.workflow + ":" + site.stepLine + " uploads" +
                " artifact \"" + artifact.id + "\" with paths [" +
                displayShapes(Array.from(observed)) + "]; " +
                MANIFEST_RELATIVE_PATH + " declares [" +
                displayShapes(Array.from(expected)) + "]." +
                (missing.length > 0
                    ? " Missing: " + displayShapes(missing) + "."
                    : "") +
                (unexpected.length > 0
                    ? " Unexpected: " + displayShapes(unexpected) + "."
                    : ""));
        }
    }
}

function checkReleaseAttachments(manifest, uploadsByWorkflow)
{
    const sidecar = manifest.checksum.suffix;
    for (const declaration of manifest.release_attachments) {
        const sites = (uploadsByWorkflow.get(declaration.workflow) || [])
            .filter(site => site.job === declaration.job);
        check(sites.length > 0,
            MANIFEST_RELATIVE_PATH + " declares a release_attachments entry" +
            " for job \"" + declaration.job + "\" in " +
            declaration.workflow + ", but that job runs no gh release upload." +
            " A declaration nothing produces is a rule that inspects nothing.");
    }

    for (const [workflow, sites] of uploadsByWorkflow) {
        for (const site of sites) {
            const declaration = manifest.release_attachments.find(entry =>
                entry.workflow === workflow && entry.job === site.job);
            if (!declaration) {
                violation(workflow + ":" + site.line + " runs gh release" +
                    " upload, but " + MANIFEST_RELATIVE_PATH + " declares no" +
                    " release_attachments entry for job \"" + site.job +
                    "\" in that workflow.");
                continue;
            }
            if (declaration.selection)
                continue;

            const prefix = normalizeShape(declaration.path_prefix);
            const expected = new Set();
            for (const entry of declaration.assets) {
                const shape = assetShape(manifest, entry.asset);
                expected.add(prefix + shape);
                if (entry.sidecar)
                    expected.add(prefix + shape + sidecar);
            }

            const observed = new Set(site.arguments
                .map(argument => argument.replace(/^['"]|['"]$/g, ""))
                .filter(argument =>
                    argument.startsWith(declaration.path_prefix))
                .map(normalizeShape));
            const missing = Array.from(expected)
                .filter(entry => !observed.has(entry));
            const unexpected = Array.from(observed)
                .filter(entry => !expected.has(entry));
            if (missing.length === 0 && unexpected.length === 0)
                continue;

            // The unsigned installer is the one asset whose presence here is a
            // release defect rather than a bookkeeping error, so it is named
            // when it is what drifted in and not otherwise.
            const undeclaredAssets = manifest.release_assets
                .filter(asset => asset.template &&
                    !declaration.assets.some(entry => entry.asset === asset.id))
                .filter(asset => unexpected.indexOf(
                    prefix + normalizeShape(asset.template)) >= 0);

            violation(workflow + ":" + site.line + " job " + site.job +
                " attaches [" + displayShapes(Array.from(observed)) + "]; " +
                MANIFEST_RELATIVE_PATH + " declares [" +
                displayShapes(Array.from(expected)) + "]." +
                (missing.length > 0
                    ? " Missing: " + displayShapes(missing) + "."
                    : "") +
                (unexpected.length > 0
                    ? " Unexpected: " + displayShapes(unexpected) + "."
                    : "") +
                (undeclaredAssets.length > 0
                    ? " A release must attach only the assets declared for" +
                        " this job; " + sortedList(undeclaredAssets
                            .map(asset => "\"" + asset.id + "\"")) +
                        " is not one of them."
                    : ""));
        }
    }
}

function checkAssetShapes(root, manifest)
{
    const declared = declaredShapes(manifest);
    const sidecar = manifest.checksum.suffix;
    const patternsToIgnore = manifest.actions_artifacts
        .filter(artifact => artifact.retention.mode === "pattern")
        .map(artifact => artifact.retention.pattern);

    for (const consumer of manifest.consumers) {
        if (!exists(root, consumer))
            continue;

        readFile(root, consumer).split(/\r?\n/).forEach((line, index) => {
            for (const shape of sweepLine(line, patternsToIgnore)) {
                const extension = CHECKSUM_EXTENSION_PATTERN.exec(shape);
                if (extension && extension[0] !== sidecar) {
                    violation(consumer + ":" + (index + 1) + " writes a \"" +
                        displayShape(shape) + "\" sidecar, but " +
                        MANIFEST_RELATIVE_PATH + " declares checksum.suffix" +
                        " \"" + sidecar + "\". One sidecar convention," +
                        " declared once.");
                    continue;
                }
                check(declared.has(shape),
                    consumer + ":" + (index + 1) + " names \"" +
                    displayShape(shape) + "\", whose shape matches no release" +
                    " asset in " + MANIFEST_RELATIVE_PATH + ". Declared" +
                    " shapes: " + displayShapes(Array.from(declared)) + ".");
            }
        });
    }

    for (const asset of manifest.release_assets) {
        if (!exists(root, asset.produced_by)) {
            violation("release asset \"" + asset.id + "\" names \"" +
                asset.produced_by + "\" as its producer, but that file does" +
                " not exist. A producer this contract cannot open is an asset" +
                " whose name nothing is checked against.");
            continue;
        }
        if (!asset.template)
            continue;

        const template = asset.variant_of
            ? manifest.asset_templates[asset.variant_of].template
            : asset.template;
        const shape = normalizeShape(template);
        const produced = readFile(root, asset.produced_by)
            .split(/\r?\n/)
            .some(line => sweepLine(line, patternsToIgnore).indexOf(shape) >= 0);
        check(produced,
            "release asset \"" + asset.id + "\" (\"" + template + "\") does" +
            " not appear in " + asset.produced_by + ", which " +
            MANIFEST_RELATIVE_PATH + " names as its producer.");
    }
}

// checksum.algorithm decides what a release publishes beside every asset, and
// the sidecar rule below can only see a producer that spells an asset name out.
// Every Linux sidecar is written from a shell variable and the packaged
// artifact test names its algorithm directly, so the declared algorithm was
// enforced by nothing except its own agreement with the declared suffix. The
// name of an algorithm is a short, distinctive token, so the copies can simply
// be found. A name that is part of a longer identifier, as in the Qt IFW
// archive checksum field, is a different fact and is left alone.
const CHECKSUM_ALGORITHM_PATTERN =
    /(?<![A-Za-z0-9_])(sha1|sha224|sha256|sha384|sha512|md5)(?![0-9])/gi;

function checkChecksumAlgorithm(root, manifest)
{
    const algorithm = manifest.checksum.algorithm;
    for (const reader of manifest.checksum.readers) {
        if (!exists(root, reader)) {
            violation(MANIFEST_RELATIVE_PATH + " names \"" + reader + "\" as" +
                " a reader of the checksum convention, but it does not exist.");
            continue;
        }
        checkReaderOwnsNoLiteral(
            root, reader, "checksum.suffix", manifest.checksum.suffix);
    }

    for (const consumer of manifest.consumers) {
        if (!exists(root, consumer))
            continue;

        readFile(root, consumer).split(/\r?\n/).forEach((line, index) => {
            CHECKSUM_ALGORITHM_PATTERN.lastIndex = 0;
            let match;
            while ((match = CHECKSUM_ALGORITHM_PATTERN.exec(line)) !== null) {
                check(match[1].toLowerCase() === algorithm,
                    consumer + ":" + (index + 1) + " names the checksum" +
                    " algorithm \"" + match[1] + "\", but " +
                    MANIFEST_RELATIVE_PATH + " checksum.algorithm is \"" +
                    algorithm + "\". One checksum convention, declared once.");
            }
        });
    }
}

function checkRetentionWorkflow(root, manifest)
{
    const relativePath = ".github/workflows/artifact-retention.yml";
    if (!exists(root, relativePath))
        return;

    const text = readFile(root, relativePath);
    const literals = retentionFamilies(manifest)
        .map(family => family.value)
        .filter(Boolean);

    text.split(/\r?\n/).forEach((line, index) => {
        for (const literal of literals) {
            check(line.indexOf(literal) < 0,
                relativePath + ":" + (index + 1) + " names artifact family \"" +
                literal + "\" literally. Retention families come from " +
                MANIFEST_RELATIVE_PATH + " through tools/release_manifest.js," +
                " so a rename can never silently stop pruning.");
        }
    });

    check(/node tools\/release_manifest\.js retention-families/.test(text),
        relativePath + " must obtain its prune families by running \"node" +
        " tools/release_manifest.js retention-families\"; that invocation was" +
        " not found.");

    const emitted = new Set(retentionFamilies(manifest)
        .filter(family => Boolean(family.value))
        .map(family => family.id));
    for (const artifact of manifest.actions_artifacts) {
        check(emitted.has(artifact.id),
            "tools/release_manifest.js retention-families omits artifact \"" +
            artifact.id + "\". Every declared artifact must belong to a prune" +
            " family.");
    }
}

// A file that reads a fact out of the manifest must not also spell it out. The
// message names the manifest field so the fix is to delete the copy, not to
// re-synchronise it.
function checkReaderOwnsNoLiteral(root, reader, field, literal)
{
    readFile(root, reader).split(/\r?\n/).forEach((line, index) => {
        check(line.indexOf(literal) < 0,
            reader + ":" + (index + 1) + " contains the literal \"" + literal +
            "\". " + field + " in " + MANIFEST_RELATIVE_PATH + " is the only" +
            " copy, and this file already reads it.");
    });

    check(/release[\\/]manifest\.json/.test(readFile(root, reader)),
        reader + " is declared as a reader of " + field + " in " +
        MANIFEST_RELATIVE_PATH + ", but it never opens release/manifest.json.");
}

function actionSites(lines, actionPattern)
{
    const sites = [];
    for (const block of structure.stepBlocks(lines)) {
        let matched = false;
        for (let index = block.firstLine; index < block.lastLine; ++index) {
            if (actionPattern.test(lines[index])) {
                matched = true;
                break;
            }
        }
        if (!matched)
            continue;

        sites.push({
            stepLine: block.firstLine + 1,
            mapping: structure.withMapping(lines, block) || {}
        });
    }
    return sites;
}

function checkQtVersion(root, manifest, workflowLines)
{
    const version = manifest.qt.version;
    for (const [workflow, lines] of workflowLines) {
        const sites = actionSites(lines, /uses:\s*\S*install-qt-action@/);
        for (const site of sites) {
            const declared = site.mapping.version;
            if (!declared) {
                violation(workflow + ":" + site.stepLine + " installs Qt but" +
                    " declares no version:. A Qt version this contract cannot" +
                    " see is a Qt version it cannot keep in sync.");
                continue;
            }
            check(declared.value === version,
                workflow + ":" + declared.line + " installs Qt " +
                declared.value + ", but " + MANIFEST_RELATIVE_PATH +
                " qt.version is " + version + ".");
        }

        // Anything left after the install steps are accounted for is a copy
        // nobody checks.
        const residue = lines
            .map(line => line.replace(
                new RegExp("version:\\s*['\"]?" + escapeRegExp(version) +
                    "['\"]?"), ""))
            .map((line, index) => ({ line, index }))
            .filter(entry => entry.line.indexOf(version) >= 0);
        for (const entry of residue) {
            violation(workflow + ":" + (entry.index + 1) + " restates the Qt" +
                " version " + version + " outside an install-qt-action step." +
                " qt.version in " + MANIFEST_RELATIVE_PATH + " is the only" +
                " copy a consumer may read.");
        }
    }
}

function qtIfwCacheKey(manifest)
{
    return manifest.qt_ifw.cache_key_template
        .split("{version}").join(manifest.qt_ifw.version)
        .split("{archive_sha256}").join(manifest.qt_ifw.archive_sha256);
}

function escapeRegExp(text)
{
    return text.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
}

// A directory whose name embeds the Qt IFW version is a copy of that version
// that no reader derives, because the files holding these two are a workflow
// step and a build configuration example, and neither can read a JSON file. The
// residue rule below cannot see them once the version moves: it strips the
// version the manifest declares now, so a directory still naming the previous
// one is invisible to it. This rule reads the version out of the name instead,
// which is what a stale copy actually looks like.
function checkQtIfwDirectoryNames(root, manifest)
{
    const ifw = manifest.qt_ifw;
    for (const consumer of manifest.consumers) {
        if (!exists(root, consumer))
            continue;

        readFile(root, consumer).split(/\r?\n/).forEach((line, index) => {
            for (const prefix of ifw.directory_prefixes) {
                const pattern = new RegExp(
                    escapeRegExp(prefix) + "([0-9]+(?:\\.[0-9]+)+)", "g");
                let match;
                while ((match = pattern.exec(line)) !== null) {
                    check(match[1] === ifw.version,
                        consumer + ":" + (index + 1) + " names the directory \"" +
                        match[0] + "\", but " + MANIFEST_RELATIVE_PATH +
                        " qt_ifw.version is " + ifw.version + ". The" +
                        " provisioner derives that directory from the" +
                        " manifest, so this copy names a directory nothing" +
                        " creates.");
                }
            }
        });
    }
}

function checkQtIfw(root, manifest, workflowLines)
{
    const ifw = manifest.qt_ifw;
    const cacheKey = qtIfwCacheKey(manifest);
    const rootDirectories = ifw.directory_prefixes
        .map(prefix => prefix + ifw.version);

    // The provisioner used to own this URL and the packaging contract test
    // asserted its exact shape. Both facts live here now, so the official
    // repository path and the Windows x64 archive name are asserted here too:
    // a URL that merely carries the right version could still point anywhere.
    const officialPrefix =
        "https://download.qt.io/online/qtsdkrepository/windows_x86/ifw/";
    const archiveName = ifw.archive_url.split("/").pop();
    check(ifw.archive_url.startsWith(officialPrefix) &&
            archiveName.startsWith(ifw.version + "-") &&
            archiveName.endsWith("ifw-win-x64.7z"),
        "qt_ifw.archive_url must be an official Qt archive under " +
        officialPrefix + " whose file name begins with qt_ifw.version " +
        ifw.version + " and ends with ifw-win-x64.7z; got \"" +
        ifw.archive_url + "\".");

    for (const reader of ifw.readers) {
        if (!exists(root, reader)) {
            violation(MANIFEST_RELATIVE_PATH + " names \"" + reader + "\" as" +
                " a reader of qt_ifw, but it does not exist. A reader this" +
                " contract cannot open is a file whose copies of the version," +
                " the archive URL and the archive checksum nobody compares.");
            continue;
        }
        checkReaderOwnsNoLiteral(root, reader, "qt_ifw.version", ifw.version);
        checkReaderOwnsNoLiteral(
            root, reader, "qt_ifw.archive_sha256", ifw.archive_sha256);
        checkReaderOwnsNoLiteral(
            root, reader, "qt_ifw.archive_url", ifw.archive_url);
    }

    for (const [workflow, lines] of workflowLines) {
        const sites = actionSites(lines, /uses:\s*actions\/cache\/(restore|save)@/);
        for (const site of sites) {
            const declared = site.mapping.key;
            if (!declared) {
                violation(workflow + ":" + site.stepLine + " caches an" +
                    " artifact but declares no key:. A cache key this" +
                    " contract cannot see can drift from the archive the" +
                    " provisioner verifies.");
                continue;
            }
            check(declared.value === cacheKey,
                workflow + ":" + declared.line + " uses cache key \"" +
                declared.value + "\". It must be exactly \"" + cacheKey +
                "\", composed from qt_ifw.cache_key_template, so the key" +
                " cannot drift from the archive the provisioner verifies.");
        }

        // Every remaining mention of the version or the archive hash in a
        // workflow has to be one of the two literals GitHub Actions forces:
        // the cache key and the runner-local IFW root directory.
        lines.forEach((line, index) => {
            let residue = line.split(cacheKey).join("");
            for (const directory of rootDirectories)
                residue = residue.split(directory).join("");
            check(residue.indexOf(ifw.version) < 0,
                workflow + ":" + (index + 1) + " restates the Qt IFW version " +
                ifw.version + " outside the cache key and the " +
                sortedList(rootDirectories) + " directory names." +
                " qt_ifw.version in " + MANIFEST_RELATIVE_PATH +
                " is the only other copy.");
            check(residue.indexOf(ifw.archive_sha256) < 0,
                workflow + ":" + (index + 1) + " restates the Qt IFW archive" +
                " checksum outside the cache key. qt_ifw.archive_sha256 in " +
                MANIFEST_RELATIVE_PATH + " is the only other copy.");
        });
    }
}

// Resolve `$x = Join-Path $y '<literal>'` chains so that a declared staged
// path can be compared with the path the builder actually signs.
function joinPathChains(scriptText)
{
    const chains = new Map();
    for (const line of scriptText.split(/\r?\n/)) {
        const assignment =
            /^\$([A-Za-z][A-Za-z0-9]*)\s*=\s*Join-Path\s+\$([A-Za-z][A-Za-z0-9]*)\s+'([^']+)'\s*$/
                .exec(line);
        if (assignment)
            chains.set(assignment[1], { parent: assignment[2], leaf: assignment[3] });
    }
    return chains;
}

function resolveChain(chains, variable)
{
    const segments = [];
    let current = variable;
    while (chains.has(current)) {
        const link = chains.get(current);
        segments.unshift(link.leaf);
        current = link.parent;
    }
    return { root: current, segments };
}

// The pattern decides whether a signature identifies the publisher, so a
// pattern that admits anything is a signing check that refuses nothing. What is
// asserted here is the two directions the release depends on rather than the
// text of the expression: it must match the publisher, and it must refuse a
// subject that only resembles it. tests/windows_ifw_contract_tests.ps1 drives
// the same distinction through the real builder against certificate fixtures,
// but it runs on Windows alone, and the manifest is edited from every host.
function checkPublisherSubjectPattern(signing)
{
    let expression = null;
    try {
        expression = new RegExp(signing.publisher_subject_pattern);
    }
    catch (error) {
        violation("signing.publisher_subject_pattern \"" +
            signing.publisher_subject_pattern + "\" is not a valid regular" +
            " expression: " + error.message);
        return;
    }

    const accepted = [
        "CN=" + signing.publisher,
        "CN=" + signing.publisher + ", O=" + signing.publisher +
            ", L=London, C=GB"
    ];
    for (const subject of accepted) {
        check(expression.test(subject),
            "signing.publisher_subject_pattern \"" +
            signing.publisher_subject_pattern + "\" does not match the" +
            " certificate subject \"" + subject + "\". A pattern that cannot" +
            " match its own publisher refuses every release signature.");
    }

    const refused = [
        "CN=Unexpected Publisher",
        "CN=Not " + signing.publisher + " Either",
        "CN=" + signing.publisher + " Holdings",
        "O=" + signing.publisher + ", CN=Someone Else"
    ];
    for (const subject of refused) {
        check(!expression.test(subject),
            "signing.publisher_subject_pattern \"" +
            signing.publisher_subject_pattern + "\" accepts the certificate" +
            " subject \"" + subject + "\". Only a whole CN component naming " +
            signing.publisher + " may be accepted.");
    }
}

function checkSigning(root, manifest)
{
    const signing = manifest.signing;
    for (const declaration of signing.publisher_declared_in) {
        if (!exists(root, declaration)) {
            violation(MANIFEST_RELATIVE_PATH + " names \"" + declaration +
                "\" as a publisher declaration, but it does not exist.");
            continue;
        }
        const declared =
            /<Publisher>([^<]*)<\/Publisher>/.exec(readFile(root, declaration));
        check(declared !== null && declared[1] === signing.publisher,
            declaration + " declares <Publisher>" +
            (declared === null ? "" : declared[1]) + "</Publisher>, but " +
            MANIFEST_RELATIVE_PATH + " signing.publisher is \"" +
            signing.publisher + "\". The installer metadata and the signature" +
            " identity must name the same publisher.");
    }

    checkPublisherSubjectPattern(signing);

    for (const reader of signing.publisher_pattern_readers) {
        if (!exists(root, reader)) {
            violation(MANIFEST_RELATIVE_PATH + " names \"" + reader + "\" as" +
                " a reader of signing.publisher_subject_pattern, but it does" +
                " not exist. A reader this contract cannot open is a file" +
                " whose copy of the pattern nobody compares.");
            continue;
        }
        checkReaderOwnsNoLiteral(root, reader, "signing.publisher_subject_pattern",
            signing.publisher_subject_pattern);
        check(/\$publisherSubjectPattern/.test(readFile(root, reader)),
            reader + " is declared as a reader of" +
            " signing.publisher_subject_pattern, but it never uses" +
            " $publisherSubjectPattern.");
    }

    // The installed binaries are signature-verified after the lifecycle test
    // installs them, which is the only check that the signature survived
    // packaging and installation. That list has to be the builder's list.
    for (const reader of signing.payload_binary_readers) {
        if (!exists(root, reader)) {
            violation(MANIFEST_RELATIVE_PATH + " names \"" + reader + "\" as" +
                " a reader of signing.payload_binaries, but it does not" +
                " exist.");
            continue;
        }
        const text = readFile(root, reader);
        check(/release[\\/]manifest\.json/.test(text),
            reader + " is declared as a reader of signing.payload_binaries," +
            " but it never opens " + MANIFEST_RELATIVE_PATH + ".");
        check(/payload_binaries/.test(text),
            reader + " is declared as a reader of signing.payload_binaries," +
            " but it never reads that field, so its own list of binaries to" +
            " verify can drift from the list the builder signs.");
    }

    if (!exists(root, signing.builder)) {
        violation(MANIFEST_RELATIVE_PATH + " names \"" + signing.builder +
            "\" as the signing builder, but it does not exist.");
        return;
    }

    // Backtick continuations first, so a wrapped call reads as one statement.
    const builder = readFile(root, signing.builder)
        .replace(/`\r?\n\s*/g, "");
    const chains = joinPathChains(builder);
    const stageRoot = resolveChain(chains, "stageRoot");

    const payloadSigned = [];
    const stageSigned = [];
    let finalSigned = 0;
    const callPattern =
        /Invoke-TrustedSigning\s+(\(Join-Path\s+\$PayloadPath\s+'([^']+)'\)|\$([A-Za-z][A-Za-z0-9]*))/g;
    let match;
    let calls = 0;
    while ((match = callPattern.exec(builder)) !== null) {
        ++calls;
        if (match[2] !== undefined) {
            payloadSigned.push(match[2].replace(/\\/g, "/"));
            continue;
        }
        if (match[3] === "artifactPath") {
            ++finalSigned;
            continue;
        }

        const resolved = resolveChain(chains, match[3]);
        const underStage = resolved.root === stageRoot.root &&
            stageRoot.segments.every(
                (segment, index) => resolved.segments[index] === segment);
        if (!underStage) {
            violation(signing.builder + " signs $" + match[3] + ", which this" +
                " contract cannot resolve to a declared signing target. Every" +
                " signed binary must be declared in signing.payload_binaries," +
                " signing.stage_binaries or signing.final_artifact.");
            continue;
        }
        stageSigned.push(resolved.segments
            .slice(stageRoot.segments.length)
            .join("/")
            .replace(/\\/g, "/"));
    }

    const totalDeclared = signing.payload_binaries.length +
        signing.stage_binaries.length + 1;
    check(calls === totalDeclared,
        signing.builder + " makes " + calls + " Invoke-TrustedSigning calls," +
        " but " + MANIFEST_RELATIVE_PATH + " declares " +
        signing.payload_binaries.length + " payload binaries plus " +
        signing.stage_binaries.length + " stage binaries plus the final" +
        " artifact, which is " + totalDeclared + ". A signing target that is" +
        " not declared is a target nobody reviews.");

    check(finalSigned === 1,
        signing.builder + " signs the finished installer " + finalSigned +
        " times; signing.final_artifact declares exactly one.");

    for (const target of signing.payload_binaries) {
        check(payloadSigned.indexOf(target) >= 0,
            signing.builder + " never signs \"" + target + "\", which " +
            MANIFEST_RELATIVE_PATH + " signing.payload_binaries declares." +
            " Expected the call: Invoke-TrustedSigning (Join-Path" +
            " $PayloadPath '" + target.replace(/\//g, "\\") + "').");
    }
    for (const target of payloadSigned) {
        check(signing.payload_binaries.indexOf(target) >= 0,
            signing.builder + " signs payload binary \"" + target + "\", which" +
            " " + MANIFEST_RELATIVE_PATH + " signing.payload_binaries does" +
            " not declare.");
    }
    for (const target of signing.stage_binaries) {
        check(stageSigned.indexOf(target) >= 0,
            signing.builder + " never signs staged binary \"" + target +
            "\", which " + MANIFEST_RELATIVE_PATH + " signing.stage_binaries" +
            " declares.");
    }
    for (const target of stageSigned) {
        check(signing.stage_binaries.indexOf(target) >= 0,
            signing.builder + " signs staged binary \"" + target + "\", which" +
            " " + MANIFEST_RELATIVE_PATH + " signing.stage_binaries does not" +
            " declare.");
    }

    check(assetShape(manifest, signing.final_artifact) !== null,
        "signing.final_artifact \"" + signing.final_artifact + "\" is not a" +
        " declared release asset.");
}

// --- Entry point ------------------------------------------------------------

function loadManifest(root)
{
    const manifest = JSON.parse(readFile(root, MANIFEST_RELATIVE_PATH));
    if (manifest.schema !== SUPPORTED_SCHEMA) {
        throw new Error("Release manifest violation: " +
            MANIFEST_RELATIVE_PATH + " declares schema " + manifest.schema +
            ", but this contract understands schema " + SUPPORTED_SCHEMA + ".");
    }
    return manifest;
}

function reportViolations()
{
    for (const message of violations)
        process.stderr.write(message + "\n");
    process.exit(1);
}

function runCheck(root)
{
    const manifest = loadManifest(root);

    checkRequiredDeclarations(manifest);
    if (violations.length > 0)
        reportViolations();

    const workflowLines = new Map();
    const sitesByWorkflow = new Map();
    const uploadsByWorkflow = new Map();
    for (const consumer of manifest.consumers) {
        if (!isWorkflow(consumer) || !exists(root, consumer))
            continue;
        const lines = readFile(root, consumer).split(/\r?\n/);
        workflowLines.set(consumer, lines);
        sitesByWorkflow.set(consumer, artifactSites(consumer, lines));
        uploadsByWorkflow.set(consumer, releaseUploadSites(consumer, lines));
    }

    for (const artifact of manifest.actions_artifacts) {
        const sites = sitesByWorkflow.get(artifact.workflow);
        check(sites !== undefined && sites.length > 0,
            "found no artifact steps in " + artifact.workflow + ", which " +
            MANIFEST_RELATIVE_PATH + " says carries \"" +
            artifactProbeName(artifact) + "\". The extractor could not read" +
            " this file's structure.");
    }

    checkManifestSelfConsistency(root, manifest);
    checkAssetTemplates(manifest);
    checkWorkflowCoverage(root, manifest);
    checkRetentionFamilies(manifest);
    checkArtifactNames(manifest, sitesByWorkflow);
    checkArtifactUploadPaths(manifest, sitesByWorkflow);
    checkAttachmentSources(manifest, sitesByWorkflow);
    checkReleaseAttachments(manifest, uploadsByWorkflow);
    checkAssetShapes(root, manifest);
    checkChecksumAlgorithm(root, manifest);
    checkRetentionWorkflow(root, manifest);
    checkQtVersion(root, manifest, workflowLines);
    checkQtIfw(root, manifest, workflowLines);
    checkQtIfwDirectoryNames(root, manifest);
    checkSigning(root, manifest);

    if (violations.length > 0)
        reportViolations();

    process.stdout.write("Release manifest contract passed: " +
        path.join(root, MANIFEST_RELATIVE_PATH) + "\n");
}

// artifact-retention.yml holds actions: write and interpolates the value of
// every row into a gh --jq expression, where an empty pattern matches every
// artifact in the repository. The families are validated here rather than only
// in check mode: this program must not be able to emit a row that selects more
// than the family it names.
function runRetentionFamilies(root)
{
    const manifest = loadManifest(root);

    checkRequiredDeclarations(manifest);
    if (violations.length > 0)
        reportViolations();

    checkRetentionFamilies(manifest);
    const families = retentionFamilies(manifest);
    for (const family of families) {
        check(typeof family.value === "string" && family.value !== "",
            "retention family \"" + family.id + "\" would be emitted with an" +
            " empty " + family.mode + " value, which selects every artifact in" +
            " the repository.");
    }
    if (violations.length > 0)
        reportViolations();

    for (const family of families) {
        process.stdout.write(
            [family.mode, family.id, family.value].join("\t") + "\n");
    }
}

const mode = process.argv[2];
const root = process.argv[3];
if (!root || (mode !== "check" && mode !== "retention-families")) {
    process.stderr.write(
        "usage: release_manifest.js check|retention-families <source-root>\n");
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
