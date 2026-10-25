# Pure (GUI-less) baseline suite for the UTF-8 / wide-string conversions that
# the dictionary backends and the Qt-facing helpers share.
#
# The binary is a plain QCoreApplication test case: it needs no display, no
# dictionaries and no network, which makes it the cheapest proof that the
# aggregate harness, the environment guard and the compile closure work.

TARGET = tst_utf8_wstring
include(../testsupport.pri)

QT += core

# Production sources under test, consumed read-only from the repository root.
SOURCES += $$GD_REPO_ROOT/utf8.cc \
           $$GD_REPO_ROOT/wstring.cc \
           $$GD_REPO_ROOT/wstring_qt.cc

SOURCES += tst_utf8_wstring.cc
