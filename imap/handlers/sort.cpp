// Copyright Oryx Mail Systems GmbH. All enquiries to info@oryx.com, please.

#include "sort.h"

#include "user.h"
#include "field.h"
#include "mailbox.h"
#include "threader.h"
#include "imapparser.h"
#include "imapsession.h"


class SortData
    : public Garbage
{
public:
    SortData(): Garbage(), s( 0 ), q( 0 ), u( false ) {}

    enum SortCriterionType {
        Arrival,
        Cc,
        Date,
        From,
        Size,
        Subject,
        To,
        Annotation,
        Unknown
    };

    class SortCriterion
        : public Garbage {
    public:
        SortCriterion()
            : t( Unknown ), reverse( false ),
              priv( false ), b1( 0 ), b2( 0 ) {}

        SortCriterionType t;
        bool reverse;
        // the rest applies only to annotation
        String annotationEntry; 
        bool priv;
        uint b1, b2;
    };

    List<SortCriterion> c;

    Selector * s;
    Query * q;
    bool u;

    bool usingCriterionType( SortCriterionType );

    void addCondition( String &, class SortCriterion * );
    void addJoin( String &, const String &, const String &, bool );
};


/*! \class Sort sort.h

    The Sort class implements the IMAP SORT extension, which is
    defined in RFC 5256.

    This class subclasses Search in order to take advantage of its
    parser, and operates quite nastily on the Query generated by
    Selector.
*/



/*! Constructs an empty Sort handler. If \a u is true, the SORT
    response will use UIDs, if it's false it will use MSNs.
*/

Sort::Sort( bool u )
    : Search( u ), d( new SortData )
{
    d->u = u;
}


void Sort::parse()
{
    // sort-criteria
    space();
    require( "(" );
    bool x = true;
    while ( x ) {
        uint m = parser()->mark();
        if ( !d->c.isEmpty() )
            space();
        SortData::SortCriterion * c = new SortData::SortCriterion;
        String s = parser()->dotLetters( 2, 10 ).lower();
        if ( s == "reverse" ) {
            space();
            s = parser()->dotLetters( 2, 10 ).lower();
            c->reverse = true;
        }
        if ( s == "arrival" ) {
            c->t = SortData::Arrival;
        }
        else if ( s == "cc" ) {
            c->t = SortData::Cc;
        }
        else if ( s == "date" ) {
            c->t = SortData::Date;
        }
        else if ( s == "from" ) {
            c->t = SortData::From;
        }
        else if ( s == "size" ) {
            c->t = SortData::Size;
        }
        else if ( s == "subject" ) {
            c->t = SortData::Subject;
        }
        else if ( s == "to" ) {
            c->t = SortData::To;
        }
        else if ( s == "annotation" ) {
            c->t = SortData::Annotation;
            space();
            c->annotationEntry = astring();
            space();
            if ( present( "value.priv" ) )
                c->priv = true;
            else if ( !present( "value.shared" ) )
                error( Bad,
                       "Annotation attribute must be "
                       "value.priv or value.shared" );
        }
        if ( ok() && c->t != SortData::Unknown ) {
            if ( !d->usingCriterionType( c->t ) )
                d->c.append( c );
        }
        else {
            parser()->restore( m );
            x = false;
        }
    }
    require( ")" );

    space();

    // search-criteria
    setCharset( astring() );
    space();
    d->s = new Selector;
    d->s->add( parseKey() );
    while ( ok() && !parser()->atEnd() ) {
        space();
        d->s->add( parseKey() );
    }
    end();
}


/*! This reimplementation hides Search::execute() entirely. */

void Sort::execute()
{
    if ( state() != Executing )
        return;

    if ( d->usingCriterionType( SortData::Subject ) ) {
        Threader * t = session()->mailbox()->threader();
        if ( !t->updated( true ) ) {
            t->refresh( this );
            return;
        }
    }

    if ( !d->q ) {
        d->s->simplify();
        d->q = d->s->query( imap()->user(), session()->mailbox(),
                            session(), this );
        String t = d->q->string();
        log( "search: " + t, Log::Debug );
        List<SortData::SortCriterion>::Iterator c( d->c );
        while ( c ) {
            if ( c->t == SortData::Annotation ) {
                c->b1 = d->s->placeHolder();
                d->q->bind( c->b1, c->annotationEntry );
                if ( c->priv ) {
                    c->b2 = d->s->placeHolder();
                    d->q->bind( c->b2, imap()->user()->id() );
                }
            }
            d->addCondition( t, c );
            ++c;
        }
        log( "with sort: " + t, Log::Debug );
        d->q->setString( t );
        d->q->execute();
    }

    if ( !d->q->done() )
        return;

    String s;
    s.reserve( d->q->rows() * 6 );
    s.append( "SORT" );
    Row * r;
    while ( (r=d->q->nextRow()) != 0 ) {
        s.append( " " );
        uint uid = r->getInt( "uid" );
        if ( d->u )
            s.append( fn( uid ) );
        else
            s.append( fn( imap()->session()->msn( uid ) ) );
    }
    respond( s, Untagged );
    finish();
}


void SortData::addCondition( String & t, class SortData::SortCriterion * c )
{
    switch ( c->t ) {
    case Arrival:
        addJoin( t, "", "mm.idate", c->reverse );
        break;
    case Cc:
        addJoin( t,
                 "left join address_fields sccaf on "
                 "(mm.message=sccaf.message and "
                 " sccaf.part='' and sccaf.number=0 and"
                 " sccaf.field=" + fn( HeaderField::Cc ) + ") "
                 "left join addresses scca on (sccaf.address=scca.id) ",
                 "scca.localpart",
                 c->reverse );
        break;
    case Date:
        addJoin( t,
                 "join date_fields sddf on (mm.message=sddf.message) ",
                 "sddf.value",
                 c->reverse );
        break;
    case From:
        addJoin( t,
                 "join address_fields sfaf on "
                 "(mm.message=sfaf.message and "
                 " sfaf.part='' and sfaf.number=0 and"
                 " sfaf.field=" + fn( HeaderField::From ) + ") "
                 "join addresses sfa on (sfaf.address=sfa.id) ",
                 "sfa.localpart",
                 c->reverse );
        break;
    case Size:
        addJoin( t,
                 "join messages m on (m.id=mm.message)",
                 "m.rfc822size",
                 c->reverse );
        break;
    case Subject:
        addJoin( t,
                 "left join thread_members sstm on "
                 "(mm.mailbox=sstm.mailbox and mm.uid=sstm.uid) "
                 "left join threads sst on "
                 "(sstm.thread=sst.id) ",
                 "lower(sst.subject)",
                 c->reverse );
        break;
    case To:
        addJoin( t,
                 "left join address_fields staf on "
                 "(mm.message=staf.message and "
                 " staf.part='' and staf.number=0 and"
                 " staf.field=" + fn( HeaderField::To ) + ") "
                 "left join addresses sta on (staf.address=sta.id) ",
                 "sta.localpart",
                 c->reverse );
        break;
    case Annotation:
        if ( c->priv )
            addJoin( t,
                     "left join annotations saa on "
                     "(mm.mailbox=saa.mailbox and mm.uid=saa.uid and"
                     " owner=$" + fn( c->b2 ) + " and name="
                     "(select id from annotation_names where lower(name)=$" +
                     fn( c->b1 ) + ")) ",
                     "saa.value",
                     c->reverse );
        else
            addJoin( t,
                     "left join annotations saa on "
                     "(mm.mailbox=saa.mailbox and mm.uid=saa.uid and"
                     " owner is null and name="
                     "(select id from annotation_names where lower(name)=$" +
                     fn( c->b1 ) + ")) ",
                     "saa.value",
                     c->reverse );
        break;
    case Unknown:
        break;
    }
}


void SortData::addJoin( String & t,
                        const String & join, const String & orderby,
                        bool desc )
{
    int w = t.find( " where " );
    if ( w < 0 )
        return;
    t = t.mid( 0, w+1 ) + join + t.mid( w+1 );
    int o = t.find( " order by " );
    if ( o < 0 )
        return;
    o += 10;
    int c = t.length();
    while ( c > o && t[c] != ',' )
        c--;
    if ( c > o )
        t = t.mid( 0, c ) + ", " + orderby +
            ( desc ? " desc" : "" ) +
            t.mid( c );
    else
        t = t.mid( 0, o ) + orderby +
            ( desc ? " desc, " : ", " ) +
            t.mid( o );

    // and include orderby in the return list so select distinct
    // doesn't complain. why does select distinct do that anyway?
    int s = t.find( "mm.uid" );
    if ( s < 0 )
        return;
    s += 6;
    t = t.mid( 0, s ) + ", " + orderby + t.mid( s );
}


bool SortData::usingCriterionType( SortCriterionType t )
{
    List<SortCriterion>::Iterator i( c );
    while ( i && i->t != t )
        ++i;
    if ( i )
        return true;
    return false;
}

