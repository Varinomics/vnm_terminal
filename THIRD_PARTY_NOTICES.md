# Third-Party Notices

`vnm_terminal` is distributed under the project license in `LICENSE`.

## vnm_terminal_surface

The app depends on the Varinomics `vnm_terminal_surface` library for terminal
parsing, process hosting, screen state, and rendering.

`vnm_terminal_surface` is distributed under the GNU General Public License
version 3 only.

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

## Qt Installer Framework

The Windows EXE installer and installed maintenance tool are built with Qt
Installer Framework 4.11.0 under GPLv3 with The Qt Company GPL Exception 1.0.
The exception text is installed as
`QT_INSTALLER_FRAMEWORK_LICENSE_EXCEPTION.txt`.

The complete corresponding upstream source is available without charge from:

- https://download.qt.io/official_releases/qt-installer-framework/4.11.0/installer-framework-everywhere-src-4.11.0.zip
- https://download.qt.io/archive/qt/6.9/6.9.3/single/qt-everywhere-src-6.9.3.zip

No Varinomics modifications are made to Qt Installer Framework or its Qt
runtime. The installer configuration and package scripts are included in this
repository under `packaging/windows/ifw`.

### libarchive 3.8.5

Qt Installer Framework uses libarchive to read and write archive files.

Copyright (c) 2003-2018 Tim Kientzle
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice,
   this list of conditions and the following disclaimer in this position and
   unchanged.
2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE AUTHOR(S) ``AS IS'' AND ANY EXPRESS OR
IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO
EVENT SHALL THE AUTHOR(S) BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER
IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.

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
