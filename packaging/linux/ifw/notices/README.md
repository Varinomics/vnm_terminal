# Linux installer engine sources and notices

The Linux `.run` installer uses the unmodified Qt Installer Framework 4.7.0
engine (revision `fad83b7a8ba6a9e3179236d11bba0ceb9b3de5b4`), built with
Qt 6.6.0. This is separate from the application's Qt runtime and the Windows
installer engine described in the application notices.

The entire engine source, including its build instructions and bundled
third-party notices, is available from Qt:

- [Qt Installer Framework 4.7.0 source](https://download.qt.io/official_releases/qt-installer-framework/4.7.0/installer-framework-everywhere-src-4.7.0.zip)
- [Qt 6.6.0 source](https://download.qt.io/archive/qt/6.6/6.6.0/single/qt-everywhere-src-6.6.0.tar.xz)

Qt Installer Framework is copyright The Qt Company Ltd. and contributors,
under GPL version 3 with The Qt Company GPL Exception 1.0; the full terms
are in `LICENSE.GPL3-EXCEPT`. The Qt sources contain their module and
third-party licensing notices; the application's `LICENSE` supplies GPLv3.

The engine reports libarchive 3.7.1, zlib 1.2.11, liblzma 5.2.4 and bzip2 1.0.8.
The bundled libarchive license is reproduced in `libarchive-COPYING`; its
corresponding source is included in the Installer Framework source archive.
The corresponding compression notices are included as `zlib-README`,
`xz-COPYING` and `bzip2-LICENSE`. Their sources are available upstream:

- [zlib 1.2.11](https://zlib.net/fossils/zlib-1.2.11.tar.gz)
- [XZ / liblzma 5.2.4](https://github.com/tukaani-project/xz/releases/tag/v5.2.4)
- [bzip2 1.0.8](https://sourceware.org/pub/bzip2/bzip2-1.0.8.tar.gz)

The application font and process-custody notices are retained separately under
`share/doc/vnm_fonts` and `share/licenses/vnm_process_custody`.
