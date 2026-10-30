#ifndef __WILCARD_HH_INCLUDED__
#define __WILCARD_HH_INCLUDED__

#include <QString>
#include <QRegularExpression>
#include <QDebug>

namespace Regex {

// Return Qt's pattern unchanged, reporting broken built-ins in release builds
// as well. An invalid expression cannot match or replace any source text.
inline QRegularExpression validated( QString const & pattern,
                                    QRegularExpression::PatternOptions options = QRegularExpression::NoPatternOption )
{
  QRegularExpression expression( pattern, options );
  if( !expression.isValid() )
    qWarning( "Invalid built-in regular expression %s: %s (offset %lld)",
              qPrintable( pattern ), qPrintable( expression.errorString() ),
              static_cast< long long >( expression.patternErrorOffset() ) );
  return expression;
}

}

// Preserved unanchored headword grammar (including backslash escapes and
// malformed brackets). It deliberately differs from Qt's wildcard converter.

QString wildcardsToRegexp( const QString & wc_str );

#endif
