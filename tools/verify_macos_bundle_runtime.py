#!/usr/bin/env python3
"""Exercise an extracted macOS bundle without the build machine's Qt environment."""

import argparse
import json
import os
from pathlib import Path
import re
import subprocess
from urllib.parse import unquote, urlparse


def verify_loaded_runtime(app_bundle, trace):
    # Inspect successful loads, not search paths: Qt can list a compiled-in SDK
    # path without using it. Embedded qrc imports belong to the executable.
    imported_modules = set()
    for location in re.findall(r'importExtension:.* loaded "([^"]+)"', trace):
        if location.startswith((":/", "qrc:/")):
            continue
        path = Path(unquote(urlparse(location).path)).resolve(strict=True)
        if not path.is_relative_to(app_bundle):
            raise RuntimeError(f"QML import resolved outside the bundle: {path}")
        imported_modules.add(path.relative_to(app_bundle).as_posix())
    if not any(path.endswith("/QtQuick/Window/qmldir") for path in imported_modules):
        raise RuntimeError("No bundle-local QtQuick.Window import was observed")

    plugin_names = {path.name for path in app_bundle.rglob("*.dylib")}
    loaded_runtime = set()
    for line in trace.splitlines():
        if not line.startswith("dyld["):
            continue
        match = re.search(r" (/.*)$", line)
        if not match:
            continue
        path = Path(match.group(1))
        if not (re.search(r"/Qt[^/]+\.framework/", str(path)) or path.name in plugin_names):
            continue
        path = path.resolve(strict=True)
        if not path.is_relative_to(app_bundle):
            raise RuntimeError(f"Qt runtime loaded outside the bundle: {path}")
        loaded_runtime.add(path.relative_to(app_bundle).as_posix())
    if not any("/QtCore.framework/" in path for path in loaded_runtime):
        raise RuntimeError("No bundle-local QtCore framework load was observed")
    if not any(path.endswith("/libqcocoa.dylib") for path in loaded_runtime):
        raise RuntimeError("No bundle-local Cocoa platform plugin load was observed")
    return sorted(imported_modules), sorted(loaded_runtime)


def verify_runtime(app_bundle, evidence_dir):
    evidence_dir.mkdir(parents=True, exist_ok=False)
    metrics_path = evidence_dir / "metrics.json"
    log_path = evidence_dir / "runtime.log"
    # An explicit environment excludes Qt/QML/DYLD overrides and SDK paths
    # installed by CI. The real desktop session still supplies Cocoa services.
    environment = {
        name: os.environ[name]
        for name in ("HOME", "USER", "LOGNAME", "TMPDIR", "LANG")
        if name in os.environ
    }
    environment.update({
        "PATH": "/usr/bin:/bin:/usr/sbin:/sbin",
        "QT_QPA_PLATFORM": "cocoa",
        "QML_IMPORT_TRACE": "1",
        "QT_DEBUG_PLUGINS": "1",
        "DYLD_PRINT_LIBRARIES": "1",
    })
    command = [
        str(app_bundle / "Contents/MacOS/vnm_terminal"),
        "--require-output", "--timeout-ms", "30000",
        "--metrics-json", str(metrics_path),
        "--", "/bin/sh", "-c",
        "printf 'macOS packaged terminal runtime check\\n'; /bin/sleep 5",
    ]
    try:
        with log_path.open("w", encoding="utf-8") as log:
            subprocess.run(command, cwd=evidence_dir, env=environment,
                           stdout=log, stderr=subprocess.STDOUT, check=True, timeout=45)
        metrics = json.loads(metrics_path.read_text(encoding="utf-8"))
        if not (metrics["output_seen"] and metrics["process_exited"]
                and metrics["process_exit_code"] == 0 and metrics["app_result"] == 0
                and not metrics["timeout_expired"] and metrics["backend_error_count"] == 0):
            raise RuntimeError("Packaged terminal did not complete its PTY command cleanly")
        if not (metrics["startup"]["visible_first_frame_completed"]
                and int(metrics["renderer_frame_evidence"]["frame_count"]) > 0
                and int(metrics["presentation"]["primary_frame_count"]) > 0):
            raise RuntimeError("Packaged terminal did not render and present a frame")
        imported_modules, loaded_runtime = verify_loaded_runtime(
            app_bundle, log_path.read_text(encoding="utf-8", errors="replace"))
    except Exception:
        print(log_path.read_text(encoding="utf-8", errors="replace"), flush=True)
        if metrics_path.is_file():
            print(metrics_path.read_text(encoding="utf-8"), flush=True)
        raise
    print(json.dumps({
        "bundle": str(app_bundle),
        "startup": metrics["startup"],
        "renderer_frame_evidence": metrics["renderer_frame_evidence"],
        "presentation": metrics["presentation"],
        "qml_imports": imported_modules,
        "loaded_qt_runtime": loaded_runtime,
    }, indent=2))
    print(f"Verified packaged macOS Cocoa/PTY runtime; evidence: {evidence_dir}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("app_bundle", type=Path)
    parser.add_argument("evidence_dir", type=Path)
    args = parser.parse_args()
    verify_runtime(args.app_bundle.resolve(strict=True), args.evidence_dir.resolve())


if __name__ == "__main__":
    main()
