/* This file is (c) 2022 Igor Kushnir <igorkuo@gmail.com>
 * Part of GoldenDict. Licensed under GPLv3 or later, see the LICENSE file */

#ifndef CATEGORIZED_LOGGING_HH_INCLUDED
#define CATEGORIZED_LOGGING_HH_INCLUDED

// Lots of changes have been made to Qt's implementation of categorized logging in versions 5.3 and 5.4.
// __VA_ARGS__ was introduced in C++11.
//
// Keep the selector defined until gddebug.cc's owning task collapses its
// consumer branches. The Qt 6 logging declarations live here; this header no
// longer provides the legacy GdLoggingCategory fallback.
#include <QLoggingCategory>
#define GD_CATEGORIZED_LOGGING

Q_DECLARE_LOGGING_CATEGORY( dictionaryResourceLc )

#if !defined(QT_NO_WARNING_OUTPUT)
/// Print a categorized warning message.
#  define gdCWarning(category, ...) \
    for (bool qt_category_enabled = category().isWarningEnabled(); qt_category_enabled; qt_category_enabled = false) \
        gdCWarningImpl( category(), __VA_ARGS__ )
#else
#  define gdCWarning(category, ...) QT_NO_QDEBUG_MACRO()
#endif

#if !defined(QT_NO_DEBUG_OUTPUT)
/// Print a categorized debug message.
#  define gdCDebug(category, ...) \
    for (bool qt_category_enabled = category().isDebugEnabled(); qt_category_enabled; qt_category_enabled = false) \
        gdCDebugImpl( category(), __VA_ARGS__ )
#else
#  define gdCDebug(category, ...) QT_NO_QDEBUG_MACRO()
#endif

void gdCWarningImpl( QLoggingCategory const & category, char const * message, ... ) Q_ATTRIBUTE_FORMAT_PRINTF( 2, 3 );
void gdCDebugImpl( QLoggingCategory const & category, char const * message, ... ) Q_ATTRIBUTE_FORMAT_PRINTF( 2, 3 );

#endif // CATEGORIZED_LOGGING_HH_INCLUDED
