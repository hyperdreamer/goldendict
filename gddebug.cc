/* This file is (c) 2013 Abs62
 * Part of GoldenDict. Licensed under GPLv3 or later, see the LICENSE file */

#include <QString>
#include "categorized_logging.hh"
#include "gddebug.hh"
#include "encoding.hh"

QString GdLocaleDecodeOnlyCodec::toUnicode( char const * data, qsizetype size ) const noexcept
{
  return Encoding::forLocale().decode( QByteArray( data, size ) );
}

QString GdLocaleDecodeOnlyCodec::toUnicode( char const * data ) const noexcept
{
  return Encoding::forLocale().decode( QByteArray( data ) );
}

QString GdLocaleDecodeOnlyCodec::toUnicode( QByteArray const & data ) const noexcept
{
  return Encoding::forLocale().decode( data );
}

QTextCodec * gdCodecForLocale() noexcept
{
  static GdLocaleDecodeOnlyCodec codec;
  return &codec;
}

#define TO_LOG_MESSAGE( msg, ap ) QString::vasprintf( msg, ap ).toUtf8().constData()

QFile * logFilePtr;

void gdWarning(const char *msg, ...)
{
  va_list ap;
  va_start(ap, msg);
  qWarning( "%s", TO_LOG_MESSAGE( msg, ap ) );
  va_end(ap);
}

void gdDebug(const char *msg, ...)
{
  va_list ap;
  va_start(ap, msg);
  qDebug( "%s", TO_LOG_MESSAGE( msg, ap ) );
  va_end(ap);
}

#ifdef GD_CATEGORIZED_LOGGING
Q_LOGGING_CATEGORY( dictionaryResourceLc, "goldendict.dictionary.resource" )
#endif

#ifdef GD_CATEGORIZED_LOGGING
void gdCWarningImpl( QLoggingCategory const & category, char const * msg, ... )
#else
void gdCWarning( GdLoggingCategory, char const * msg, ... )
#endif
{
  va_list ap;
  va_start( ap, msg );
#ifdef GD_CATEGORIZED_LOGGING
  QMessageLogger( QT_MESSAGELOG_FILE, QT_MESSAGELOG_LINE, QT_MESSAGELOG_FUNC, category.categoryName() )
      .warning( "%s", TO_LOG_MESSAGE( msg, ap ) );
#else
  qWarning( "%s", TO_LOG_MESSAGE( msg, ap ) );
#endif
  va_end( ap );
}

#ifdef GD_CATEGORIZED_LOGGING
void gdCDebugImpl( QLoggingCategory const & category, char const * msg, ... )
#else
void gdCDebug( GdLoggingCategory, char const * msg, ... )
#endif
{
  va_list ap;
  va_start( ap, msg );
#ifdef GD_CATEGORIZED_LOGGING
  QMessageLogger( QT_MESSAGELOG_FILE, QT_MESSAGELOG_LINE, QT_MESSAGELOG_FUNC, category.categoryName() )
      .debug( "%s", TO_LOG_MESSAGE( msg, ap ) );
#else
  qDebug( "%s", TO_LOG_MESSAGE( msg, ap ) );
#endif
  va_end( ap );
}
