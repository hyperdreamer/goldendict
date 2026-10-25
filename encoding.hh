#ifndef GOLDENDICT_ENCODING_HH_INCLUDED
#define GOLDENDICT_ENCODING_HH_INCLUDED

#include <QByteArray>
#include <QString>

#include <memory>
#include <string>

#include "ex.hh"

/// Immutable character-set description. Conversion state is created per call.
class Encoding
{
public:
    DEF_EX( Ex, "Encoding error", std::exception )

    class exUnrepresentable : public Ex
    {
        std::string message;

    public:
        exUnrepresentable(): message( "Character not representable in target encoding" ) {}
        explicit exUnrepresentable( std::string const & context ):
            message( std::string( "Character not representable in target encoding " ) + context )
        {
        }

        const char * what() const throw() override { return message.c_str(); }
        ~exUnrepresentable() throw() override {}
    };

    Encoding() noexcept;

    static std::unique_ptr< Encoding > byName( QString const & name );
    static Encoding utf8();
    static Encoding forLocale() noexcept;

    enum class DetectedBy
    {
        Header,
        Bom,
        Meta,
        XmlDeclaration,
        Fallback
    };

    static Encoding forHtml( QByteArray const & body,
                             QByteArray const & contentTypeHeader,
                             QString const & fallback = QStringLiteral( "UTF-8" ),
                             DetectedBy * detectedBy = nullptr );

    bool isValid() const noexcept;
    QString name() const;
    QString decode( QByteArray const & data ) const noexcept;
    QByteArray encode( QString const & text ) const;

private:
    explicit Encoding( QString canonicalName, bool decodeOnly ) noexcept;

    QString canonicalName_;
    bool decodeOnly_;
};

// Private conversion policies shared by the legacy engine callers and their
// pure tests. Raw container bytes are always inputs, never rewritten in place.
namespace EncodingCallers
{
QByteArray epwingSearchBytes( Encoding const & codec, QString const & word );
QByteArray slobTextToUtf8( Encoding const & codec, QByteArray const & bytes );
QString zipFilenameForDisplay( Encoding const & locale, QByteArray const & bytes );
}

#endif // GOLDENDICT_ENCODING_HH_INCLUDED
