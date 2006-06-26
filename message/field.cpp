// Copyright Oryx Mail Systems GmbH. All enquiries to info@oryx.com, please.

#include "field.h"

#include "date.h"
#include "ustring.h"
#include "address.h"
#include "datefield.h"
#include "mimefields.h"
#include "addressfield.h"
#include "stringlist.h"
#include "parser.h"
#include "utf.h"


static struct {
    const char * name;
    HeaderField::Type type;
} fieldNames[] = {
    { "From", HeaderField::From },
    { "Resent-From", HeaderField::ResentFrom },
    { "Sender", HeaderField::Sender },
    { "Resent-Sender", HeaderField::ResentSender },
    { "Return-Path", HeaderField::ReturnPath },
    { "Reply-To", HeaderField::ReplyTo },
    { "To", HeaderField::To },
    { "Cc", HeaderField::Cc },
    { "Bcc", HeaderField::Bcc },
    { "Resent-To", HeaderField::ResentTo },
    { "Resent-Cc", HeaderField::ResentCc },
    { "Resent-Bcc", HeaderField::ResentBcc },
    { "Message-Id", HeaderField::MessageId },
    { "Resent-Message-Id", HeaderField::ResentMessageId },
    { "In-Reply-To", HeaderField::InReplyTo },
    { "References", HeaderField::References },
    { "Date", HeaderField::Date },
    { "Orig-Date", HeaderField::OrigDate },
    { "Resent-Date", HeaderField::ResentDate },
    { "Subject", HeaderField::Subject },
    { "Comments", HeaderField::Comments },
    { "Keywords", HeaderField::Keywords },
    { "Content-Type", HeaderField::ContentType },
    { "Content-Transfer-Encoding", HeaderField::ContentTransferEncoding },
    { "Content-Disposition", HeaderField::ContentDisposition },
    { "Content-Description", HeaderField::ContentDescription },
    { "Content-Language", HeaderField::ContentLanguage },
    { "Content-Location", HeaderField::ContentLocation },
    { "Content-Base", HeaderField::ContentBase },
    { "Content-Md5", HeaderField::ContentMd5 },
    { "Content-Id", HeaderField::ContentId },
    { "Mime-Version", HeaderField::MimeVersion },
    { "Received", HeaderField::Received },
    { 0, HeaderField::Other },
};


class HeaderFieldData
    : public Garbage
{
public:
    HeaderFieldData()
        : type( HeaderField::Other ), hasData( false ), hasValue( false )
    {}

    HeaderField::Type type;

    String name;
    String data;
    String value;
    String error;

    bool hasData;
    bool hasValue;
};


/*! \class HeaderField field.h
    This class models a single RFC 822 header field (e.g. From).

    This class is responsible for parsing and verifying header fields.
    Each field has a type(), name(), and value(). It is valid() if no
    error() was recorded during parsing by the various functions that
    parse() field values, e.g. parseText()).

    Users may obtain HeaderField objects only via create().
*/


/*! This private function is used by create() and assemble() to create a
    HeaderField object of a type appropriate to the given \a name.
*/

HeaderField *HeaderField::fieldNamed( const String &name )
{
    int i = 0;
    String n = name.headerCased();
    while ( fieldNames[i].name && n != fieldNames[i].name )
        i++;

    HeaderField::Type t = fieldNames[i].type;
    HeaderField *hf;

    switch ( t ) {
    case InReplyTo:
    case Subject:
    case Comments:
    case Keywords:
    case ContentDescription:
    case MimeVersion:
    case Received:
    case ContentLocation:
    case ContentBase:
    case ContentMd5:
    case Other:
        hf = new HeaderField( fieldNames[i].type );
        break;

    case From:
    case ResentFrom:
    case Sender:
    case ResentSender:
    case ReturnPath:
    case ReplyTo:
    case To:
    case Cc:
    case Bcc:
    case ResentTo:
    case ResentCc:
    case ResentBcc:
    case MessageId:
    case ContentId:
    case ResentMessageId:
    case References:
        hf = new AddressField( t );
        break;

    case Date:
    case OrigDate:
    case ResentDate:
        hf = new DateField( t );
        break;

    case ContentType:
        hf = new ::ContentType;
        break;

    case ContentTransferEncoding:
        hf = new ::ContentTransferEncoding;
        break;

    case ContentDisposition:
        hf = new ::ContentDisposition;
        break;

    case ContentLanguage:
        hf = new ::ContentLanguage;
        break;
    }

    hf->setName( n );
    return hf;
}


/*! This static function returns a pointer to a new HeaderField object
    that represents the given field \a name (case-insensitive) and its
    \a value (which is parsed appropriately). Neither \a name nor
    value may contain the separating ':'.

    This function is for use by the message parser.
*/

HeaderField *HeaderField::create( const String &name,
                                  const String &value )
{
    HeaderField *hf = fieldNamed( name );
    hf->parse( value );
    if ( hf->d->hasData )
        hf->d->hasValue = false;
    return hf;
}


/*! This static function returns a pointer to a new HeaderField object
    that represents the given field \a name (case-insensitive) and the
    field \a data retrieved from the database.

    This function is for use by the message fetcher.
*/

HeaderField *HeaderField::assemble( const String &name,
                                    const String &data )
{
    HeaderField *hf = fieldNamed( name );
    hf->reassemble( data );
    return hf;
}


/*! Constructs a HeaderField of type \a t. */

HeaderField::HeaderField( HeaderField::Type t )
    : d( new HeaderFieldData )
{
    d->type = t;
}


/*! Exists only to avoid compiler warnings. */

HeaderField::~HeaderField()
{
}


/*! Returns the type of this header field, as set by the constructor
    based on the name(). Unknown fields have type HeaderField::Other.
*/

HeaderField::Type HeaderField::type() const
{
    return d->type;
}


/*! Returns the canonical name of this header field. */

String HeaderField::name() const
{
    return d->name;
}


/*! Sets the name of this HeaderField to \a n. */

void HeaderField::setName( const String &n )
{
    d->name = n;
}


/*! Returns the RFC 2822 representation of this header field, with its
    contents properly folded and, if necessary, RFC 2047 encoded. This
    is a string we can hand out to clients.
*/

String HeaderField::value()
{
    if ( !d->hasValue )
        reassemble( d->data );

    return d->value;
}


/*! Sets the value of this HeaderField to \a s. */

void HeaderField::setValue( const String &s )
{
    d->hasValue = true;
    d->value = s;
    if ( d->data == d->value )
        d->data = s;
}


/*! Returns the contents of this header field in a representation that
    is meant for storage in the database (unfolded and UTF-8 encoded,
    with RFC 2047 encoded-words expanded). Only the Injector should
    need to use this function.

    Use value() if you want a valid RFC 2822 representation.
*/

String HeaderField::data()
{
    if ( !d->hasData )
        parse( d->value );

    return d->data;
}


/*! Sets the parsed representation of this HeaderField to \a s,
    overriding any value() that it had already. The next time value()
    is called, the canonical representation will be generated by
    reassemble().
*/

void HeaderField::setData( const String &s )
{
    d->hasValue = false;
    d->hasData = true;
    d->data = s;
    if ( d->value == d->data )
        d->value = s;
}


/*! Returns true if this header field is valid (or unparsed, as is the
    case for all unknown fields), and false if an error was detected
    during parsing.
*/

bool HeaderField::valid() const
{
    return d->error.isEmpty();
}


/*! Returns true if this field has been successfully parsed, and
    currently contains the database representation of its contents.
*/

bool HeaderField::parsed() const
{
    return d->hasData;
}


/*! Returns a suitable error message if this header field has a known
    parse error, and an empty string if the field is valid() or -- as
    is the case for all unknown fields -- not parsed.
*/

String HeaderField::error() const
{
    return d->error;
}


/*! Records the error text \a s encountered during parsing. */

void HeaderField::setError( const String &s )
{
    d->error = s;
}


/*! Every HeaderField subclass must define a parse() function that takes
    a string \a s from a message and sets the field data(). This default
    function handles fields that are not specially handled by subclasses
    using functions like parseText().
*/

void HeaderField::parse( const String &s )
{
    // Most fields share the same external and database representations.
    // For any that don't (cf. 2047) , we'll just setData() again later.
    setValue( s );

    switch ( d->type ) {
    case From:
    case ResentFrom:
    case Sender:
    case ReturnPath:
    case ResentSender:
    case To:
    case Cc:
    case Bcc:
    case ReplyTo:
    case ResentTo:
    case ResentCc:
    case ResentBcc:
    case MessageId:
    case ContentId:
    case ResentMessageId:
    case References:
    case Date:
    case OrigDate:
    case ResentDate:
    case ContentType:
    case ContentTransferEncoding:
    case ContentDisposition:
    case ContentLanguage:
        // These should be handled by their own parse().
        break;

    case Subject:
    case Comments:
        parseText( s );
        break;

    case MimeVersion:
        parseMimeVersion( s );
        break;

    case ContentLocation:
        parseContentLocation( s );
        break;

    case ContentBase:
        parseContentBase( s );

    case InReplyTo:
    case Keywords:
    case Received:
    case ContentMd5:
    case ContentDescription:
    case Other:
        parseOther( s );
        break;
    }
}


/*! Like parse(), this function must be reimplemented by subclasses. Its
    responsibility is to use \a s (as retrieved from the database) to
    set the field's value().
*/

void HeaderField::reassemble( const String &s )
{
    switch ( d->type ) {
    default:
        // setData() and fill in subclass structures.
        parse( s );
        // We assume that, for most fields, we can use the database
        // representation in an RFC 822 message.
        setValue( d->data );
        break;

    case Subject:
    case Comments:
        setValue( wrap( encodeText( s ) ) );
        break;
    }
}


/*! Parses the *text production from \a s, as modified to include
    encoded-words by RFC 2047. This is used to parse the Subject
    and Comments fields.
*/

void HeaderField::parseText( const String &s )
{
    Parser822 p( unwrap( s ) );
    String t( p.text() );
    if ( p.atEnd() )
        setData( t );
}


/*! Parses any (otherwise uncovered and presumably unstructured) field
    in \a s, and accepts it only if it does not contain NULs or 8-bit
    characters.
*/

void HeaderField::parseOther( const String &s )
{
    bool bad = false;

    uint i = 0;
    while ( i < s.length() ) {
        if ( s[i] == '\0' || s[i] > 127 )
            bad = true;
        i++;
    }

    if ( !bad )
        setData( s );
}


/*! Parses the Mime-Version field from \a s and records the first
    problem found.

    Only version 1.0 is accepted. Since some message generators
    incorrectly send comments or trailing garbage, this parser accepts
    them.
*/

void HeaderField::parseMimeVersion( const String &s )
{
    Parser822 p( s );
    p.comment();
    String v = p.dotAtom();
    if ( v != "1.0" )
        setError( "Could not parse '" + s.simplified() + "'" );
    setData( v );
}


/*! Parses the Content-Location header field in \a s and records the
    first problem found.
*/

void HeaderField::parseContentLocation( const String &s )
{
    uint e = s.length()-1;
    while ( e > 0 &&
            ( s[e] == ' ' || s[e] == '\t' || 
              s[e] == '\n' || s[e] == 'r' ) )
        e--;

    Parser822 p( s.mid( 0, e+1 ) );
    String t;

    p.whitespace();
    uint b = p.index();
    e = b;
    bool ok = true;
    String r;
    while ( ok ) {
        ok = false;
        char c = p.character();
        // RFC 1738 unreserved
        if ( ( c >= 'a' && c <= 'z' ) || // alpha
             ( c >= 'A' && c <= 'Z' ) ||
             ( c >= '0' && c <= '9' ) || // letter
             ( c == '$' || c ==  '-' || // safe
               c ==  '_' || c == '.' ||
               c == '+' ) ||
             ( c == '!' || c ==  '*' || // extra
               c ==  '\'' || c ==  '(' ||
               c ==  ')' || c ==  ',' ) ) {
            ok = true;
            r.append( c );
        }
        // RFC 1738 reserved
        else if ( c == ';' || c == '/' || c == '?' ||
                  c == ':' || c == '@' || c == '&' ||
                  c == '=' ) {
            ok = true;
            r.append( c );
        }
        // RFC 1738 escape
        else if ( c == '%' ) {
            String hex;
            hex.append( p.character() );
            hex.append( p.character() );
            (void)hex.number( &ok, 16 );
            r.append( '%' );
            r.append( hex.lower() );
        }
        // seen in real life, sent by buggy programs
        else if ( c == ' ' ) {
            ok = true;
            r.append( "%20" );
        }
        // and another kind of bug, except that in this case, is there
        // a right way? let's not flame programs which do this.
        else if ( c == '\r' || c == '\n' ) {
            ok = true;
            p.whitespace();
        }
        if ( ok )
            e = p.index();
    }
    p.whitespace();

    if ( !p.atEnd() )
        setError( "Junk at position " + fn( e ) + ": " + s.mid( e ) );
    setData( r );
}


/*! Parses the Content-Location header field in \a s and records the
    first problem found. Somewhat overflexibly assumes that if there
    is a colon, the URL is absolute, so it accepts -:/asr as a valid
    URL.
*/

void HeaderField::parseContentBase( const String & s )
{
    parseContentLocation( s );
    uint i = data().find( ':' );
    if ( i <= 0 )
        setError( "URL has no scheme" );
}


/*! Returns the name corresponding to the field type \a t, or 0 if there
    is no such field.
*/

const char *HeaderField::fieldName( HeaderField::Type t )
{
    uint i = 0;
    while ( fieldNames[i].name && fieldNames[i].type != t )
        i++;
    return fieldNames[i].name;
}


/*! Returns the Type corresponding to field name \a n, or 0 if \a n
    isn't known. */

uint HeaderField::fieldType( const String & n )
{
    String fn = n.headerCased();
    uint i = 0;
    while ( fieldNames[i].name && fn != fieldNames[i].name )
        i++;
    if ( fieldNames[i].name )
        return fieldNames[i].type;
    return 0;
}


/*! Returns an unwrapped version of the string \a s, where any CRLF-SP
    is replaced by a single space.

    XXX: We use this function to unwrap only Subject and Comments fields
    at the moment, since they're the only ones we transform. Unwrapping
    should eventually be handled in the higher-level parser instead. We
    must assume here that every [CR]LF is actually followed by an SP.
*/

String HeaderField::unwrap( const String &s )
{
    String t;

    uint last = 0;
    uint n = 0;
    while ( n < s.length() ) {
        if ( s[n] == '\012' ||
             ( s[n] == '\015' && s[n+1] == '\012' ) )
        {
            t.append( s.mid( last, n-last ) );
            if ( s[n] == '\015' )
                n++;
            if ( s[n+1] == ' ' || s[n+1] == '\t' ) {
                t.append( " " );
                n++;
            }
            last = n+1;
        }
        n++;
    }
    t.append( s.mid( last ) );

    return t;
}


/*! Returns a version of \a s with long lines wrapped according to the
    rules in RFC [2]822. This function is not static, because it needs
    to look at the field name.

    XXX: Well, except that we ignore the rules right now.
*/

String HeaderField::wrap( const String &s )
{
    String t;

    uint n = 0;
    uint last = 0;
    bool first = true;
    uint l = d->name.length() + 2;

    // We'll consider every space a potential wrapping point, and just
    // try to fit as many tokens onto each line as possible. This is a
    // cheap hack.

    do {
        String w;
        n = s.find( ' ', last );
        if ( n > 0 ) {
            w = s.mid( last, n-last );
            n++;
        }
        else {
            w = s.mid( last );
        }
        last = n;

        if ( first ) {
            first = false;
        }
        else if ( l + 1 + w.length() > 78 ) {
            t.append( "\015\012 " );
            l = 1;
        }
        else {
            t.append( " " );
            l += 1;
        }

        l += w.length();
        t.append( w );
    }
    while ( last > 0 );

    return t;
}


/*! This static function returns an RFC 2047 encoded-word representing
    \a w, which is assumed to be a UTF-8 encoded string.

    XXX: This doesn't split encoded-words to ensure that each
    encoded-word is shorter than 75 characters.
*/

String HeaderField::encodeWord( const String &w )
{
    if ( w.isEmpty() )
        return "";

    Utf8Codec u;
    UString us( u.toUnicode( w ) );
    Codec * c = Codec::byString( us );
    String cw( c->fromUnicode( us ) );

    String t( "=?" );
    t.append( c->name() );
    t.append( "?" );
    String qp = cw.eQP( true );
    String b64 = cw.e64();
    if ( qp.length() <= b64.length() ) {
        t.append( "q?" );
        t.append( qp );
    }
    else {
        t.append( "b?" );
        t.append( b64 );
    }
    t.append( "?=" );

    return t;
}


/*! This static function returns the RFC 2047-encoded version of \a s,
    which is assumed to be a UTF-8 encoded string.
*/

String HeaderField::encodeText( const String &s )
{
    String t;

    uint n = 0;
    uint last = 0;
    bool encoded = false;

    do {
        if ( !t.isEmpty() )
            t.append( " " );

        String w;
        n = s.find( ' ', last );
        if ( n > 0 ) {
            w = s.mid( last, n-last );
            n++;
        }
        else {
            w = s.mid( last );
        }
        last = n;

        uint i = 0;
        while ( i < w.length() && w[i] > 0 && w[i] < 128 )
            i++;
        if ( i >= w.length() ) {
            t.append( w );
            encoded = false;
        }
        else {
            if ( encoded )
                w = " " + w;
            encoded = true;
            t.append( encodeWord( w ) );
        }
    }
    while ( last > 0 );

    return t;
}


/*! This static function returns the RFC 2047-encoded version of \a s,
    which is assumed to be a UTF-8 encoded phrase.
*/

String HeaderField::encodePhrase( const String &s )
{
    String t;
    StringList::Iterator it( StringList::split( ' ', s ) );

    while ( it ) {
        String w( *it );
        ++it;

        if ( !t.isEmpty() )
            t.append( " " );

        if ( w.boring() ) {
            t.append( w );
        }
        else {
            while ( it && !(*it).boring() ) {
                w.append( " " );
                w.append( *it );
                ++it;
            }
            t.append( encodeWord( w ) );
        }
    }

    return t;
}
