#!/usr/bin/env python3
"""Inspect expanded runtime files and report equal-content candidates for review."""

import argparse
import hashlib
import json
from pathlib import Path
import re


def duplicate_role(paths):
    parents = {str(Path(path).parent).replace("\\", "/") for path in paths}
    if (len(paths) == 2 and len(parents) == 1
            and next(iter(parents)).endswith("qml/QtQuick/Controls/Basic")
            and {Path(path).name for path in paths} == {
                "HorizontalHeaderViewDelegate.qml", "VerticalHeaderViewDelegate.qml"}):
        return "Distinct Qt types registered separately in Basic/qmldir"
    privacy = [re.fullmatch(
        r"Contents/Frameworks/(Qt\w+)\.framework/Versions/[^/]+/Resources/PrivacyInfo\.xcprivacy",
        path) for path in paths]
    if all(privacy) and len({match[1] for match in privacy}) == len(paths):
        return "Privacy manifest belongs to each distinct Qt framework bundle"
    if set(paths) in (
        {"usr/share/doc/vnm_terminal/LICENSE", "usr/share/licenses/vnm_process_custody/LICENSE"},
        {"LICENSE", "share/licenses/vnm_process_custody/LICENSE"},
    ):
        return "GPL license texts identify the application and custody component separately"
    if set(paths) in (
        {"LICENSE", "Licenses/LICENSE.txt"},
        {"LICENSE", "Licenses/LICENSE.txt", "share/licenses/vnm_process_custody/LICENSE"},
    ):
        return "IFW's native License operation manages Licenses/LICENSE.txt beside component notices"
    notices = {
        "libgssapi-krb5-2", "libk5crypto3", "libkrb5-3", "libkrb5support0",
        "libxcb-xkb1", "libxcb-shm0", "libxcb-randr0", "libxcb-render0",
        "libxcb-shape0", "libxcb-xfixes0", "libxcb-glx0", "libxcb-sync1",
        "libwayland-cursor0", "libwayland-egl1", "libxkbcommon-x11-0", "libxkbcommon0",
    }
    if all(Path(path).parent.parent.as_posix() == "usr/share/doc"
           and Path(path).name == "copyright" and Path(path).parent.name in notices
           for path in paths):
        return "linuxdeploy retains the copyright notice at each supplied component's package path"
    return None


def inspect_contents(root):
    hashes = {}
    files = {}
    for path in sorted(root.rglob("*")):
        if not path.is_file() or path.is_symlink():
            continue
        relative = path.relative_to(root).as_posix()
        if path.suffix.lower() in {
            ".a", ".lib", ".o", ".obj", ".pdb", ".debug", ".dwo", ".dwp",
            ".map", ".cpp", ".c", ".h", ".hpp",
        } or any(part.endswith(".dSYM") for part in path.parts):
            raise RuntimeError(f"Development/source/relinking material: {relative}")
        with path.open("rb") as stream:
            header = stream.read(512)
            if (header.startswith((b"PK\x03\x04", b"PK\x05\x06", b"!<arch>\n",
                                   b"7z\xbc\xaf\x27\x1c", b"\x1f\x8b", b"BZh",
                                   b"\xfd7zXZ\x00", b"\x28\xb5\x2f\xfd"))
                    or header[257:262] == b"ustar"):
                raise RuntimeError(f"Nested archive requires expansion and inspection: {relative}")
            stream.seek(0)
            digest = hashlib.sha256()
            for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(chunk)
            digest = digest.hexdigest()
        files[relative] = digest
        hashes.setdefault(digest, []).append(relative)
    candidates = [
        {"sha256": digest, "paths": paths, "role": duplicate_role(paths)}
        for digest, paths in sorted(hashes.items()) if len(paths) > 1
    ]
    print(json.dumps({
        "payload": str(root),
        "file_count": len(files),
        "equal_content_candidates": candidates,
    }, indent=2))
    if any(candidate["role"] is None for candidate in candidates):
        raise RuntimeError("New equal-content paths require role assessment before publication")
    return files


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("payload", type=Path)
    parser.add_argument("--reference", type=Path,
                        help="Require packed files to equal the final staging tree")
    args = parser.parse_args()
    files = inspect_contents(args.payload.resolve(strict=True))
    if args.reference:
        reference = inspect_contents(args.reference.resolve(strict=True))
        if files != reference:
            changed = sorted(path for path in files.keys() | reference.keys()
                             if files.get(path) != reference.get(path))
            raise RuntimeError(f"Packed content differs from staging: {changed}")


if __name__ == "__main__":
    main()
