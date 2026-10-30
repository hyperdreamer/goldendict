# GoldenDict test aggregate.
#
# Ordinary mode -- the acceptance gate:
#
#   qmake6 tests/tests.pro -o tests/Makefile
#   make -C tests -j$(nproc)
#   QT_QPA_PLATFORM=xcb xvfb-run -a timeout 900 make -C tests check
#
# Ordinary mode is a closed acceptance inventory: exactly the 19 required suites.
# Discovery is validated against that inventory; missing or extra projects fail
# configuration instead of silently reducing the gate. Closure mode is separate.
#
# Test-only compile closure -- used while a production consumer has not switched
# to the new modules yet, so its closure is still syntax/MOC/UI-checked:
#
#   mkdir -p /tmp/gd-closure
#   qmake6 tests/tests.pro -o /tmp/gd-closure/Makefile \
#       GD_COMPILE_CLOSURE=1 \
#       GD_COMPILE_SOURCES="utf8.cc wstring.cc wstring_qt.cc" \
#       GD_COMPILE_HEADERS="utf8.hh wstring.hh" \
#       GD_COMPILE_FORMS=""
#   make -C /tmp/gd-closure -j$(nproc)
#
# The input lists are repository-relative; each entry is translated against the
# repository root. The result is a static archive (`libgdtestclosure.a`) built
# from the shared testsupport project shape and the tests/support sources. It is
# not an application and not an additional testcase: it has no `check` target.
# Any of the four variables may also be supplied through the environment.

# Accept both the qmake assignment (`qmake ... GD_COMPILE_CLOSURE=1`) and the
# environment (`GD_COMPILE_CLOSURE=1 qmake ...`).
isEmpty(GD_COMPILE_CLOSURE): GD_COMPILE_CLOSURE = $$(GD_COMPILE_CLOSURE)
isEmpty(GD_COMPILE_SOURCES): GD_COMPILE_SOURCES = $$(GD_COMPILE_SOURCES)
isEmpty(GD_COMPILE_HEADERS): GD_COMPILE_HEADERS = $$(GD_COMPILE_HEADERS)
isEmpty(GD_COMPILE_FORMS): GD_COMPILE_FORMS = $$(GD_COMPILE_FORMS)

GD_CLOSURE_MODE = false
!isEmpty(GD_COMPILE_CLOSURE) {
    !equals(GD_COMPILE_CLOSURE, 0) {
        !equals(GD_COMPILE_CLOSURE, false) {
            !equals(GD_COMPILE_CLOSURE, no) {
                GD_CLOSURE_MODE = true
            }
        }
    }
}

equals(GD_CLOSURE_MODE, true) {

    include(testsupport.pri)

    # The archive is a compile check, not a testcase: drop the testcase
    # machinery the shared project shape installs for real suites.
    CONFIG -= testcase console
    TEMPLATE = lib
    CONFIG += staticlib
    TARGET = gdtestclosure

    GD_CLOSURE_BUILD_ROOT = $$OUT_PWD/build/closure
    OBJECTS_DIR = $$GD_CLOSURE_BUILD_ROOT/obj
    MOC_DIR     = $$GD_CLOSURE_BUILD_ROOT/moc
    UI_DIR      = $$GD_CLOSURE_BUILD_ROOT/ui
    RCC_DIR     = $$GD_CLOSURE_BUILD_ROOT/rcc

    # An in-source closure build has no directory to put its outputs in, and qmake
    # does not create these for a staticlib: create them here so the documented
    # command works on a clean tree instead of failing on a missing directory.
    !exists($$OBJECTS_DIR): system(mkdir -p $$OBJECTS_DIR)
    !exists($$MOC_DIR):     system(mkdir -p $$MOC_DIR)
    !exists($$UI_DIR):      system(mkdir -p $$UI_DIR)
    !exists($$RCC_DIR):     system(mkdir -p $$RCC_DIR)

    # The archive is compiled against the same Qt feature set as the final
    # application so a production header cannot silently compile differently in
    # the test closure.
    QT += core gui widgets xml network sql printsupport help svg \
          webenginecore webenginewidgets webchannel dbus pdf pdfwidgets

    # tests/support is always part of the closure (see testsupport.pri); MOC the
    # support headers as well so a Q_OBJECT there is checked too.
    HEADERS += $$files($$GD_TESTS_ROOT/support/*.hh)

    isEmpty(GD_COMPILE_SOURCES) {
        error("GD_COMPILE_CLOSURE=1 requires a non-empty GD_COMPILE_SOURCES list")
    }

    for(GD_ENTRY, GD_COMPILE_SOURCES) {
        GD_PATH = $$absolute_path($$GD_ENTRY, $$GD_REPO_ROOT)
        !exists($$GD_PATH): error("GD_COMPILE_SOURCES entry does not exist: $$GD_ENTRY (resolved to $$GD_PATH)")
        SOURCES += $$GD_PATH
    }

    for(GD_ENTRY, GD_COMPILE_HEADERS) {
        GD_PATH = $$absolute_path($$GD_ENTRY, $$GD_REPO_ROOT)
        !exists($$GD_PATH): error("GD_COMPILE_HEADERS entry does not exist: $$GD_ENTRY (resolved to $$GD_PATH)")
        HEADERS += $$GD_PATH
    }

    for(GD_ENTRY, GD_COMPILE_FORMS) {
        GD_PATH = $$absolute_path($$GD_ENTRY, $$GD_REPO_ROOT)
        !exists($$GD_PATH): error("GD_COMPILE_FORMS entry does not exist: $$GD_ENTRY (resolved to $$GD_PATH)")
        FORMS += $$GD_PATH
    }

} else {

    TEMPLATE = subdirs

    # tests/tst_*/tst_*.pro -- one suite per directory. `files()` only expands a
    # wildcard in its last path component, hence the two-level discovery; the
    # lists are assigned before the loops because qmake does not expand a
    # function call reliably inside a `for()` header.
    GD_TEST_DIRECTORIES = $$files($$PWD/tst_*)
    GD_TEST_PROJECTS =
    for(GD_TEST_DIRECTORY, GD_TEST_DIRECTORIES) {
        contains(GD_TEST_DIRECTORY, .*[.]pro$): next()
        GD_TEST_PROJECT_FILES = $$files($$GD_TEST_DIRECTORY/tst_*.pro)
        for(GD_TEST_PROJECT, GD_TEST_PROJECT_FILES) {
            GD_TEST_PROJECTS += $$GD_TEST_PROJECT
        }
    }

    GD_REQUIRED_SUITE_NAMES = \
        tst_article_bridge tst_article_history tst_article_load \
        tst_article_schemes tst_article_stream tst_btree_index tst_encoding \
        tst_external_content tst_hotkeys tst_htmlescape tst_langcoder \
        tst_linkrewrite tst_platformwindow \
        tst_portal_hotkeys tst_print_pipeline tst_regex tst_save_article \
        tst_utf8_wstring tst_website_frames
    GD_TEST_NAMES =
    for(GD_TEST_PROJECT, GD_TEST_PROJECTS) {
        GD_TEST_NAME = $$basename(GD_TEST_PROJECT)
        GD_TEST_NAME ~= s/\\.pro$//
        GD_TEST_NAMES += $$GD_TEST_NAME
    }
    GD_TEST_NAMES = $$sorted(GD_TEST_NAMES)
    !count(GD_TEST_NAMES, 19): error("Expected exactly 19 required suites; found: $$GD_TEST_NAMES")
    !equals(GD_TEST_NAMES, $$GD_REQUIRED_SUITE_NAMES): error("Required suite inventory mismatch: $$GD_TEST_NAMES")
    for(GD_TEST_NAME, GD_REQUIRED_SUITE_NAMES) {
        GD_REQUIRED_PROJECT = $$PWD/$$GD_TEST_NAME/$${GD_TEST_NAME}.pro
        !exists($$GD_REQUIRED_PROJECT): error("Missing required suite project: $$GD_REQUIRED_PROJECT")
        !contains(GD_TEST_PROJECTS, $$re_escape($$GD_REQUIRED_PROJECT)): error("Required suite not registered: $$GD_REQUIRED_PROJECT")
    }
    message("tests/tests.pro: exact 19 suites: $$GD_TEST_NAMES")
    SUBDIRS += $$GD_TEST_PROJECTS
}
