// Structural reader for the CI workflow files, shared by the release-manifest
// gate and the dependency-lock gate. Both have to see the same jobs, steps and
// step inputs, and a fork of this reader would mean a workflow one gate can no
// longer parse is still parsed by the other, which is the quiet outcome both
// gates exist to prevent.
//
// This is deliberately a text scanner and not a YAML parser: no YAML parser is
// available to a node program in this repository without adding a dependency,
// and none is added. The trade is stated where it matters: a caller that finds
// nothing where it expected something must treat that as a failure, because a
// silently empty extraction is the one result this reader must never be
// allowed to produce.

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

// The line span of every job, so that a rule can ask what one job contains
// without re-deriving the job boundaries at each use.
function jobRegions(lines)
{
    const regions = new Map();
    let current = null;
    for (let index = 0; index < lines.length; ++index) {
        const header = /^ {2}([A-Za-z0-9_-]+):\s*$/.exec(lines[index]);
        if (!header)
            continue;
        if (current !== null)
            regions.get(current).lastLine = index;
        current = header[1];
        regions.set(current, { firstLine: index, lastLine: lines.length });
    }
    return regions;
}

// The `needs:` of every job, in either the flow-sequence or the block-sequence
// form. Both appear in this tree.
function jobNeeds(lines)
{
    const needs = new Map();
    let job = null;
    for (let index = 0; index < lines.length; ++index) {
        const header = /^ {2}([A-Za-z0-9_-]+):\s*$/.exec(lines[index]);
        if (header) {
            job = header[1];
            needs.set(job, []);
            continue;
        }
        if (job === null)
            continue;

        const inline = /^ {4}needs:\s*(.+?)\s*$/.exec(lines[index]);
        if (inline) {
            needs.set(job, inline[1]
                .replace(/^\[|\]$/g, "")
                .split(",")
                .map(entry => entry.trim().replace(/^['"]|['"]$/g, ""))
                .filter(Boolean));
            continue;
        }
        if (!/^ {4}needs:\s*$/.test(lines[index]))
            continue;

        const listed = [];
        for (let scan = index + 1; scan < lines.length; ++scan) {
            const item = /^ {6}-\s*(.+?)\s*$/.exec(lines[scan]);
            if (!item)
                break;
            listed.push(item[1].replace(/^['"]|['"]$/g, ""));
        }
        needs.set(job, listed);
    }
    return needs;
}

module.exports = {
    stepBlocks,
    withMapping,
    jobNameAt,
    jobRegions,
    jobNeeds
};
