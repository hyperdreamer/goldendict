# Pure suite for the source-tagged regex migration ledger and the typed FTS
# search/highlight values.
#
# The ledger (fixtures/regex/cases.json) records, for every migrated pattern
# family, the source location, the old engine/options, the migrated pattern and
# options, and the fixed expected matches/captures/replacement. This suite is the
# executable half of the ledger: it compiles every row, compares the pinned
# expectations, and proves the traps the migration must not walk into (two-digit
# hex, two-digit octal, anchored trailing newline, zero-width loops, greedy vs
# minimal counts, QRegExp's own mark/space/dot class tables).
#
# The engine helpers that own these patterns are converted by their owner tasks
# (Task 9 for the dictionary parser families, Task 10 for the FTS normalizers); a
# row whose consumer has not moved yet names its 'productionOwner' and is a fixed
# expectation for that task, never a test-only reimplementation of it. A row
# already exercised by a production-linked suite names that 'productionRoute'
# instead, and a row without any live consumer is anchored to a fixture pin. The
# wildcard rows that drive QRegularExpression::wildcardToRegularExpression use
# exactly the API the owner uses; the rows with 'expectedNormalizerPattern' pin
# the preserved escape grammar of wildcard.cc, which deliberately differs.
#
# Task 10 adds the FTS half. ftshelpers.cc and fulltextsearch.cc are linked as
# production sources, never copied: parseSearchString() builds the
# FtsHighlightSpec the dialog hands to the page, ArticleUrls::lookupDocument/
# parse() must reproduce the same compiled expression after a URL round trip, and
# the production dialog is constructed with a plain QWidget parent. That is why
# the dialog's own closure is linked here: the point is that it no longer has a
# MainWindow to reach into.
#
# The FTS closure needs Dictionary::Request/DataRequest from dictionary.cc, and
# dictionary.cc also holds needToRebuildIndex(), whose ZipFile/SplitFile path is
# not part of this task. The function-sections/--gc-sections pair drops that
# unreferenced function, so the link stays inside this task's converted sources
# instead of pulling the still-unconverted SplitFile reader into the suite.
#
# Task 27 adds the production save collector helper (savearticle.hh). Until that
# header exists the define is absent and this project makes no save-coverage
# claim; the guarded saveCollector test appears exactly when the real helper does.

TARGET = tst_regex
include(../testsupport.pri)

QT += core gui widgets xml

SOURCES += $$GD_REPO_ROOT/articleurls.cc \
           $$GD_REPO_ROOT/config.cc \
           $$GD_REPO_ROOT/delegate.cc \
           $$GD_REPO_ROOT/dictionary.cc \
           $$GD_REPO_ROOT/folding.cc \
           $$GD_REPO_ROOT/fsencoding.cc \
           $$GD_REPO_ROOT/ftshelpers.cc \
           $$GD_REPO_ROOT/fulltextsearch.cc \
           $$GD_REPO_ROOT/gddebug.cc \
           $$GD_REPO_ROOT/mutex.cc \
           $$GD_REPO_ROOT/wildcard.cc \
           $$GD_REPO_ROOT/wstring_qt.cc \
           tst_regex.cc

HEADERS += $$GD_REPO_ROOT/articleurls.hh \
           $$GD_REPO_ROOT/dictionary.hh \
           $$GD_REPO_ROOT/ftshelpers.hh \
           $$GD_REPO_ROOT/fulltextsearch.hh

FORMS += $$GD_REPO_ROOT/fulltextsearch.ui

unix {
    QMAKE_CXXFLAGS += -ffunction-sections -fdata-sections
    QMAKE_LFLAGS += -Wl,--gc-sections
}

# The production save collector is delivered by Task 27. Until then the helper
# under test does not exist and the suite must not pretend it covered it.
exists($$PWD/../../savearticle.hh) {
    DEFINES += GD_SAVE_COLLECTOR_AVAILABLE
}

DISTFILES += $$GD_REPO_ROOT/tests/fixtures/regex/cases.json
