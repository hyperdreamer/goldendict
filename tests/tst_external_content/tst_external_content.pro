TARGET = tst_external_content
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
SOURCES += tst_external_content.cc

GD_BRIDGE_QRC = $$GD_ARTICLE_RESOURCE_DIR/external-content-bridge-resources.qrc
GD_BRIDGE_XML = <RCC> <qresource prefix=\"/scripts\">
GD_BRIDGE_XML += <file alias=\"article-bootstrap.js\">$$GD_REPO_ROOT/scripts/article-bootstrap.js</file>
GD_BRIDGE_XML += <file alias=\"article-dom.js\">$$GD_REPO_ROOT/scripts/article-dom.js</file>
GD_BRIDGE_XML += <file alias=\"website-frame.js\">$$GD_REPO_ROOT/scripts/website-frame.js</file>
GD_BRIDGE_XML += </qresource> </RCC>
write_file($$GD_BRIDGE_QRC, GD_BRIDGE_XML)
RESOURCES += $$GD_BRIDGE_QRC

# Observe real Chromium request objects and production calls. Context rows
# substitute only URL *values*, never manufacture a Qt private request object.
GD_OBSERVED_SYMBOLS = \
    _ZN6Config18getNetworkCacheDirEv \
    _ZN12ArticleMaker14adjustFilePathER7QString \
    _ZN9QFileInfo6existsERK7QString \
    _ZNK9QFileInfo6existsEv \
    _ZN12QApplication12activeWindowEv \
    _ZNK4QUrl4hostE6QFlagsINS_25ComponentFormattingOptionEE \
    _ZNK24QWebEngineUrlRequestInfo10requestUrlEv \
    _ZNK24QWebEngineUrlRequestInfo13firstPartyUrlEv \
    _ZNK24QWebEngineUrlRequestInfo9initiatorEv \
    _ZN24QWebEngineUrlRequestInfo5blockEb \
    _ZN24QWebEngineUrlRequestInfo8redirectERK4QUrl \
    _ZN17QWebEngineProfile14clearHttpCacheEv \
    _ZN17QWebEngineProfileC1ERK7QStringP7QObject \
    _ZN13QNetworkProxy19setApplicationProxyERKS_ \
    _ZN20QNetworkProxyFactory19systemProxyForQueryERK18QNetworkProxyQuery
for(GD_SYMBOL, GD_OBSERVED_SYMBOLS): QMAKE_LFLAGS += -Wl,--wrap=$$GD_SYMBOL
