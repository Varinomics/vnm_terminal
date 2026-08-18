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
// Extraction is text-driven: no YAML parser is available without adding a
// dependency, and none is added. An extraction that finds nothing where the
// manifest says something exists is itself a violation, so a workflow this
// program can no longer read fails rather than passing vacuously.

const fs = require("fs");
const path = require("path");

const MANIFEST_RELATIVE_PATH = "release/manifest.json";
const SUPPORTED_SCHEMA = 1;
const VERSION_PROBE = "0.0.0";
const CHECKSUM_EXTENSION_PATTERN = /\.(sha1|sha224|sha256|sha384|sha512|md5)$/;

// One opaque stand-in for every substitution syntax the tree uses, so that a
// glob, a shell variable, a PowerShell subexpression, a batch variable and a
// manifest placeholder all compare equal. Displayed as {}.
const SUBSTITUTION = "\u0001";
const SHAPE_PATTERN = new RegExp(
    "vnm[_-]terminal[_-]v[A-Za-z0-9_.@" + SUBSTITUTION + "-]*", "g");

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
    return relativePath.startsWith(".github/workflows/");
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

// --- Workflow structure -----------------------------------------------------

function indentOf(line)
{
    return line.length - line.replace(/^\s*/, "").length;
}

// A step block runs from its `- name:` line to the next non-blank line at or
// left of the dash.
function stepBlocks(lines)
{
    const blocks = [];
    for (let index = 0; index < lines.length; ++index) {
        const start = /^(\s*)- name:/.exec(lines[index]);
        if (!start)
            continue;

        const indent = start[1].length;
        let end = index + 1;
        while (end < lines.length) {
            const line = lines[end];
            if (line.trim() !== "" && indentOf(line) <= indent)
                break;
            ++end;
        }
        blocks.push({ firstLine: index, lastLine: end, indent });
    }
    return blocks;
}

// The `with:` mapping of a step, as { key: { value, blockLines, line } }.
function withMapping(lines, block)
{
    let withIndex = -1;
    for (let index = block.firstLine; index < block.lastLine; ++index) {
        if (/^\s*with:\s*$/.test(lines[index])) {
            withIndex = index;
            break;
        }
    }
    if (withIndex < 0)
        return null;

    const withIndent = indentOf(lines[withIndex]);
    let keyIndent = -1;
    const mapping = {};
    for (let index = withIndex + 1; index < block.lastLine; ++index) {
        const line = lines[index];
        if (line.trim() === "" || /^\s*#/.test(line))
            continue;

        const lineIndent = indentOf(line);
        if (lineIndent <= withIndent)
            break;
        if (keyIndent < 0)
            keyIndent = lineIndent;
        if (lineIndent !== keyIndent)
            continue;

        const entry = /^\s*([A-Za-z0-9_-]+):\s*(.*)$/.exec(line);
        if (!entry)
            continue;

        const blockLines = [];
        if (entry[2] === "|" || entry[2] === ">") {
            for (let scan = index + 1; scan < block.lastLine; ++scan) {
                const scanned = lines[scan];
                if (scanned.trim() === "")
                    continue;
                if (indentOf(scanned) <= keyIndent)
                    break;
                if (/^\s*#/.test(scanned))
                    continue;
                blockLines.push(scanned.trim());
            }
        }
        mapping[entry[1]] = {
            value: entry[2].replace(/^['"]|['"]$/g, "").trim(),
            blockLines,
            line: index + 1
        };
    }
    return mapping;
}

function jobNameAt(lines, lineIndex)
{
    let jobName = null;
    for (let index = 0; index <= lineIndex; ++index) {
        const job = /^ {2}([A-Za-z0-9_-]+):\s*$/.exec(lines[index]);
        if (job)
            jobName = job[1];
    }
    return jobName;
}

function artifactSites(workflowPath, lines)
{
    const sites = [];
    for (const block of stepBlocks(lines)) {
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

        const mapping = withMapping(lines, block) || {};
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
            job: jobNameAt(lines, block.firstLine),
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
            job: jobNameAt(lines, index),
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

            violation(workflow + ":" + site.line + " job " + site.job +
                " attaches [" + displayShapes(Array.from(observed)) + "]; " +
                MANIFEST_RELATIVE_PATH + " declares [" +
                displayShapes(Array.from(expected)) + "]." +
                (missing.length > 0
                    ? " Missing: " + displayShapes(missing) + "."
                    : "") +
                (unexpected.length > 0
                    ? " Unexpected: " + displayShapes(unexpected) +
                        ". A release must never attach the unsigned build."
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
        if (!asset.template || !exists(root, asset.produced_by))
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
    if (!exists(root, reader))
        return;

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
    for (const block of stepBlocks(lines)) {
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
            mapping: withMapping(lines, block) || {}
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

function checkQtIfw(root, manifest, workflowLines)
{
    const ifw = manifest.qt_ifw;
    const cacheKey = qtIfwCacheKey(manifest);
    const rootDirectory = ifw.root_directory_prefix + ifw.version;

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
            const residue = line
                .split(cacheKey).join("")
                .split(rootDirectory).join("");
            check(residue.indexOf(ifw.version) < 0,
                workflow + ":" + (index + 1) + " restates the Qt IFW version " +
                ifw.version + " outside the cache key and the \"" +
                rootDirectory + "\" root directory. qt_ifw.version in " +
                MANIFEST_RELATIVE_PATH + " is the only other copy.");
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

    for (const reader of signing.publisher_pattern_readers) {
        checkReaderOwnsNoLiteral(root, reader, "signing.publisher_subject_pattern",
            signing.publisher_subject_pattern);
        check(exists(root, reader) &&
                /\$publisherSubjectPattern/.test(readFile(root, reader)),
            reader + " is declared as a reader of" +
            " signing.publisher_subject_pattern, but it never uses" +
            " $publisherSubjectPattern.");
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

function runCheck(root)
{
    const manifest = loadManifest(root);

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
    checkRetentionFamilies(manifest);
    checkArtifactNames(manifest, sitesByWorkflow);
    checkArtifactUploadPaths(manifest, sitesByWorkflow);
    checkReleaseAttachments(manifest, uploadsByWorkflow);
    checkAssetShapes(root, manifest);
    checkRetentionWorkflow(root, manifest);
    checkQtVersion(root, manifest, workflowLines);
    checkQtIfw(root, manifest, workflowLines);
    checkSigning(root, manifest);

    if (violations.length > 0) {
        for (const message of violations)
            process.stderr.write(message + "\n");
        process.exit(1);
    }

    process.stdout.write("Release manifest contract passed: " +
        path.join(root, MANIFEST_RELATIVE_PATH) + "\n");
}

function runRetentionFamilies(root)
{
    for (const family of retentionFamilies(loadManifest(root))) {
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

if (mode === "check")
    runCheck(root);
else
    runRetentionFamilies(root);
