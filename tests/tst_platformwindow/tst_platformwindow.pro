# Platform-safe window geometry, flags and activation (specification §4.13,
# design §6.6.3).
#
# The suite links the production platformwindow.cc, never a copy of it.
# popupFlags() takes the QPA name as an argument, so the Wayland policy is
# pinned on the xcb build this gate runs — no faked
# QGuiApplication::platformName(), no Wayland session required.
#
# Two capability variants are built from this one project:
#
#   # the gate binary: HAVE_X11 (from testsupport.pri) and, when KF6WindowSystem
#   # is installed, HAVE_KWINDOWSYSTEM -- exactly the application's configuration
#   qmake6 tests/tst_platformwindow/tst_platformwindow.pro \
#       -o tests/tst_platformwindow/Makefile
#   make -C tests/tst_platformwindow -j$(nproc)
#   QT_QPA_PLATFORM=xcb xvfb-run -a timeout 60 tests/tst_platformwindow/tst_platformwindow
#   QT_QPA_PLATFORM=offscreen timeout 60 tests/tst_platformwindow/tst_platformwindow
#
#   # the portable variant: neither the X11 nor the KDE capability, so the same
#   # sources must compile and the same fixtures must hold without either seam
#   qmake6 tests/tst_platformwindow/tst_platformwindow.pro \
#       CONFIG+=gd_no_platform_capabilities -o tests/tst_platformwindow/Makefile.portable
#   make -C tests/tst_platformwindow -f Makefile.portable -j$(nproc)
#   QT_QPA_PLATFORM=offscreen timeout 60 tests/tst_platformwindow/tst_platformwindow_portable
#
# The variant gets its own binary name and its own build root, so objects
# compiled with HAVE_X11/HAVE_KWINDOWSYSTEM are never reused for it. Its Makefile
# lives in this directory (OUT_PWD == IN_PWD), which is where qmake resolves its
# own source-level metaobject rule against.

TARGET = tst_platformwindow
include(../testsupport.pri)

QT += core gui widgets

INCLUDEPATH += $$GD_REPO_ROOT

SOURCES += $$GD_REPO_ROOT/platformwindow.cc \
           $$GD_REPO_ROOT/gddebug.cc \
           $$GD_REPO_ROOT/hotkeywrapper.cc \
           $$GD_REPO_ROOT/qtsingleapplication/src/qtsingleapplication.cpp \
           $$GD_REPO_ROOT/qtsingleapplication/src/qtlocalpeer.cpp \
           tst_platformwindow.cc

HEADERS += $$GD_REPO_ROOT/platformwindow.hh \
           $$GD_REPO_ROOT/hotkeybackend.hh \
           $$GD_REPO_ROOT/hotkeywrapper.hh \
           $$GD_REPO_ROOT/qtsingleapplication/src/qtsingleapplication.h \
           $$GD_REPO_ROOT/qtsingleapplication/src/qtlocalpeer.h \
           $$GD_REPO_ROOT/config.hh

INCLUDEPATH += $$GD_REPO_ROOT/qtsingleapplication/src

unix:!mac {
    # x11Display() borrows Qt's display; the suite only needs Xlib itself to
    # prove the borrowed handle is live and to emulate a panel-like desktop
    # reservation on the xvfb gate.
    LIBS += -lX11 -lXtst
}

# hotkeywrapper.cc is compiled here because the runtime platform decision of the X11
# recorder is the same rule this suite already pins for the window helpers, and the
# portable variant is the build that proves the recorder's no-X11 paths compile and
# stay disabled. Its X11 session needs Xlib as well (linked above).

# The KDE windowing-system helper is an optional application dependency
# (design §7.4): when KF6WindowSystem is available the helper is linked and
# HAVE_KWINDOWSYSTEM is defined, which is the configuration the activation
# fixture in this suite describes.
!CONFIG(gd_no_platform_capabilities) {
    CONFIG += link_pkgconfig
    packagesExist(KF6WindowSystem) {
        PKGCONFIG += KF6WindowSystem
        DEFINES += HAVE_KWINDOWSYSTEM
    }
}

CONFIG(gd_no_platform_capabilities) {
    TARGET = tst_platformwindow_portable
    DEFINES -= HAVE_X11 HAVE_KWINDOWSYSTEM
    # Lets the suite assert that the variant really dropped the capability
    # instead of silently reusing the library configuration.
    DEFINES += GD_TEST_PLATFORMWINDOW_PORTABLE

    GD_PLATFORMWINDOW_VARIANT_BUILD_ROOT = $$absolute_path($$OUT_PWD/build/tests/$$TARGET)
    OBJECTS_DIR = $$GD_PLATFORMWINDOW_VARIANT_BUILD_ROOT/obj
    MOC_DIR     = $$GD_PLATFORMWINDOW_VARIANT_BUILD_ROOT/moc
    UI_DIR      = $$GD_PLATFORMWINDOW_VARIANT_BUILD_ROOT/ui
    RCC_DIR     = $$GD_PLATFORMWINDOW_VARIANT_BUILD_ROOT/rcc
}
