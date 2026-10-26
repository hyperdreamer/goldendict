# Pure suite for the source-tagged regex migration ledger.
#
# The ledger (fixtures/regex/cases.json) records, for every migrated pattern
# family, the source location, the old engine/options, the migrated pattern and
# options, and the fixed expected matches/captures/replacement. This suite is the
# executable half of the ledger: it compiles every row, compares the pinned
# expectations, and proves the traps the migration must not walk into (two-digit
# hex, two-digit octal, anchored trailing newline, zero-width loops, greedy vs
# minimal counts).
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

TARGET = tst_regex
include(../testsupport.pri)

QT += core

SOURCES += $$GD_REPO_ROOT/wildcard.cc \
           tst_regex.cc

DISTFILES += $$GD_REPO_ROOT/tests/fixtures/regex/cases.json
