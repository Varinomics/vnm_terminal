# Windows installer engine

The Windows installer builds Qt Installer Framework 4.11.0 with the responsive
Modern wizard margins and high-DPI logo rendering adopted from Soter. Its native
publication change also publishes the packaged maintenance executable by a
same-directory rename, preserving its signature without temporary executable
copies. The product controller owns replacement of an existing installation.
The patch also corrects embedded archive enumeration with Qt 6.12, whose directory
listing flags otherwise cause IFW 4.11 to skip the packaged files.

Build the engine with a static Qt 6.12.0 prefix and static xz/bzip2 prefixes:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass `
  -File tools/build_windows_private_ifw.ps1 `
  -QtIfwRepositoryPath C:/deps/installer-framework `
  -QtRoot C:/deps/qt-static -XzRoot C:/deps/xz -Bzip2Root C:/deps/bzip2 `
  -OutputRoot C:/build/vnm-terminal-ifw -Jobs 4
```

The script selects the source recorded in `source-lock.json`, verifies its tree,
applies `qt-ifw-4.11-terminal.patch`, and uses Visual Studio 2026 x64 with MSVC
v145. Its qmake and jom commands run through `queued-build`. Qt IFW's qmake
Makefiles do not provide a FASTBuild backend.

Set `IFW_ROOT` to `C:/build/vnm-terminal-ifw/build`. Packaging and cache reuse
validate the recorded patch and binary hashes with
`tools/assert_windows_private_ifw.ps1`. The build retains its source archive,
patch, generated Makefiles, object files and exact input identities. Pass the
dependency provisioner's `source-material` directory as `-InputMaterialRoot` to
create the separate corresponding-source packet. It is published as
`vnm_terminal_v<version>_windows_installer_source.zip`; the installed application
does not need it. See `RELINK-INSTRUCTIONS.txt` for automated and offline rebuilds.

The maintenance component declares its packaged installer base when its script
loads. Qt IFW then includes that component once in its installation estimate;
it does not add a second copy of the running installer's embedded base.
The engine keeps that signed base at its extracted path until maintenance
metadata is ready. Publication failure propagates into installation rollback;
it preserves the original base and creates no uninstall registration. Update
mode and bases supplied from a different directory retain the framework path.
