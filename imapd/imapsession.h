// Copyright Oryx Mail Systems GmbH. All enquiries to info@oryx.com, please.

#ifndef IMAPSESSION_H
#define IMAPSESSION_H

#include "session.h"

class Mailbox;
class IMAP;


class ImapSession
    : public Session
{
public:
    ImapSession( IMAP *, Mailbox *, bool );
    ~ImapSession();

    IMAP * imap() const;

    void emitExpunge( uint );
    void emitExists( uint );

    void setAnnotateUpdates( bool );
    bool annotateUpdates() const;

private:
    class ImapSessionData * d;
};


#endif
