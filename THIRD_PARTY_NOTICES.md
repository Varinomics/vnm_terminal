# Third-Party Notices

`vnm_terminal` is distributed under the project license in `LICENSE`.

## vnm_terminal_surface

The app depends on the Varinomics `vnm_terminal_surface` library for terminal
parsing, process hosting, screen state, and rendering.

`vnm_terminal_surface` is distributed under the BSD 2-Clause license.

Repository:

- https://github.com/Varinomics/vnm_terminal_surface

## vnm_qml_chrome

The app depends on the Varinomics `vnm_qml_chrome` library for shared Qt Quick
window chrome and frame-shell components.

Repository:

- https://github.com/imakris/vnm_qml_chrome

The dependency does not include a license file and is treated as an internal
Varinomics dependency.

## Qt 6

Qt 6 Core, Gui, Qml, Quick, Quick Controls 2, and Quick Layouts are required.
Windows portable packages also deploy the Qt platform, image-format, and Quick
Controls 2 runtime plugins used by those modules. The project uses Qt through
either a commercial Qt license held by the distributor or an LGPLv3-compatible
dynamic-linking posture. No GPL-only Qt module is allowed in the product
dependency graph.

Qt upstream notices and license texts are supplied by the installed Qt package
and the Qt Company distribution materials:

- https://www.qt.io/licensing/
- https://doc.qt.io/qt-6/licenses-used-in-qt.html

## CMDG

The source tree vendors CMDG as a terminal graphics workload and demo source
used by CMDG/Nelostie validation paths.

Repository:

- https://github.com/Byproduct/CMDG

CMDG carries a custom permissive `MEGAKORP License v0.0000001` in
`THIRD_PARTY/CMDG/LICENSE`. The license allows use, modification, and
distribution, including commercial use, requires credit to the original
creators, and disclaims warranty.

CMDG is built only for the CMDG/Nelostie benchmark path. Its project file
restores these direct NuGet dependencies:

- NAudio 2.2.1, MIT license
- NVorbis 0.10.5, MIT license
- System.Drawing.Common 9.0.2, MIT license, with .NET third-party notices

The `net8.0` transitive package graph also includes Microsoft.NETCore.Platforms
3.1.0, Microsoft.Win32.Registry 4.7.0,
Microsoft.Win32.SystemEvents 9.0.2, NAudio.Asio/Core/Midi/Wasapi/WinMM 2.2.1,
System.Memory 4.5.3, System.Security.AccessControl 4.7.0,
System.Security.Principal.Windows 4.7.0, and System.ValueTuple 4.5.0.

The NuGet package metadata and license files are supplied by the restored
packages:

- https://www.nuget.org/packages/NAudio/2.2.1
- https://www.nuget.org/packages/NVorbis/0.10.5
- https://www.nuget.org/packages/System.Drawing.Common/9.0.2

One vendored CMDG scene also identifies vehicle model assets as CC0 assets by
eracoon from OpenGameArt:

- https://opengameart.org/content/vehicles-assets-pt1

## GCC / MinGW Runtime Libraries

Windows portable packages built with the Qt MinGW kit include the GCC and
MinGW runtime DLLs required by the application, such as `libgcc_s_seh-1.dll`,
`libstdc++-6.dll`, and `libwinpthread-1.dll`.

The GCC runtime libraries are distributed under GPLv3 with the GCC Runtime
Library Exception 3.1. MinGW-w64 runtime components are permissively licensed;
license details are carried by the upstream sources and distribution package.

References:

- https://gcc.gnu.org/onlinedocs/libstdc++/manual/license.html
- https://www.gnu.org/licenses/gcc-exception-3.1-faq.html
- https://www.mingw-w64.org/support/
