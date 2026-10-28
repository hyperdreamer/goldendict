TARGET = tst_article_history
include(../testsupport.pri)
GD_ARTICLE_EXTRA_SOURCES = $$GD_REPO_ROOT/articlehistory.cc
GD_ARTICLE_EXTRA_HEADERS = $$GD_REPO_ROOT/articlehistory.hh
include(../article-test.pri)
SOURCES += tst_article_history.cc
