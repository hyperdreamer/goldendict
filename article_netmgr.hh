/* This file is (c) 2008-2011 Konstantin Isakov <ikm@goldendict.org>
 * Part of GoldenDict. Licensed under GPLv3 or later, see the LICENSE file */

#ifndef __ARTICLE_NETMGR_HH_INCLUDED__
#define __ARTICLE_NETMGR_HH_INCLUDED__

#include "dictionary.hh"
#include "article_maker.hh"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QSslError>
#include <QObject>
#include <vector>

/// Explicit application requests and dictionary-resource construction. Chromium
/// document traffic belongs to ArticleSchemeHandler/ArticleRequestInterceptor.
class ArticleNetworkAccessManager: public QNetworkAccessManager
{
  Q_OBJECT

  std::vector< sptr< Dictionary::Class > > const & dictionaries;
  ArticleMaker const & articleMaker;
  bool const & hideHeader;

public:
  ArticleNetworkAccessManager( QObject * parent,
                              std::vector< sptr< Dictionary::Class > > const & dictionaries_,
                              ArticleMaker const & articleMaker_,
                              bool const & disallowContentFromOtherSites_,
                              bool const & hideHeader_ ):
    QNetworkAccessManager( parent ), dictionaries( dictionaries_ ),
    articleMaker( articleMaker_ ), hideHeader( hideHeader_ )
  {
    // Retain the constructor boundary for explicit-network consumers while
    // renderer-origin decisions move to the profile interceptor (Task 15).
    Q_UNUSED( disallowContentFromOtherSites_ );
    connect( this, &QNetworkAccessManager::sslErrors, this, &ArticleNetworkAccessManager::ignoreSSLErrors );
  }

  sptr< Dictionary::DataRequest > getResource( QUrl const & url, QString & contentType );

protected:
  QNetworkReply * createRequest( Operation op, QNetworkRequest const & req,
                                QIODevice * outgoingData = nullptr ) override;

private slots:
  void ignoreSSLErrors( QNetworkReply * reply, QList< QSslError > const & errors );
};

/// QNetworkReply adapter for native consumers of internal URLs. WebEngine
/// receives QBuffers from ArticleSchemeHandler and never uses this adapter.
class ArticleResourceReply: public QNetworkReply
{
  Q_OBJECT

  sptr< Dictionary::DataRequest > req;
  qint64 alreadyRead = 0;
  bool cancelled = false;

public:
  ArticleResourceReply( QObject * parent, QNetworkRequest const & request,
                       sptr< Dictionary::DataRequest > const & req_, QString const & contentType );
  ~ArticleResourceReply() override;
  void abort() override;
  qint64 bytesAvailable() const override;

protected:
  qint64 readData( char * data, qint64 maxSize ) override;

private slots:
  void reqUpdated();
  void reqFinished();
};

#endif
