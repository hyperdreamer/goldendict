# Trust-boundary suite for the btree index and chunked storage.
#
# A plain QCoreApplication test case: no display, no dictionary data, no
# Chromium. Every fixture is built by the production builder (buildIndex /
# ChunkedStorage::Writer) into a temporary directory and then either read back
# or mutated byte-wise to model a corrupt, truncated or foreign index.
#
# The suite exists because a malformed node record used to abort the whole
# application through an empty-vector front(); each slot asserts a contained
# failure instead, so the binary staying alive is part of the expected result.

TARGET = tst_btree_index
include(../testsupport.pri)

QT += core gui

# Production sources under test, consumed read-only from the repository root.
SOURCES += $$GD_REPO_ROOT/btreeidx.cc \
           $$GD_REPO_ROOT/chunkedstorage.cc \
           $$GD_REPO_ROOT/file.cc \
           $$GD_REPO_ROOT/dictionary.cc \
           $$GD_REPO_ROOT/mutex.cc \
           $$GD_REPO_ROOT/utf8.cc \
           $$GD_REPO_ROOT/wstring.cc \
           $$GD_REPO_ROOT/wstring_qt.cc \
           $$GD_REPO_ROOT/fsencoding.cc \
           $$GD_REPO_ROOT/wildcard.cc \
           $$GD_REPO_ROOT/zipfile.cc \
           $$GD_REPO_ROOT/splitfile.cc \
           $$GD_REPO_ROOT/folding.cc \
           $$GD_REPO_ROOT/gddebug.cc

# dictionary.hh carries the Q_OBJECT Request classes the readers derive from.
HEADERS += $$GD_REPO_ROOT/dictionary.hh

# zlib frames every btree node and every chunk record.
LIBS += -lz

SOURCES += tst_btree_index.cc
