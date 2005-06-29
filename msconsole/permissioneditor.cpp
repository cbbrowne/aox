// Copyright Oryx Mail Systems GmbH. All enquiries to info@oryx.com, please.

#include <limits.h>

#include "cstring.h"

#include "permissioneditor.h"

#include "permissions.h"
#include "mailbox.h"
#include "query.h"

#include <qapplication.h>
#include <qpushbutton.h>
#include <qcheckbox.h>
#include <qptrlist.h>
#include <qlayout.h>
#include <qlabel.h>


class PermissionEditorData
{
public:
    PermissionEditorData()
        : add( 0 ), mailbox( 0 ), tll( 0 )
    {}

    QPushButton * add;
    Mailbox * mailbox;
    QGridLayout * tll;
    QPtrList<PermissionEditorRow> rows;
};


/*! \class PermissionEditor permissioneditor.h

    The PermissionEditor class presents the RFC 2086 access control
    list for a mailbox on-screen, allows editing them and finally
    writing them back to the database.

    The class has no real public API - it receives its commands from
    the user via the UI, not from the rest of the program.
*/


/*! Constructs a PermissionEditor visually located in \a parent. After
    construction the editor shows nothing, since there is no Mailbox
    yet.
*/

PermissionEditor::PermissionEditor( QWidget * parent )
    : QWidget( parent ), d( new PermissionEditorData )
{
    d->add = new QPushButton( tr( "Add" ), this );
    setBackgroundColor( Qt::green );
}


/*! Deletes whatever was shown and starts showing the ACL for \a
    mailbox.
*/

void PermissionEditor::setMailbox( Mailbox * mailbox )
{
    if ( mailbox == d->mailbox )
        return;
    delete d->tll;
    d->tll = 0;
    d->rows.setAutoDelete( true );
    d->rows.clear();

    d->mailbox = mailbox;
    (void)new PermissionEditorFetcher( this, mailbox );

    // Mailbox::owner() really should return User*
    PermissionEditorRow * r = new PermissionEditorRow( this );
    r->label()->setText( QString::fromLatin1( "(owner)" ) );
    uint i = 0;
    while( i < Permissions::NumRights ) {
        QCheckBox * b = r->button( (Permissions::Right)i );
        b->setChecked( true );
        b->setEnabled( false );
        i++;
    }
    d->rows.append( r );
}


/*! Returns a pointer to the currently displayed Mailbox. The value is
    0 initially, meaning that no mailbox is being displayed.
*/

Mailbox * PermissionEditor::mailbox() const
{
    return d->mailbox;
}


/*! Creates a new QLayout object to lay out all the current ACL rows.

*/

void PermissionEditor::setupLayout()
{
    delete d->tll;
    d->tll = new QGridLayout( this, 4, 1 + d->rows.count(), 6 );
    QPtrListIterator<PermissionEditorRow> it( d->rows );
    uint col = 0;
    PermissionEditorRow * r = 0;
    while ( (r=it.current()) != 0 ) {
        ++it;
        if ( !it ) {
            d->tll->addWidget( d->add, 0, col );
            d->add->show();
        }
        uint i = 0;
        while ( i < Permissions::NumRights ) {
            d->tll->addWidget( r->button( (Permissions::Right)i ),
                               i+1, col );
            r->button( (Permissions::Right)i )->show();
            i++;
        }
        col++;
    }
    QApplication::postEvent( parentWidget(),
                             new QEvent( QEvent::LayoutHint ) );
    QApplication::sendEvent( this,
                             new QEvent( QEvent::LayoutHint ) );
    dumpObjectTree();
}


class PermissionEditorRowData
{
public:
    PermissionEditorRowData()
        : label( 0 )
    {
        uint i = 0;
        while( i < Permissions::NumRights )
            buttons[i++] = 0;
    }

    QCheckBox *buttons[Permissions::NumRights];
    QLabel * label;
};


/*! \class PermissionEditorRow permissioneditor.h

    PermissionEditorRow is a container for the widgets needed to
    control a single row in the grid used by PermissionEditor. It
    exists only to provide teh button() and label() functions.
*/


/*! Constructs the widgets necessary for a single Permissions object
    (or for "anyone"). Each widget created has \a parent as parent.
*/

PermissionEditorRow::PermissionEditorRow( PermissionEditor * parent )
    : QObject( parent ), d( new PermissionEditorRowData )
{
    d->label = new QLabel( parent );
    uint i = 0;
    while( i < Permissions::NumRights )
        d->buttons[i++] = new QCheckBox( parent );
}


/*! Deletes the buttons and label held in this row. This is probably
    the only Oryx destructor that's actually necessary, and it's
    necessary because many other pointers to the child widgets need to
    be removed.
*/

PermissionEditorRow::~PermissionEditorRow()
{
    delete d->label;
    uint i = 0;
    while( i < Permissions::NumRights )
        delete d->buttons[i++];
    d = 0;
}


/*! Returns a pointer to the button displaying and controlling \a
    right.
*/

QCheckBox * PermissionEditorRow::button( Permissions::Right right ) const
{
    return d->buttons[right];
}


/*! Returns a pointer to the label at the top of the column. */

QLabel * PermissionEditorRow::label() const
{
    return d->label;
}


class PermissionEditorFetcherData
{
public:
    PermissionEditorFetcherData(): q( 0 ), e( 0 ), m( 0 ) {}
    Query * q;
    PermissionEditor * e;
    Mailbox * m;
    String anyone;
};


/*! Creates an object to fetch all the ACLs for \a m and call
    the PermissionEditor::add() of \a e for each.
*/

PermissionEditorFetcher::PermissionEditorFetcher( PermissionEditor * e,
                                                  Mailbox * m )
    : EventHandler(), d( new PermissionEditorFetcherData )
{
    d->q = new Query( "select identifier, rights "
                      "from permissions where mailbox=$1 "
                      "order by identifier",
                      this );
    d->q->bind( 1, m->id() );
    d->q->execute();
    d->e = e;
    d->m = m;
}


void PermissionEditorFetcher::execute()
{
    qDebug( "fetcher %s", d->m->name().cstr() );
    Row * r;
    while ( (r=d->q->nextRow()) != 0 ) {
        String rights( r->getString( "rights" ) );
        String id( r->getString( "identifier" ) );
        if ( d->m != d->e->mailbox() )
            ;
        else if ( id == "anyone" )
            d->anyone = rights;
        else
            d->e->add( id, rights );
    }
    if ( !d->q->done() || d->m != d->e->mailbox() )
        return;
    if ( !d->anyone.isEmpty() )
        d->e->add( "anyone", d->anyone );
    d->e->setupLayout();
}


/*! Creates and shows a PermissionEditorRow indicating that \a
    identifier has \a rights, and allowing change.
*/

void PermissionEditor::add( const String & identifier, const String & rights )
{
    PermissionEditorRow * r = new PermissionEditorRow( this );
    r->label()->setText( QString::fromUtf8( identifier.data(),
                                            identifier.length() ) );
    uint i = 0;
    while( i < Permissions::NumRights ) {
        char rc = Permissions::rightChar( (Permissions::Right)i );
        if ( rights.find( rc ) >= 0 )
            r->button( (Permissions::Right)i )->setChecked( true );
        i++;
    }
    d->rows.append( r );
}
