TARGET = tst_save_article
include(../testsupport.pri)
GD_ARTICLE_EXTRA_SOURCES = $$GD_REPO_ROOT/articleprofile.cc \
                           $$GD_REPO_ROOT/articlebridge.cc \
                           $$GD_REPO_ROOT/articlewebview.cc \
                           $$GD_REPO_ROOT/articlehistory.cc \
                           $$GD_REPO_ROOT/websiteframes.cc \
                           $$GD_REPO_ROOT/articleview.cc \
                           $$GD_REPO_ROOT/groupcombobox.cc \
                           $$GD_REPO_ROOT/gestures.cc \
                           $$GD_REPO_ROOT/webmultimediadownload.cc \
                           $$GD_REPO_ROOT/programs.cc \
                           $$GD_REPO_ROOT/filetype.cc \
                           $$GD_REPO_ROOT/parsecmdline.cc \
                           $$GD_REPO_ROOT/savearticle.cc
GD_ARTICLE_EXTRA_HEADERS = $$GD_REPO_ROOT/articleprofile.hh \
                           $$GD_REPO_ROOT/articlebridge.hh \
                           $$GD_REPO_ROOT/articlewebview.hh \
                           $$GD_REPO_ROOT/articlehistory.hh \
                           $$GD_REPO_ROOT/websiteframes.hh \
                           $$GD_REPO_ROOT/articleview.hh \
                           $$GD_REPO_ROOT/groupcombobox.hh \
                           $$GD_REPO_ROOT/gestures.hh \
                           $$GD_REPO_ROOT/webmultimediadownload.hh \
                           $$GD_REPO_ROOT/programs.hh \
                           $$GD_REPO_ROOT/audioplayerinterface.hh \
                           $$GD_REPO_ROOT/savearticle.hh
GD_ARTICLE_EXTRA_FORMS = $$GD_REPO_ROOT/authentication.ui \
                         $$GD_REPO_ROOT/articleview.ui
include(../article-test.pri)
SOURCES += tst_save_article.cc

GD_BRIDGE_QRC = $$GD_ARTICLE_RESOURCE_DIR/article-view-bridge-resources.qrc
GD_BRIDGE_XML = <RCC> <qresource prefix=\"/scripts\">
GD_BRIDGE_XML += <file alias=\"article-bootstrap.js\">$$GD_REPO_ROOT/scripts/article-bootstrap.js</file>
GD_BRIDGE_XML += <file alias=\"article-dom.js\">$$GD_REPO_ROOT/scripts/article-dom.js</file>
GD_BRIDGE_XML += <file alias=\"website-frame.js\">$$GD_REPO_ROOT/scripts/website-frame.js</file>
GD_BRIDGE_XML += </qresource> </RCC>
write_file($$GD_BRIDGE_QRC, GD_BRIDGE_XML)
RESOURCES += $$GD_BRIDGE_QRC
