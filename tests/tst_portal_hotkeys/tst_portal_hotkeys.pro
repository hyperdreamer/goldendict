TARGET = tst_portal_hotkeys
include(../testsupport.pri)
QT += core gui dbus

SOURCES += $$GD_REPO_ROOT/portalhotkeys.cc \
           tst_portal_hotkeys.cc
HEADERS += $$GD_REPO_ROOT/portalhotkeys.hh \
           $$GD_REPO_ROOT/hotkeybackend.hh
