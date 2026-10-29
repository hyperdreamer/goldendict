# Shared build support for the GoldenDict test suites.
#
# Included by every tests/tst_*/tst_*.pro and by tests/tests.pro when a
# test-only compile closure is requested. It supplies
#   * the repository and tests include roots,
#   * the feature definitions the application project defines, so production
#     sources see the same configuration when a suite compiles them,
#   * per-target object/MOC/UI/RCC directories outside the source tree,
#   * the app/testcase/c++17/testlib project shape every test binary uses.
#
# The wall watchdog that terminates a wedged binary even when its event loop
# stalls lives in tests/support/fixtures.cc (TestSupport::installWatchdog): a
# helper thread owned by the test process rather than a qmake recipe.

GD_TESTS_ROOT = $$PWD
GD_REPO_ROOT = $$clean_path($$PWD/..)

# Every target gets its own out-of-source build directory so a shadow build and
# an in-source build never mix objects of two suites.
isEmpty(TARGET): TARGET = $$basename(_PRO_FILE_PWD_)
GD_TEST_BUILD_ROOT = $$OUT_PWD/build/tests
OBJECTS_DIR = $$GD_TEST_BUILD_ROOT/$$TARGET/obj
MOC_DIR     = $$GD_TEST_BUILD_ROOT/$$TARGET/moc
UI_DIR      = $$GD_TEST_BUILD_ROOT/$$TARGET/ui
RCC_DIR     = $$GD_TEST_BUILD_ROOT/$$TARGET/rcc

TEMPLATE = app
CONFIG += c++17 exceptions rtti stl console testcase
QT += testlib

# The display-prerequisite checks open a bounded connection to the selected
# endpoint (X11 unix socket/TCP port or the Wayland compositor socket), so the
# shared support needs QtNetwork in every target. QtDBus is needed by the private
# portal-bus fixture; linking it does not connect to any bus by itself.
QT += network dbus widgets webenginecore webenginewidgets
# RequiredEnvironment drives a real renderer/JS for QApplication suites. Pure
# QCoreApplication suites remain renderer-free at runtime (60 s watchdog).

INCLUDEPATH += $$GD_REPO_ROOT $$GD_TESTS_ROOT $$GD_TESTS_ROOT/support

# Shared test support (fixtures, environment gate, watchdog, HTTP origins, the
# private portal bus). A glob so a new support translation unit joins every suite
# without editing the suite file.
#
# One support file is deliberately excluded: support/dictionary_stub.cc reaches
# the *production* DataRequestReader in articleschemes.cc, so a suite that links
# it also has to link that closure (and QtWebEngine). Keeping it out of the glob
# is what lets a pure suite stay free of Chromium. Suites that need it link it
# explicitly -- tests/article-test.pri does so for the Browser/Article targets,
# tst_utf8_wstring does it for the fixture self-tests.
GD_TEST_SUPPORT_SOURCES = $$files($$GD_TESTS_ROOT/support/*.cc)
GD_TEST_SUPPORT_SOURCES -= $$GD_TESTS_ROOT/support/dictionary_stub.cc
SOURCES += $$GD_TEST_SUPPORT_SOURCES

# Feature definitions mirroring goldendict.pro. Each entry copies the *CONFIG
# condition* of the application project, not just the define, so a test build can
# be configured exactly like the application it verifies and a guarded production
# implementation is never compiled out silently.
#
# HAVE_X11 is a build capability only: whether the running suite actually uses X11
# is decided by the chosen QPA platform, never by this define.

# goldendict.pro enables ZIM and Chinese conversion by default on win32 only; on
# every other platform the switch has to be asked for explicitly. This has to run
# before the configuration record below so the record sees the final CONFIG.
win32 {
    CONFIG += zim_support

    !CONFIG( no_chinese_conversion_support ): CONFIG += chinese_conversion_support
}

# The configuration record: which feature switches this build selected. It is
# deliberately separate from the define block below so the configuration probe
# can prove that a selected switch actually produced its feature macro -- a
# missing MAKE_EXTRA_TIFF_HANDLER must be an error, not "a disabled feature".
unix:!mac: DEFINES += GD_TEST_CONFIG_HAVE_X11
!CONFIG( no_ffmpeg_player ): DEFINES += GD_TEST_CONFIG_FFMPEG_PLAYER
!CONFIG( no_extra_tiff_handler ): DEFINES += GD_TEST_CONFIG_EXTRA_TIFF_HANDLER
CONFIG( zim_support ): DEFINES += GD_TEST_CONFIG_ZIM_SUPPORT
CONFIG( no_epwing_support ): DEFINES += GD_TEST_CONFIG_NO_EPWING_SUPPORT
CONFIG( chinese_conversion_support ): DEFINES += GD_TEST_CONFIG_CHINESE_CONVERSION_SUPPORT
CONFIG( old_hunspell ): DEFINES += GD_TEST_CONFIG_OLD_HUNSPELL

unix:!mac {
    DEFINES += HAVE_X11
}

!CONFIG( no_ffmpeg_player ) {
    DEFINES += MAKE_FFMPEG_PLAYER
}

CONFIG( zim_support ) {
    DEFINES += MAKE_ZIM_SUPPORT
}

# The extra TIFF handler is a default feature: tiff.cc/hh wrap their whole
# implementation in #ifdef MAKE_EXTRA_TIFF_HANDLER, so a build that forgets the
# define still succeeds while compiling an empty translation unit.
!CONFIG( no_extra_tiff_handler ) {
    DEFINES += MAKE_EXTRA_TIFF_HANDLER
}

CONFIG( no_epwing_support ) {
    DEFINES += NO_EPWING_SUPPORT
}

CONFIG( chinese_conversion_support ) {
    DEFINES += MAKE_CHINESE_CONVERSION_SUPPORT
}

CONFIG( old_hunspell ) {
    DEFINES += OLD_HUNSPELL_INTERFACE
}

GD_PROGRAM_VERSION = 1.5.1+git
GD_PROGRAM_DATA_DIR = /usr/local/share/goldendict/
DEFINES += PROGRAM_VERSION=\\\"$$GD_PROGRAM_VERSION\\\"
DEFINES += PROGRAM_DATA_DIR=\\\"$$GD_PROGRAM_DATA_DIR\\\"

# Last-resort repository root for binaries whose working directory is outside
# the checkout, see TestSupport::repositoryRoot().
DEFINES += GD_TEST_SOURCE_ROOT=\\\"$$GD_REPO_ROOT\\\"
