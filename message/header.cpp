// Copyright Oryx Mail Systems GmbH. All enquiries to info@oryx.com, please.

#include "header.h"

#include "field.h"
#include "datefield.h"
#include "mimefields.h"
#include "addressfield.h"
#include "multipart.h"
#include "address.h"
#include "ustring.h"
#include "codec.h"
#include "date.h"
#include "utf.h"


static const char *crlf = "\015\012";


class HeaderData
    : public Garbage
{
public:
    HeaderData()
        : mode( Header::Rfc2822 ),
          defaultType( Header::TextPlain ),
          verified( false )
    {}

    Header::Mode mode;
    Header::DefaultType defaultType;

    bool verified;
    String error;

    List< HeaderField > fields;
};


/*! \class Header header.h
    The Header class models an RFC 2822 or MIME header.

    Essentially, it's a container for HeaderField objects which can
    check whether its contents make sense and are legal (see RFC 2822
    page 19), and will give them to callers on demand.

    Fields are available by calling field() with the right type. This
    works well for some fields, but fields which can occur several
    times have a problem. Will have to solve that eventually.

    Some fields are also available as values, e.g. date().
*/

/*! Constructs an empty Header in \a m mode. If \a m is Rfc2822, the
    header's validity will follow RFC 2822 rules, while if \a m is
    Mime, RFC 2045-2049 rules are used.
*/

Header::Header( Mode m )
    : d( new HeaderData )
{
    d->mode = m;
}


/*! Returns the header's mode, either Mime or Rfc2822, which is set
    using the constructor and decides whether a particular header is
    valid. For example, in Rfc2822 mode a Date field is mandatory,
    while in Mime mode it's not allowed.
*/

Header::Mode Header::mode() const
{
    return d->mode;
}


/*! Returns true if this Header fills all the conditions laid out in
    RFC 2821 for validity, and false if not.
*/

bool Header::valid() const
{
    verify();
    return d->error.isEmpty();
}


/*! Returns a one-line error message describing the first error
    detected in this Header, or an empty string if there is no error.
*/

String Header::error() const
{
    verify();
    return d->error;
}


/*! Appends the HeaderField \a hf to this Header.

    If \a hf is a To/Cc/Reply-To/Bcc field, and the same address field
    already is present in this header, the addresses in \a hf are
    merged into the existing field and \a hf is discarded. This is
    nominally incorrect, and we do it to accept mail from a variety of
    buggy mail senders. More address fields may be added to the list
    if necessary.
*/

void Header::add( HeaderField *hf )
{
    HeaderField::Type t = hf->type();

    if ( t == HeaderField::To || t == HeaderField::Cc ||
         t == HeaderField::Bcc || t == HeaderField::ReplyTo ||
         t == HeaderField::From )
    {
        AddressField *first = addressField( t );
        AddressField *next = (AddressField *)hf;
        if ( first ) {
            List< Address > *old = first->addresses();
            List< Address >::Iterator it( next->addresses() );
            while ( it ) {
                old->append( it );
                ++it;
            }
            Address::uniquify( old );
            first->update();
            return;
        }
    }
    d->fields.append( hf );
    d->verified = false;
}


/*! Creates a header field with the supplied \a name and \a value, and
    appends it to this Header, adjusting validity as necessary.
*/

void Header::add( const String &name, const String &value )
{
    add( HeaderField::create( name, value ) );
}


/*! This private helper removes all fields with type \a t from the
    header.
*/

void Header::removeField( HeaderField::Type t )
{
    List<HeaderField>::Iterator it( d->fields );
    while ( it ) {
        if ( it->type() == t )
            d->fields.take( it );
        else
            ++it;
    }
    d->verified = false;
}


/*! Returns a pointer to a list containing all the HeaderField objects
    in this Header. Neither the list nor the HeaderField objects it in
    may be modified or freed by the caller - Header keeps other
    pointers to these objects.
*/

List< HeaderField > *Header::fields() const
{
    return &d->fields;
}


/*! This function returns a pointer to the header field with type \a t
    and index \a n, or a null pointer if there is no such field in this
    header.

    if \a n is 0, as it is by default, the first field with type \a t
    is returned. 1 refer to the second.
*/

HeaderField * Header::field( HeaderField::Type t, uint n ) const
{
    List<HeaderField>::Iterator it( d->fields );
    while ( n > 0 && it ) {
        while ( it && it->type() != t )
            ++it;
        n--;
        if ( it )
            ++it;
    }
    while ( it && it->type() != t )
        ++it;
    return it;
}


/*! Returns a pointer to the address field of type \a t at index \a n in
    this header, or a null pointer if no such field exists.
*/

AddressField *Header::addressField( HeaderField::Type t, uint n ) const
{
    return (AddressField *)field( t, n );
}


/*! Returns the header's data \a t, which is the normal date by
    default, but can also be orig-date or resent-date. If there is no
    such field or \a t is meaningless, date() returns a null pointer.
*/

Date *Header::date( HeaderField::Type t ) const
{
    DateField *hf = (DateField *)field( t );
    if ( !hf )
        return 0;
    return hf->date();
}


/*! Returns the header's subject. For the moment, this is a simple
    string. It'll have to morph soon, to handle RFC 2047 at least.
*/

String Header::subject() const
{
    HeaderField * s = field( HeaderField::Subject );
    if ( s )
        return s->value().simplified();
    return "";
}


/*! Returns the header's in-reply-to value. This comes straight from
    the RFC 2822 representation.
*/

String Header::inReplyTo() const
{
    HeaderField * s = field( HeaderField::InReplyTo );
    if ( s )
        return s->value().simplified();
    return "";
}


/*! Returns the header's message-id \a t, which is the normal
    message-id by default but can also be the first resent-message-id
    or the content-id.  The returned string is in the cleanest
    possible form. If there is no such message-id, messageId() returns
    an empty string.
*/

String Header::messageId( HeaderField::Type t ) const
{
    AddressField *af = addressField( t );
    if ( !af )
        return "";
    return af->value();
}


/*! Returns a pointer to the addresses in the \a t header field, which
    must be an address field such as From or Bcc. If not, or if the
    field is empty, addresses() returns a null pointer.
*/

List< Address > *Header::addresses( HeaderField::Type t ) const
{
    List< Address > * a = 0;
    AddressField * af = addressField( t );
    if ( af )
        a = af->addresses();
    if ( a && a->isEmpty() )
        a = 0;
    return a;
}


/*! Returns a pointer to the Content-Type header field, or a null
    pointer if there isn't one.
*/

ContentType *Header::contentType() const
{
    return (ContentType *)
        field( HeaderField::ContentType );
}


/*! Returns a pointer to the Content-Transfer-Encoding header field,
    or a null pointer if there isn't one.
*/

ContentTransferEncoding *Header::contentTransferEncoding() const
{
    return (ContentTransferEncoding *)
        field( HeaderField::ContentTransferEncoding );
}


/*! Returns a pointer to the Content-Disposition header field, or a null
    pointer if there isn't one.
*/

ContentDisposition *Header::contentDisposition() const
{
    return (ContentDisposition *)
        field( HeaderField::ContentDisposition );
}


/*! Returns the value of the Content-Description field, or an empty
    string if there isn't one. RFC 2047 encoding is not considered -
    should it be?
*/

String Header::contentDescription() const
{
    HeaderField *hf = field( HeaderField::ContentDescription );
    if ( !hf )
        return "";
    return hf->value().simplified();
}


/*! Returns the value of the Content-Location field, or an empty string
    if there isn't one. The URI is not validated in any way.
*/

String Header::contentLocation() const
{
    HeaderField *hf = field( HeaderField::ContentLocation );
    if ( !hf )
        return "";
    return hf->value();
}


/*! Returns a pointer to the Content-Language header field, or a null
    pointer if there isn't one.
*/

ContentLanguage *Header::contentLanguage() const
{
    return (ContentLanguage *)
        field( HeaderField::ContentLanguage );
}




static struct {
    HeaderField::Type t;
    int min;
    int max;
    Header::Mode m;
} conditions[] = {
    { HeaderField::Sender, 0, 1, Header::Rfc2822 },
    { HeaderField::ReplyTo, 0, 1, Header::Rfc2822 },
    { HeaderField::To, 0, 1, Header::Rfc2822 },
    { HeaderField::Cc, 0, 1, Header::Rfc2822 },
    { HeaderField::Bcc, 0, 1, Header::Rfc2822 },
    { HeaderField::MessageId, 0, 1, Header::Rfc2822 },
    { HeaderField::References, 0, 1, Header::Rfc2822 },
    { HeaderField::Subject, 0, 1, Header::Rfc2822 },
    { HeaderField::From, 1, 1, Header::Rfc2822 },
    { HeaderField::Date, 1, 1, Header::Rfc2822 },
    { HeaderField::MimeVersion, 0, 1, Header::Rfc2822 },
    { HeaderField::MimeVersion, 0, 0, Header::Mime },
    { HeaderField::ContentType, 0, 1, Header::Rfc2822 },
    { HeaderField::ContentType, 0, 1, Header::Mime },
    { HeaderField::ContentTransferEncoding, 0, 1, Header::Rfc2822 },
    { HeaderField::ContentTransferEncoding, 0, 1, Header::Mime },
    { HeaderField::ReturnPath, 0, 1, Header::Rfc2822 },
    { HeaderField::Other, 0, 0, Header::Rfc2822 } // magic end marker
};


/*! This private function verifies that the entire header is
    consistent and legal, and that each contained HeaderField is
    legal.
*/

void Header::verify() const
{
    if ( d->verified )
        return;
    d->verified = true;
    d->error.truncate( 0 );

    List<HeaderField>::Iterator it( d->fields );
    while ( it ) {
        if ( !it->valid() ) {
            d->error = it->name() + ": " + it->error();
            return;
        }
        ++it;
    }

    int occurrences[(int)HeaderField::Other];
    int i = 0;
    while ( i < HeaderField::Other )
        occurrences[i++] = 0;
    it = d->fields.first();
    while ( it ) {
        HeaderField::Type t = it->type();
        ++it;
        if ( t < HeaderField::Other )
            occurrences[(int)t]++;
    }

    i = 0;
    while ( d->error.isEmpty() && conditions[i].t != HeaderField::Other ) {
        if ( conditions[i].m == d->mode &&
             ( occurrences[conditions[i].t] < conditions[i].min ||
               occurrences[conditions[i].t] > conditions[i].max ) ) {
            if ( conditions[i].max < occurrences[conditions[i].t] )
                d->error = fn( occurrences[conditions[i].t] ) + " " +
                           HeaderField::fieldName( conditions[i].t ) +
                           " fields seen. At most " +
                           fn( conditions[i].max ) + " may be present.";
            else
                d->error = fn( occurrences[conditions[i].t] ) + " " +
                           HeaderField::fieldName( conditions[i].t ) +
                           " fields seen. At least " +
                           fn( conditions[i].min ) + " must be present.";
        }
        i++;
    }

    // strictly speaking, if From contains more than one address,
    // sender should contain one. we don't enforce that, because it
    // causes too much spam to be rejected that would otherwise go
    // through. we'll filter spam with something that's a little less
    // accidental, and which does not clutter up the logs with so many
    // misleading error messages.

    // we graciously ignore all the Resent-This-Or-That restrictions.
}


static bool sameAddresses( AddressField *a, AddressField *b )
{
    if ( !a || !b )
        return false;

    List< Address > *l = a->addresses();
    List< Address > *m = b->addresses();

    if ( !l || !m )
        return false;

    if ( l->count() != m->count() )
        return false;

    List<Address>::Iterator it( m );
    while ( it ) {
        String lp = it->localpart();
        String dom = it->domain().lower();
        List<Address>::Iterator i( l );
        while ( i && !( i->localpart() == lp && i->domain().lower() == dom ) )
            ++i;
        if ( !i )
            return false;
        ++it;
    }
    return true;
}


/*! Removes any redundant header fields from this header. For example,
    if 'sender' or 'reply-to' points to the same address as 'from',
    that field can be removed.
*/

void Header::simplify()
{
    HeaderField *cde = field( HeaderField::ContentDescription );
    if ( cde && cde->value().isEmpty() ) {
        removeField( HeaderField::ContentDescription );
        cde = 0;
    }

    ContentTransferEncoding *cte = contentTransferEncoding();
    if ( cte && cte->encoding() == String::Binary )
        removeField( HeaderField::ContentTransferEncoding );

    ContentDisposition *cdi = contentDisposition();
    if ( cdi ) {
        ContentType *ct = contentType();

        if ( d->mode == Rfc2822 && ( !ct || ct->type() == "text" ) &&
             cdi->disposition() == ContentDisposition::Inline &&
             cdi->parameters()->isEmpty() )
        {
            removeField( HeaderField::ContentDisposition );
            cdi = 0;
        }
    }

    ContentType * ct = contentType();
    if ( ct ) {
        if ( ct->parameters()->isEmpty() && cte == 0 && cdi == 0 && cde == 0 &&
             d->defaultType == TextPlain &&
             ct->type() == "text" && ct->subtype() == "plain" ) {
            removeField( HeaderField::ContentType );
            ct = 0;
        }
    }
    else if ( d->defaultType == MessageRfc822 ) {
        add( "Content-Type", "message/rfc822" );
        ct = contentType();
    }

    if ( mode() == Mime ) {
        removeField( HeaderField::MimeVersion );
    }
    else if ( ct == 0 && cte == 0 && cde == 0 && cdi == 0 &&
         !field( HeaderField::ContentLocation ) &&
         !field( HeaderField::ContentBase ) ) {
        removeField( HeaderField::MimeVersion );
    }
    else {
        if ( mode() == Rfc2822 && !field( HeaderField::MimeVersion ) )
            add( "Mime-Version", "1.0" );
    }

    HeaderField *m = field( HeaderField::MessageId );
    if ( m && m->value().isEmpty() )
        removeField( HeaderField::MessageId );

    if ( sameAddresses( addressField( HeaderField::From ),
                        addressField( HeaderField::ReplyTo ) ) )
        removeField( HeaderField::ReplyTo );

    if ( sameAddresses( addressField( HeaderField::From ),
                        addressField( HeaderField::Sender ) ) )
        removeField( HeaderField::Sender );

    if ( !addresses( HeaderField::Sender ) )
        removeField( HeaderField::Sender );
    if ( !addresses( HeaderField::ReturnPath ) )
        removeField( HeaderField::ReturnPath );
    if ( !addresses( HeaderField::To ) )
        removeField( HeaderField::To );
    if ( !addresses( HeaderField::Cc ) )
        removeField( HeaderField::Cc );
    if ( !addresses( HeaderField::Bcc ) )
        removeField( HeaderField::Bcc );
    if ( !addresses( HeaderField::ReplyTo ) )
        removeField( HeaderField::ReplyTo );
}


/*! Repairs a few harmless and common problems, such as inserting two
    Date fields with the same value. Assumes that \a p is its
    companion body, and may look at it to decide what/how to repair.
*/

void Header::repair( Multipart * p )
{
    if ( valid() ) {
        // it seems to be valid, but have we been able to parse everything?
        List<HeaderField>::Iterator it( d->fields );
        while ( it && it->parsed() )
            ++it;
        // we parsed everything and it's valid. repair not necessary.
        if ( !it )
            return;
    }

    // We remove duplicates of any field that may occur only once.
    // (Duplication has been observed for Date/Subject/M-V/C-T-E/C-T/M-I.)

    int occurrences[ (int)HeaderField::Other ];
    int i = 0;
    while ( i < HeaderField::Other )
        occurrences[i++] = 0;

    List< HeaderField >::Iterator it( d->fields );
    while ( it ) {
        HeaderField::Type t = it->type();
        if ( t < HeaderField::Other )
            occurrences[(int)t]++;
        ++it;
    }

    i = 0;
    while ( conditions[i].t != HeaderField::Other ) {
        if ( conditions[i].m == d->mode &&
             occurrences[conditions[i].t] > conditions[i].max )
        {
            uint n = 0;
            HeaderField * h = field( conditions[i].t, 0 );
            List< HeaderField >::Iterator it( d->fields );
            while ( it ) {
                if ( it->type() == conditions[i].t ) {
                    n++;
                    if ( n > 1 && h->value() == it->value() )
                        d->fields.take( it );
                    else
                        ++it;
                }
                else {
                    ++it;
                }
            }
        }
        i++;
    }

    // We retain only the first valid Date field, Return-Path,
    // Message-Id and References fields. If there is one or more valid
    // such field, we delete all invalid fields and subsequent valid
    // fields, otherwise we leave the fields as they are.

    // Several senders appear to send duplicate dates. qmail is
    // mentioned in the references chains of most examples we have.

    // We don't know who adds duplicate message-id and return-path
    // fields.

    // The only case we've seen of duplicate references involved
    // Thunderbird 1.5.0.4 and Scalix. Uncertain whose
    // bug. Thunderbird 1.5.0.5 looks correct.

    i = 0;
    while ( i < HeaderField::Other ) {
        if ( occurrences[i] > 1 &&
             ( i == HeaderField::Date ||
               i == HeaderField::ReturnPath ||
               i == HeaderField::MessageId ||
               i == HeaderField::References ) ) {
            List< HeaderField >::Iterator it( d->fields );
            HeaderField * firstValid = 0;
            while ( it && !firstValid ) {
                if ( it->type() == i && it->valid() )
                    firstValid = it;
                ++it;
            }
            if ( firstValid ) {
                List< HeaderField >::Iterator it( d->fields );
                while ( it ) {
                    if ( it->type() == i && it != firstValid )
                        d->fields.take( it );
                    else
                        ++it;
                }
            }
        }
        ++i;
    }

    // If there is no Date field and this is an RFC822 header, we look
    // for a sensible date.
    
    if ( occurrences[(int)HeaderField::Date] == 0 && mode() == Rfc2822 ) {
        List< HeaderField >::Iterator it( d->fields );
        Date date;
        while ( it ) {
            // First, we take the date from the oldest valid-looking
            // Received field.
            if ( it->type() == HeaderField::Received ) {
                String v = it->value();
                int i = v.find( ';' );
                if ( i >= 0 && !v.mid( i+1 ).contains( ';' ) ) {
                    Date tmp;
                    tmp.setRfc822( v.mid( i+1 ) );
                    if ( tmp.valid() )
                        date = tmp;
                }
            }
            ++it;
        }

        if ( !date.valid() && p ) {
            Multipart * parent = p->parent();
            while ( parent && parent->header() &&
                    !( parent->header()->date() &&
                       parent->header()->date()->valid() ) )
                parent = parent->parent();
            if ( parent )
                date = *parent->header()->date();
        }

        if ( !date.valid() ) {
            // As last resort, use the current date, time and timezone.
            date.setCurrentTime();
        }

        add( "Date", date.rfc822() );
    }

    // If there is no From field, try to use either Return-Path or
    // Sender from this Header, or From, Return-Path or Sender from
    // the Header of the closest encompassing Multipart that has such
    // a field.

    if ( occurrences[(int)HeaderField::From] == 0 && mode() == Rfc2822 ) {
        Multipart * parent = p;
        Header * h = this;
        List<Address> * a = 0;
        while ( ( h || parent ) && !a ) {
            a = h->addresses( HeaderField::From );
            if ( !a || a->isEmpty() || a->first()->type() != Address::Normal )
                a = h->addresses( HeaderField::ReturnPath );
            if ( !a || a->isEmpty() || a->first()->type() != Address::Normal )
                a = h->addresses( HeaderField::Sender );
            if ( !a || a->isEmpty() || a->first()->type() != Address::Normal )
                a = 0;
            if ( parent )
                parent = parent->parent();
            if ( parent )
                h = parent->header();
            else
                h = 0;
        }
        if ( a )
            add( "From", a->first()->toString() );
    }
    
    // If there are several content-type fields, and they agree except
    // that one has options and the others not, remove the option-less
    // ones.

    if ( occurrences[(int)HeaderField::ContentType] > 1 ) {
        ContentType * ct = contentType();
        ContentType * other = ct;
        ContentType * good = 0;
        uint n = 0;
        bool bad = false;
        while ( other && !bad ) {
            if ( other->parameter( "charset" ).lower() == "us-ascii" )
                // XXX: wrong place for this code
                other->removeParameter( "charset" );
            if ( other->type() != ct->type() ||
                 other->subtype() != ct->subtype() ) {
                bad = true;
            }
            else if ( !other->parameters()->isEmpty() ) {
                if ( good )
                    bad = true;
                good = other;
            }
            other = (ContentType *)field( HeaderField::ContentType, ++n );
        }
        if ( good && !bad ) {
            List<HeaderField>::Iterator it( d->fields );
            while ( it ) {
                if ( it->type() == HeaderField::ContentType && it != good )
                    d->fields.take( it );
                else
                    ++it;
            }
        }
    }

    // If there is an unacceptable Received field somewhere, remove it
    // and all the older Received fields.

    if ( occurrences[(int)HeaderField::Received] > 0 ) {
        bool bad = false;
        List<HeaderField>::Iterator it( d->fields );
        while ( it ) {
            List<HeaderField>::Iterator h( it );
            ++it;
            if ( h->type() == HeaderField::Received ) {
                if ( !h->parsed() )
                    bad = true;
                if ( bad )
                    d->fields.take( h );
            }
        }
    }

    // For some header fields which can contain errors, our best
    // option is to remove them. A field belongs here if it can be
    // parsed somehow and can be dropped without changing the meaning
    // of the rest of the message.

    if ( occurrences[(int)HeaderField::ContentLocation] ||
         occurrences[(int)HeaderField::ContentId] ||
         occurrences[(int)HeaderField::MessageId] ) {
        List< HeaderField >::Iterator it( d->fields );
        while ( it ) {
            if ( ( it->type() == HeaderField::ContentLocation ||
                   it->type() == HeaderField::ContentId ||
                   it->type() == HeaderField::MessageId ) &&
                 !it->valid() ) {
                d->fields.take( it );
            }
            else {
                ++it;
            }
        }
    }

    d->verified = false;
}


/*! Returns the canonical text representation of this Header. */

String Header::asText() const
{
    String r;

    List< HeaderField >::Iterator it( d->fields );
    while ( it ) {
        appendField( r, it );
        ++it;
    }

    return r;
}


/*! Appends the string representation of the field \a hf to \a r. Does
    nothing if \a hf is 0.

    This function doesn't wrap. That's probably a bug. How to fix it?

    (The details of the function are liable to change.)
*/

void Header::appendField( String &r, HeaderField *hf ) const
{
    if ( !hf )
        return;

    r.append( hf->name() );
    r.append( ": " );
    r.append( hf->value() );
    r.append( crlf );
}


/*! Scans for fields containing unlabelled 8-bit content and encodes
    them using \a c.

    At the moment, this covers most unstructured fields. The exact
    scope of the function may change.
*/

void Header::fix8BitFields( class Codec * c )
{
    Utf8Codec utf8;
    List< HeaderField >::Iterator it( d->fields );
    while ( it ) {
        List< HeaderField >::Iterator f = it;
        ++it;
        if ( !f->parsed() &&
             ( f->type() == HeaderField::Subject ||
               f->type() == HeaderField::Comments ||
               f->type() == HeaderField::Keywords ||
               f->type() == HeaderField::ContentDescription ||
               // XXX: This should be more fine-grained:
               f->type() == HeaderField::Other ) )
        {
            String v = f->value();
            uint i = 0;
            while ( v[i] < 128 && v[i] > 0 )
                i++;
            if ( i < v.length() ) {
                c->setState( Codec::Valid );
                UString u = c->toUnicode( v );
                if ( c->wellformed() ) {
                    String s( utf8.fromUnicode( u ) );
                    f->setData( HeaderField::encodeText( s ) );
                }
                else if ( f->type() == HeaderField::Other ) {
                    d->fields.remove( f );
                }
                else if ( d->error.isEmpty() ) {
                    d->error = "Cannot parse header field " + f->name() +
                               " either as US-ASCII or " + c->name();
                }
            }
        }
        else if ( f->type() == HeaderField::ContentType ||
                  f->type() == HeaderField::ContentTransferEncoding ||
                  f->type() == HeaderField::ContentDisposition ||
                  f->type() == HeaderField::ContentLanguage )
        {
            MimeField * mf = (MimeField*)((HeaderField*)f);
            StringList::Iterator p( mf->parameters() );
            while ( p ) {
                StringList::Iterator a( p );
                ++p;
                String v = mf->parameter( *a );
                uint i = 0;
                while ( v[i] < 128 && v[i] > 0 )
                    i++;
                if ( i < v.length() ) {
                    // so. we have an argument containing unencoded
                    // 8-bit material. what to do?
                    c->setState( Codec::Valid );
                    UString u = c->toUnicode( v );
                    if ( c->wellformed() ) {
                        // we could parse it, so let's encode it using
                        // RFC 2047 encoding. later we probably want
                        // to use RFC 2231 encoding, but that's
                        // premature at the moment. don't know whether
                        // readers support it.
                        String hack( utf8.fromUnicode( u ) );
                        mf->addParameter( *a,
                                          HeaderField::encodeWord( hack ) );
                    }
                    else {
                        // unparsable. just remove it?
                        mf->removeParameter( *a );
                    }
                }
            }
        }
    }
}


/*! Notifies this Header that if no ContentType is set, its default
    type is \a t. The initial value is TextPlain.
*/

void Header::setDefaultType( DefaultType t )
{
    d->defaultType = t;
}


/*! Returns whatever was set using setDefaultType(), or TextPlain if
    setDefaultType() hasn't been called.
*/

Header::DefaultType Header::defaultType() const
{
    return d->defaultType;
}
