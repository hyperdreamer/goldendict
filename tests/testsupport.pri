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

INCLUDEPATH += $$GD_REPO_ROOT $$GD_TESTS_ROOT $$GD_TESTS_ROOT/support

# Shared test support (fixtures, environment gate, watchdog). A glob so a new
# support translation unit joins every suite without editing the suite file.
GD_TEST_SUPPORT_SOURCES = $$files($$GD_TESTS_ROOT/support/*.cc)
SOURCES += $$GD_TEST_SUPPORT_SOURCES

# Feature definitions mirroring goldendict.pro on unix. HAVE_X11 is a build
# capability only: whether the running suite actually uses X11 is decided by the
# chosen QPA platform, never by this define.
unix:!mac {
    DEFINES += HAVE_X11
}
!CONFIG( no_ffmpeg_player ) {
    DEFINES += MAKE_FFMPEG_PLAYER
}

GD_PROGRAM_VERSION = 1.5.1+git
GD_PROGRAM_DATA_DIR = /usr/local/share/goldendict/
DEFINES += PROGRAM_VERSION=\\\"$$GD_PROGRAM_VERSION\\\"
DEFINES += PROGRAM_DATA_DIR=\\\"$$GD_PROGRAM_DATA_DIR\\\"

# Last-resort repository root for binaries whose working directory is outside
# the checkout, see TestSupport::repositoryRoot().
DEFINES += GD_TEST_SOURCE_ROOT=\\\"$$GD_REPO_ROOT\\\"
