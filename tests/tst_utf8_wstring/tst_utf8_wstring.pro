# Pure (GUI-less) baseline suite for the UTF-8 / wide-string conversions that
# the dictionary backends and the Qt-facing helpers share.
#
# The binary is a plain QCoreApplication test case: it needs no display and no
# dictionary data, which makes it the cheapest proof that the aggregate harness,
# the environment guard and the compile closure work.
#
# The suite also carries the focused configuration probe of the shared project.
# tiff.cc is the clearest guarded production implementation available: its whole
# body sits inside #ifdef MAKE_EXTRA_TIFF_HANDLER, so linking it here fails with
# an undefined GdTiff::tiffToQImage when the mirrored feature configuration loses
# that define, while a build configured with no_extra_tiff_handler still compiles
# it to the expected empty translation unit.
#
# Finally it is the home of the shared fixture self-tests (test/support): the
# deterministic dictionary readers, the loopback HTTP origins and the private
# portal bus. Those self-tests link the *real* production reader from
# articleschemes.cc, which is why this target -- and only this pure target -- adds
# the small article reader closure below. Nothing here starts Chromium: only
# makeDataRequestReader() is reachable, so the binary stays a QCoreApplication.

TARGET = tst_utf8_wstring
include(../testsupport.pri)

# QtGui is needed by the TIFF handler (QImage) and by the dictionary fixtures
# (QIcon), not by this QCoreApplication binary itself; no GUI platform plugin is
# required at run time. QtWebEngineCore/Widgets are link-time only: the suite
# never constructs a profile, a page or a view.
QT += core gui webenginecore webenginewidgets

# Production sources under test, consumed read-only from the repository root.
SOURCES += $$GD_REPO_ROOT/utf8.cc \
           $$GD_REPO_ROOT/wstring.cc \
           $$GD_REPO_ROOT/wstring_qt.cc \
           $$GD_REPO_ROOT/tiff.cc \
           $$GD_REPO_ROOT/articleschemes.cc \
           $$GD_REPO_ROOT/dictionary.cc \
           $$GD_REPO_ROOT/mutex.cc

# dictionary.hh carries the Q_OBJECT Request classes the fixture request derives
# from, so it has to be MOC'd. articleschemes.hh deliberately is not: only its
# free reader factory is used here, never the QObject handler.
HEADERS += $$GD_REPO_ROOT/dictionary.hh

# The dictionary fixture reaches the production reader, so it joins this target
# explicitly (tests/testsupport.pri keeps it out of the shared glob).
SOURCES += $$GD_TESTS_ROOT/support/dictionary_stub.cc

# The extra TIFF handler links the system libtiff, exactly as goldendict.pro does
# when MAKE_EXTRA_TIFF_HANDLER is enabled.
!CONFIG( no_extra_tiff_handler ): LIBS += -ltiff

# Only the reader factory of articleschemes.cc is referenced. Compiled with
# per-function sections, the unread handler/device sections are dropped and the
# linked binary stays a small QCoreApplication executable instead of dragging in
# the whole article closure.
unix {
    QMAKE_CXXFLAGS += -ffunction-sections -fdata-sections
    QMAKE_LFLAGS += -Wl,--gc-sections
}

SOURCES += tst_utf8_wstring.cc
