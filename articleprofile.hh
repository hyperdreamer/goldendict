/* Part of GoldenDict. Licensed under GPLv3 or later, see LICENSE. */
#ifndef GOLDENDICT_ARTICLEPROFILE_HH_INCLUDED
#define GOLDENDICT_ARTICLEPROFILE_HH_INCLUDED

#include <QObject>
#include <QPointer>
#include <QMap>
#include <memory>

class QAuthenticator;
class QUrl;
class QWidget;
class QWebEnginePage;
class QWebEngineProfile;
class ArticleNetworkAccessManager;
namespace Config { struct Class; }

class ProxyAuthAdapter : public QObject
{
  Q_OBJECT
public:
  ProxyAuthAdapter( Config::Class & config, QWidget * dialogParent, QObject * parent = nullptr );
  void authenticate( QUrl const & requestUrl, QAuthenticator * auth, QString const & proxyHost );
private:
  Config::Class & config;
  QPointer< QWidget > dialogParent;
  QMap< QString, QPair< QString, QString > > attempted;
};

class OriginAuthAdapter : public QObject
{
  Q_OBJECT
public:
  OriginAuthAdapter( QWidget * dialogParent, QObject * parent = nullptr );
  void authenticate( QUrl const & requestUrl, QAuthenticator * auth );
private:
  QPointer< QWidget > dialogParent;
};

/// GUI owner of the single article profile. Normal shutdown destroys pages first.
/// attachPage is the only attachment seam, also used by temporary/DevTools pages.
class ArticleProfileController : public QObject
{
  Q_OBJECT
public:
  ArticleProfileController( Config::Class & config, ArticleNetworkAccessManager & resources,
                            QWidget * windowParent );
  ~ArticleProfileController() override;
  QWebEngineProfile * profile() const noexcept;
  void attachPage( QWebEnginePage * page );
  void applyPreferences();
  void clearForExit();
signals:
  void exitCachesReady( bool timedOut );
private:
  struct State;
  std::unique_ptr< State > state;
  void finishExit( bool timedOut );
};
#endif
