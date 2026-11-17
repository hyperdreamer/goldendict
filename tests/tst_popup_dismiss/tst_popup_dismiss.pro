# Scan-popup focus-destination dismissal.
# Link the production module; acceptance explicitly selects xcb under xvfb.
TARGET = tst_popup_dismiss
include(../testsupport.pri)

QT += core gui widgets

INCLUDEPATH += $$GD_REPO_ROOT

SOURCES += $$GD_REPO_ROOT/popupdismiss.cc \
           tst_popup_dismiss.cc

HEADERS += $$GD_REPO_ROOT/popupdismiss.hh
