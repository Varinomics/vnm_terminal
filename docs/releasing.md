# Releasing vnm_terminal

A release is described by two checked-in declarations. `release/manifest.json`
says what a release is: the artifacts the workflows produce, the assets those
artifacts carry, the checksum convention, the signing identity, and the Qt and
Qt Installer Framework versions it is built with. `release/dependencies.lock.json`
says what it was built from: the commit of each owned dependency and the commit
each pinned third-party tag named. `tools/release_manifest.js` and
`tools/dependencies_lock.js` compare both against the workflows and scripts, and
`ctest` runs them on every host.

## Cutting a release

1. **Refresh the dependency lock and commit it.**

   ```bash
   node tools/dependencies_lock.js refresh .
   git diff release/dependencies.lock.json
   git commit release/dependencies.lock.json -m "Refresh the release dependency lock"
   ```

   This is not optional and it is not a formality. A `release: published` run
   resolves the dependency commits from the lock and then resolves each branch
   head again, and it fails when the two disagree. If any of the four owned
   repositories has moved since the lock was last refreshed, all three release
   workflows stop at their first job. Refreshing also re-reads the commit each
   third-party tag currently names, so an upstream tag that moved shows up in
   the diff rather than in a differently linked binary.

2. **Run the gates locally.**

   ```bash
   node tools/release_manifest.js check .
   node tools/dependencies_lock.js check .
   ctest --test-dir <build> --output-on-failure
   ```

3. **Bump the version if this release changes it**, in the `project()` call in
   `CMakeLists.txt`. The asset names are rendered from it and nothing else needs
   editing for a version bump.

4. **Tag the commit that carries the refreshed lock**, and publish the release.
   The tag has to name that commit: the signing job checks the release source
   out by tag, reads the lock from it, and refuses a payload whose recorded
   dependency commits disagree with it.

## What a release run does

- `resolve-dependencies` resolves every dependency commit once, from the lock,
  and every build, package and test job of that run checks out what it resolved.
  One workflow run therefore builds one source.
- The Windows package job records `build_provenance.json`: the commit of the
  application, of each owned dependency, and of each third-party clone, plus the
  Qt and Qt Installer Framework versions taken from the manifest.
- The signing job compares that record with the lock at the tag before it signs
  anything, and verifies the payload file by file against the hashes the package
  job recorded.
- The attaching jobs download only the artifacts `release_attachments` declares
  as their sources. That is what keeps the unsigned build out of a release: the
  unsigned portable archive has the same file name as the archive rebuilt from
  the signed payload, so an extra download into that directory would replace it.

## Re-signing a release that failed after packaging

Dispatch the CI Windows workflow with `release_tag` set to the published tag.
The run resolves from the lock without the staleness comparison, so a tag can be
rebuilt after master has moved on, and it refuses to sign unless the tag names
the commit that was built.

A tag cut before `release/dependencies.lock.json` existed cannot be rebuilt this
way. There is nothing at such a tag recording which dependency commits it was
built from, so no rebuild of it could be verified against anything; the resolve
job says so and stops.

## Bumping Qt or the Qt Installer Framework

Edit `qt.version` or the `qt_ifw` block in `release/manifest.json`. The gate
then names every place that has to follow: the `install-qt-action` steps, the
Qt IFW cache keys, and every directory name built from the version, including
the runner-local root in the Windows workflow and the documented root in
`build_config.bat.example`. Two things it cannot name are listed below.

## Facts the manifest deliberately does not own

The manifest owns a fact when one place can be authoritative and the copies can
be found mechanically. These copies are left alone on purpose, and this is the
record of why. A reader finding one of them should not treat it as an oversight.

| Where | What it restates | Why it stays |
| --- | --- | --- |
| `build_config.bat.example`, `build_portable.bat`, `README.md`, `benchmarks/cmdg_nelostie/run_canonical_atlas_cmdg_gate.ps1` | the Qt version, inside a local installation path or as a documented baseline | These describe a developer's own Qt installation and the minimum the source builds against. `qt.version` is the Qt a release is built with, and the gate binds it to the `install-qt-action` steps that install it. A local path is an example and a baseline is a floor; neither has to move when the release Qt moves, and none of these files can read a JSON declaration. |
| `THIRD_PARTY_NOTICES.md` | the Qt Installer Framework version and the URL of its source archive | A licence-compliance statement about the version actually distributed, so it does have to move with `qt_ifw.version`. It is not gated because a rule that recognises a version in a notices file cannot tell this statement apart from every other version in it, and a rule that fires on the wrong line is worse than this note. Update it when you bump the version. |
| `tools/provision_linux_ifw.sh`, `.github/workflows/ci-linux.yml`, `README.md` | the Linux Qt Installer Framework version and its archive checksum | `qt_ifw` in the manifest is the Windows tool: its rules assert an official Windows x64 archive under the Windows repository path. The Linux installer is built with a different Installer Framework release, provisioned by its own script against its own pinned checksum, and the two move independently. Bumping the Linux tool touches these three files and nothing in the manifest. |

The dependency lock owns the commits, not the dependency policy. Every owned
repository still tracks `master`, in `FetchContent` declarations and in the
ordinary CI runs that resolve master fresh on every push. The lock records what
one release was cut from so that it can be rebuilt, and the gate refuses a lock
entry whose branch is anything but `master` for exactly that reason.
