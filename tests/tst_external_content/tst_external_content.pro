TARGET = tst_external_content
include(../testsupport.pri)
GD_ARTICLE_EXTRA_SOURCES = $$GD_REPO_ROOT/articleprofile.cc
GD_ARTICLE_EXTRA_HEADERS = $$GD_REPO_ROOT/articleprofile.hh
GD_ARTICLE_EXTRA_FORMS = $$GD_REPO_ROOT/authentication.ui
include(../article-test.pri)
SOURCES += tst_external_content.cc

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
