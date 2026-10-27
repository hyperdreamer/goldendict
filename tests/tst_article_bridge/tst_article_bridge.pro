TARGET = tst_article_bridge
include(../testsupport.pri)
GD_ARTICLE_EXTRA_SOURCES = $$GD_REPO_ROOT/articleprofile.cc \
                           $$GD_REPO_ROOT/articlebridge.cc \
                           $$GD_REPO_ROOT/articlewebview.cc
GD_ARTICLE_EXTRA_HEADERS = $$GD_REPO_ROOT/articleprofile.hh \
                           $$GD_REPO_ROOT/articlebridge.hh \
                           $$GD_REPO_ROOT/articlewebview.hh
GD_ARTICLE_EXTRA_FORMS = $$GD_REPO_ROOT/authentication.ui
include(../article-test.pri)
SOURCES += tst_article_bridge.cc
# Hold selected real completions after Chromium evaluates the production call.
QMAKE_LFLAGS += -Wl,--wrap=_ZN14QWebEnginePage13runJavaScriptERK7QStringjRKSt8functionIFvRK8QVariantEE \
                -Wl,--wrap=_ZN15QWebEngineFrame13runJavaScriptERK7QStringjRKSt8functionIFvRK8QVariantEE

# Use the shipped resource aliases, without the unrelated version.txt generated
# by the final application build.
GD_BRIDGE_QRC = $$GD_ARTICLE_RESOURCE_DIR/bridge-resources.qrc
GD_BRIDGE_XML = <RCC> <qresource prefix=\"/scripts\">
GD_BRIDGE_XML += <file alias=\"article-bootstrap.js\">$$GD_REPO_ROOT/scripts/article-bootstrap.js</file>
GD_BRIDGE_XML += <file alias=\"article-dom.js\">$$GD_REPO_ROOT/scripts/article-dom.js</file>
GD_BRIDGE_XML += </qresource> </RCC>
write_file($$GD_BRIDGE_QRC, GD_BRIDGE_XML)
RESOURCES += $$GD_BRIDGE_QRC
