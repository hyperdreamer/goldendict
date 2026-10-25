/* This file is (c) 2022 Igor Kushnir <igorkuo@gmail.com>
 * Part of GoldenDict. Licensed under GPLv3 or later, see the LICENSE file */

#ifndef CATEGORIZED_LOGGING_HH_INCLUDED
#define CATEGORIZED_LOGGING_HH_INCLUDED

// Qt 6 provides categorized logging and variadic macros unconditionally, so the
// pre-5.4 compatibility shims GoldenDict used to carry are gone. The wrappers
// stay because the implementation routes every message through gdWarning()'s
// locale codec replacer before logging it.

#include <QLoggingCategory>

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
