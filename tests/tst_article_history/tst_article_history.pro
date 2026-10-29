TARGET = tst_article_history
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
                           $$GD_REPO_ROOT/parsecmdline.cc
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
                           $$GD_REPO_ROOT/audioplayerinterface.hh
GD_ARTICLE_EXTRA_FORMS = $$GD_REPO_ROOT/authentication.ui \
                         $$GD_REPO_ROOT/articleview.ui
include(../article-test.pri)
SOURCES += tst_article_history.cc
# Observe production's integer-keyed state calls and retain real renderer
# results for the deadline/generation probe; never substitute a navigation map.
QMAKE_LFLAGS += -Wl,--wrap=_ZN14ArticleHistory10setCurrentEiRK12ArticleState \
                -Wl,--wrap=_ZN14ArticleHistory6commitERK4QUrli14NavigationKind \
                -Wl,--wrap=_ZNK14ArticleHistory8stateForEiP12ArticleState \
                -Wl,--wrap=_ZN14QWebEnginePage13runJavaScriptERK7QStringjRKSt8functionIFvRK8QVariantEE
GD_BRIDGE_QRC = $$GD_ARTICLE_RESOURCE_DIR/article-view-bridge-resources.qrc
GD_BRIDGE_XML = <RCC> <qresource prefix=\"/scripts\">
GD_BRIDGE_XML += <file alias=\"article-bootstrap.js\">$$GD_REPO_ROOT/scripts/article-bootstrap.js</file>
GD_BRIDGE_XML += <file alias=\"article-dom.js\">$$GD_REPO_ROOT/scripts/article-dom.js</file>
GD_BRIDGE_XML += <file alias=\"website-frame.js\">$$GD_REPO_ROOT/scripts/website-frame.js</file>
GD_BRIDGE_XML += </qresource> </RCC>
write_file($$GD_BRIDGE_QRC, GD_BRIDGE_XML)
RESOURCES += $$GD_BRIDGE_QRC
