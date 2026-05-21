# Third-Party Notices

`vnm_terminal` is distributed under the project license in `LICENSE`.

## vnm_terminal_surface

The app depends on the Varinomics `vnm_terminal_surface` library for terminal
parsing, process hosting, screen state, and rendering.

`vnm_terminal_surface` is distributed under the BSD 2-Clause license.

Repository:

- https://github.com/Varinomics/vnm_terminal_surface

## Qt 6

Qt 6 Core, Gui, and Quick are required. The project uses Qt through either a
commercial Qt license held by the distributor or an LGPLv3-compatible
dynamic-linking posture. No GPL-only Qt module is allowed in the product
dependency graph.

Qt upstream notices and license texts are supplied by the installed Qt package
and the Qt Company distribution materials:

- https://www.qt.io/licensing/
- https://doc.qt.io/qt-6/licenses-used-in-qt.html

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
