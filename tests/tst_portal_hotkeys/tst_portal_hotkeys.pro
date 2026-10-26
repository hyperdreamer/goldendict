TARGET = tst_portal_hotkeys
include(../testsupport.pri)
QT += core dbus
QT -= gui

SOURCES += $$GD_REPO_ROOT/portalhotkeys.cc \
           tst_portal_hotkeys.cc
HEADERS += $$GD_REPO_ROOT/portalhotkeys.hh
