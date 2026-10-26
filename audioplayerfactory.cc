/* This file is (c) 2018 Igor Kushnir <igorkuo@gmail.com>
 * Part of GoldenDict. Licensed under GPLv3 or later, see the LICENSE file */

#include <QScopedPointer>
#include <QObject>
#include "audioplayerfactory.hh"
#include "ffmpegaudioplayer.hh"
#include "externalaudioplayer.hh"
#include "gddebug.hh"

AudioPlayerFactory::AudioPlayerFactory( Config::Preferences const & p ) :
  useInternalPlayer( p.useInternalPlayer ),
  internalPlayerBackend( p.internalPlayerBackend ),
  audioPlaybackProgram( p.audioPlaybackProgram )
{
  reset();
}

void AudioPlayerFactory::setPreferences( Config::Preferences const & p )
{
  if( p.useInternalPlayer != useInternalPlayer )
  {
    useInternalPlayer = p.useInternalPlayer;
    internalPlayerBackend = p.internalPlayerBackend;
    audioPlaybackProgram = p.audioPlaybackProgram;
    reset();
  }
  else
  if( useInternalPlayer && p.internalPlayerBackend != internalPlayerBackend )
  {
    internalPlayerBackend = p.internalPlayerBackend;
    reset();
  }
  else
  if( !useInternalPlayer && p.audioPlaybackProgram != audioPlaybackProgram )
  {
    audioPlaybackProgram = p.audioPlaybackProgram;
    ExternalAudioPlayer * const externalPlayer =
        qobject_cast< ExternalAudioPlayer * >( playerPtr.data() );
    if( externalPlayer )
      setAudioPlaybackProgram( *externalPlayer );
    else
      gdWarning( "External player was expected, but it does not exist.\n" );
  }
}

void AudioPlayerFactory::reset()
{
  // The internal player has one implementation left: ffmpeg. A build without it
  // (no_ffmpeg_player) has no internal player at all; load() has already cleared
  // the runtime useInternalPlayer flag for that build, and if a caller still asks
  // for the internal player it falls through to the external one, as before.
  if( useInternalPlayer )
  {
#ifdef MAKE_FFMPEG_PLAYER
    Q_ASSERT( Config::InternalPlayerBackend::defaultBackend().isFfmpeg()
              && "Adjust the code below after changing the default backend." );

    if( !playerPtr || !qobject_cast< Ffmpeg::AudioPlayer * >( playerPtr.data() ) )
      playerPtr.reset( new Ffmpeg::AudioPlayer );
    return;
#endif
  }

  QScopedPointer< ExternalAudioPlayer > externalPlayer( new ExternalAudioPlayer );
  setAudioPlaybackProgram( *externalPlayer );
  playerPtr.reset( externalPlayer.take() );
}

void AudioPlayerFactory::setAudioPlaybackProgram( ExternalAudioPlayer & externalPlayer )
{
  externalPlayer.setPlayerCommandLine( audioPlaybackProgram.trimmed() );
}
