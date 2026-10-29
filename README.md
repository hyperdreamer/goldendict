## Introduction

<b>GoldenDict</b> is a feature-rich dictionary lookup program, supporting multiple dictionary formats (StarDict/Babylon/Lingvo/Dictd/AARD/MDict/SDict) and online dictionaries, featuring perfect article rendering with the complete markup, illustrations and other content retained, and allowing you to type in words without any accents or correct case.

This version targets **Qt 6** on Linux (native Wayland and explicit X11). The Qt 4
and Qt 5 builds, the WebKit article view and the Qt Multimedia playback backend
have been removed; see [Limitations](#limitations) for the behaviour that changed
with them. Windows and macOS sources are retained but are not validated by this
migration.

## Requirements

* Linux, GCC with C++17 support (validated with GCC 16 and Qt 6.12), GNU make, Git
* Qt 6 development packages, including the QtWebEngine, Qt WebChannel, Qt SVG, Qt
  Help and Qt PDF modules
* Various libraries (zlib, bzip2, lzo2, hunspell, X11/Xtst, ...)

The application links exactly these Qt 6 modules:

```
core gui widgets xml network sql printsupport help svg
webenginecore webenginewidgets webchannel dbus pdf pdfwidgets
```

There is no Qt5Compat, QtMultimedia, QtWebKit or X11Extras dependency, and no
`svgwidgets` module (the SVG use is `QSvgRenderer`).

### Installing External Deps on Arch Linux

    sudo pacman -S --needed base-devel git pkg-config qt6-base qt6-webengine \
         qt6-webchannel qt6-svg qt6-tools hunspell libeb libtiff lzo bzip2 zlib \
         libvorbis libao ffmpeg libx11 libxtst kwindowsystem

`kwindowsystem` is optional: it enables best-effort window activation on Wayland
(see [Optional KDE window-system helper](#optional-kde-window-system-helper)).

### Installing External Deps on Debian/Ubuntu Linux

    sudo apt-get install git pkg-config build-essential qt6-base-dev qt6-webengine-dev \
         qt6-webchannel-dev qt6-svg-dev qt6-tools-dev qt6-tools-dev-tools \
         libvorbis-dev zlib1g-dev libhunspell-dev x11proto-record-dev libxtst-dev \
         liblzo2-dev libbz2-dev libao-dev libavformat-dev libavcodec-dev \
         libavutil-dev libswresample-dev libtiff-dev libeb-dev libkf6windowsystem-dev

## Runtime

GoldenDict needs a working Qt platform plugin and, for article rendering, a
working QtWebEngine (Chromium) runtime. An explicitly selected platform is
respected: `QT_QPA_PLATFORM=wayland` runs on a native Wayland compositor,
`QT_QPA_PLATFORM=xcb` runs through XWayland or plain X11. The build never forces
`xcb`; the platform and the selected hotkey backend are logged at startup.

Hotkeys use the desktop portal on Wayland and the compiled X11 `RECORD` backend on
X11. `HAVE_X11` only records that the X11 backend was compiled; it is not evidence
that the running session is X11.

## How to build

First, clone this repository, e.g.:

    git clone https://github.com/goldendict/goldendict.git

Then build with the Qt 6 qmake:

    cd goldendict && qmake6 && make -j$(nproc)

Alternatively, load `goldendict.pro` in Qt Creator.

`qmake6 goldendict.pro` is the exact equivalent when the checkout directory is not
named `goldendict` (a plain `qmake6` resolves the project by directory name, and an
ambiguous directory with two `.pro` files is rejected).

### Build in the source tree

Configure and build in the source tree. Out-of-source (shadow) build trees are not
supported: two translation units declare a `Q_OBJECT` class in the `.cc` file and
include their own generated `.moc`, and qmake 6.12 emits the dependency on that file
without the rule that produces it, so a shadow tree fails to build.

Note: to compile with `libhunspell` older than 1.5 pass `"CONFIG+=old_hunspell"` to `qmake6`.

### Optional KDE window-system helper

On Wayland, bringing an existing window to the front is a best-effort request. When
the KDE `KWindowSystem` framework is available, GoldenDict uses it for that request.
The helper is enabled only when **both** its header and its library are found:

* default header probe: `/usr/include/KF6/KWindowSystem/kwindowsystem.h`
* default library search: the compiler's own system paths for `libKF6WindowSystem`

Either path can be given explicitly:

    qmake6 KWINDOWSYSTEM_INCLUDEPATH=/usr/include/KF6/KWindowSystem \
           KWINDOWSYSTEM_LIBDIR=/usr/lib

If the header and the library are not both found, GoldenDict is built without
`HAVE_KWINDOWSYSTEM`, omits the include path and `-lKF6WindowSystem`, and keeps
activation best-effort. A missing framework never breaks core Wayland startup.

### Building with Chinese conversion support

To add Chinese conversion support you need at first install libopencc-dev package:

    sudo apt-get install libopencc-dev

Then pass `"CONFIG+=chinese_conversion_support"` to `qmake6`

    qmake6 "CONFIG+=chinese_conversion_support"

Chinese conversion is enabled by default on Windows only; pass
`"CONFIG+=no_chinese_conversion_support"` to turn it off there.

### Building with Zim dictionaries support

To add Zim and Slob formats support you need at first install lzma-dev and zstd-dev packages:

    sudo apt-get install liblzma-dev libzstd-dev

Then pass `"CONFIG+=zim_support"` to `qmake6`

    qmake6 "CONFIG+=zim_support"

Zim support always links `-llzma`; `libzstd` is linked once, through pkg-config when
the `libzstd` package is discoverable and through `-lzstd` otherwise.

### Building without extra tiff handler

If you have problem building with libtiff-dev package, you can pass
`"CONFIG+=no_extra_tiff_handler"` to `qmake6` in order to disable extra tiff support
(without such extra support some b/w tiff images will not be displayed):

    qmake6 "CONFIG+=no_extra_tiff_handler"

### Building without Epwing format support

If you have problem building with libeb-dev package, you can pass
`"CONFIG+=no_epwing_support"` to `qmake6` in order to disable Epwing format support

    qmake6 "CONFIG+=no_epwing_support"

### Building without the internal audio player

If you have problem building with FFmpeg/libao (for example, Ubuntu older than 12.04), you can pass
`"CONFIG+=no_ffmpeg_player"` to `qmake6` in order to disable the FFmpeg+libao internal audio player back end:

    qmake6 "CONFIG+=no_ffmpeg_player"

A `no_ffmpeg_player` build keeps the stored internal-player preference text
verbatim, hides the internal playback controls and keeps external playback
available.

<b>NB:</b> All additional settings for `qmake6` that you need must be combined in one `qmake6` launch, for example:

    qmake6 "CONFIG+=zim_support" "CONFIG+=no_extra_tiff_handler" "CONFIG+=no_ffmpeg_player"


Then, invoke `make clean` before `make` because the setting change:

    make clean && make

### Removed build switches

* `no_qtmultimedia_player` / `MAKE_QTMULTIMEDIA_PLAYER`: the Qt Multimedia player is
  gone. `CONFIG+=no_ffmpeg_player` is the only player switch left.
* The Qt 4 / Qt 5 dual build, its `QT_MAJOR_VERSION` branches and the
  `lrelease-qt4` fallback: one Qt 6 build remains.
* The QtWebKit article view (`QT += webkit`/`webkitwidgets`) and the
  `X11Extras`/`Multimedia` Qt modules: replaced by QtWebEngine and the ffmpeg and
  external playback back ends.

## How to test

The root project carries a test gate that builds and runs the aggregated suites in
`tests/` (`tests/tst_*/tst_*.pro`; a new suite joins the aggregate without editing
`tests/tests.pro`):

    qmake6 && make check

The gate has four targets, so a single wrapped run and the separate phases are both
available:

    make tests-makefile    # regenerate Makefile.tests from tests/tests.pro
    make tests-build       # build every suite
    make tests-run         # run every suite (make -f Makefile.tests check)
    make check             # tests-run

`GD_TEST_WRAPPER` is expanded by `make`, never by `qmake`, and wraps `tests-build`
and `tests-run` separately, which is how the gate script observes both invocations:

    GD_TEST_WRAPPER=$PWD/scripts/test-wrapper.sh make check

The suites can also be built and run directly:

    qmake6 tests/tests.pro -o tests/Makefile
    make -C tests -j$(nproc)
    QT_QPA_PLATFORM=xcb xvfb-run -a timeout 900 make -C tests check

Two gate scripts verify the gate itself and the Qt 6-only source/link contract:

    scripts/verify-test-gate.sh    # four probes: failure propagation, missing
                                   # environment, wrapper observation, negative control
    scripts/check-qt6-only.sh <Makefile> <application-link-log>
                                   # rejects Qt4/Qt5/WebKit/Multimedia tokens and
                                   # Qt5 libraries in the real link command

## Installation

Installation is an optional step since the built binary can be used as-is without installation. But you can properly install via:

    make install

<b>NB:</b> Don't do that on Windows!

You can uninstall via:

    make uninstall

## Limitations

The migration keeps the dictionary formats, the indexing algorithms, the on-disk
signatures and the FTS index format version. The following parity limits are
documented, not silent:

* Website iframe mode honours the site's `X-Frame-Options`/CSP restrictions; inline
  mode remains the existing escape hatch (its URL-base/port bug is corrected).
* Portal shortcuts accept one combination. Existing two-key defaults and
  multi-key sequences stay stored and are shown as unavailable until changed.
  Clipboard-change scanning and explicit lookup remain separate behaviours. Hover
  translation and X11 focus restoration remain X11-only.
* Portal dialogs are unparented on Wayland because the portal's `parent_window`
  handle is empty. Activation requests may be declined by the compositor.
* Print preview and direct print render a frozen PDF snapshot. New printer layout
  metrics change scale and render resolution, not the HTML reflow.
* QtMultimedia internal playback and WebKit browser plugins are unavailable; ffmpeg
  and external playback remain, with the configured audio-preference migration.
* Chromium treats `HttpCachingProxy` as an HTTP proxy; the retained QNAM-based
  downloads resolve independently under the same settings. Third-party response
  headers are honoured.
* Windows and macOS builds, CMake, and a parallel Qt 4/Qt 5 build are out of scope.
  Existing platform-only sources are retained with mechanically corrected Qt 6 API
  but are not a validated release for those platforms.

The detailed evidence for these limits, including which results are automated and
X11-verified and which native Wayland session checks are still not recorded, is in
[`docs/wayland-limitations.md`](docs/wayland-limitations.md). A window activation
request is a best-effort hint: a compositor may decline it, so a window that does not
come to the front is not a bug in GoldenDict.

## License

This project is licensed under the <b>GNU GPLv3+</b> license, a copy of which can be found in the `LICENSE.txt` file.

## Support

Users looking for support should file an issue in the official [GoldenDict issue tracker](https://github.com/goldendict/goldendict/issues),
or even better: submit a [pull request](https://github.com/goldendict/goldendict/pulls) if you have a fix available.
General questions should be asked on the [official GoldenDict forum](http://goldendict.org/forum/).
