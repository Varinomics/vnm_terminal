// Strict, dependency-free workflow reader shared by both release gates. It
// turns the GitHub Actions YAML subset used by this repository into one
// normalized intermediate representation (IR): jobs own normalized needs,
// outputs and steps; steps own normalized name, uses, with, env and run data.
// Callers never rediscover structure with regular expressions of their own.
//
// This is not a general YAML parser. Unsupported YAML must fail closed at the
// boundary: anchors, aliases, merge keys, and flow-style step items are
// rejected rather than disappearing from the release contract. Quoted scalar
// values and both named and unnamed block-mapping steps are supported.

const fs = require("fs");
const path = require("path");

const WORKFLOW_DIRECTORY = ".github/workflows";

function workflowFiles(root)
{
    const directory = path.join(root, WORKFLOW_DIRECTORY);
    if (!fs.existsSync(directory))
        return [];

    return fs.readdirSync(directory)
        .filter(name => /\.ya?ml$/.test(name))
        .map(name => WORKFLOW_DIRECTORY + "/" + name)
        .sort();
}

function violation(workflowPath, line, detail)
{
    throw new Error("Workflow structure violation: " + workflowPath + ":" +
        line + " " + detail);
}

function indentOf(line)
{
    const prefix = /^\s*/.exec(line)[0];
    if (prefix.indexOf("\t") >= 0)
        return -1;
    return prefix.length;
}

function contentLine(line)
{
    return line.trim() !== "" && !/^\s*#/.test(line);
}

// YAML comments start at an unquoted # preceded by whitespace. GitHub
// expressions frequently contain single-quoted strings, so quote state has to
// be respected even though the surrounding scalar itself is plain.
function withoutComment(value)
{
    let single = false;
    let double = false;
    for (let index = 0; index < value.length; ++index) {
        const current = value[index];
        if (current === "'" && !double) {
            if (single && value[index + 1] === "'") {
                ++index;
                continue;
            }
            single = !single;
            continue;
        }
        if (current === '"' && !single &&
            (index === 0 || value[index - 1] !== "\\")) {
            double = !double;
            continue;
        }
        if (current === "#" && !single && !double &&
            (index === 0 || /\s/.test(value[index - 1]))) {
            return value.slice(0, index).trimEnd();
        }
    }
    return value.trimEnd();
}

function normalizedScalar(raw, workflowPath, line)
{
    const value = withoutComment(raw).trim();
    if (value === "")
        return "";
    if (/^[&*][A-Za-z0-9_-]+(?:\s|$)/.test(value)) {
        violation(workflowPath, line,
            "uses an anchor or alias, which the normalized workflow IR does" +
            " not support.");
    }
    if (value[0] === "{" || value[0] === "[") {
        violation(workflowPath, line,
            "uses a flow value, which the normalized workflow IR does not" +
            " support here.");
    }

    if (value[0] === "'") {
        if (value.length < 2 || value[value.length - 1] !== "'")
            violation(workflowPath, line, "contains an unterminated quoted scalar.");
        return value.slice(1, -1).replace(/''/g, "'");
    }
    if (value[0] === '"') {
        try {
            return JSON.parse(value);
        }
        catch (error) {
            violation(workflowPath, line,
                "contains an invalid double-quoted scalar: " + error.message);
        }
    }
    return value;
}

function mappingEntry(line, indent)
{
    const expression = new RegExp("^ {" + indent +
        "}([A-Za-z0-9_-]+):(?:\\s*(.*))?$");
    return expression.exec(line);
}

function blockScalar(lines, start, end, parentIndent, header,
    workflowPath)
{
    if (!/^[|>][+-]?$/.test(header))
        return null;

    const body = [];
    let bodyIndent = null;
    let index = start;
    for (; index < end; ++index) {
        const line = lines[index];
        if (line.trim() === "") {
            body.push("");
            continue;
        }
        const indent = indentOf(line);
        if (indent < 0)
            violation(workflowPath, index + 1, "uses a tab for indentation.");
        if (indent <= parentIndent)
            break;
        if (bodyIndent === null)
            bodyIndent = indent;
        if (indent < bodyIndent) {
            violation(workflowPath, index + 1,
                "dedents inside a block scalar in a shape the workflow IR" +
                " cannot normalize.");
        }
        body.push(line.slice(bodyIndent));
    }

    let value = body.join("\n");
    if (header[0] === ">")
        value = value.replace(/([^\n])\n(?=[^\n])/g, "$1 ");
    if (header.endsWith("+") || (!header.endsWith("-") && body.length > 0))
        value += "\n";
    return { value, next: index };
}

function scalarRecord(lines, index, end, parentIndent, raw, workflowPath)
{
    const header = withoutComment(raw).trim();
    const block = blockScalar(lines, index + 1, end, parentIndent, header,
        workflowPath);
    if (block)
        return { value: block.value, line: index + 1, next: block.next };
    return {
        value: normalizedScalar(raw, workflowPath, index + 1),
        line: index + 1,
        next: index + 1
    };
}

function childMapping(lines, start, end, indent, workflowPath, context)
{
    const result = {};
    let index = start;
    while (index < end) {
        const line = lines[index];
        if (!contentLine(line)) {
            ++index;
            continue;
        }
        const actualIndent = indentOf(line);
        if (actualIndent < 0)
            violation(workflowPath, index + 1, "uses a tab for indentation.");
        if (actualIndent < indent)
            break;
        if (actualIndent > indent) {
            violation(workflowPath, index + 1,
                "has unexpected nesting inside " + context + ".");
        }
        if (/^\s*<<:/.test(line)) {
            violation(workflowPath, index + 1,
                "uses a YAML merge key, which the workflow IR does not support.");
        }
        const entry = mappingEntry(line, indent);
        if (!entry)
            violation(workflowPath, index + 1,
                "is not a block-mapping entry inside " + context + ".");
        if (Object.prototype.hasOwnProperty.call(result, entry[1])) {
            violation(workflowPath, index + 1,
                "duplicates " + context + " key " + entry[1] + ".");
        }
        const record = scalarRecord(lines, index, end, indent,
            entry[2] || "", workflowPath);
        if (record.value === "") {
            violation(workflowPath, index + 1,
                context + " key " + entry[1] + " has no scalar value.");
        }
        result[entry[1]] = { value: record.value, line: record.line };
        index = record.next;
    }
    return { values: result, next: index };
}

function directStepEntry(text, workflowPath, line)
{
    if (text === "")
        return null;
    if (/^[{[&*]/.test(text)) {
        violation(workflowPath, line,
            "uses a flow-style, anchored, or aliased step item; steps must be" +
            " block mappings.");
    }
    if (/^<<:/.test(text)) {
        violation(workflowPath, line,
            "uses a YAML merge key, which the workflow IR does not support.");
    }
    const entry = /^([A-Za-z0-9_-]+):(?:\s*(.*))?$/.exec(text);
    if (!entry) {
        violation(workflowPath, line,
            "does not start a block-mapping step the workflow IR can inspect.");
    }
    return { key: entry[1], raw: entry[2] || "", line: line };
}

function parseStep(lines, start, end, workflowPath)
{
    const first = /^ {6}-\s*(.*)$/.exec(lines[start]);
    if (!first)
        violation(workflowPath, start + 1, "is not a six-space step item.");

    const entries = [];
    const inline = directStepEntry(first[1], workflowPath, start + 1);
    let firstBodyLine = start + 1;
    if (inline) {
        if (inline.key === "with" || inline.key === "env") {
            if (withoutComment(inline.raw).trim() !== "") {
                violation(workflowPath, inline.line,
                    inline.key + " must be a block mapping; flow mappings and" +
                    " aliases are unsupported.");
            }
            const mapping = childMapping(lines, start + 1, end, 10,
                workflowPath, "step " + inline.key);
            entries.push({
                key: inline.key,
                mapping: mapping.values,
                line: inline.line
            });
            firstBodyLine = mapping.next;
        }
        else {
            const record = scalarRecord(lines, start, end, 6,
                inline.raw, workflowPath);
            if (record.value === "") {
                violation(workflowPath, inline.line,
                    "step key " + inline.key + " is empty.");
            }
            entries.push({ key: inline.key, record, line: inline.line });
            firstBodyLine = record.next;
        }
    }

    for (let index = firstBodyLine; index < end;) {
        const line = lines[index];
        if (!contentLine(line)) {
            ++index;
            continue;
        }
        const indent = indentOf(line);
        if (indent < 0)
            violation(workflowPath, index + 1, "uses a tab for indentation.");
        if (indent !== 8) {
            violation(workflowPath, index + 1,
                "has nesting outside a recognized step field.");
        }
        if (/^\s*<<:/.test(line)) {
            violation(workflowPath, index + 1,
                "uses a YAML merge key, which the workflow IR does not support.");
        }
        const entry = mappingEntry(line, 8);
        if (!entry)
            violation(workflowPath, index + 1, "is not a step block-mapping entry.");

        const key = entry[1];
        const raw = entry[2] || "";
        if (key === "with" || key === "env") {
            if (withoutComment(raw).trim() !== "") {
                violation(workflowPath, index + 1,
                    key + " must be a block mapping; flow mappings and aliases" +
                    " are unsupported.");
            }
            const mapping = childMapping(lines, index + 1, end, 10,
                workflowPath, "step " + key);
            entries.push({ key, mapping: mapping.values, line: index + 1 });
            index = mapping.next;
            continue;
        }

        const record = scalarRecord(lines, index, end, 8, raw, workflowPath);
        if (record.value === "")
            violation(workflowPath, index + 1, "step key " + key + " is empty.");
        entries.push({ key, record, line: index + 1 });
        index = record.next;
    }

    const step = {
        line: start + 1,
        name: null,
        uses: null,
        with: {},
        env: {},
        run: null,
        if: null,
        continueOnError: null,
        timeoutMinutes: null,
        shell: null,
        workingDirectory: null
    };
    const seen = new Set();
    for (const entry of entries) {
        if (seen.has(entry.key))
            violation(workflowPath, entry.line,
                "duplicates step key " + entry.key + ".");
        seen.add(entry.key);
        if (entry.key === "with" || entry.key === "env") {
            step[entry.key] = entry.mapping;
            continue;
        }
        if (entry.key === "name" || entry.key === "uses" ||
            entry.key === "run") {
            step[entry.key] = {
                value: entry.record.value,
                line: entry.record.line
            };
            continue;
        }
        const executionFields = {
            "if": "if",
            "continue-on-error": "continueOnError",
            "timeout-minutes": "timeoutMinutes",
            "shell": "shell",
            "working-directory": "workingDirectory"
        };
        if (Object.prototype.hasOwnProperty.call(executionFields, entry.key)) {
            step[executionFields[entry.key]] = {
                value: entry.record.value,
                line: entry.record.line
            };
        }
    }
    if (step.uses === null && step.run === null) {
        violation(workflowPath, step.line,
            "has neither uses nor run, so it is not an executable Actions step.");
    }
    if (step.uses !== null && step.run !== null) {
        violation(workflowPath, step.line,
            "declares both uses and run, which GitHub Actions does not allow.");
    }
    return step;
}

function parseSteps(lines, start, end, workflowPath)
{
    const starts = [];
    for (let index = start; index < end; ++index) {
        if (!contentLine(lines[index]))
            continue;
        const indent = indentOf(lines[index]);
        if (indent < 0)
            violation(workflowPath, index + 1, "uses a tab for indentation.");
        if (indent !== 6 || !/^ {6}-/.test(lines[index])) {
            violation(workflowPath, index + 1,
                "is not a block-mapping item directly under steps.");
        }
        starts.push(index);
        for (++index; index < end; ++index) {
            if (contentLine(lines[index]) && indentOf(lines[index]) === 6) {
                --index;
                break;
            }
        }
    }

    const steps = [];
    for (let index = 0; index < starts.length; ++index) {
        steps.push(parseStep(lines, starts[index],
            index + 1 < starts.length ? starts[index + 1] : end,
            workflowPath));
    }
    return steps;
}

function parseNeeds(lines, index, end, raw, workflowPath)
{
    const scalar = withoutComment(raw).trim();
    if (scalar === "") {
        const needs = [];
        for (let scan = index + 1; scan < end; ++scan) {
            if (!contentLine(lines[scan]))
                continue;
            const item = /^ {6}-\s*(.+?)\s*$/.exec(lines[scan]);
            if (!item)
                break;
            needs.push(normalizedScalar(item[1], workflowPath, scan + 1));
        }
        return needs;
    }
    if (scalar[0] !== "[")
        return [normalizedScalar(scalar, workflowPath, index + 1)];
    if (scalar[scalar.length - 1] !== "]")
        violation(workflowPath, index + 1, "contains an unterminated needs list.");
    return scalar.slice(1, -1).split(",")
        .map(value => normalizedScalar(value, workflowPath, index + 1))
        .filter(Boolean);
}

function parseJob(lines, start, end, id, workflowPath)
{
    const job = {
        id,
        line: start + 1,
        needs: [],
        outputs: {},
        steps: []
    };
    for (let index = start + 1; index < end;) {
        const line = lines[index];
        if (!contentLine(line)) {
            ++index;
            continue;
        }
        const indent = indentOf(line);
        if (indent < 0)
            violation(workflowPath, index + 1, "uses a tab for indentation.");
        if (indent !== 4) {
            ++index;
            continue;
        }
        if (/^\s*<<:/.test(line)) {
            violation(workflowPath, index + 1,
                "uses a YAML merge key, which the workflow IR does not support.");
        }
        const entry = mappingEntry(line, 4);
        if (!entry) {
            violation(workflowPath, index + 1,
                "is not a job block-mapping entry.");
        }
        const key = entry[1];
        const raw = entry[2] || "";
        if (key === "needs") {
            job.needs = parseNeeds(lines, index, end, raw, workflowPath);
            ++index;
            continue;
        }
        if (key === "outputs") {
            if (withoutComment(raw).trim() !== "") {
                violation(workflowPath, index + 1,
                    "job outputs must be a block mapping.");
            }
            const mapping = childMapping(lines, index + 1, end, 6,
                workflowPath, "job outputs");
            job.outputs = mapping.values;
            index = mapping.next;
            continue;
        }
        if (key === "steps") {
            if (withoutComment(raw).trim() !== "") {
                violation(workflowPath, index + 1,
                    "steps must be a block sequence; flow steps and aliases" +
                    " are unsupported.");
            }
            let sectionEnd = index + 1;
            while (sectionEnd < end) {
                if (contentLine(lines[sectionEnd]) &&
                    indentOf(lines[sectionEnd]) <= 4) {
                    break;
                }
                ++sectionEnd;
            }
            job.steps = parseSteps(lines, index + 1, sectionEnd, workflowPath);
            index = sectionEnd;
            continue;
        }
        const rawValue = withoutComment(raw).trim();
        if (/^[&*][A-Za-z0-9_-]+(?:\s|$)/.test(rawValue)) {
            violation(workflowPath, index + 1,
                "uses an anchor or alias in a job field.");
        }
        ++index;
    }
    return job;
}

function readWorkflow(root, relativePath)
{
    const text = fs.readFileSync(path.join(root, relativePath), "utf8");
    const lines = text.split(/\r?\n/);
    const jobsLine = lines.findIndex(line => /^jobs:\s*(?:#.*)?$/.test(line));
    if (jobsLine < 0)
        violation(relativePath, 1, "declares no block-mapping jobs section.");

    const starts = [];
    for (let index = jobsLine + 1; index < lines.length; ++index) {
        if (!contentLine(lines[index]))
            continue;
        const indent = indentOf(lines[index]);
        if (indent < 0)
            violation(relativePath, index + 1, "uses a tab for indentation.");
        if (indent === 0)
            break;
        if (indent !== 2)
            continue;
        if (/^\s*<<:/.test(lines[index])) {
            violation(relativePath, index + 1,
                "uses a YAML merge key in jobs.");
        }
        const header = /^ {2}([A-Za-z0-9_-]+):\s*(?:#.*)?$/.exec(lines[index]);
        if (!header) {
            violation(relativePath, index + 1,
                "does not declare a block-mapping job.");
        }
        starts.push({ index, id: header[1] });
    }
    if (starts.length === 0)
        violation(relativePath, jobsLine + 1, "declares no jobs.");

    const jobs = new Map();
    for (let index = 0; index < starts.length; ++index) {
        const start = starts[index];
        if (jobs.has(start.id))
            violation(relativePath, start.index + 1, "duplicates job " + start.id + ".");
        jobs.set(start.id, parseJob(lines, start.index,
            index + 1 < starts.length ? starts[index + 1].index : lines.length,
            start.id, relativePath));
    }
    return { path: relativePath, jobs };
}

function readWorkflows(root)
{
    const workflows = new Map();
    for (const relativePath of workflowFiles(root))
        workflows.set(relativePath, readWorkflow(root, relativePath));
    return workflows;
}

function stepScalars(step)
{
    const scalars = [];
    for (const field of [
        step.name,
        step.uses,
        step.run,
        step.if,
        step.continueOnError,
        step.timeoutMinutes,
        step.shell,
        step.workingDirectory
    ]) {
        if (field)
            scalars.push(field.value);
    }
    for (const mapping of [step.with, step.env]) {
        for (const entry of Object.values(mapping))
            scalars.push(entry.value);
    }
    return scalars;
}

module.exports = {
    WORKFLOW_DIRECTORY,
    workflowFiles,
    readWorkflow,
    readWorkflows,
    stepScalars
};
