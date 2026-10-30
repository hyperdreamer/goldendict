# Pure suite for the centralized link rewriting rules.
#
# The four production entry points (linkrewrite.cc) and the existing audio
# wrapper (audiolink.cc) are linked as production sources, never copied: the
# suite asserts the exact markup the dictionary engines must emit, plus the
# quote/backslash contract of the unchanged audio script writer.
#
# No display, dictionary or network resource is needed; fixtures/linkrewrite/
# cases.json holds every fixed expected value.

CONFIG += zim_support
TARGET = tst_linkrewrite
include(../testsupport.pri)

QT += core widgets xml svg
LIBS += -lz
FORMS += $$GD_REPO_ROOT/fulltextsearch.ui

unix {
    QMAKE_CXXFLAGS += -ffunction-sections -fdata-sections
    QMAKE_LFLAGS += -Wl,--gc-sections
}

SOURCES += $$GD_REPO_ROOT/linkrewrite.cc \
           $$GD_REPO_ROOT/encoding.cc \
           $$GD_REPO_ROOT/gddebug.cc \
           $$GD_REPO_ROOT/audiolink.cc \
           $$GD_REPO_ROOT/mdictparser.cc \
           $$GD_REPO_ROOT/stardict.cc \
           $$GD_REPO_ROOT/dictdfiles.cc \
           $$GD_REPO_ROOT/dictserver.cc \
           $$GD_REPO_ROOT/zim.cc \
           $$GD_REPO_ROOT/htmlescape.cc \
           tst_linkrewrite.cc

HEADERS += $$GD_REPO_ROOT/linkrewrite.hh \
           $$GD_REPO_ROOT/audiolink.hh

DISTFILES += $$GD_REPO_ROOT/tests/fixtures/linkrewrite/cases.json
