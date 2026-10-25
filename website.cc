/* This file is (c) 2008-2012 Konstantin Isakov <ikm@goldendict.org>
 * Part of GoldenDict. Licensed under GPLv3 or later, see the LICENSE file */

#include "website.hh"
#include "wstring_qt.hh"
#include "utf8.hh"
#include <QUrl>
#include "encoding.hh"
#include <QDir>
#include <QFileInfo>
#include "gddebug.hh"

#include <QRegularExpression>

namespace WebSite {

using namespace Dictionary;

namespace {

class WebSiteDictionary: public Dictionary::Class
{
  string name;
  QByteArray urlTemplate;
  QString iconFilename;
  bool inside_iframe;
  QNetworkAccessManager & netMgr;

public:

  WebSiteDictionary( string const & id, string const & name_,
                     QString const & urlTemplate_,
                     QString const & iconFilename_,
                     bool inside_iframe_,
                     QNetworkAccessManager & netMgr_ ):
    Dictionary::Class( id, vector< string >() ),
    name( name_ ),
    urlTemplate( QUrl( urlTemplate_ ).toEncoded() ),
    iconFilename( iconFilename_ ),
    inside_iframe( inside_iframe_ ),
    netMgr( netMgr_ )
  {
    dictionaryDescription = urlTemplate_;
  }

  virtual string getName() throw()
  { return name; }

  virtual map< Property, string > getProperties() throw()
  { return map< Property, string >(); }

  virtual unsigned long getArticleCount() throw()
  { return 0; }

  virtual unsigned long getWordCount() throw()
  { return 0; }

  virtual sptr< WordSearchRequest > prefixMatch( wstring const & word,
                                                 unsigned long ) THROW_SPEC( std::exception );

  virtual sptr< DataRequest > getArticle( wstring const &,
                                          vector< wstring > const & alts,
                                          wstring const & context, bool )
    THROW_SPEC( std::exception );

  virtual sptr< Dictionary::DataRequest > getResource( string const & name ) THROW_SPEC( std::exception );

  void isolateWebCSS( QString & css );

protected:

  virtual void loadIcon() throw();
};

sptr< WordSearchRequest > WebSiteDictionary::prefixMatch( wstring const & /*word*/,
                                                          unsigned long ) THROW_SPEC( std::exception )
{
  sptr< WordSearchRequestInstant > sr = new WordSearchRequestInstant;

  sr->setUncertain( true );

  return sr;
}

void WebSiteDictionary::isolateWebCSS( QString & css )
{
  isolateCSS( css, ".website" );
}

class WebSiteArticleRequest: public WebSiteDataRequestSlots
{
  QNetworkReply * netReply;
  QString url;
  Class * dictPtr;
  QNetworkAccessManager & mgr;

public:

  WebSiteArticleRequest( QString const & url, QNetworkAccessManager & _mgr,
                         Class * dictPtr_ );
  ~WebSiteArticleRequest()
  {}

  virtual void cancel();

private:

  virtual void requestFinished( QNetworkReply * );
};

void WebSiteArticleRequest::cancel()
{
  finish();
}

WebSiteArticleRequest::WebSiteArticleRequest( QString const & url_,
                                              QNetworkAccessManager & _mgr,
                                              Class * dictPtr_ ):
  url( url_ ), dictPtr( dictPtr_ ), mgr( _mgr )
{
  connect( &mgr, SIGNAL( finished( QNetworkReply * ) ),
           this, SLOT( requestFinished( QNetworkReply * ) ),
           Qt::QueuedConnection );

  QUrl reqUrl( url );

  netReply = mgr.get( QNetworkRequest( reqUrl ) );

#ifndef QT_NO_OPENSSL
  connect( netReply, SIGNAL( sslErrors( QList< QSslError > ) ),
           netReply, SLOT( ignoreSslErrors() ) );
#endif
}

void WebSiteArticleRequest::requestFinished( QNetworkReply * r )
{
  if ( isFinished() ) // Was cancelled
    return;

  if ( r != netReply )
  {
    // Well, that's not our reply, don't do anything
    return;
  }

  if ( netReply->error() == QNetworkReply::NoError )
  {
    // Check for redirect reply

    QVariant possibleRedirectUrl = netReply->attribute( QNetworkRequest::RedirectionTargetAttribute );
    QUrl redirectUrl = possibleRedirectUrl.toUrl();
    if( !redirectUrl.isEmpty() )
    {
      QUrl newUrl = netReply->url().resolved( redirectUrl );
      disconnect( netReply, 0, 0, 0 );
      netReply->deleteLater();
      netReply = mgr.get( QNetworkRequest( newUrl ) );
#ifndef QT_NO_OPENSSL
      connect( netReply, SIGNAL( sslErrors( QList< QSslError > ) ),
               netReply, SLOT( ignoreSslErrors() ) );
#endif
      return;
    }

    // Handle reply data

    QByteArray replyData = netReply->readAll();
    const Encoding codec = Encoding::forHtml( replyData, netReply->rawHeader( "Content-Type" ) );
    QString articleString = resolveInlineUrls( codec.decode( replyData ), netReply->url() );
    articleString = detail::mapStylesheets( articleString, QString::fromStdString( dictPtr->getId() ) );

    // Check for unclosed <span> and <div>

    static const QRegularExpression openSpan( QStringLiteral( "<\\s*span\\b" ), QRegularExpression::CaseInsensitiveOption );
    static const QRegularExpression closeSpan( QStringLiteral( "<\\s*/span\\s*>" ), QRegularExpression::CaseInsensitiveOption );
    static const QRegularExpression openDiv( QStringLiteral( "<\\s*div\\b" ), QRegularExpression::CaseInsensitiveOption );
    static const QRegularExpression closeDiv( QStringLiteral( "<\\s*/div\\s*>" ), QRegularExpression::CaseInsensitiveOption );
    Q_ASSERT( openSpan.isValid() && closeSpan.isValid() && openDiv.isValid() && closeDiv.isValid() );
    int openTags = articleString.count( openSpan );
    int closedTags = articleString.count( closeSpan );
    while( openTags > closedTags )
    {
      articleString += "</span>";
      closedTags += 1;
    }

    openTags = articleString.count( openDiv );
    closedTags = articleString.count( closeDiv );
    while( openTags > closedTags )
    {
      articleString += "</div>";
      closedTags += 1;
    }

    // See Issue #271: A mechanism to clean-up invalid HTML cards.
    articleString += "</font>""</font>""</font>""</font>""</font>""</font>"
                     "</font>""</font>""</font>""</font>""</font>""</font>"
                     "</b></b></b></b></b></b></b></b>"
                     "</i></i></i></i></i></i></i></i>"
                     "</a></a></a></a></a></a></a></a>";

    QByteArray articleBody = articleString.toUtf8();

    QString divStr = QString( "<div class=\"website\"" );
    divStr += dictPtr->isToLanguageRTL() ? " dir=\"rtl\">" : ">";

    articleBody.prepend( divStr.toUtf8() );
    articleBody.append( "</div>" );

    articleBody.prepend( "<div class=\"website_padding\"></div>" );

    Mutex::Lock _( dataMutex );

    size_t prevSize = data.size();

    data.resize( prevSize + articleBody.size() );

    memcpy( &data.front() + prevSize, articleBody.data(), articleBody.size() );

    hasAnyData = true;

  }
  else
  {
    if( netReply->url().scheme() == "file" )
    {
      gdWarning( "WebSites: Failed loading article from \"%s\", reason: %s\n", dictPtr->getName().c_str(),
                 netReply->errorString().toUtf8().data() );
    }
    else
    {
      setErrorString( netReply->errorString() );
    }
  }

  disconnect( netReply, 0, 0, 0 );
  netReply->deleteLater();

  finish();
}

sptr< DataRequest > WebSiteDictionary::getArticle( wstring const & str,
                                                   vector< wstring > const &,
                                                   wstring const & context, bool )
  THROW_SPEC( std::exception )
{
  QByteArray urlString;

  // Context contains the right url to go to
  if ( context.size() )
    urlString = Utf8::encode( context ).c_str();
  else
  {
    urlString = detail::substituteUrlPlaceholders( urlTemplate, gd::toQString( str ) );
  }

  if( inside_iframe )
  {
    // Just insert link in <iframe> tag

    sptr< DataRequestInstant > dr = new DataRequestInstant( true );

    string result = "<div class=\"website_padding\"></div>";

    result += string( "<iframe id=\"gdexpandframe-" ) + getId() +
                      "\" name=\"gdexpandframe-" + getId() +
                      "\" src=\"" + urlString.data() +
                      "\" onmouseover=\"processIframeMouseOver('gdexpandframe-" + getId() + "');\" "
                      "onmouseout=\"processIframeMouseOut();\" "
                      "scrolling=\"no\" marginwidth=\"0\" marginheight=\"0\" "
                      "frameborder=\"0\" vspace=\"0\" hspace=\"0\" "
                      "style=\"overflow:visible; width:100%; display:none;\">"
                      "</iframe>";

    dr->getData().resize( result.size() );

    memcpy( &( dr->getData().front() ), result.data(), result.size() );

    return dr;
  }

  // To load data from site

  return new WebSiteArticleRequest( urlString, netMgr, this );
}

class WebSiteResourceRequest: public WebSiteDataRequestSlots
{
  QNetworkReply * netReply;
  QString url;
  WebSiteDictionary * dictPtr;
  QNetworkAccessManager & mgr;

public:

  WebSiteResourceRequest( QString const & url_, QNetworkAccessManager & _mgr,
                          WebSiteDictionary * dictPtr_ );
  ~WebSiteResourceRequest()
  {}

  virtual void cancel();

private:

  virtual void requestFinished( QNetworkReply * );
};

WebSiteResourceRequest::WebSiteResourceRequest( QString const & url_,
                                                QNetworkAccessManager & _mgr,
                                                WebSiteDictionary * dictPtr_ ):
  url( url_ ), dictPtr( dictPtr_ ), mgr( _mgr )
{
  connect( &mgr, SIGNAL( finished( QNetworkReply * ) ),
           this, SLOT( requestFinished( QNetworkReply * ) ),
           Qt::QueuedConnection );

  QUrl reqUrl( url );

  netReply = mgr.get( QNetworkRequest( reqUrl ) );

#ifndef QT_NO_OPENSSL
  connect( netReply, SIGNAL( sslErrors( QList< QSslError > ) ),
           netReply, SLOT( ignoreSslErrors() ) );
#endif
}

void WebSiteResourceRequest::cancel()
{
  finish();
}

void WebSiteResourceRequest::requestFinished( QNetworkReply * r )
{
  if ( isFinished() ) // Was cancelled
    return;

  if ( r != netReply )
  {
    // Well, that's not our reply, don't do anything
    return;
  }

  if ( netReply->error() == QNetworkReply::NoError )
  {
    // Check for redirect reply

    QVariant possibleRedirectUrl = netReply->attribute( QNetworkRequest::RedirectionTargetAttribute );
    QUrl redirectUrl = possibleRedirectUrl.toUrl();
    if( !redirectUrl.isEmpty() )
    {
      const QUrl resolved = netReply->url().resolved( redirectUrl );
      disconnect( netReply, 0, 0, 0 );
      netReply->deleteLater();
      netReply = mgr.get( QNetworkRequest( resolved ) );
#ifndef QT_NO_OPENSSL
      connect( netReply, SIGNAL( sslErrors( QList< QSslError > ) ),
               netReply, SLOT( ignoreSslErrors() ) );
#endif
      return;
    }

    // Handle reply data

    QByteArray replyData = netReply->readAll();
    const Encoding codec = Encoding::forHtml( replyData, netReply->rawHeader( "Content-Type" ) );
    QString cssString = detail::resolveCss( codec.decode( replyData ), netReply->url() );

    dictPtr->isolateWebCSS( cssString );

    QByteArray cssData = cssString.toUtf8();

    Mutex::Lock _( dataMutex );

    size_t prevSize = data.size();

    data.resize( prevSize + cssData.size() );

    memcpy( &data.front() + prevSize, cssData.data(), cssData.size() );

    hasAnyData = true;
  }
  else
    setErrorString( netReply->errorString() );

  disconnect( netReply, 0, 0, 0 );
  netReply->deleteLater();

  finish();
}

sptr< Dictionary::DataRequest > WebSiteDictionary::getResource( string const & name ) THROW_SPEC( std::exception )
{
  QString link = QString::fromUtf8( name.c_str() );
  // Current resources retain the entire absolute URL as one encoded path.
  // The dispatch layer removes that path's encoding once, not the remote URL's
  // own percent escapes. Accept old scheme/host paths for in-flight articles.
  if( !link.startsWith( "http://" ) && !link.startsWith( "https://" ) )
  {
    if( link.startsWith( "http%3A", Qt::CaseInsensitive ) || link.startsWith( "https%3A", Qt::CaseInsensitive ) )
      link = QUrl::fromPercentEncoding( link.toUtf8() );
    else
    {
      const auto pos = link.indexOf( '/' );
      if( pos > 0 ) link.replace( pos, 1, "://" );
    }
  }
  return new WebSiteResourceRequest( link, netMgr, this );
}

void WebSiteDictionary::loadIcon() throw()
{
  if ( dictionaryIconLoaded )
    return;

  if( !iconFilename.isEmpty() )
  {
    QFileInfo fInfo(  QDir( Config::getConfigDir() ), iconFilename );
    if( fInfo.isFile() )
      loadIconFromFile( fInfo.absoluteFilePath(), true );
  }
  if( dictionaryIcon.isNull() )
    dictionaryIcon = dictionaryNativeIcon = QIcon(":/icons/internet.png");
  dictionaryIconLoaded = true;
}

}

vector< sptr< Dictionary::Class > > makeDictionaries( Config::WebSites const & ws,
                                                      QNetworkAccessManager & mgr )
  THROW_SPEC( std::exception )
{
  vector< sptr< Dictionary::Class > > result;

  for( int x = 0; x < ws.size(); ++x )
  {
    if ( ws[ x ].enabled )
      result.push_back( new WebSiteDictionary( ws[ x ].id.toUtf8().data(),
                                               ws[ x ].name.toUtf8().data(),
                                               ws[ x ].url,
                                               ws[ x ].iconFilename,
                                               ws[ x ].inside_iframe,
                                               mgr )
                      );
  }

  return result;
}

}
