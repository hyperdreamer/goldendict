#ifndef GOLDENDICT_LINKREWRITE_HH_INCLUDED
#define GOLDENDICT_LINKREWRITE_HH_INCLUDED

#include <QString>

namespace LinkRewrite {

QString applyGdLookup( QString const & html );
QString applyBres( QString const & html, QString const & dictId );
QString applyGico( QString const & html, QString const & dictId );
QString applyGdau( QString const & html, QString const & dictId );

} // namespace LinkRewrite

#endif // GOLDENDICT_LINKREWRITE_HH_INCLUDED
