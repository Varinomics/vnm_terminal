# Releasing vnm_terminal

A release is described by two checked-in declarations.
`release/dependencies.lock.json` says what a release was built from: the commit
of each owned dependency and the commit each pinned third-party tag named.
`release/artifacts.json` says which Actions artifacts a release produces, so
that the retention workflow can derive the families it prunes instead of
restating their names. `tools/dependencies_lock.js` and
`tools/release_artifacts.js` compare both against the workflows, and `ctest`
runs them on every host.

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
   node tools/dependencies_lock.js check .
   node tools/release_artifacts.js check .
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
  application, of each owned dependency, and of each third-party clone, taken
  from the trees the run actually compiled.
- The signing job compares that record with the lock at the tag before it signs
  anything, and verifies the payload file by file against the hashes the package
  job recorded.
- The job that attaches the Windows packages downloads the signed artifact and
  nothing else. That is what keeps the unsigned build out of a release: the
  unsigned portable archive has the same file name as the archive rebuilt from
  the signed payload, so an extra download into that directory would replace it.
  `tests/windows_ifw_contract_tests.ps1` asserts the absence of that download.

## Re-signing a release that failed after packaging

Dispatch the CI Windows workflow with `release_tag` set to the published tag.
The run resolves from the lock without the staleness comparison, so a tag can be
rebuilt after master has moved on, and it refuses to sign unless the tag names
the commit that was built.

A tag cut before `release/dependencies.lock.json` existed cannot be rebuilt this
way. There is nothing at such a tag recording which dependency commits it was
built from, so no rebuild of it could be verified against anything; the resolve
job says so and stops.

The dependency lock owns the commits, not the dependency policy. Every owned
repository still tracks `master`, in `FetchContent` declarations and in the
ordinary CI runs that resolve master fresh on every push. The lock records what
one release was cut from so that it can be rebuilt, and the gate refuses a lock
entry whose branch is anything but `master` for exactly that reason.
