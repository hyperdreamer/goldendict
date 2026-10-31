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

bool hotkeyBindingSettles( HotkeyBackend::State state ) noexcept
{
    using State = HotkeyBackend::State;
    return state == State::Bound || state == State::Partial || state == State::Unsupported;
}

bool hotkeyBindingInFlight( HotkeyBackend::State state ) noexcept
{
    using State = HotkeyBackend::State;
    return state == State::Creating || state == State::Binding;
}

bool hotkeyInstallShouldBind( bool backendPresent, bool unchangedSet,
                              HotkeyBackend::State state,
                              HotkeyInstallIntent intent ) noexcept
{
    // Ordered, first-match-wins: Unsupported is final, while an actual edit
    // preempts an in-flight bind and replaces a settled supported binding.
    if ( !backendPresent )
        return true;
    if ( state == HotkeyBackend::State::Unsupported )
        return false;
    if ( hotkeyBindingInFlight( state ) && unchangedSet )
        return false;
    if ( hotkeyBindingInFlight( state ) && !unchangedSet )
        return true;
    if ( !unchangedSet )
        return true;
    if ( hotkeyBindingSettles( state ) )
        return false;
    if ( intent == HotkeyInstallIntent::UserAction )
        return true;
    return false;
}

bool HotkeyInstallGate::shouldBind( bool backendPresent, QVector< HotkeyBinding > const & desired,
                                    HotkeyBackend::State state, HotkeyInstallIntent intent )
{
    bool unchangedSet = primed && lastDesired.size() == desired.size();
    for ( int i = 0; unchangedSet && i < desired.size(); ++i )
        unchangedSet = desired[i].id == lastDesired[i].id && desired[i].sequence == lastDesired[i].sequence;

    if ( !hotkeyInstallShouldBind( backendPresent, unchangedSet, state, intent ) )
        return false;

    lastDesired = desired;
    primed = true;
    return true;
}

void HotkeyInstallGate::reset()
{
    lastDesired.clear();
    primed = false;
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
