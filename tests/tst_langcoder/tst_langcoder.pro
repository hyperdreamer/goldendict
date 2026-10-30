TARGET = tst_langcoder
include(../testsupport.pri)

QT += core gui

SOURCES += $$GD_REPO_ROOT/langcoder.cc \
           $$GD_REPO_ROOT/language.cc \
           $$GD_REPO_ROOT/folding.cc \
           $$GD_REPO_ROOT/wstring_qt.cc \
           tst_langcoder.cc

HEADERS += $$GD_REPO_ROOT/langcoder.hh
