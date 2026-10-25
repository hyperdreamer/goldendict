# Pure (GUI-less) baseline suite for the UTF-8 / wide-string conversions that
# the dictionary backends and the Qt-facing helpers share.
#
# The binary is a plain QCoreApplication test case: it needs no display, no
# dictionaries and no network, which makes it the cheapest proof that the
# aggregate harness, the environment guard and the compile closure work.
#
# The suite also carries the focused configuration probe of the shared project.
# tiff.cc is the clearest guarded production implementation available: its whole
# body sits inside #ifdef MAKE_EXTRA_TIFF_HANDLER, so linking it here fails with
# an undefined GdTiff::tiffToQImage when the mirrored feature configuration loses
# that define, while a build configured with no_extra_tiff_handler still compiles
# it to the expected empty translation unit.

TARGET = tst_utf8_wstring
include(../testsupport.pri)

# QtGui is needed by the TIFF handler (QImage), not by this QCoreApplication
# binary itself; no GUI platform plugin is required at run time.
QT += core gui

# Production sources under test, consumed read-only from the repository root.
SOURCES += $$GD_REPO_ROOT/utf8.cc \
           $$GD_REPO_ROOT/wstring.cc \
           $$GD_REPO_ROOT/wstring_qt.cc \
           $$GD_REPO_ROOT/tiff.cc

# The extra TIFF handler links the system libtiff, exactly as goldendict.pro does
# when MAKE_EXTRA_TIFF_HANDLER is enabled.
!CONFIG( no_extra_tiff_handler ): LIBS += -ltiff

SOURCES += tst_utf8_wstring.cc
