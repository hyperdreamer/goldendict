/* This file is part of GoldenDict. Licensed under GPLv3 or later, see LICENSE. */
#include "hotkeybackend.hh"
#include "hotkeywrapper.hh"
#include "portalhotkeys.hh"

#include <QDBusConnection>
#include <QGuiApplication>

namespace {
class UnsupportedBackend final : public HotkeyBackend
{
public:
    explicit UnsupportedBackend( QObject * parent ): HotkeyBackend( parent ) {}
    bool isSupported() const noexcept override { return false; }
    State state() const noexcept override { return State::Unsupported; }
    void bind( QVector< HotkeyBinding > const & ) override
    {
        emit stateChanged( State::Unsupported,
            tr( "Global hotkeys are unavailable on platform %1." )
                .arg( QGuiApplication::platformName() ) );
    }
    void unbind() override {}
};
}

std::unique_ptr< HotkeyBackend > createHotkeyBackend( QObject * parent )
{
    const QString platform = QGuiApplication::platformName().toLower();
    // Never use HAVE_X11 or DISPLAY as evidence of an active X11 QPA.
    if ( platform.startsWith( QLatin1String( "wayland" ) ) )
        return std::make_unique< PortalBackend >( QDBusConnection::sessionBus(), parent );
#ifdef HAVE_X11
    if ( HotkeyRuntime::isX11Session( platform ) )
        return std::make_unique< X11RecordBackend >( parent );
#endif
    return std::make_unique< UnsupportedBackend >( parent );
}
