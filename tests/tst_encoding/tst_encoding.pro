TARGET = tst_encoding
# Exercise the actual SLOB reader and private article formatter in the pure
# suite. Discard the unused dictionary/UI engine when linking on Linux.
CONFIG += zim_support
include(../testsupport.pri)

QT += core widgets xml
FORMS += $$GD_REPO_ROOT/fulltextsearch.ui
unix:!mac {
    QMAKE_CXXFLAGS += -ffunction-sections -fdata-sections
    QMAKE_LFLAGS += -Wl,--gc-sections
}

SOURCES += $$GD_REPO_ROOT/encoding.cc \
           $$GD_REPO_ROOT/gddebug.cc \
           tst_encoding.cc

HEADERS += $$GD_REPO_ROOT/encoding.hh \
           $$GD_REPO_ROOT/linkrewrite.hh \
           $$GD_REPO_ROOT/gddebug.hh

DISTFILES += $$GD_REPO_ROOT/tests/fixtures/encoding/vectors.json
