TARGET = tst_website_frames
include(../testsupport.pri)
GD_ARTICLE_EXTRA_SOURCES = $$GD_REPO_ROOT/articleprofile.cc \
                           $$GD_REPO_ROOT/articlebridge.cc \
                           $$GD_REPO_ROOT/articlewebview.cc \
                           $$GD_REPO_ROOT/websiteframes.cc
GD_ARTICLE_EXTRA_HEADERS = $$GD_REPO_ROOT/articleprofile.hh \
                           $$GD_REPO_ROOT/articlebridge.hh \
                           $$GD_REPO_ROOT/articlewebview.hh \
                           $$GD_REPO_ROOT/websiteframes.hh
GD_ARTICLE_EXTRA_FORMS = $$GD_REPO_ROOT/authentication.ui
SOURCES += $$GD_REPO_ROOT/articleinspector.cc
HEADERS += $$GD_REPO_ROOT/articleinspector.hh
include(../article-test.pri)
SOURCES += tst_website_frames.cc
QMAKE_LFLAGS += -Wl,--wrap=_ZN15QWebEngineFrame13runJavaScriptERK7QStringjRKSt8functionIFvRK8QVariantEE \
                -Wl,--wrap=_ZNK15QWebEngineFrame8childrenEv
GD_FRAME_QRC = $$GD_ARTICLE_RESOURCE_DIR/frame-resources.qrc
GD_FRAME_XML = <RCC> <qresource prefix=\"/scripts\">
GD_FRAME_XML += <file alias=\"article-bootstrap.js\">$$GD_REPO_ROOT/scripts/article-bootstrap.js</file>
GD_FRAME_XML += <file alias=\"article-dom.js\">$$GD_REPO_ROOT/scripts/article-dom.js</file>
GD_FRAME_XML += <file alias=\"website-frame.js\">$$GD_REPO_ROOT/scripts/website-frame.js</file>
GD_FRAME_XML += </qresource> </RCC>
write_file($$GD_FRAME_QRC, GD_FRAME_XML)
RESOURCES += $$GD_FRAME_QRC
