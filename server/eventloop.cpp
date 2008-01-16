// Copyright Oryx Mail Systems GmbH. All enquiries to info@oryx.com, please.

#include "eventloop.h"

#include "allocator.h"
#include "connection.h"
#include "buffer.h"
#include "string.h"
#include "server.h"
#include "scope.h"
#include "timer.h"
#include "graph.h"
#include "list.h"
#include "log.h"
#include "sys.h"

// time
#include <time.h>
// errno
#include <errno.h>
// struct timeval, fd_set
#include <sys/time.h>
#include <sys/types.h>
#include <sys/select.h>
// getsockopt, SOL_SOCKET, SO_ERROR
#include <sys/socket.h>
// read, select
#include <unistd.h>


static EventLoop * loop;


class LoopData
    : public Garbage
{
public:
    LoopData()
        : log( new Log( Log::Server ) ), startup( false ),
          stop( false )
    {}

    Log *log;
    bool startup;
    bool stop;
    List< Connection > connections;
    List< Timer > timers;
};


/*! \class EventLoop eventloop.h
    This class dispatches event notifications to a list of Connections.

    An EventLoop maintains a list of participating Connection objects,
    and periodically informs them about any events (e.g., read/write,
    errors, timeouts) that occur. The loop continues until something
    calls stop().
*/


/*! Creates the global EventLoop object or, if \a l is non-zero, sets
    the global EventLoop to \a l. This function expects to be called
    very early during the startup sequence.
*/

void EventLoop::setup( EventLoop * l )
{
    ::loop = l;
    if ( !l )
        ::loop = new EventLoop;
    Allocator::addEternal( ::loop, "global event loop" );
}


/*! Creates a new EventLoop. */

EventLoop::EventLoop()
    : d( new LoopData )
{
}


/*! Exists only to avoid compiler warnings. */

EventLoop::~EventLoop()
{
}


/*! Adds \a c to this EventLoop's list of active Connections.

    If shutdown() has been called already, addConnection() ignores \a
    c, so that shutdown proceeds unhampered. This is likely to disturb
    \a c a little, but it's better than the alternative: Aborting the
    shutdown.
*/

void EventLoop::addConnection( Connection * c )
{
    if ( d->stop ) {
        log( "Cannot add new Connection objects during shutdown",
             Log::Error );
        return;
    }

    Scope x( d->log );

    if ( d->connections.find( c ) )
        return;

    d->connections.prepend( c );
    if ( c->type() != Connection::LogClient )
        log( "Added " + c->description(), Log::Debug );
    setConnectionCounts();
}


/*! Removes \a c from this EventLoop's list of active
    Connections.
*/

void EventLoop::removeConnection( Connection *c )
{
    Scope x( d->log );

    if ( d->connections.remove( c ) == 0 )
        return;

    if ( c->type() != Connection::LogClient )
        log( "Removed " + c->description(), Log::Debug );
    setConnectionCounts();
}


/*! Returns a (non-zero) pointer to the list of Connections that have
    been added to this EventLoop.
*/

List< Connection > *EventLoop::connections() const
{
    return &d->connections;
}


static GraphableNumber * sizeinram = 0;


/*! Starts the EventLoop and runs it until stop() is called. */

void EventLoop::start()
{
    Scope x( d->log );
    time_t gc = time(0);

    log( "Starting event loop", Log::Debug );

    while ( !d->stop ) {
        Connection * c;

        uint timeout = INT_MAX;
        int maxfd = -1;

        fd_set r, w;
        FD_ZERO( &r );
        FD_ZERO( &w );

        // Figure out what events each connection wants.

        List< Connection >::Iterator it( d->connections );
        while ( it ) {
            c = it;
            ++it;

            if ( c->active() &&
                 !( inStartup() && c->type() == Connection::Listener ) )
            {
                int fd = c->fd();
                if ( fd > maxfd )
                    maxfd = fd;
                if ( c->canRead() && c->state() != Connection::Closing )
                    FD_SET( fd, &r );
                if ( c->canWrite() ||
                     c->state() == Connection::Connecting ||
                     c->state() == Connection::Closing )
                    FD_SET( fd, &w );
                if ( c->timeout() > 0 && c->timeout() < timeout )
                    timeout = c->timeout();
            }

        }

        // Figure out whether any timers need attention soon

        List< Timer >::Iterator t( d->timers );
        while ( t ) {
            if ( t->active() && t->timeout() < timeout )
                timeout = t->timeout();
            ++t;
        }

        // Look for interesting input

        struct timeval tv;
        tv.tv_sec = timeout - time( 0 );
        tv.tv_usec = 0;

        if ( tv.tv_sec < 0 )
            tv.tv_sec = 0;
        if ( tv.tv_sec > 60 )
            tv.tv_sec = 60;

        int n = select( maxfd+1, &r, &w, 0, &tv );
        time_t now = time( 0 );

        // Graph our size before processing events
        if ( !sizeinram )
            sizeinram = new GraphableNumber( "memory-used" );
        sizeinram->setValue( Allocator::inUse() + Allocator::allocated() );

        // Find out when we stop working, ie. when we no longer
        // allocate RAM during even processing and have freed what we
        // could. We want to run GC as soon as that happens.
        uint alloc = Allocator::allocated();
        if ( alloc > 16384 && tv.tv_sec > 1 )
            tv.tv_sec = 3;

        if ( n < 0 ) {
            if ( errno == EINTR ) {
                // We should see this only for signals we've handled,
                // and we don't need to do anything further.
            }
            else if ( errno == EBADF ) {
                // one of the FDs was closed. we react by forgetting
                // that connection, letting the rest of the server go
                // on.
                List< Connection >::Iterator it( d->connections );
                while ( it ) {
                    Connection * c = it;
                    ++it;
                    // we check the window size for each socket to see
                    // which ones are bad.
                    int dummy;
                    if ( ::setsockopt( c->fd(), SOL_SOCKET, SO_RCVBUF,
                                       (char*)&dummy, sizeof(dummy) ) < 0 ) {
                        if ( c->state() == Connection::Closing ) {
                            // if a socket is closed by the peer while
                            // we're trying to close it, we smile and
                            // go on our way.
                        }
                        else {
                            c->log( "Socket " + fn( c->fd() ) +
                                    " was unexpectedly closed: "
                                    "Removing corresponding connection: " +
                                    c->description(), Log::Error );
                            c->log( "Please notify info@oryx.com about what "
                                    "happened with this connection" );
                        }
                        removeConnection( c );
                    }
                }
            }
            else {
                log( Server::name() + ": select() returned errno " +
                     fn( errno ),
                     Log::Disaster );
                return;
            }
        }

        // Collect garbage if a) we've allocated something, but the
        // last event loop iteration didn't allocate any more, b)
        // we've increased our memory use by both 20% and 8MB since
        // the last GC or c) we've allocated at least 128KB and
        // haven't collected garbage in the past minute.

        if ( !d->stop &&
             ( ( alloc && Allocator::allocated() == alloc ) ||
               ( Allocator::allocated() > 8*1024*1024 &&
                 Allocator::allocated() * 5 > Allocator::inUse() ) ||
               ( now - gc > 60 && Allocator::allocated() >= 131072 ) ) )
        {
            Allocator::free();
            gc = time( 0 );
        }

        // Graph our size after processing all the events too

        sizeinram->setValue( Allocator::inUse() + Allocator::allocated() );

        // Any interesting timers?

        if ( !d->timers.isEmpty() ) {
            uint now = time( 0 );
            t = d->timers.first();
            while ( t ) {
                Timer * tmp = t;
                ++t;
                if ( tmp->active() && tmp->timeout() <= now )
                    tmp->execute();
            }
        }

        // Figure out what each connection cares about.

        it = d->connections.first();
        while ( it ) {
            c = it;
            ++it;
            int fd = c->fd();
            if ( fd >= 0 ) {
                dispatch( c, FD_ISSET( fd, &r ), FD_ISSET( fd, &w ), now );
                FD_CLR( fd, &r );
                FD_CLR( fd, &w );
            }
            else {
                removeConnection( c );
            }
        }
    }

    // This is for event loop shutdown. A little brutal. Proper
    // shutdown should first get rid of listeners, then a (long)
    // while later call this.
    log( "Shutting down event loop", Log::Debug );
    List< Connection >::Iterator it( d->connections );
    while ( it ) {
        Connection * c = it;
        ++it;
        try {
            Scope x( c->log() );
            if ( c->state() == Connection::Connected )
                c->react( Connection::Shutdown );
            if ( c->state() == Connection::Connected )
                c->write();
        } catch ( Exception e ) {
            // we don't really care at this point, do we?
        }
    }

    log( "Event loop stopped", Log::Debug );
}


/*! Dispatches events to the connection \a c, based on its current
    state, the time \a now and the results from select: \a r is true
    if the FD may be read, and \a w is true if we know that the FD may
    be written to. If \a now is past that Connection's timeout, we
    must sent a Timeout event.
*/

void EventLoop::dispatch( Connection *c, bool r, bool w, uint now )
{
    try {
        Scope x( c->log() );
        if ( c->timeout() != 0 && now >= c->timeout() ) {
            c->setTimeout( 0 );
            c->react( Connection::Timeout );
            w = true;
        }

        if ( c->state() == Connection::Connecting ) {
            bool error = false;
            bool connected = false;

            if ( ( w && !r ) || c->isPending( Connection::Connect ) ) {
                connected = true;
            }
            else if ( c->isPending( Connection::Error ) ) {
                error = true;
            }
            else if ( w && r ) {
                // This might indicate a connection error, or a successful
                // connection with outstanding data. (Stevens suggests the
                // getsockopt to disambiguate the two, cf. UNPv1 15.4.)
                int errval;
                int errlen = sizeof( int );
                ::getsockopt( c->fd(), SOL_SOCKET, SO_ERROR, (void *)&errval,
                              (socklen_t *)&errlen );

                if ( errval == 0 )
                    connected = true;
                else
                    error = true;
            }

            if ( connected ) {
                c->setState( Connection::Connected );
                c->react( Connection::Connect );
                w = true;
            }
            else if ( error ) {
                c->react( Connection::Error );
                c->setState( Connection::Closing );
                w = r = false;
            }
        }

        if ( r ) {
            c->read();
            c->react( Connection::Read );

            if ( !c->canRead() ) {
                c->setState( Connection::Closing );
                c->react( Connection::Close );
            }

            w = true;
        }

        if ( w ) {
            c->write();
            if ( c->writeBuffer()->error() != 0 ) {
                c->setState( Connection::Closing );
                c->react( Connection::Close );
            }
        }
    }
    catch ( Exception e ) {
        String s;
        switch (e) {
        case Range:
            s = "Out-of-range memory access";
            break;
        case Memory:
            s = "Out of memory";
            break;
        case FD:
            s = "FD error";
            break;
        };
        s.append( " while processing " + c->description() );
        d->log->log( s, Log::Error );
        c->close();
    }

    if ( c->state() == Connection::Closing && !c->canWrite() )
        c->close();
    if ( !c->valid() )
        removeConnection( c );
}


/*! Instructs this EventLoop to perform an orderly shutdown, by sending
    each participating Connection a Shutdown event before closing, and
    then deleting each one.
*/

void EventLoop::stop()
{
    d->stop = true;
}


/*! Closes all Connections except \a c1 and \a c2. This helps TlsProxy
    do its work.
*/

void EventLoop::closeAllExcept( Connection * c1, Connection * c2 )
{
    List< Connection >::Iterator it( d->connections );
    while ( it ) {
        Connection * c = it;
        ++it;
        if ( c != c1 && c != c2 ) {
            removeConnection( c );
            c->close();
        }
    }
}


/*! Closes all Connection except Listeners. When we fork, this allows
    us to keep the connections on one side of the fence.
*/

void EventLoop::closeAllExceptListeners()
{
    List< Connection >::Iterator it( d->connections );
    while ( it ) {
        Connection * c = it;
        ++it;
        if ( c->type() != Connection::Listener ) {
            removeConnection( c );
            c->close();
        }
    }
}


/*! Flushes the write buffer of all connections. */

void EventLoop::flushAll()
{
    List< Connection >::Iterator it( d->connections );
    while ( it ) {
        Connection * c = it;
        ++it;
        c->write();
    }
}


/*! Returns true if this EventLoop is still attending to startup chores,
    and not yet processing Listener requests.
*/

bool EventLoop::inStartup() const
{
    return d->startup;
}


/*! Sets the startup state of this EventLoop to \a p. If \a p is true,
    then Listeners will not be processed until this function is called
    again with \a p set to false.
*/

void EventLoop::setStartup( bool p )
{
    d->startup = p;
}


/*! Returns true if this EventLoop is shutting down (ie. stop() has
    been called), and false if it's starting up or operating normally.
*/

bool EventLoop::inShutdown() const
{
    return d->stop;
}


/*! Returns a pointer to the global event loop, or 0 if setup() has not
    yet been called.
*/

EventLoop * EventLoop::global()
{
    return ::loop;
}


/*! This static function is just a convenient shorthand for calling
    stop() on the global() EventLoop.
*/

void EventLoop::shutdown()
{
    ::loop->stop();
}


/*! Records that \a t exists, so that the event loop will process \a
    t.
*/

void EventLoop::addTimer( Timer * t )
{
    d->timers.append( t );
}


/*! Forgets that \a t exists. The event loop will henceforth never
    call \a t.
*/

void EventLoop::removeTimer( Timer * t )
{
    List<Timer>::Iterator i( d->timers.find( t ) );
    if ( i )
        d->timers.take( i );
}

static GraphableNumber * imapgraph = 0;
static GraphableNumber * pop3graph = 0;
static GraphableNumber * smtpgraph = 0;
static GraphableNumber * othergraph = 0;
static GraphableNumber * internalgraph = 0;
static GraphableNumber * httpgraph = 0;
static GraphableNumber * dbgraph = 0;



/*! Scans the event loop and stores the current number of different
    connections using GraphableNumber.
*/

void EventLoop::setConnectionCounts()
{
    uint imap = 0;
    uint pop3 = 0;
    uint smtp = 0;
    uint other = 0;
    uint internal = 0;
    uint http = 0;
    uint db = 0;
    bool listeners = false;
    List<Connection>::Iterator c( d->connections );
    while ( c ) {
        switch( c->type() ) {
        case Connection::Client:
        case Connection::LogServer:
        case Connection::OryxServer:
        case Connection::OryxClient:
        case Connection::OryxConsole:
        case Connection::LogClient:
        case Connection::TlsProxy:
        case Connection::TlsClient:
        case Connection::RecorderClient:
        case Connection::RecorderServer:
        case Connection::Pipe:
            internal++;
            break;
        case Connection::DatabaseClient:
            db++;
            break;
        case Connection::ImapServer:
            imap++;
            break;
        case Connection::SmtpServer:
            smtp++;
            break;
        case Connection::SmtpClient:
        case Connection::ManageSieveServer:
        case Connection::EGDServer:
            other++;
            break;
        case Connection::Pop3Server:
            pop3++;
            break;
        case Connection::HttpServer:
            http++;
            break;
        case Connection::Listener:
            listeners = true;
            // we don't count these, we only count connections
            break;
        }
        ++c;
    }
    if ( !listeners )
        return;
    if ( !imapgraph ) {
        imapgraph = new GraphableNumber( "imap-connections" );
        pop3graph = new GraphableNumber( "pop3-connections" );
        smtpgraph = new GraphableNumber( "smtp-connections" );
        othergraph = new GraphableNumber( "other-connections" );
        internalgraph = new GraphableNumber( "internal-connections" );
        httpgraph = new GraphableNumber( "http-connections" );
        dbgraph = new GraphableNumber( "db-connections" );
    }
    imapgraph->setValue( imap );
    pop3graph->setValue( pop3 );
    smtpgraph->setValue( smtp );
    othergraph->setValue( other );
    internalgraph->setValue( internal );
    httpgraph->setValue( http );
    dbgraph->setValue( db );
}
