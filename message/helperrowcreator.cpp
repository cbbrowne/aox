// Copyright Oryx Mail Systems GmbH. All enquiries to info@oryx.com, please.

#include "helperrowcreator.h"

#include "transaction.h"
#include "query.h"

#include "annotationname.h"
#include "fieldname.h"
#include "flag.h"


/*! \class HelperRowCreator helperrowcreator.h

    The HelperRowCreator class contains common logic and some code to
    add rows to the helper tables flag_names, annotation_names and
    header_fields. It's inherited by one class per table.

    In theory this could handle bodyparts and addresses, but I think
    not. Those are different. Those tables grow to be big. These three
    tables frequently contain less than one row per thousand messages,
    so we need to optimise this class for inserting zero, one or at
    most a few rows.
*/


class HelperRowCreatorData
    : public Garbage
{
public:
    HelperRowCreatorData()
        : s( 0 ), c( 0 ), t( 0 ), sp( false ), done( false ) {}

    Query * s;
    Query * c;
    Transaction * t;
    String n;
    String e;
    bool sp;
    bool done;
};


/*!  Constructs an empty HelperRowCreator refering to \a table, using
     \a transaction. If an error related to \a constraint occurs,
     execute() will roll back to a savepoint and try again.
*/

HelperRowCreator::HelperRowCreator( const String & table,
                                    Transaction * transaction,
                                    const String & constraint )
    : EventHandler(), d( new HelperRowCreatorData )
{
    setLog( new Log( Log::Server ) );
    d->t = transaction;
    d->n = table + "_creator";
    d->e = constraint;
}


/*! Returns true if this object is done with the Transaction, and
    false if it will use the Transaction for one or more queries.
*/

bool HelperRowCreator::done() const
{
    return d->done;
}


void HelperRowCreator::execute()
{
    while ( !d->done ) {
        if ( d->s && !d->s->done() )
            return;
        if ( d->c && !d->c->done() )
            return;

        if ( !d->s ) {
            d->s = makeSelect();
            if ( d->s ) {
                d->t->enqueue( d->s );
                d->t->execute();
            }
            else {
                d->done = true;
            }
        }

        if ( d->s && d->s->done() && !d->c ) {
            processSelect( d->s );
            d->s = 0;
            d->c = makeCopy();
            if ( d->c ) {
                if ( !d->sp ) {
                    d->t->enqueue( new Query( "savepoint " + d->n, 0 ) );
                    d->sp = true;
                }
                d->t->enqueue( d->c );
                d->t->execute();
            }
            else {
                d->done = true;
            }
        }

        if ( d->c && d->c->done() ) {
            Query * c = d->c;
            d->c = 0;
            if ( !c->failed() ) {
                // We inserted, hit no race.
            }
            else if ( c->error().contains( d->e ) ) {
                // We inserted, but there was a race and we lost it.
                d->t->enqueue( new Query( "rollback to savepoint "+d->n, 0 ) );
            }
            else {
                // Total failure. The Transaction is now in Failed
                // state, and there's nothing we can do other than
                // notify our owner about it.
                d->done = true;
                d->sp = false;
            }
        }
    }

    if ( d->sp ) {
        d->t->enqueue( new Query( "release savepoint " + d->n, 0 ) );
        String ed = d->n;
        ed.replace( "creator", "extended" );
        d->t->enqueue( new Query( "notify " + ed, 0 ) );
    }
    d->t->notify();
}


/*! \fn Query * HelperRowCreator::makeSelect()

    This pure virtual function is called to make a query to return the
    IDs of rows already in the database, or of newly inserted rows.

    If nothing needs to be done, the makeSelect() can return a null
    pointer.

    If makeSelect() returns non-null, the returned Query should have
    this object as owner.
 */


/*! \fn void HelperRowCreator::processSelect( Query * q )

    This pure virtual function is called to process the result of the
    makeSelect() Query. \a q is the Query returned by makeSelect()
    (never 0).
 */


/*! \fn Query * HelperRowCreator::makeCopy()

    This pure virtual function is called to make a query to insert the
    necessary rows to the table.

    If nothing needs to be inserted, makeCopy() can return 0.

    If makeCopy() returns non-null, the returned Query should have
    this object as owner.
 */


class FlagCreatorData
    : public Garbage
{
public:
    FlagCreatorData( const StringList & f ): flags( f ) {}
    StringList flags;
};


/*! \class FlagCreator helperrowcreator.h

    This class issuses queries using a supplied Transaction to add new
    flags to the database.
*/


/*! Starts constructing the queries needed to create the specified \a
    flags in the transaction \a t. This object will notify the
    Transaction::owner() when it's done.

    \a t will fail if flag creation fails for some reason (typically
    bugs). Transaction::error() should say what went wrong.
*/

FlagCreator::FlagCreator( const StringList & flags, Transaction * t )
    : HelperRowCreator( "flag_names", t, "fn_uname" ),
      d( new FlagCreatorData( flags ) )
{
}


Query * FlagCreator::makeSelect()
{
    Query * s = new Query( "select id, name from flag_names where "
                           "lower(name)=any($1::text[])", this );

    StringList sl;
    StringList::Iterator it( d->flags );
    while ( it ) {
        String name( *it );
        if ( Flag::id( name ) == 0 )
            sl.append( name.lower() );
        ++it;
    }

    if ( sl.isEmpty() )
        return 0;
    s->bind( 1, sl );
    return s;
}


void FlagCreator::processSelect( Query * s )
{
    while ( s->hasResults() ) {
        Row * r = s->nextRow();
        Flag::add( r->getString( "name" ), r->getInt( "id" ) );
    }
}


Query * FlagCreator::makeCopy()
{
    Query * c = new Query( "copy flag_names (name) from stdin with binary",
                           this );
    bool any = false;
    StringList::Iterator it( d->flags );
    while ( it ) {
        if ( Flag::id( *it ) == 0 ) {
            c->bind( 1, *it );
            c->submitLine();
            any = true;
        }
        ++it;
    }

    if ( !any )
        return 0;
    return c;

}


/*! \class FieldNameCreator helperrowcreator.h

    The FieldNameCreator is a HelperRowCreator to insert rows into the
    field_names table. Nothing particular.
*/


/*! Creates an object to ensure that all entries in \a f are present
    in field_names, using \a tr for all its queryies.
*/


FieldNameCreator::FieldNameCreator( const StringList & f,
                                    Transaction * tr )
    : HelperRowCreator( "field_names", tr,  "field_names_name_key" ),
      names( f )
{
}


Query * FieldNameCreator::makeSelect()
{
    Query * q = new Query( "select id, name from field_names where "
                           "name=any($1::text[])", this );

    StringList sl;
    StringList::Iterator it( names );
    while ( it ) {
        if ( FieldName::id( *it ) == 0 )
            sl.append( *it );
        ++it;
    }
    if ( sl.isEmpty() )
        return 0;
    q->bind( 1, sl );
    return q;
}


void FieldNameCreator::processSelect( Query * q )
{
    while ( q->hasResults() ) {
        Row * r = q->nextRow();
        FieldName::add( r->getString( "name" ), r->getInt( "id" ) );
    }
}


Query * FieldNameCreator::makeCopy()
{
    Query * q = new Query( "copy field_names (name) from stdin with binary",
                           this );
    StringList::Iterator it( names );
    bool any = false;
    while ( it ) {
        if ( FieldName::id( *it ) == 0 ) {
            q->bind( 1, *it );
            q->submitLine();
            any = true;
        }
        ++it;
    }

    if ( !any )
        return 0;
    return q;
}


/*! \class AnnotationNameCreator helperrowcreator.h

    The AnnotationNameCreator is a HelperRowCreator to insert rows into
    the annotation_names table. Nothing particular.
*/


/*! Creates an object to ensure that all entries in \a f are present
    in annotation_names, using \a t for all its queryies.
*/

AnnotationNameCreator::AnnotationNameCreator( const StringList & f,
                                              Transaction * t )
    : HelperRowCreator( "annotation_names", t, "annotation_names_name_key" ),
      names( f )
{
}

Query *  AnnotationNameCreator::makeSelect()
{
    Query * q = new Query( "select id, name from annotation_names where "
                           "name=any($1::text[])", this );

    StringList sl;
    StringList::Iterator it( names );
    while ( it ) {
        String name( *it );
        if ( AnnotationName::id( name ) == 0 )
            sl.append( name );
        ++it;
    }
    if ( sl.isEmpty() )
        return 0;

    q->bind( 1, sl );
    return q;
}


void AnnotationNameCreator::processSelect( Query * q )
{
    while ( q->hasResults() ) {
        Row * r = q->nextRow();
        AnnotationName::add( r->getString( "name" ), r->getInt( "id" ) );
    }
}


Query * AnnotationNameCreator::makeCopy()
{
    Query * q = new Query( "copy annotation_names (name) "
                           "from stdin with binary", this );
    StringList::Iterator it( names );
    bool any = false;
    while ( it ) {
        if ( AnnotationName::id( *it ) == 0 ) {
            any = true;
            q->bind( 1, *it );
            q->submitLine();
        }
        ++it;
    }

    if ( !any )
        return 0;
    return q;
}


