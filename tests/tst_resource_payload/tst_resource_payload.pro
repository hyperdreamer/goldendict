TARGET = tst_resource_payload
include(../testsupport.pri)
QT += core gui
QMAKE_CXXFLAGS += -D_GLIBCXX_ASSERTIONS
SOURCES += $$GD_REPO_ROOT/dictionary.cc \
           $$GD_REPO_ROOT/file.cc \
           $$GD_REPO_ROOT/fsencoding.cc \
           $$GD_REPO_ROOT/mutex.cc \
           $$GD_REPO_ROOT/utf8.cc \
           $$GD_REPO_ROOT/wstring.cc \
           $$GD_REPO_ROOT/wstring_qt.cc \
           $$GD_REPO_ROOT/wildcard.cc \
           $$GD_REPO_ROOT/folding.cc \
           $$GD_REPO_ROOT/gddebug.cc \
           $$GD_REPO_ROOT/zipfile.cc \
           $$GD_REPO_ROOT/splitfile.cc \
           $$GD_REPO_ROOT/imagepayload.cc \
           $$GD_REPO_ROOT/bglresource.cc \
           $$GD_REPO_ROOT/tiff.cc \
           tst_resource_payload.cc
HEADERS += $$GD_REPO_ROOT/dictionary.hh \
           $$GD_REPO_ROOT/payload.hh \
           $$GD_REPO_ROOT/imagepayload.hh \
           $$GD_REPO_ROOT/bglresource.hh
LIBS += -lz
!CONFIG(no_extra_tiff_handler): LIBS += -ltiff
