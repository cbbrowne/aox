// Copyright 2009 The Archiveopteryx Developers <info@aox.org>

#include "undelete.h"

#include "searchsyntax.h"
#include "transaction.h"
#include "integerset.h"
#include "selector.h"
#include "mailbox.h"
#include "query.h"
#include "map.h"
#include "utf.h"

#include <stdlib.h> // exit()
#include <stdio.h> // printf()


class UndeleteData
    : public Garbage
{
public:
    UndeleteData(): state( 0 ), m( 0 ), t( 0 ),
                    find( 0 ), uidnext( 0 ), usernames( 0 ) {}

    uint state;
    Mailbox * m;
    Transaction * t;

    Query * find;
    Query * uidnext;
    Query * usernames;
};


static AoxFactory<Undelete>
f( "undelete", "", "Recover a message that has been deleted.",
   "    Synopsis: undelete [-n] <mailbox> <search>\n\n"
   "    Searches for deleted messages in the specified mailbox and\n"
   "    recovers those that match the search.\n"
   "    The -n option causes a dummy undelete.\n"
   "    Messages can be restored after an IMAP EXPUNGE or POP3 DELE\n"
   "    until aox vacuum permanently removes them (some weeks) later.\n" );

/*! \class Undelete Undelete.h
    This class handles the "aox undelete" command.
*/

Undelete::Undelete( EStringList * args )
    : AoxCommand( args ), d( new UndeleteData )
{
}


void Undelete::execute()
{
    if ( d->state == 0 ) {
        database( true );
        Mailbox::setup();
        d->state = 1;
        parseOptions();
    }

    if ( d->state == 1 ) {
        if ( !choresDone() )
            return;
        d->state = 2;
    }

    if ( d->state == 2 ) {
        Utf8Codec c;
        UString m = c.toUnicode( next() );

        if ( !c.valid() )
            error( "Encoding error in mailbox name: " + c.error() );
        else if ( m.isEmpty() )
            error( "No mailbox name" );
        else
            d->m = Mailbox::find( m, true );
        if ( !d->m )
            error( "No such mailbox: " + m.utf8() );

        Selector * s = parseSelector( args() );
        if ( !s )
            exit( 1 );
        s->simplify();

        d->t = new Transaction( this );
        if ( d->m->deleted() ) {
            if ( !d->m->create( d->t, 0 ) )
                error( "Mailbox was deleted; recreating failed: " +
                       d->m->name().utf8() );
            printf( "aox: Note: Mailbox %s is recreated.\n"
                    "     Its ownership and permissions could not be restored.\n",
                    d->m->name().utf8().cstr() );
        }

        EStringList wanted;
        wanted.append( "uid" );
        if ( opt( 'v' ) ) {
            wanted.append( "deleted_by" );
            wanted.append( "deleted_at::text" );
            wanted.append( "reason" );
            d->usernames = new Query( "select id, login from users", 0 );
            d->t->enqueue( d->usernames );
        }

        d->find = s->query( 0, d->m, 0, 0, true, &wanted, true );
        d->t->enqueue( d->find );

        d->uidnext = new Query( "select uidnext, nextmodseq "
                                "from mailboxes "
                                "where id=$1 for update", this );
        d->uidnext->bind( 1, d->m->id() );
        d->t->enqueue( d->uidnext );

        d->t->execute();
        d->state = 3;
    }

    if ( d->state == 3 ) {
        if ( !d->uidnext->done() )
            return;

        Row * r = d->uidnext->nextRow();
        if ( !r )
            error( "Internal error - could not read mailbox UID" );
        uint uidnext = r->getInt( "uidnext" );
        int64 modseq = r->getBigint( "nextmodseq" );

        Map<EString> logins;
        if ( d->usernames ) {
            while ( d->usernames->hasResults() ) {
                r = d->usernames->nextRow();
                logins.insert( r->getInt( "id" ),
                               new EString( r->getEString( "login" ) ) );
            }
        }

        Map<EString> why;
        IntegerSet s;
        while ( d->find->hasResults() ) {
            r = d->find->nextRow();
            uint uid = r->getInt( "uid" );
            s.add( uid );
            if ( d->usernames )
                why.insert( uid,
                            new EString(
                                " - Message " + fn( uid ) + " was deleted by " +
                                (*logins.find( r->getInt( "deleted_by" ) )).quoted() +
                                " at " + r->getEString( "deleted_at" ) +
                                "\n   Reason: " +
                                r->getEString( "reason" ).simplified().quoted() ) );
        }

        if ( s.isEmpty() )
            error( "No such deleted message (search returned 0 results)" );

        printf( "aox: Undeleting %d messages into %s\n",
                s.count(), d->m->name().utf8().cstr() );

        Map<EString>::Iterator i( why );
        while ( i ) {
            printf( "%s\n", i->cstr() );
            ++i;
        }

        Query * q;

        q = new Query( "create temporary sequence s start " + fn( uidnext ),
                       0 );
        d->t->enqueue( q );

        q = new Query( "insert into mailbox_messages "
                       "(mailbox,uid,message,modseq) "
                       "select $1,nextval('s'),message,$2 "
                       "from deleted_messages "
                       "where mailbox=$1 and uid=any($3)", 0 );
        q->bind( 1, d->m->id() );
        q->bind( 2, modseq );
        q->bind( 3, s );
        d->t->enqueue( q );

        q = new Query( "delete from deleted_messages "
                       "where mailbox=$1 and uid=any($2)", 0 );
        q->bind( 1, d->m->id() );
        q->bind( 2, s );
        d->t->enqueue( q );

        q = new Query( "update mailboxes "
                       "set uidnext=nextval('s'), nextmodseq=$1 "
                       "where id=$2", 0 );
        q->bind( 1, modseq + 1 );
        q->bind( 2, d->m->id() );
        d->t->enqueue( q );

        d->t->enqueue( new Query( "drop sequence s", 0 ) );

        Mailbox::refreshMailboxes( d->t );

        if ( opt( 'n' ) ) {
            printf( "aox: Cancelling undeleting due to -n. Rerun without -n to actually undelete.\n" );
            d->t->rollback();
        }
        else {
            d->t->commit();
        }
        d->state = 4;
    }

    if ( d->state == 4 ) {
        if ( !d->t->done() )
            return;

        if ( d->t->failed() )
            error( "Undelete failed." );
        finish();
    }
}
