TARGET = tst_article_stream
include(../testsupport.pri)
include(../article-test.pri)
SOURCES += tst_article_stream.cc

# Observe actual production calls and then forward to Qt/the real getters.
# No substitute job, handler, device, reader or renderer is linked.
GD_OBSERVED_SYMBOLS = \
    _ZN23QWebEngineUrlRequestJob5replyERK10QByteArrayP9QIODevice \
    _ZN23QWebEngineUrlRequestJob4failENS_5ErrorE \
    _ZN10Dictionary11DataRequest8dataSizeEv \
    _ZN10Dictionary11DataRequest12getDataSliceEmmPv \
    _ZN10Dictionary11DataRequest11getFullDataEv \
    _ZN10Dictionary7Request10isFinishedEv \
    _ZN10Dictionary7Request14getErrorStringEv
for(GD_SYMBOL, GD_OBSERVED_SYMBOLS): QMAKE_LFLAGS += -Wl,--wrap=$$GD_SYMBOL
