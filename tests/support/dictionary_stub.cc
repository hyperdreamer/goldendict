/* This file is part of GoldenDict. Licensed under GPLv3 or later, see the
 * LICENSE file.
 *
 * Test-only dictionary fixtures, see tests/support/dictionary_stub.hh.
 */

#include "dictionary_stub.hh"

#include <QThread>

#include <algorithm>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <string>

namespace TestSupport {

namespace {

std::runtime_error makeRuntimeError( QString const & message )
{
    const QByteArray utf8 = message.toUtf8();

    return std::runtime_error( std::string( utf8.constData(), static_cast< size_t >( utf8.size() ) ) );
}

} // namespace

////////// ControlledDataRequest

ControlledDataRequest::ControlledDataRequest():
    probe( std::make_shared< ReadProbeState >() ),
    lifetime( std::make_shared< char >( 0 ) )
{
}

ControlledDataRequest::ControlledDataRequest( QByteArray const & bytes ):
    ControlledDataRequest()
{
    // Instant completion, exactly like Dictionary::DataRequestInstant: the
    // producer state is final and finished() has already been emitted when the
    // constructor returns, so a consumer that connects afterwards must recheck
    // isFinished() -- the contract the scheme handler is pinned to.
    append( bytes );
    finishSuccess();
}

ControlledDataRequest::~ControlledDataRequest() = default;

void ControlledDataRequest::append( QByteArray const & bytes )
{
    {
        std::lock_guard< std::mutex > lock( producerMutex );

        if ( state != Completion::Pending )
            return;

        Mutex::Lock _( dataMutex );

        if ( !bytes.isEmpty() )
            data.insert( data.end(), bytes.constData(), bytes.constData() + bytes.size() );

        // A produced byte means the resource exists; before that dataSize() has
        // to stay -1 so "unknown" and "empty" remain distinguishable.
        hasAnyData = true;
    }

    update();
}

void ControlledDataRequest::finishSuccess()
{
    terminate( Completion::Success, QString() );
}

void ControlledDataRequest::finishNotFound()
{
    terminate( Completion::NotFound, QString() );
}

void ControlledDataRequest::finishError( QString const & message )
{
    terminate( Completion::Error, message );
}

void ControlledDataRequest::terminate( Completion target, QString const & message )
{
    {
        std::lock_guard< std::mutex > lock( producerMutex );

        if ( state != Completion::Pending )
            return;

        state = target;
        error = message;
        ++terminals;

        {
            Mutex::Lock _( dataMutex );

            // A successful completion publishes existence even with zero bytes;
            // not-found and error leave dataSize() at -1 (not-found) or keep
            // whatever partial data was produced (error wins over partial bytes
            // in the production handler).
            if ( target == Completion::Success )
                hasAnyData = true;
        }
    }

    if ( !message.isNull() )
        setErrorString( message );

    // finish() outside every fixture lock: its slot chain is foreign code.
    finish();
}

void ControlledDataRequest::cancel()
{
    {
        std::lock_guard< std::mutex > lock( producerMutex );

        ++cancels;

        if ( state != Completion::Pending )
            return;

        // A cancelled request must still finish: the Request contract requires
        // at least one finish() during/after cancel().
        state = Completion::Success;
        ++terminals;
    }

    finish();
}

void ControlledDataRequest::setFullDataException( QString const & message )
{
    std::lock_guard< std::mutex > lock( probe->mutex );

    ReadProbeState::Operation & operation = probe->operation( ReadOperation::FullData );
    operation = ReadProbeState::Operation();
    operation.armedMessage = message;
}

void ControlledDataRequest::setSliceException( QString const & message )
{
    std::lock_guard< std::mutex > lock( probe->mutex );

    ReadProbeState::Operation & operation = probe->operation( ReadOperation::Slice );
    operation = ReadProbeState::Operation();
    operation.armedMessage = message;
}

void ControlledDataRequest::setStreamCopyException( QString const & message )
{
    std::lock_guard< std::mutex > lock( probe->mutex );

    ReadProbeState::Operation & operation = probe->operation( ReadOperation::StreamCopy );
    operation = ReadProbeState::Operation();
    operation.armedMessage = message;
}

ReadProbeSnapshot ControlledDataRequest::readProbe( ReadOperation which ) const
{
    std::lock_guard< std::mutex > lock( probe->mutex );

    ReadProbeState::Operation const & operation = probe->operation( which );

    ReadProbeSnapshot snapshot;
    snapshot.entered = operation.entered;
    snapshot.thrown = operation.thrown;
    snapshot.returned = operation.returned;
    snapshot.callingThreads = operation.callingThreads;

    return snapshot;
}

std::shared_ptr< ReadProbeState > ControlledDataRequest::probeState() const
{
    return probe;
}

std::weak_ptr< void const > ControlledDataRequest::lifetimeToken() const
{
    return lifetime;
}

QByteArray ControlledDataRequest::producedBytes()
{
    Mutex::Lock _( dataMutex );

    if ( data.empty() )
        return QByteArray();

    return QByteArray( data.data(), static_cast< qsizetype >( data.size() ) );
}

int ControlledDataRequest::terminalEmissions() const
{
    std::lock_guard< std::mutex > lock( producerMutex );

    return terminals;
}

int ControlledDataRequest::cancelCount() const
{
    std::lock_guard< std::mutex > lock( producerMutex );

    return cancels;
}

ControlledDataRequest::Completion ControlledDataRequest::completion() const
{
    std::lock_guard< std::mutex > lock( producerMutex );

    return state;
}

QString ControlledDataRequest::errorMessage() const
{
    std::lock_guard< std::mutex > lock( producerMutex );

    return error;
}

bool ControlledDataRequest::isTerminal()
{
    return isFinished();
}

////////// ThrowingDataRequestReader

ThrowingDataRequestReader::ThrowingDataRequestReader( std::shared_ptr< ReadProbeState > state_,
                                                      std::shared_ptr< DataRequestReader const > delegate_ ):
    state( std::move( state_ ) ),
    delegate( std::move( delegate_ ) )
{
}

std::optional< QString > ThrowingDataRequestReader::enter( ReadOperation operation ) const
{
    std::lock_guard< std::mutex > lock( state->mutex );

    ReadProbeState::Operation & record = state->operation( operation );
    ++record.entered;
    record.callingThreads.insert( QThread::currentThreadId() );

    if ( record.armedMessage.isEmpty() )
        return std::nullopt;

    // One-shot: the next matching operation forwards normally again.
    QString const message = record.armedMessage;
    record.armedMessage.clear();

    return message;
}

void ThrowingDataRequestReader::recordThrow( ReadOperation operation ) const
{
    std::lock_guard< std::mutex > lock( state->mutex );

    ++state->operation( operation ).thrown;
}

void ThrowingDataRequestReader::recordReturn( ReadOperation operation ) const
{
    std::lock_guard< std::mutex > lock( state->mutex );

    ++state->operation( operation ).returned;
}

QByteArray ThrowingDataRequestReader::fullData( Dictionary::DataRequest & request ) const
{
    if ( auto const armed = enter( ReadOperation::FullData ) )
    {
        recordThrow( ReadOperation::FullData );
        throw makeRuntimeError( *armed );
    }

    QByteArray bytes = delegate->fullData( request );

    recordReturn( ReadOperation::FullData );

    return bytes;
}

void ThrowingDataRequestReader::dataSlice( Dictionary::DataRequest & request, size_t offset,
                                           size_t size, void * destination ) const
{
    if ( auto const armed = enter( ReadOperation::Slice ) )
    {
        recordThrow( ReadOperation::Slice );
        throw makeRuntimeError( *armed );
    }

    delegate->dataSlice( request, offset, size, destination );

    recordReturn( ReadOperation::Slice );
}

void ThrowingDataRequestReader::copyStreamBytes( char * destination, char const * source,
                                                 qint64 count ) const
{
    // Only the shared probe state and the supplied span are consulted: no
    // producer, no handler, no job. This is the operation that runs on the
    // engine's IO thread.
    if ( auto const armed = enter( ReadOperation::StreamCopy ) )
    {
        recordThrow( ReadOperation::StreamCopy );
        throw makeRuntimeError( *armed );
    }

    delegate->copyStreamBytes( destination, source, count );

    recordReturn( ReadOperation::StreamCopy );
}

std::shared_ptr< ReadProbeState > ThrowingDataRequestReader::probeState() const
{
    return state;
}

DataRequestReaderFactory controlledReaderFactory()
{
    return []( sptr< Dictionary::DataRequest > const & request )
               -> std::shared_ptr< DataRequestReader const > {
        // The production reader is looked up for every job, exactly as the
        // application's default factory does, so a suite can never pass by
        // substituting a test implementation for the real one.
        std::shared_ptr< DataRequestReader const > production = makeDataRequestReader();

        if ( auto * controlled = dynamic_cast< ControlledDataRequest * >( request.get() ) )
        {
            // Copy the shared probe state now; the reader never keeps the
            // producer (or its sptr) alive.
            return std::make_shared< ThrowingDataRequestReader >( controlled->probeState(),
                                                                 production );
        }

        return production;
    };
}

////////// FixtureDictionary

FixtureDictionary::FixtureDictionary( std::string const & id_, QString const & displayName ):
    Dictionary::Class( id_, std::vector< std::string >() ),
    name( displayName.toStdString() ),
    articleBody( "<p id=\"fixture-article\">fixture article body</p>" )
{
    dictionaryDescription = displayName;
}

FixtureDictionary::~FixtureDictionary() = default;

void FixtureDictionary::setArticleBody( QByteArray const & body )
{
    std::lock_guard< std::mutex > lock( observationMutex );

    articleBody = body;
}

void FixtureDictionary::setResources( QMap< QString, QByteArray > const & configured )
{
    std::lock_guard< std::mutex > lock( observationMutex );

    resources = configured;
}

void FixtureDictionary::setArticlePending( bool pending )
{
    std::lock_guard< std::mutex > lock( observationMutex );

    articlePending = pending;
}

void FixtureDictionary::setResourcePending( bool pending )
{
    std::lock_guard< std::mutex > lock( observationMutex );

    resourcePending = pending;
}

std::string FixtureDictionary::getName() noexcept
{
    return name;
}

std::map< Dictionary::Property, std::string > FixtureDictionary::getProperties() noexcept
{
    return { { Dictionary::Author, "GoldenDict test fixtures" },
             { Dictionary::Description, "Deterministic dictionary double" } };
}

unsigned long FixtureDictionary::getArticleCount() noexcept
{
    return 1;
}

unsigned long FixtureDictionary::getWordCount() noexcept
{
    return 1;
}

sptr< Dictionary::WordSearchRequest > FixtureDictionary::prefixMatch( gd::wstring const &,
                                                                      unsigned long )
{
    return new Dictionary::WordSearchRequestInstant();
}

sptr< Dictionary::DataRequest > FixtureDictionary::getArticle(
    gd::wstring const & word, std::vector< gd::wstring > const & alternates,
    gd::wstring const & context, bool ignoreDiacritics )
{
    sptr< ControlledDataRequest > request;

    {
        std::lock_guard< std::mutex > lock( observationMutex );

        ++articles;
        lookup.word = word;
        lookup.alternates = alternates;
        lookup.context = context;
        lookup.ignoreDiacritics = ignoreDiacritics;

        if ( articlePending )
            request = new ControlledDataRequest();
        else
            request = new ControlledDataRequest( articleBody );

        articleRequest = request;
    }

    return request;
}

sptr< Dictionary::DataRequest > FixtureDictionary::getResource( std::string const & name_ )
{
    sptr< ControlledDataRequest > request;

    {
        std::lock_guard< std::mutex > lock( observationMutex );

        const QString requested = QString::fromUtf8( name_.data(), static_cast< int >( name_.size() ) );
        resourceNames.append( requested );

        if ( resourcePending )
            request = new ControlledDataRequest();
        else if ( resources.contains( requested ) )
            request = new ControlledDataRequest( resources.value( requested ) );
        else
        {
            // A missing resource finishes as the production backends do:
            // finished, dataSize() == -1, no error string.
            request = new ControlledDataRequest();
            request->finishNotFound();
        }

        resourceRequest = request;
    }

    return request;
}

sptr< Dictionary::DataRequest > FixtureDictionary::getSearchResults( QString const &, int, bool, int,
                                                                     int, bool, bool, QThreadPool * )
{
    {
        std::lock_guard< std::mutex > lock( observationMutex );
        ++searches;
    }

    return new Dictionary::DataRequestInstant( false );
}

int FixtureDictionary::articleRequestCount() const
{
    std::lock_guard< std::mutex > lock( observationMutex );

    return articles;
}

FixtureDictionary::Lookup FixtureDictionary::lastLookup() const
{
    std::lock_guard< std::mutex > lock( observationMutex );

    return lookup;
}

QStringList FixtureDictionary::requestedResources() const
{
    std::lock_guard< std::mutex > lock( observationMutex );

    return resourceNames;
}

int FixtureDictionary::searchRequestCount() const
{
    std::lock_guard< std::mutex > lock( observationMutex );

    return searches;
}

sptr< ControlledDataRequest > FixtureDictionary::lastArticleRequest() const
{
    std::lock_guard< std::mutex > lock( observationMutex );

    return articleRequest;
}

sptr< ControlledDataRequest > FixtureDictionary::lastResourceRequest() const
{
    std::lock_guard< std::mutex > lock( observationMutex );

    return resourceRequest;
}

void FixtureDictionary::clearObservations()
{
    std::lock_guard< std::mutex > lock( observationMutex );

    articles = 0;
    searches = 0;
    lookup = Lookup();
    resourceNames.clear();
    articleRequest.reset();
    resourceRequest.reset();
}

} // namespace TestSupport
