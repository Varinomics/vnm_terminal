# Releasing vnm_terminal

Every new `vnm_terminal_surface` release requires a coordinated `vnm_terminal`
release using the same version in both repositories' `project()` declarations,
tags, and GitHub releases. Follow the surface's
[release guide](https://github.com/Varinomics/vnm_terminal_surface/blob/master/docs/releasing.md)
for provider-first publication and source-release validation. Matching versions
record release coordination; the required surface targets and API determine
build compatibility.

A release is described by two checked-in declarations.
`release/dependencies.lock.json` says what a release was built from: the commit
of each owned dependency and the commit each pinned third-party tag named.
`release/artifacts.json` says which Actions artifacts a release produces, so
that the retention workflow can derive the families it prunes instead of
restating their names. `tools/dependencies_lock.js` and
`tools/release_artifacts.js` compare both against the workflows, and `ctest`
runs them on every host.

## Local packaging

### Windows

Copy [`build_config.bat.example`](../build_config.bat.example) to
`build_config.bat` and set the local Qt, MinGW, and dependency paths.

Build the source-based Qt Installer Framework engine using the
[private IFW instructions](../packaging/windows/private-ifw/README.md), then
set `IFW_ROOT` to its `build` directory. The package builder checks the
engine's patch and binary provenance; rebuild the engine when those inputs
change.

Prepare the source-built ConPTY runtime:

```powershell
pwsh.exe -NoProfile -File tools/provision_windows_conpty.ps1 `
  -SourceRoot ../vnm_terminal_surface -OutputRoot build_ifw/conpty -Slots 2
```

Set `VNM_TERMINAL_CONPTY_PACKAGE_DIR` in `build_config.bat` to the resulting
`package` directory, then build:

```powershell
queued-build --slots 4 -- cmd.exe /d /c build_windows_packages.bat
```

Local and CI verification installers are named
`vnm_terminal_v<version>_windows_x64_unsigned.exe`. A release EXE drops the
`_unsigned` suffix only after its payload, maintenance tool, and final installer
are signed as Varinomics Ltd and timestamped.

### Linux RUN installer

Build from a staged CMake install tree containing the application and runtime:

```sh
queued-build --slots 1 -- tools/build_linux_ifw_installer.sh \
  --payload AppDir \
  --ifw-root /path/to/QtInstallerFramework/4.7
```

## Cutting a release

1. **Finalize the version**, in the `project()` call in `CMakeLists.txt`. The
   asset names are rendered from it and nothing else needs editing for a version
   bump. For a coordinated surface release, use the same version in both
   repositories.

2. **Refresh the dependency lock.**

   ```bash
   node tools/dependencies_lock.js refresh .
   git diff release/dependencies.lock.json
   ```

   This is not optional and it is not a formality. A `release: published` run
   resolves the dependency commits from the lock and then resolves each branch
   head again, and it fails when the two disagree. If any of the five owned
   repositories has moved since the lock was last refreshed, all three release
   workflows stop at their first job. Refreshing also re-reads the commit each
   third-party tag currently names, so an upstream tag that moved shows up in
   the diff rather than in a differently linked binary.

3. **Review, commit, push, and use applicable hosted CI.** Obtain independent
   review, then stage and commit the finalized version and dependency lock and
   push to `master`. Publish the provider's source changes first for a
   coordinated release. Applicable hosted CI runs the declaration checks and
   platform tests; do not duplicate that validation locally. If no applicable
   CI covers a required check, run the relevant local check once before
   publication, following the shared governance and build-queue rules.
   Inspect the candidate's package results as described below before tagging.

4. **Tag the commit that carries the refreshed lock**, and publish the release.
   The tag has to name that commit: the signing job checks the release source
   out by tag, reads the lock from it, and refuses a payload whose recorded
   dependency commits disagree with it.

## Runtime payload inspection

The platform workflows inspect both staging trees and expanded deliverables
before uploading packages. Windows checks the portable ZIP against its final
staging tree and checks the actual EXE installation before uninstalling it,
including the installer and maintenance tool. The same ZIP check runs again
after signing. Linux checks the staged runtime, the actual RUN installation,
and extracted DEB, RPM and AppImage contents. macOS checks the staged app and
extracted ZIP, including architecture and native rendering/PTY execution.

The existing platform binary readers check controlled executables for embedded
debug data. Build settings require true Release inputs; stripping does not
make another configuration a release input. Supplied Qt/system runtimes retain
their vendor metadata under the vendor-binary exception, and inspection logs
identify those binaries. Source-built ConPTY is checked at receipt intake and
as part of the delivered Windows runtime. Matching installer source remains a
separate download; source/relink objects and unexpanded nested archives fail
the runtime content check.

`tools/inspect_runtime_contents.py` inventories all regular files by content,
including renamed copies. It preserves symlink-based loader layouts and reports
equal-content groups with their assessed roles. The adopted roles are distinct
Basic header-delegate types registered in Qt's `qmldir`, privacy manifests at
each Qt framework's required path, and component-specific Linux license notices.
Qt IFW's native License operation also retains the declared application license
at `Licenses/LICENSE.txt`, using native platform path casing; that managed
installation path remains alongside the application's license.
Other groups stop packaging for role assessment; equality alone is not a
packaging defect. Investigate the actual readers and layout before changing
either the package or the role check. Keep legitimate resource identities;
remove unnecessary copies and verify the resulting runtime loading.

Before publication, review the current candidate's inspection output and source
download routes, rather than substituting a previous artifact's results.
Signing/repacking uses the verified payload and recipe; final expansion repeats
the same role checks, and the portable ZIP must match signed staging byte for
byte. A materially new path group therefore requires assessment before upload.
macOS deploys the Basic style selected by the application, preserves its shared
implementation, and omits unused style trees and Qt Creator metadata.

## What a release run does

- `resolve-dependencies` resolves every dependency commit once, from the lock,
  and every build, package and test job of that run checks out what it resolved.
  One workflow run therefore builds one source.
- Release packages record the application, owned-dependency, and third-party
  commits from the trees each platform actually compiled:
  - Linux attaches `vnm_terminal_linux_x64_build_provenance.json`.
  - Windows attaches `vnm_terminal_windows_x64_build_provenance.json`.
  - macOS embeds `vnm_terminal_build_provenance.json` under
    `Contents/Resources` and attaches no standalone provenance JSON.
- macOS builds and tests natively on Intel and Apple Silicon, then attaches
  separate `macos_x64_unnotarized.zip` and `macos_arm64_unnotarized.zip` bundles.
  Each bundle records its architecture in `vnm_terminal_build_info.txt`; every
  bundled Mach-O component must contain that architecture. CI checks the staged
  bundle and the extracted ZIP before uploading either architecture's artifact.
  Deployment scans `packaging/vnm_terminal_runtime_imports.qml` for the embedded
  UI's Qt imports. The extracted app must open its Cocoa window, render frames,
  and complete a PTY command with the build machine's Qt environment removed;
  loader and QML traces must resolve the Qt runtime from inside the bundle.
  The bundle includes the FreeType acknowledgement and the FreeType/msdfgen
  license files from the pinned sources; CI compares those files after ZIP extraction.
  The bundle verifiers come from the workflow commit, so a tagged rebuild uses
  the same checks without changing the tagged application source.
- The Windows signing job compares the Windows provenance record with the lock
  at the tag before it signs anything, and verifies the payload file by file
  against the hashes the package job recorded.
- The job that attaches the Windows packages downloads the signed artifact and
  the separate installer corresponding-source archive. That keeps the unsigned
  build out of a release: the
  unsigned portable archive has the same file name as the archive rebuilt from
  the signed payload, so an extra download into that directory would replace it.
  `tests/windows_ifw_contract_tests.ps1` asserts the absence of that download.
- The source-built Windows installer engine has a separate
  `vnm_terminal_v<version>_windows_installer_source.zip` download and checksum.
  It contains the matching Qt, Installer Framework and compression sources,
  the appearance patch, build recipes and provenance. It is published alongside
  the signed packages and stays outside the installed application payload.

## Re-signing a release that failed after packaging

Dispatch the CI Windows workflow with `release_tag` set to the published tag.
The run resolves from the lock without the staleness comparison, so a tag can be
rebuilt after master has moved on, and it refuses to sign unless the tag names
the commit that was built.

A tag cut before `release/dependencies.lock.json` existed cannot be rebuilt this
way. There is nothing at such a tag recording which dependency commits it was
built from, so no rebuild of it could be verified against anything; the resolve
job says so and stops.

## Recovering Linux packages

Dispatch CI Linux from the branch containing the packaging correction, with
`release_tag` set to the existing release. The application and dependencies
still come from that immutable tag and its lock. The Linux finalizer and IFW
assembler come from the exact workflow commit, outside the application tree.
The package provenance's workflow run ID links to that public recipe commit;
the application source tuple continues to identify the tagged compiled source.
This allows a packaging correction without moving the release tag. Verify all
four regenerated formats before promoting the release as latest.

The dependency lock owns the commits, not the dependency policy. Every owned
repository still tracks `master`, in `FetchContent` declarations and in the
ordinary CI runs that resolve master fresh on every push. The lock records what
one release was cut from so that it can be rebuilt, and the gate refuses a lock
entry whose branch is anything but `master` for exactly that reason.
