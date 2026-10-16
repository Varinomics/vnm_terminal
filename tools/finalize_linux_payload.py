#!/usr/bin/env python3
"""Remove development data from a staged Linux runtime, preserving loader paths."""

import argparse
import filecmp
import os
from pathlib import Path
import re
import shutil
import subprocess

from inspect_runtime_contents import inspect_contents


def inspect_payload(payload):
    inspect_contents(payload)
    prefix = payload / "usr" if (payload / "usr/bin/vnm_terminal").is_file() else payload
    for name in ("vnm_terminal", "vnm_process_custody_owner"):
        executable = prefix / "bin" / name
        if not executable.is_file() or executable.is_symlink():
            raise RuntimeError(f"Missing controlled executable: {executable}")
    for path in sorted(payload.rglob("*")):
        if not path.is_file() or path.is_symlink():
            continue
        with path.open("rb") as stream:
            if stream.read(4) != b"\x7fELF":
                continue
        if path.name in {"vnm_terminal", "vnm_process_custody_owner"}:
            sections = subprocess.run(["readelf", "-WS", str(path)], check=True,
                                      capture_output=True, text=True).stdout
            if re.search(r"\.(?:z?debug\S*|stab\S*|symtab|gdb_index|gnu_debug\S*)\s",
                         sections):
                raise RuntimeError(f"Controlled Release binary retains debug data: {path}")
        else:
            print(f"Supplied vendor runtime (debug metadata permitted): {path}")


def finalize_payload(payload):
    prefix = payload / "usr"
    for name in ("vnm_terminal", "vnm_process_custody_owner"):
        executable = prefix / "bin" / name
        if not executable.is_file() or executable.is_symlink():
            raise RuntimeError(f"Missing owned executable: {executable}")
        subprocess.run(["strip", "--strip-unneeded", str(executable)], check=True)

    # The fonts are linked into the executable. This install target also exports
    # its SDK archive, which has no role in a runtime package.
    for archive in prefix.glob("lib*/**/libvnm_fonts.a"):
        archive.unlink()
    font_headers = prefix / "include/vnm_fonts"
    if font_headers.is_dir():
        shutil.rmtree(font_headers)
    for font_exports in prefix.glob("lib*/**/cmake/vnm_fonts"):
        shutil.rmtree(font_exports)

    runtime_directories = list(prefix.glob("lib*/vnm_terminal"))
    runtime_directories += list(prefix.glob("lib*/*/vnm_terminal"))
    if len(runtime_directories) != 1:
        raise RuntimeError("Expected one deployed vnm_terminal library directory")
    runtime = runtime_directories[0]

    # linuxdeploy copies Qt/ICU to usr/lib and rewrites the app RUNPATH there.
    # qt.conf and the deployed plugins still use the original runtime directory.
    # Preserve both paths, sharing bytes only when the existing SONAME target is
    # identical; never deduplicate unrelated resources or change library versions.
    linked_bytes = 0
    linked_files = 0
    for pattern in ("libQt6*.so*", "libicu*.so*"):
        for deployed in (prefix / "lib").glob(pattern):
            original = runtime / deployed.name
            if (deployed.is_symlink() or not deployed.is_file()
                    or not original.is_file()):
                continue
            if not filecmp.cmp(deployed, original, shallow=False):
                raise RuntimeError(f"Conflicting deployed library: {deployed}")
            linked_bytes += deployed.stat().st_size
            deployed.unlink()
            deployed.symlink_to(os.path.relpath(original, deployed.parent))
            linked_files += 1
    print(f"Finalized {payload}: shared {linked_files} libraries ({linked_bytes} bytes)")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    roots = parser.add_mutually_exclusive_group(required=True)
    roots.add_argument("--payload", type=Path)
    roots.add_argument("--cpack-root", type=Path)
    parser.add_argument("--inspect-only", action="store_true",
                        help="Inspect expanded packages or installed IFW files without modifying them")
    args = parser.parse_args()
    if args.payload:
        payload = args.payload.resolve(strict=True)
    else:
        binaries = list(args.cpack_root.rglob("usr/bin/vnm_terminal"))
        if len(binaries) != 1:
            raise RuntimeError("Expected one vnm_terminal runtime in the CPack staging tree")
        payload = binaries[0].parents[2]
    if not args.inspect_only:
        finalize_payload(payload)
    inspect_payload(payload)


if __name__ == "__main__":
    main()
