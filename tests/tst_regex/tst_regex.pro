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
# The engine helpers that consume these patterns are migrated by their owner
# tasks (Task 9 converts the engine and wildcard implementations). Rows whose
# consumer has not moved yet name their productionOwner; they are fixed
# expectations for that task, never a test-only reimplementation of it. The
# wildcard rows drive QRegularExpression::wildcardToRegularExpression itself,
# which is exactly the API the owner uses.

TARGET = tst_regex
include(../testsupport.pri)

QT += core

SOURCES += tst_regex.cc

DISTFILES += $$GD_REPO_ROOT/tests/fixtures/regex/cases.json
