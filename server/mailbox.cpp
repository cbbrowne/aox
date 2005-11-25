// Copyright Oryx Mail Systems GmbH. All enquiries to info@oryx.com, please.

#include "mailbox.h"

#include "log.h"
#include "map.h"
#include "dict.h"
#include "user.h"
#include "query.h"
#include "scope.h"
#include "event.h"
#include "string.h"
#include "message.h"
#include "fetcher.h"
#include "allocator.h"
#include "stringlist.h"
#include "transaction.h"


class MailboxData
    : public Garbage
{
public:
    MailboxData()
        : type( Mailbox::Synthetic ), id( 0 ),
          uidnext( 0 ), uidvalidity( 0 ), owner( 0 ),
          parent( 0 ), children( 0 ), messages( 0 ),
          flagFetcher( 0 ), headerFetcher( 0 ),
          triviaFetcher( 0 ), bodyFetcher( 0 ),
          annotationFetcher( 0 ),
          watchers( 0 )
    {}

    String name;

    Mailbox::Type type;

    uint id;
    uint uidnext;
    uint uidvalidity;
    uint owner;

    Mailbox * parent;
    List< Mailbox > * children;
    Map<Message> * messages;
    Fetcher * flagFetcher;
    Fetcher * headerFetcher;
    Fetcher * triviaFetcher;
    Fetcher * bodyFetcher;
    Fetcher * annotationFetcher;
    List<EventHandler> * watchers;
};


static Mailbox * root = 0;
static Map<Mailbox> * mailboxes = 0;


/*! \class Mailbox mailbox.h
    This class represents a node in the global mailbox hierarchy.

    Every Mailbox has a unique name() within the hierarchy. Any
    Mailbox that can contain messages has a non-zero numeric id() and
    attributes like uidvalidity() and uidnext(). Mailboxes have a
    parent() and may have a number of children().

    Some mailboxes aren't quite real. A Mailbox can be deleted(), in
    which case it can contain no messags. If recreated, a deleted()
    mailbox preserves its uidvalidity() and uid series. It can also be
    synthetic(), meaning that it exists only in RAM, not in the database.

    This class maintains a tree of mailboxes, based on the contents of
    the mailboxes table and descriptive messages from the OCServer. It
    can find() a named mailbox in this hierarchy. Synthetic messages
    are internal nodes in the tree, necessary to connect the root to
    the leaves.
*/


class MailboxReader
    : public EventHandler
{
public:
    EventHandler * owner;
    Query * query;

    MailboxReader( EventHandler * ev )
        : owner( ev ), query( 0 )
    {
        query =
            new Query( "select m.*,v.id as view from "
                       "mailboxes m left join views v on (m.id=v.view)",
                       this );
    }


    MailboxReader( const String & n )
        : owner( 0 ), query( 0 )
    {
        query =
            new Query( "select m.*,v.id as view from "
                       "mailboxes m left join views v on (m.id=v.view) "
                       "where name=$1", this );
        query->bind( 1, n );
    }


    void execute() {
        while ( query->hasResults() ) {
            Row * r = query->nextRow();

            String n = r->getString( "name" );
            Mailbox * m = Mailbox::obtain( n );
            if ( n != m->d->name )
                m->d->name = n;
            m->setId( r->getInt( "id" ) );

            if ( r->getBoolean( "deleted" ) )
                m->setType( Mailbox::Deleted );
            else if ( r->isNull( "view" ) )
                m->setType( Mailbox::Ordinary );
            else
                m->setType( Mailbox::View );

            m->d->uidvalidity = r->getInt( "uidvalidity" );
            m->setUidnext( r->getInt( "uidnext" ) );
            if ( !r->isNull( "owner" ) )
                m->setOwner( r->getInt( "owner" ) );

            if ( m->d->id )
                ::mailboxes->insert( m->d->id, m );
        }

        if ( owner ) {
            if ( query->failed() )
                log( "Couldn't create mailbox tree: " + query->error(),
                     Log::Disaster );
            owner->execute();
        }
    }
};


/*! This static function is responsible for building a tree of
    Mailboxes from the contents of the mailboxes table. It expects to
    be called by ::main().

    If \a owner is non-null (the default is null), this function calls
    EventHandler::waitFor() on \a owner to wait for setup to complete.
*/

void Mailbox::setup( EventHandler * owner )
{
    ::root = new Mailbox( "/" );
    Allocator::addEternal( ::root, "root mailbox" );

    ::mailboxes = new Map<Mailbox>;
    Allocator::addEternal( ::mailboxes, "mailbox tree" );

    MailboxReader * mr = new MailboxReader( owner );
    if ( owner )
        owner->waitFor( mr->query );
    mr->query->execute();
}


/*! This function reloads this mailbox from the database.
    (This is still a hack.)
*/

void Mailbox::refresh()
{
    MailboxReader * mr = new MailboxReader( name() );
    mr->query->execute();
}


/*! Creates a Mailbox named \a name. This constructor is only meant to
    be used via Mailbox::obtain(). */

Mailbox::Mailbox( const String &name )
    : d( new MailboxData )
{
    d->name = name;
}


/*! Returns the fully qualified name of this Mailbox. */

String Mailbox::name() const
{
    return d->name;
}


/*! Sets the type of this Mailbox to \a t. The initial value is
    Synthetic (because it has to be something).
*/

void Mailbox::setType( Type t )
{
    d->type = t;
}


/*! Returns the type of this Mailbox. May be Synthetic, Ordinary,
    Deleted, or View.
*/

Mailbox::Type Mailbox::type() const
{
    return d->type;
}


/*! Returns the database ID of this Mailbox, or 0 if this Mailbox is
    synthetic().
*/

uint Mailbox::id() const
{
    return d->id;
}


/*! Notifies this Mailbox that its database ID is \a i. If \a i is 0,
    the Mailbox is synthetic.
*/

void Mailbox::setId( uint i ) const
{
    d->id = i;
}


/*! Returns the next UID value that will be used for this mailbox. */

uint Mailbox::uidnext() const
{
    return d->uidnext;
}


/*! Notifies this Mailbox that its correct uidvalidity is \a i. Should
    generally not be called.
*/

void Mailbox::setUidvalidity( uint i )
{
    d->uidvalidity = i;
}


/*! Returns the UIDVALIDITY value of this Mailbox. This never changes. */

uint Mailbox::uidvalidity() const
{
    return d->uidvalidity;
}


/*! Returns true if this Mailbox has been synthesized in-RAM in order
    to fully connect the mailbox tree, and false if the Mailbox exists
    in the database.
*/

bool Mailbox::synthetic() const
{
    return d->type == Synthetic;
}


/*! Returns true if this mailbox isn't "special". */

bool Mailbox::ordinary() const
{
    return d->type == Ordinary;
}


/*! Returns true if this mailbox is currently deleted. */

bool Mailbox::deleted() const
{
    return d->type == Deleted;
}


/*! Returns true if this mailbox is really a view. */

bool Mailbox::view() const
{
    return d->type == Ordinary;
}


/*! Returns true if this Mailbox represents a user's "home directory",
    e.g. /users/ams. (This is currently determined only by looking at
    the mailbox name, but it should be based on a flag that is set by
    the tree builder.)
*/

bool Mailbox::isHome() const
{
    if ( d->name.startsWith( "/users/" ) &&
         d->name.find( '/', 7 ) == -1 )
        return true;
    return false;
}


/*! Returns the numeric user id of the owner of this mailbox, or 0 if
    the mailbox has no defined owner (or is not yet known to have one).
*/

uint Mailbox::owner() const
{
    return d->owner;
}


/*! Returns a pointer to the parent of this Mailbox, or 0 if it is the
    root Mailbox.
*/

Mailbox *Mailbox::parent() const
{
    return d->parent;
}


/*! Returns a pointer to a List of this Mailbox's children, or 0 if it
    has none.
*/

List< Mailbox >* Mailbox::children() const
{
    return d->children;
}


/*! Returns true if this mailbox has at least one real, existing child
    mailbox, including indirect children, and false if not.
*/

bool Mailbox::hasChildren() const
{
    List<Mailbox>::Iterator it( d->children );
    while ( it ) {
        if ( !it->deleted() && !it->synthetic() )
            return true;
        if ( it->hasChildren() )
            return true;
        ++it;
    }
    return false;
}


/*! Returns a pointer to the Mailbox object at the root of the global
    hierarchy.
*/

Mailbox *Mailbox::root()
{
    return ::root;
}


/*! Returns a pointer to the Mailbox with \a id, or a null pointer if
    there is no such (known) Mailbox.

    Deleted mailboxes are included in the search, but synthetic ones
    aren't.
*/

Mailbox * Mailbox::find( uint id )
{
    if ( !::mailboxes )
        return 0;
    return ::mailboxes->find( id );
}


/*! Returns a pointer to a Mailbox named \a name, or 0 if the named
    mailbox doesn't exist. If \a deleted is true, deleted mailboxes
    are included in the search. The \a name must be fully-qualified.
*/

Mailbox *Mailbox::find( const String &name, bool deleted )
{
    Mailbox * m = obtain( name, false );
    if ( !m )
        return 0;
    if ( m->deleted() && !deleted )
        return 0;
    if ( m->synthetic() )
        return 0;
    return m;
}


/*! Returns a pointer to the closest existing parent mailbox for \a
    name, or a null pointer if \a name doesn't look like a mailbox
    name at all, or if no parent mailboxes of \a name exist.

    Never returns a null pointer or a pointer to a nonexistent mailbox.
*/

Mailbox * Mailbox::closestParent( const String & name )
{
    if ( name[0] != '/' )
        return 0;

    Mailbox * candidate = ::root;
    Mailbox * good = ::root;
    uint i = 1;
    while ( candidate && candidate->name() != name ) {
        if ( candidate && !candidate->deleted() &&
             ( !candidate->synthetic() || candidate->isHome() ) )
            good = candidate;
        if ( name[i] == '/' )
            return 0; // two slashes -> syntax error
        while ( i < name.length() && name[i] != '/' )
            i++;
        if ( !candidate->children() ) {
            candidate = 0;
        }
        else {
            String next = name.mid( 0, i );
            List<Mailbox>::Iterator it( candidate->children() );
            while ( it && it->name() != next )
                ++it;
            candidate = it;
            i++;
       }
    }
    return good;
}


/*! Obtain a mailbox with \a name, creating Mailbox objects as
    necessary and permitted.

    if \a create is true (this is the default) and there is no such
    Mailbox, obtain() creates one, including any necessary parents.
    The new mailbox is initially synthetic().

    If \a create is false and there is no such Mailbox, obtain()
    returns null without creating anything.
*/

Mailbox * Mailbox::obtain( const String & name, bool create )
{
    if ( name[0] != '/' )
        return 0;

    uint i = name.length();
    while ( i > 0 && name[i] != '/' )
        i--;
    Mailbox * parent = ::root;
    if ( i > 0 )
        parent = obtain( name.mid( 0, i ), create );
    else if ( ::root->name() == name )
        return ::root;
    if ( !parent )
        return 0;
    if ( !create && !parent->children() )
        return 0;

    if ( !parent->d->children )
        parent->d->children = new List<Mailbox>;
    List<Mailbox>::Iterator it( parent->d->children );
    String lower = name.lower();
    while ( it ) {
        if ( it->name().lower() == lower )
            return it;
        ++it;
    }
    if ( !create )
        return 0;

    Mailbox * m = new Mailbox( name );
    m->d->parent = parent;
    parent->d->children->append( m );
    return m;
}


/*! Sets this Mailbox's owner to \a n (which is assumed to be a valid
    user id).
*/

void Mailbox::setOwner( uint n )
{
    d->owner = n;
}


/*! Changes this Mailbox's uidnext value to \a n. No checks are
    performed - although uidnext should monotonically increase, this
    function gives you total liberty.

    Only OCClient is meant to call this function. Calling it elsewhere
    will likely disturb either OCClient, ocd, ImapSession or Arnt.
*/

void Mailbox::setUidnext( uint n )
{
    if ( n == d->uidnext )
        return;
    d->uidnext = n;
    if ( !d->watchers )
        return;
    List<EventHandler>::Iterator it( d->watchers );
    while ( it ) {
        EventHandler * h = it;
        ++it;
        h->execute();
    }
}


/*! Changes this Mailbox's deletedness to \a del.

    Only OCClient is *meant* to call this function -- see setUidnext().
    But don't check to see if that's really true.
*/

void Mailbox::setDeleted( bool del )
{
    if ( del )
        d->type = Deleted;
    else
        d->type = Ordinary;
}


/*! If this Mailbox does not exist, this function enqueues a Query to
    create it in the Transaction \a t and returns the Query. Otherwise
    it returns 0 and does nothing. It does not commit the transaction.

    If \a owner is non-null, the new mailbox is owned by by \a owner.
*/

Query * Mailbox::create( Transaction * t, User * owner )
{
    Query * q;

    if ( deleted() ) {
        q = new Query( "update mailboxes set deleted='f',owner=$2 "
                       "where id=$1", 0 );
        q->bind( 1, id() );
    }
    else if ( id() == 0 ) {
        q = new Query( "insert into mailboxes "
                       "(name,owner,uidnext,uidvalidity,deleted) "
                       "values ($1,$2,1,1,'f')", 0 );
        q->bind( 1, name() );
    }
    else {
        return 0;
    }

    if ( owner )
        q->bind( 2, owner->id() );
    else
        q->bindNull( 2 );

    t->enqueue( q );

    MailboxReader * mr = new MailboxReader( name() );
    t->enqueue( mr->query );

    return q;
}


/*! If this Mailbox can be deleted, this function enqueues a Query to do
    so in the Transaction \a t and returns the Query. If not, it returns
    0 and does nothing. It does not commit the transaction.
*/

Query * Mailbox::remove( Transaction * t )
{
    if ( synthetic() || deleted() )
        return 0;

    Query * q =
        new Query( "update mailboxes set deleted='t',owner=null "
                   "where id=$1", 0 );
    q->bind( 1, id() );
    t->enqueue( q );

    q = new Query( "delete from permissions where mailbox=$1", 0 );
    q->bind( 1, id() );
    t->enqueue( q );

    q = new Query( "delete from views where source=$1", 0 );
    q->bind( 1, id() );
    t->enqueue( q );

    q = new Query( "delete from messages where mailbox=$1", 0 );
    q->bind( 1, id() );
    t->enqueue( q );

    MailboxReader * mr = new MailboxReader( name() );
    t->enqueue( mr->query );

    return q;
}


/*! Returns a pointer to the message with \a uid in this Mailbox. If
    there is no such message and \a create is true, message() creates
    one dynamically. \a create is true by default. If this Mailbox
    cannot contain messages, message() returns a null pointer.

    This is a bit of a memory leak - messages are never deleted. When
    the last session on a Mailbox is closed, we should drop these
    messages. But we don't, yet.
*/

Message * Mailbox::message( uint uid, bool create ) const
{
    if ( synthetic() || deleted() )
        return 0;
    if ( !d->messages )
        d->messages = new Map<Message>;
    Message * m = d->messages->find( uid );
    if ( create && !m ) {
        m = new Message;
        m->setUid( uid );
        m->setMailbox( this );
        d->messages->insert( uid, m );
    }
    return m;
}


/*! Forgets all about the Message objects in this Mailbox. This
    interacts very poorly with active fetchers. Basically, if there's
    a fetcher active, clear() will cause horrid confusion.
*/

void Mailbox::clear()
{
    d->messages = 0;
}


/*! Starts retrieving the header fields of \a messages, and will
    notify \a handler whenever at least one message becomes available.
*/

void Mailbox::fetchHeaders( const MessageSet & messages,
                            EventHandler * handler )
{
    if ( !d->headerFetcher )
        d->headerFetcher = new MessageHeaderFetcher( this );
    d->headerFetcher->insert( messages, handler );
}


/*! Starts retrieving the internaldate and rfc822size from \a
    messages, and will notify \a handler whenever at least one message
    becomes available.
*/

void Mailbox::fetchTrivia( const MessageSet & messages,
                           EventHandler * handler )
{
    if ( !d->triviaFetcher )
        d->triviaFetcher = new MessageTriviaFetcher( this );
    d->triviaFetcher->insert( messages, handler );
}


/*! Starts retrieving the body parts fields of \a messages, and will
    notify \a handler whenever at least one message becomes available.
*/

void Mailbox::fetchBodies( const MessageSet & messages,
                           EventHandler * handler )
{
    if ( !d->bodyFetcher )
        d->bodyFetcher = new MessageBodyFetcher( this );
    d->bodyFetcher->insert( messages, handler );

}


/*! Starts retrieving the flags of \a messages, and will notify \a handler
    whenever at least one message becomes available.
*/

void Mailbox::fetchFlags( const MessageSet & messages,
                          EventHandler * handler )
{
    if ( !d->flagFetcher )
        d->flagFetcher = new MessageFlagFetcher( this );
    d->flagFetcher->insert( messages, handler );
}


/*! Starts retrieving the annotations of \a messages, and will notify
    \a handler whenever at least one message becomes available.
*/

void Mailbox::fetchAnnotations( const MessageSet & messages,
                                EventHandler * handler )
{
    if ( !d->annotationFetcher )
        d->annotationFetcher = new MessageAnnotationFetcher( this );
    d->annotationFetcher->insert( messages, handler );
}


/*! Makes the Mailbox forget that \a f exists. The next time the
    Mailbox needs a suitable Fetcher, it will create one.
*/

void Mailbox::forget( Fetcher * f )
{
    if ( d->headerFetcher == f )
        d->headerFetcher = 0;
    else if ( d->flagFetcher == f )
        d->flagFetcher = 0;
    else if ( d->bodyFetcher == f )
        d->bodyFetcher = 0;
    else if ( d->triviaFetcher == f )
        d->triviaFetcher = 0;
    else if ( d->annotationFetcher == f )
        d->annotationFetcher = 0;
}


/*! Adds \a eh to the list of event handlers that should be notified
    whenever new messags are injected into this mailbox. In the future
    other changes may also be communicated via this interface.

    If \a eh watches this mailbox already, addWatcher() does nothing.
*/

void Mailbox::addWatcher( EventHandler * eh )
{
    if ( !d->watchers )
        d->watchers = new List<EventHandler>;
    if ( eh && !d->watchers->find( eh ) )
        d->watchers->append( eh );
}


/*! Removes \a eh from the list of watchers for this mailbox, or does
    nothing if \a eh doesn't watch this mailbox.
*/

void Mailbox::removeWatcher( EventHandler * eh )
{
    if ( !d->watchers || !eh )
        return;

    d->watchers->remove( eh );
    if ( d->watchers->isEmpty() )
        d->watchers = 0;
}
