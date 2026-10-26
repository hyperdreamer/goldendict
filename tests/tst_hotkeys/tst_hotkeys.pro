# Hotkey syntax, capability contract and portable audio config migration.
#
# The suite links the production modules, never copies of them: config.cc (the
# real load()/save() XML code and HotKey conversion), hotkeysyntax.cc (the real
# PortableText <-> XDG preferred_trigger conversion), ffmpegaudio.cc and
# audioplayerfactory.cc (the real factory selection). HotkeyBackend is a pure
# interface, so the suite derives a recording backend from the real header and
# proves the published signal/state contract through QSignalSpy.
#
# The two fixtures under tests/fixtures/config are loaded through the real
# Config::load()/Config::save() against an isolated HOME, so the assertions are
# about the production XML path and the production save point, not about a
# test-only parser.
#
# `CONFIG+=no_ffmpeg_player` builds the same suite against the no_ffmpeg_player
# variant: the audio cases branch on the feature the binary was compiled with and
# assert the other half of §4.13 (verbatim stored backend text, no internal
# player, external playback retained).
#
# The recorder cases (§2.10) link the production hotkeywrapper.cc as well: the real
# matcher, the real RecorderCore and the real X11RecordBackend. The recording rules
# are driven through a fixture RecorderSession, so they need no RECORD server, and
# the X11 paths are only taken when the chosen QPA is an actual xcb session:
#
#   QT_QPA_PLATFORM=xcb xvfb-run -a timeout 60 tests/tst_hotkeys/tst_hotkeys
#   QT_QPA_PLATFORM=offscreen timeout 60 tests/tst_hotkeys/tst_hotkeys nonXcbCapability
#
# The offscreen run is required: it is the case that proves a non-X11 session
# reports itself unsupported and never reaches an Xlib call.

TARGET = tst_hotkeys
include(../testsupport.pri)

QT += core gui widgets xml

INCLUDEPATH += $$GD_REPO_ROOT \
               $$GD_REPO_ROOT/qtsingleapplication/src

SOURCES += $$GD_REPO_ROOT/atomic_rename.cc \
           $$GD_REPO_ROOT/audioplayerfactory.cc \
           $$GD_REPO_ROOT/config.cc \
           $$GD_REPO_ROOT/externalaudioplayer.cc \
           $$GD_REPO_ROOT/externalviewer.cc \
           $$GD_REPO_ROOT/folding.cc \
           $$GD_REPO_ROOT/gddebug.cc \
           $$GD_REPO_ROOT/hotkeywrapper.cc \
           $$GD_REPO_ROOT/hotkeysyntax.cc \
           $$GD_REPO_ROOT/parsecmdline.cc \
           $$GD_REPO_ROOT/platformwindow.cc \
           $$GD_REPO_ROOT/qtsingleapplication/src/qtsingleapplication.cpp \
           $$GD_REPO_ROOT/qtsingleapplication/src/qtlocalpeer.cpp \
           $$GD_REPO_ROOT/broken_xrecord.cc \
           $$GD_REPO_ROOT/keyboardstate.cc \
           $$GD_REPO_ROOT/wildcard.cc \
           $$GD_REPO_ROOT/wstring_qt.cc \
           tst_hotkeys.cc

HEADERS += $$GD_REPO_ROOT/audioplayerfactory.hh \
           $$GD_REPO_ROOT/broken_xrecord.hh \
           $$GD_REPO_ROOT/hotkeywrapper.hh \
           $$GD_REPO_ROOT/keyboardstate.hh \
           $$GD_REPO_ROOT/platformwindow.hh \
           $$GD_REPO_ROOT/qtsingleapplication/src/qtsingleapplication.h \
           $$GD_REPO_ROOT/qtsingleapplication/src/qtlocalpeer.h \
           $$GD_REPO_ROOT/audioplayerinterface.hh \
           $$GD_REPO_ROOT/externalaudioplayer.hh \
           $$GD_REPO_ROOT/externalviewer.hh \
           $$GD_REPO_ROOT/ffmpegaudio.hh \
           $$GD_REPO_ROOT/ffmpegaudioplayer.hh \
           $$GD_REPO_ROOT/hotkeybackend.hh \
           $$GD_REPO_ROOT/hotkeysyntax.hh \
           $$GD_REPO_ROOT/config.hh

unix:!mac {
    # The recorder borrows Qt's display and grabs combinations on its root window;
    # the live proof drives the server's own input generator (XTEST).
    LIBS += -lX11 -lXtst
}

# The internal player's real decoder/service, only in a build that has it.
!CONFIG( no_ffmpeg_player ) {
    SOURCES += $$GD_REPO_ROOT/ffmpegaudio.cc
    CONFIG += link_pkgconfig
    PKGCONFIG += ao \
                 libavutil \
                 libavformat \
                 libavcodec \
                 libswresample
}

# A no_ffmpeg_player build must not reuse objects compiled with the player, so the
# variant gets its own binary name and build root. Build it with the Makefile in
# this directory (OUT_PWD == IN_PWD), which is where qmake resolves its own
# source-level metaobject rule against:
#
#   qmake6 tests/tst_hotkeys/tst_hotkeys.pro CONFIG+=no_ffmpeg_player \
#       -o tests/tst_hotkeys/Makefile.noffmpeg
#   make -C tests/tst_hotkeys -f Makefile.noffmpeg -j$(nproc)
#   xvfb-run -a timeout 60 tests/tst_hotkeys/tst_hotkeys_noffmpeg
#
# (When the Makefile is generated with `-o` outside the project directory, qmake
# points the object dependency of an `#include "<target>.moc"` at
# <project dir>/<MOC_DIR> while its rule writes the OUT_PWD copy, so such a build
# is not self-consistent for any suite in this tree.)
CONFIG(no_ffmpeg_player) {
    TARGET = tst_hotkeys_noffmpeg

    GD_HOTKEYS_VARIANT_BUILD_ROOT = $$absolute_path($$OUT_PWD/build/tests/$$TARGET)
    OBJECTS_DIR = $$GD_HOTKEYS_VARIANT_BUILD_ROOT/obj
    MOC_DIR     = $$GD_HOTKEYS_VARIANT_BUILD_ROOT/moc
    UI_DIR      = $$GD_HOTKEYS_VARIANT_BUILD_ROOT/ui
    RCC_DIR     = $$GD_HOTKEYS_VARIANT_BUILD_ROOT/rcc
}

DISTFILES += $$GD_REPO_ROOT/tests/fixtures/config/legacy.xml \
             $$GD_REPO_ROOT/tests/fixtures/config/additive.xml
