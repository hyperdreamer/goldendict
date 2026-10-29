TARGET = tst_print_pipeline
include(../testsupport.pri)
QT += printsupport pdf pdfwidgets
GD_ARTICLE_EXTRA_SOURCES = $$GD_REPO_ROOT/articleprofile.cc \
                           $$GD_REPO_ROOT/articlebridge.cc \
                           $$GD_REPO_ROOT/articlewebview.cc \
                           $$GD_REPO_ROOT/websiteframes.cc \
                           $$GD_REPO_ROOT/printpipeline.cc
GD_ARTICLE_EXTRA_HEADERS = $$GD_REPO_ROOT/articleprofile.hh \
                           $$GD_REPO_ROOT/articlebridge.hh \
                           $$GD_REPO_ROOT/articlewebview.hh \
                           $$GD_REPO_ROOT/websiteframes.hh \
                           $$GD_REPO_ROOT/printpipeline.hh
GD_ARTICLE_EXTRA_FORMS = $$GD_REPO_ROOT/authentication.ui
include(../article-test.pri)
SOURCES += tst_print_pipeline.cc

GD_PRINT_QRC = $$GD_ARTICLE_RESOURCE_DIR/print-resources.qrc
GD_PRINT_XML = <RCC> <qresource prefix=\"/scripts\">
GD_PRINT_XML += <file alias=\"article-bootstrap.js\">$$GD_REPO_ROOT/scripts/article-bootstrap.js</file>
GD_PRINT_XML += <file alias=\"article-dom.js\">$$GD_REPO_ROOT/scripts/article-dom.js</file>
GD_PRINT_XML += <file alias=\"website-frame.js\">$$GD_REPO_ROOT/scripts/website-frame.js</file>
GD_PRINT_XML += </qresource> </RCC>
write_file($$GD_PRINT_QRC, GD_PRINT_XML)
RESOURCES += $$GD_PRINT_QRC
