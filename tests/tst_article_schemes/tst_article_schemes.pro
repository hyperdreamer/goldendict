TARGET = tst_article_schemes
include(../testsupport.pri)

# config.hh (the host of Config::InputPhrase) pulls in QKeySequence, which lives
# in QtGui on Qt 6. The binary itself is a QCoreApplication suite: it needs no
# display and no engine.
QT += core gui

# The pure URL closure only. The scheme handler, its streaming device and the
# request interceptor are declared in articleschemes.hh by this task and are
# linked here only once their owning tasks have implemented them; the reader
# contract is asserted at compile time instead.
SOURCES += $$GD_REPO_ROOT/articleurls.cc \
           tst_article_schemes.cc

HEADERS += $$GD_REPO_ROOT/articleurls.hh \
           $$GD_REPO_ROOT/articleschemes.hh
