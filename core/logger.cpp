// Copyright Oryx Mail Systems GmbH. All enquiries to info@oryx.com, please.

#include "logger.h"


static Logger *logger = 0;


/*! \class Logger logger.h
    Abstract base class for things that log messages.

    All subclasses of Logger must implement the send() virtual function,
    and take responsibility for correctly logging the lines of text that
    are passed to it.

    A program creates one instance of a Logger subclass at startup and
    uses Logger::global() to process any messages sent to a Log object
    thereafter.
*/

/*! Stores the address of the newly-created Logger for global(). */

Logger::Logger()
{
    ::logger = this;
}


/*! \fn void Logger::send( const String &s )

    This virtual function logs \a s in a manner decided by the
    subclass. \a s is assumed to already have a trailing CRLF.
*/


/*! This virtual destructor exists only to ensure that global() doesn't
    return a bad pointer.
*/

Logger::~Logger()
{
    ::logger = 0;
}


/*! Returns a pointer to the global Logger. */

Logger *Logger::global()
{
    return ::logger;
}
