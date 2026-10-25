TARGET = tst_encoding
include(../testsupport.pri)

QT += core

SOURCES += $$GD_REPO_ROOT/encoding.cc \
           $$GD_REPO_ROOT/gddebug.cc \
           tst_encoding.cc

HEADERS += $$GD_REPO_ROOT/encoding.hh \
           $$GD_REPO_ROOT/linkrewrite.hh \
           $$GD_REPO_ROOT/gddebug.hh

DISTFILES += $$GD_REPO_ROOT/tests/fixtures/encoding/vectors.json
