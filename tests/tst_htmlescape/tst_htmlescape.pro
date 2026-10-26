TARGET = tst_htmlescape
include(../testsupport.pri)

QT += core gui

SOURCES += $$GD_REPO_ROOT/htmlescape.cc \
           tst_htmlescape.cc

HEADERS += $$GD_REPO_ROOT/htmlescape.hh
