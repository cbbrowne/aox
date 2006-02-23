/****************************************************************************
*																			*
*						  Unix Randomness-Gathering Code					*
*	Copyright Peter Gutmann, Paul Kendall, and Chris Wedgwood 1996-2005		*
*																			*
****************************************************************************/

/* This module is part of the cryptlib continuously seeded pseudorandom
   number generator.  For usage conditions, see random.c */

/* Define the following to print diagnostic information on where randomness
   is coming from */

/* #define DEBUG_RANDOM	*/

/* Comment out the following to disable printed warnings about possible
   conflicts with signal handlers and other system services */

#define DEBUG_CONFLICTS

/* General includes */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "crypt.h"

/* Unix and Unix-like systems share the same makefile, make sure that the
   user isn't trying to use the Unix randomness code under a non-Unix (but
   Unix-like) system.  This would be pretty unlikely since the makefile
   automatically adjusts itself based on the environment it's running in,
   but we use the following safety check just in case.  We have to perform
   this check before we try any includes because Unix and the Unix-like
   systems don't have the same header files */

#ifdef __BEOS__
  #error For the BeOS build you need to edit $MISCOBJS in the makefile to use 'beos' and not 'unix'
#endif /* BeOS has its own randomness-gathering file */
#ifdef __ECOS__
  #error For the eCOS build you need to edit $MISCOBJS in the makefile to use 'ecos' and not 'unix'
#endif /* eCOS has its own randomness-gathering file */
#ifdef __ITRON__
  #error For the uITRON build you need to edit $MISCOBJS in the makefile to use 'itron' and not 'unix'
#endif /* uITRON has its own randomness-gathering file */
#ifdef __PALMSOURCE__
  #error For the PalmOS build you need to edit $MISCOBJS in the makefile to use 'palmos' and not 'unix'
#endif /* PalmOS has its own randomness-gathering file */
#ifdef __RTEMS__
  #error For the RTEMS build you need to edit $MISCOBJS in the makefile to use 'rtems' and not 'unix'
#endif /* RTEMS has its own randomness-gathering file */
#if defined( __TANDEM_NSK__ ) || defined( __TANDEM_OSS__ )
  #error For the Tandem build you need to edit $MISCOBJS in the makefile to use 'tandem' and not 'unix'
#endif /* Tandem OSS has its own randomness-gathering file */
#if defined( __VXWORKS__ )
  #error For the VxWorks build you need to edit $MISCOBJS in the makefile to use 'vxworks' and not 'unix'
#endif /* VxWorks has its own randomness-gathering file */

/* OS-specific includes */

#if defined( __hpux ) && ( OSVERSION >= 10 )
  #define _XOPEN_SOURCE_EXTENDED
#endif /* Workaround for inconsistent PHUX networking headers */
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#if !( defined( __QNX__ ) || defined( __MVS__ ) )
  #include <sys/errno.h>
  #include <sys/ipc.h>
#endif /* !( QNX || MVS ) */
#include <sys/time.h>	/* SCO and SunOS need this before resource.h */
#ifndef __QNX__
  #if defined( _MPRAS ) && !( defined( _XOPEN_SOURCE ) && \
	  defined( __XOPEN_SOURCE_EXTENDED ) )
	/* On MP-RAS 3.02, the X/Open test macros must be set to include
	   getrusage(). */
	#define _XOPEN_SOURCE 1
	#define _XOPEN_SOURCE_EXTENDED 1
	#define MPRAS_XOPEN_DEFINES
  #endif /* MP-RAS */
  #include <sys/resource.h>
  #if defined( MPRAS_XOPEN_DEFINES )
	#undef _XOPEN_SOURCE
	#undef _XOPEN_SOURCE_EXTENDED
	#undef MPRAS_XOPEN_DEFINES
  #endif /* MP-RAS */
#endif /* QNX */
#if defined( _AIX ) || defined( __QNX__ )
  #include <sys/select.h>
#endif /* Aches || QNX */
#ifdef _AIX
  #include <sys/systemcfg.h>
#endif /* Aches */
#ifdef __CYGWIN__
  #include <signal.h>
  #include <sys/ipc.h>
  #include <sys/sem.h>
  #include <sys/shm.h>
#endif /* CYGWIN */
#if !( defined( __QNX__ ) || defined( __CYGWIN__ ) )
  #include <sys/shm.h>
#endif /* QNX || Cygwin */
#ifndef __MVS__
  #if 0		/* Deprecated - 6/4/03 */
	#include <sys/signal.h>
  #else
	#include <signal.h>
  #endif /* 0 */
#endif /* !MVS */
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>	/* Verschiedene komische Typen */
#include <sys/un.h>
#if defined( __hpux ) && ( OSVERSION == 9 )
  #include <vfork.h>
#endif /* __hpux 9.x, after that it's in unistd.h */
#include <sys/wait.h>
/* #include <kitchensink.h> */

#if defined( sun ) || defined( __ultrix__ ) || defined( __hpux )
  #define HAS_VFORK
#endif /* Unixen that have vfork() */

/* Crays and QNX 4.x don't have a rlimit/rusage so we have to fake the
   functions and structs */

#if defined( _CRAY ) || \
	( defined( __QNX__ ) && OSVERSION <= 4 )
  #define setrlimit( x, y )
  struct rlimit { int dummy1, dummy2; };
  struct rusage { int dummy; };
#endif /* Systems without rlimit/rusage */

/* The size of the intermediate buffer used to accumulate polled data */

#define RANDOM_BUFSIZE	4096

/* The structure containing information on random-data sources.  Each record
   contains the source and a relative estimate of its usefulness (weighting)
   which is used to scale the number of kB of output from the source (total =
   data_bytes / usefulness).  Usually the weighting is in the range 1-3 (or 0
   for especially useless sources), resulting in a usefulness rating of 1...3
   for each kB of source output (or 0 for the useless sources).

   If the source is constantly changing (certain types of network statistics
   have this characteristic) but the amount of output is small, the weighting
   is given as a negative value to indicate that the output should be treated
   as if a minimum of 1K of output had been obtained.  If the source produces
   a lot of output then the scale factor is fractional, resulting in a
   usefulness rating of < 1 for each kB of source output.

   In order to provide enough randomness to satisfy the requirements for a
   slow poll, we need to accumulate at least 20 points of usefulness (a
   typical system should get about 30 points).

   Some potential options are missed out because of special considerations.
   pstat -i and pstat -f can produce amazing amounts of output (the record is
   600K on an Oracle server) that floods the buffer and doesn't yield
   anything useful (apart from perhaps increasing the entropy of the vmstat
   output a bit), so we don't bother with this.  pstat in general produces
   quite a bit of output, but it doesn't change much over time, so it gets
   very low weightings.  netstat -s produces constantly-changing output but
   also produces quite a bit of it, so it only gets a weighting of 2 rather
   than 3.  The same holds for netstat -in, which gets 1 rather than 2.  In
   addition some of the lower-ranked sources are either rather heavyweight
   or of low value, and frequently duplicate information obtained by other
   means like kstat or /procfs.  To avoid waiting for a long time for them
   to produce output, we only use them if earlier, quicker sources aren't
   available.

   Some binaries are stored in different locations on different systems so
   alternative paths are given for them.  The code sorts out which one to run
   by itself, once it finds an exectable somewhere it moves on to the next
   source.  The sources are arranged roughly in their order of usefulness,
   occasionally sources that provide a tiny amount of relatively useless
   data are placed ahead of ones that provide a large amount of possibly
   useful data because another 100 bytes can't hurt, and it means the buffer
   won't be swamped by one or two high-output sources. All the high-output
   sources are clustered towards the end of the list for this reason.  Some
   binaries are checked for in a certain order, for example under Slowaris
   /usr/ucb/ps understands aux as an arg, but the others don't.  Some systems
   have conditional defines enabling alternatives to commands that don't
   understand the usual options but will provide enough output (in the form
   of error messages) to look like they're the real thing, causing
   alternative options to be skipped (we can't check the return either
   because some commands return peculiar, non-zero status even when they're
   working correctly).

   In order to maximise use of the buffer, the code performs a form of run-
   length compression on its input where a repeated sequence of bytes is
   replaced by the occurrence count mod 256.  Some commands output an awful
   lot of whitespace, this measure greatly increases the amount of data we
   can fit in the buffer.

   When we scale the weighting using the SC() macro, some preprocessors may
   give a division by zero warning for the most obvious expression 'weight ?
   1024 / weight : 0' (and gcc 2.7.2.2 dies with a division by zero trap), so
   we define a value SC_0 that evaluates to zero when fed to '1024 / SC_0' */

#define SC( weight )	( 1024 / weight )	/* Scale factor */
#define SC_0			16384				/* SC( SC_0 ) evalutes to 0 */

static struct RI {
	const char *path;		/* Path to check for existence of source */
	const char *arg;		/* Args for source */
	const int usefulness;	/* Usefulness of source */
	FILE *pipe;				/* Pipe to source as FILE * */
	int pipeFD;				/* Pipe to source as FD */
	pid_t pid;				/* pid of child for waitpid() */
	int length;				/* Quantity of output produced */
	const BOOLEAN hasAlternative;	/* Whether source has alt.location */
	} dataSources[] = {
	/* Sources that are always polled */
	{ "/bin/vmstat", "-s", SC( -3 ), NULL, 0, 0, 0, TRUE },
	{ "/usr/bin/vmstat", "-s", SC( -3 ), NULL, 0, 0, 0, FALSE },
	{ "/bin/vmstat", "-c", SC( -3 ), NULL, 0, 0, 0, TRUE },
	{ "/usr/bin/vmstat", "-c", SC( -3 ), NULL, 0, 0, 0, FALSE },
	{ "/usr/bin/pfstat", NULL, SC( -2 ), NULL, 0, 0, 0, FALSE },
	{ "/bin/vmstat", "-i", SC( -2 ), NULL, 0, 0, 0, TRUE },
	{ "/usr/bin/vmstat", "-i", SC( -2 ), NULL, 0, 0, 0, FALSE },
#if defined( _AIX ) || defined( __SCO_VERSION__ )
	{ "/usr/bin/vmstat", "-f", SC( -1 ), NULL, 0, 0, 0, FALSE },
#endif /* OS-specific extensions to vmstat */
	{ "/usr/ucb/netstat", "-s", SC( 2 ), NULL, 0, 0, 0, TRUE },
	{ "/usr/bin/netstat", "-s", SC( 2 ), NULL, 0, 0, 0, TRUE },
	{ "/usr/sbin/netstat", "-s", SC( 2 ), NULL, 0, 0, 0, TRUE },
	{ "/bin/netstat", "-s", SC( 2 ), NULL, 0, 0, 0, TRUE },
	{ "/usr/etc/netstat", "-s", SC( 2 ), NULL, 0, 0, 0, FALSE },
	{ "/usr/bin/nfsstat", NULL, SC( 2 ), NULL, 0, 0, 0, FALSE },
	{ "/usr/ucb/netstat", "-m", SC( -1 ), NULL, 0, 0, 0, TRUE },
	{ "/usr/bin/netstat", "-m", SC( -1 ), NULL, 0, 0, 0, TRUE },
	{ "/usr/sbin/netstat", "-m", SC( -1 ), NULL, 0, 0, 0, TRUE },
	{ "/bin/netstat", "-m", SC( -1 ), NULL, 0, 0, 0, TRUE },
	{ "/usr/etc/netstat", "-m", SC( -1 ), NULL, 0, 0, 0, FALSE },
	{ "/usr/ucb/netstat", "-in", SC( -1 ), NULL, 0, 0, 0, TRUE },
	{ "/usr/bin/netstat", "-in", SC( -1 ), NULL, 0, 0, 0, TRUE },
	{ "/usr/sbin/netstat", "-in", SC( -1 ), NULL, 0, 0, 0, TRUE },
	{ "/bin/netstat", "-in", SC( -1 ), NULL, 0, 0, 0, TRUE },
	{ "/usr/etc/netstat", "-in", SC( -1 ), NULL, 0, 0, 0, FALSE },
#ifndef __SCO_VERSION__
	{ "/usr/sbin/snmp_request", "localhost public get 1.3.6.1.2.1.7.1.0", SC( -1 ), NULL, 0, 0, 0, FALSE }, /* UDP in */
	{ "/usr/sbin/snmp_request", "localhost public get 1.3.6.1.2.1.7.4.0", SC( -1 ), NULL, 0, 0, 0, FALSE }, /* UDP out */
	{ "/usr/sbin/snmp_request", "localhost public get 1.3.6.1.2.1.4.3.0", SC( -1 ), NULL, 0, 0, 0, FALSE }, /* IP ? */
	{ "/usr/sbin/snmp_request", "localhost public get 1.3.6.1.2.1.6.10.0", SC( -1 ), NULL, 0, 0, 0, FALSE }, /* TCP ? */
	{ "/usr/sbin/snmp_request", "localhost public get 1.3.6.1.2.1.6.11.0", SC( -1 ), NULL, 0, 0, 0, FALSE }, /* TCP ? */
	{ "/usr/sbin/snmp_request", "localhost public get 1.3.6.1.2.1.6.13.0", SC( -1 ), NULL, 0, 0, 0, FALSE }, /* TCP ? */
#else
	{ "/usr/sbin/snmpstat", "-an localhost public", SC( SC_0 ), NULL, 0, 0, 0, FALSE },
	{ "/usr/sbin/snmpstat", "-in localhost public", SC( SC_0 ), NULL, 0, 0, 0, FALSE }, /* Subset of netstat info */
	{ "/usr/sbin/snmpstat", "-Sn localhost public", SC( SC_0 ), NULL, 0, 0, 0, FALSE },
#endif /* SCO/UnixWare vs.everything else */
	{ "/usr/bin/mpstat", NULL, SC( 1 ), NULL, 0, 0, 0, FALSE },
	{ "/usr/bin/w", NULL, SC( 1 ), NULL, 0, 0, 0, TRUE },
	{ "/usr/bsd/w", NULL, SC( 1 ), NULL, 0, 0, 0, FALSE },
	{ "/usr/bin/df", NULL, SC( 1 ), NULL, 0, 0, 0, TRUE },
	{ "/bin/df", NULL, SC( 1 ), NULL, 0, 0, 0, FALSE },
	{ "/usr/sbin/portstat", NULL, SC( 1 ), NULL, 0, 0, 0, FALSE },
	{ "/usr/bin/iostat", NULL, SC( SC_0 ), NULL, 0, 0, 0, FALSE },
	{ "/usr/bin/uptime", NULL, SC( SC_0 ), NULL, 0, 0, 0, TRUE },
	{ "/usr/bsd/uptime", NULL, SC( SC_0 ), NULL, 0, 0, 0, FALSE },
	{ "/usr/bin/vmstat", "-f", SC( SC_0 ), NULL, 0, 0, 0, TRUE },
	{ "/bin/vmstat", "-f", SC( SC_0 ), NULL, 0, 0, 0, TRUE },
	{ "/usr/ucb/netstat", "-n", SC( 0.5 ), NULL, 0, 0, 0, TRUE },
	{ "/usr/bin/netstat", "-n", SC( 0.5 ), NULL, 0, 0, 0, TRUE },
	{ "/usr/sbin/netstat", "-n", SC( 0.5 ), NULL, 0, 0, 0, TRUE },
	{ "/bin/netstat", "-n", SC( 0.5 ), NULL, 0, 0, 0, TRUE },
	{ "/usr/etc/netstat", "-n", SC( 0.5 ), NULL, 0, 0, 0, FALSE },

	/* End-of-lightweight-sources section marker */
	{ "", NULL, SC( SC_0 ), NULL, 0, 0, 0, FALSE },

	/* Potentially heavyweight or low-value sources that are only polled if
	   alternative sources aren't available */
#if defined( __sgi ) || defined( __hpux )
	{ "/bin/ps", "-el", SC( 0.3 ), NULL, 0, 0, 0, TRUE },
#endif /* SGI || PHUX */
	{ "/usr/sbin/ntptrace", "-r2 -t1 -nv", SC( -1 ), NULL, 0, 0, 0, FALSE },
	{ "/usr/ucb/ps", "aux", SC( 0.3 ), NULL, 0, 0, 0, TRUE },
	{ "/usr/bin/ps", "aux", SC( 0.3 ), NULL, 0, 0, 0, TRUE },
	{ "/bin/ps", "aux", SC( 0.3 ), NULL, 0, 0, 0, FALSE },
	{ "/usr/bin/ipcs", "-a", SC( 0.5 ), NULL, 0, 0, 0, TRUE },
	{ "/bin/ipcs", "-a", SC( 0.5 ), NULL, 0, 0, 0, FALSE },
							/* Unreliable source, depends on system usage */
	{ "/etc/pstat", "-p", SC( 0.5 ), NULL, 0, 0, 0, TRUE },
	{ "/bin/pstat", "-p", SC( 0.5 ), NULL, 0, 0, 0, FALSE },
	{ "/etc/pstat", "-S", SC( 0.2 ), NULL, 0, 0, 0, TRUE },
	{ "/bin/pstat", "-S", SC( 0.2 ), NULL, 0, 0, 0, FALSE },
	{ "/etc/pstat", "-v", SC( 0.2 ), NULL, 0, 0, 0, TRUE },
	{ "/bin/pstat", "-v", SC( 0.2 ), NULL, 0, 0, 0, FALSE },
	{ "/etc/pstat", "-x", SC( 0.2 ), NULL, 0, 0, 0, TRUE },
	{ "/bin/pstat", "-x", SC( 0.2 ), NULL, 0, 0, 0, FALSE },
	{ "/etc/pstat", "-t", SC( 0.1 ), NULL, 0, 0, 0, TRUE },
	{ "/bin/pstat", "-t", SC( 0.1 ), NULL, 0, 0, 0, FALSE },
							/* pstat is your friend */
#ifndef __SCO_VERSION__
	{ "/usr/sbin/sar", "-AR", SC( 0.05 ), NULL, 0, 0, 0, FALSE }, /* Only updated hourly */
#endif /* SCO/UnixWare */
	{ "/usr/bin/last", "-n 50", SC( 0.3 ), NULL, 0, 0, 0, TRUE },
#ifdef __sgi
	{ "/usr/bsd/last", "-50", SC( 0.3 ), NULL, 0, 0, 0, FALSE },
#endif /* SGI */
#ifdef __hpux
	{ "/etc/last", "-50", SC( 0.3 ), NULL, 0, 0, 0, FALSE },
#endif /* PHUX */
	{ "/usr/bsd/last", "-n 50", SC( 0.3 ), NULL, 0, 0, 0, FALSE },
#ifdef sun
	{ "/usr/bin/showrev", "-a", SC( 0.1 ), NULL, 0, 0, 0, FALSE },
	{ "/usr/sbin/swap", "-l", SC( SC_0 ), NULL, 0, 0, 0, FALSE },
	{ "/usr/sbin/prtconf", "-v", SC( SC_0 ), NULL, 0, 0, 0, FALSE },
#endif /* SunOS/Slowaris */
	{ "/usr/sbin/psrinfo", NULL, SC( SC_0 ), NULL, 0, 0, 0, FALSE },
	{ "/usr/local/bin/lsof", "-lnwP", SC( 0.3 ), NULL, 0, 0, 0, FALSE },
							/* Output is very system and version-dependent */
	{ "/usr/sbin/snmp_request", "localhost public get 1.3.6.1.2.1.5.1.0", SC( 0.1 ), NULL, 0, 0, 0, FALSE }, /* ICMP ? */
	{ "/usr/sbin/snmp_request", "localhost public get 1.3.6.1.2.1.5.3.0", SC( 0.1 ), NULL, 0, 0, 0, FALSE }, /* ICMP ? */
	{ "/etc/arp", "-a", SC( 0.1 ), NULL, 0, 0, 0, TRUE },
	{ "/usr/etc/arp", "-a", SC( 0.1 ), NULL, 0, 0, 0, TRUE },
	{ "/usr/bin/arp", "-a", SC( 0.1 ), NULL, 0, 0, 0, TRUE },
	{ "/usr/sbin/arp", "-a", SC( 0.1 ), NULL, 0, 0, 0, FALSE },
	{ "/usr/sbin/ripquery", "-nw 1 127.0.0.1", SC( 0.1 ), NULL, 0, 0, 0, FALSE },
	{ "/bin/lpstat", "-t", SC( 0.1 ), NULL, 0, 0, 0, TRUE },
	{ "/usr/bin/lpstat", "-t", SC( 0.1 ), NULL, 0, 0, 0, TRUE },
	{ "/usr/ucb/lpstat", "-t", SC( 0.1 ), NULL, 0, 0, 0, FALSE },
	{ "/usr/bin/tcpdump", "-c 5 -efvvx", SC( 1 ), NULL, 0, 0, 0, FALSE },
							/* This is very environment-dependant.  If
							   network traffic is low, it'll probably time
							   out before delivering 5 packets, which is OK
							   because it'll probably be fixed stuff like ARP
							   anyway */
	{ "/usr/sbin/advfsstat", "-b usr_domain", SC( SC_0 ), NULL, 0, 0, 0, FALSE },
	{ "/usr/sbin/advfsstat", "-l 2 usr_domain", SC( 0.5 ), NULL, 0, 0, 0, FALSE },
	{ "/usr/sbin/advfsstat", "-p usr_domain", SC( SC_0 ), NULL, 0, 0, 0, FALSE },
							/* This is a complex and screwball program.  Some
							   systems have things like rX_dmn, x = integer,
							   for RAID systems, but the statistics are
							   pretty dodgy */
#if 0
	/* The following aren't enabled since they're somewhat slow and not very
	   unpredictable, however they give an indication of the sort of sources
	   you can use (for example the finger might be more useful on a
	   firewalled internal network) */
	{ "/usr/bin/finger", "@ml.media.mit.edu", SC( 0.9 ), NULL, 0, 0, 0, FALSE },
	{ "/usr/local/bin/wget", "-O - http://lavarand.sgi.com/block.html", SC( 0.9 ), NULL, 0, 0, 0, FALSE },
	{ "/bin/cat", "/usr/spool/mqueue/syslog", SC( 0.9 ), NULL, 0, 0, 0, FALSE },
#endif /* 0 */

	/* End-of-sources marker */
	{ NULL, NULL, 0, NULL, 0, 0, 0, FALSE }
	};

/* Variables to manage the child process that fills the buffer */

static pid_t gathererProcess = 0;/* The child process that fills the buffer */
static BYTE *gathererBuffer;	/* Shared buffer for gathering random noise */
static int gathererMemID;		/* ID for shared memory */
static int gathererBufSize;		/* Size of the shared memory buffer */
static struct sigaction gathererOldHandler;	/* Previous signal handler */

/* The struct at the start of the shared memory buffer used to communicate
   information from the child to the parent */

typedef struct {
	int usefulness;				/* Usefulness of data in buffer */
	int noBytes;				/* No.of bytes in buffer */
	} GATHERER_INFO;

/****************************************************************************
*																			*
*								Utility Functions							*
*																			*
****************************************************************************/

#if defined( __hpux ) && ( OSVERSION == 0 || OSVERSION == 9 )

/* PHUX 9.x doesn't support getrusage in libc (wonderful...).  The reason we
   check for a version 0 as well as 9 is that some PHUX unames report the
   version as 09 rather than 9, which looks like version 0 when you take the
   first digit */

#include <syscall.h>

static int getrusage( int who, struct rusage *rusage )
	{
	return( syscall( SYS_getrusage, who, rusage ) );
	}
#endif /* __hpux */

#if defined( __hpux )

/* PHUX doesn't have wait4() either so we emulate it with waitpid() and
   getrusage() */

pid_t wait4( pid_t pid, int *status, int options, struct rusage *rusage )
	{
	const pid_t waitPid = waitpid( pid, status, options );

	getrusage( RUSAGE_CHILDREN, rusage );
	return( waitPid );
	}
#endif /* PHUX */

/* Cray Unicos and QNX 4.x have neither wait4() nor getrusage, so we fake
   it */

#if defined( _CRAY ) || ( defined( __QNX__ ) && OSVERSION <= 4 )

pid_t wait4( pid_t pid, int *status, int options, struct rusage *rusage )
	{
	return( waitpid( pid, status, options ) );
	}
#endif /* Cray Unicos || QNX 4.x */

/* Under SunOS 4.x popen() doesn't record the pid of the child process.  When
   pclose() is called, instead of calling waitpid() for the correct child, it
   calls wait() repeatedly until the right child is reaped.  The problem whit
   this behaviour is that this reaps any other children that happen to have
   died at that moment, and when their pclose() comes along, the process hangs
   forever.

   This behaviour may be related to older SVR3-compatible SIGCLD handling in
   which, under the SIG_IGN disposition, the status of the child was discarded
   (i.e. no zombies were generated) so that when the parent called wait() it
   would block until all children terminated, whereupon wait() would return -1
   with errno set to ECHILD.

   The fix for this problem is to use a wrapper for popen()/pclose() that
   saves the pid in the dataSources structure (code adapted from GNU-libc's
   popen() call).  Doing our own popen() has other advantages as well, for
   example we use the more secure execl() to run the child instead of the
   dangerous system().

   Aut viam inveniam aut faciam */

static FILE *my_popen( struct RI *entry )
	{
	int pipedes[ 2 ];
	FILE *stream;

	/* Create the pipe.  Note that under QNX the pipe resource manager
	   'pipe' must be running in order to use pipes */
	if( pipe( pipedes ) < 0 )
		return( NULL );

	/* Fork off the child ("vfork() is like an OS orgasm.  All OSes want to
	   do it, but most just end up faking it" - Chris Wedgwood).  If your OS
	   supports it, you should try and use vfork() here because it's rather
	   more efficient and has guaranteed copy-on-write semantics that prevent
	   cryptlib object data from being copied to the child.  Many modern
	   Unixen use COW for forks anyway (e.g. Linux, for which vfork() is just
	   an alias for fork()), so we get most of the benefits of vfork() with a
	   plain fork(), however there's another problem with fork that isn't
	   fixed by COW.  Any large program, when forked, requires (at least
	   temporarily) a lot of address space.  That is, when the process is
	   forked the system needs to allocate many virtual pages (backing store)
	   even if those pages are never used.  If the system doesn't have enough
	   swap space available to support this, the fork() will fail when the
	   system tries to reserver backing store for pages that are never
	   touched.  Even in non-large processes this can cause problems when (as
	   with the randomness-gatherer) many children are forked at once.

	   In the absence of threads the use of pcreate() (which only requires
	   backing store for the new processes' stack, not the entire process)
	   would do the trick, however pcreate() isn't compatible with threads,
	   which makes it of little use for the default thread-enabled cryptlib
	   build

	   Although OSF/1 has vfork(), it has nasty interactions with threading
	   and can cause other problems with handling of children, so we don't
	   use it */
#ifdef HAS_VFORK
	entry->pid = vfork();
#else
	entry->pid = fork();
#endif /* Unixen that have vfork() */
	if( entry->pid == ( pid_t ) -1 )
		{
		/* The fork failed */
		close( pipedes[ 0 ] );
		close( pipedes[ 1 ] );
		return( NULL );
		}

	if( entry->pid == ( pid_t ) 0 )
		{
		struct passwd *passwd;

		/* We are the child.  Make the read side of the pipe be stdout */
		if( dup2( pipedes[ STDOUT_FILENO ], STDOUT_FILENO ) < 0 )
			exit( 127 );

		/* If we're root, give up our permissions to make sure we don't
		   inadvertently read anything sensitive.  If the getpwnam() fails
		   (this can happen if we're chrooted with no "nobody" entry in the
		   local passwd file) we default to -1, which is usually nobody.  We
		   don't check whether this succeeds since it's not a major security
		   problem but just a precaution */
		if( geteuid() == 0 )
			{
			static uid_t gathererUID = ( uid_t ) -1, gathererGID = ( uid_t ) -1;

			if( gathererProcess == ( uid_t ) -1 && \
				( passwd = getpwnam( "nobody" ) ) != NULL )
				{
				gathererUID = passwd->pw_uid;
				gathererGID = passwd->pw_gid;
				}
#if 0		/* Not available on some OSes */
			setgid( gathererGID );
			setegid( gathererGID );
			setuid( gathererUID );
			seteuid( gathererUID );
#else
			setregid( gathererGID, gathererGID );
			setreuid( gathererUID, gathererUID );
#endif /* 0 */
			}

		/* Close the pipe descriptors */
		close( pipedes[ STDIN_FILENO ] );
		close( pipedes[ STDOUT_FILENO ] );

		/* Try and exec the program */
		execl( entry->path, entry->path, entry->arg, NULL );

		/* Die if the exec failed.  Since vfork() doesn't duplicate the stdio
		   buffers (or anything else for that matter), we have to use _exit()
		   rather than exit() to ensure that the shutdown actions don't upset
		   the parent's state */
#ifdef HAS_VFORK
		_exit( 127 );
#else
		exit( 127 );
#endif /* Unixen that have vfork() */
		}

	/* We are the parent.  Close the irrelevant side of the pipe and open the
	   relevant side as a new stream.  Mark our side of the pipe to close on
	   exec, so new children won't see it */
	close( pipedes[ STDOUT_FILENO ] );
	fcntl( pipedes[ STDIN_FILENO ], F_SETFD, FD_CLOEXEC );
	stream = fdopen( pipedes[ STDIN_FILENO ], "r" );
	if( stream == NULL )
		{
		int savedErrno = errno;

		/* The stream couldn't be opened or the child structure couldn't be
		   allocated.  Kill the child and close the other side of the pipe */
		kill( entry->pid, SIGKILL );
		close( pipedes[ STDOUT_FILENO ] );
		waitpid( entry->pid, NULL, 0 );
		entry->pid = 0;
		errno = savedErrno;
		return( NULL );
		}

	return( stream );
	}

static int my_pclose( struct RI *entry, struct rusage *rusage )
	{
	pid_t pid;
	int status = 0;

	/* Close the pipe */
	fclose( entry->pipe );
	entry->pipe = NULL;

	/* Wait for the child to terminate, ignoring the return value from the
	   process because some programs return funny values that would result
	   in the input being discarded even if they executed successfully.
	   This isn't a problem because the result data size threshold will
	   filter out any programs that exit with a usage message without
	   producing useful output */
	do
		/* We use wait4() instead of waitpid() to get the last bit of
		   entropy data, the resource usage of the child */
		pid = wait4( entry->pid, NULL, 0, rusage );
	while( pid == -1 && errno == EINTR );
	if( pid != entry->pid )
		status = -1;
	entry->pid = 0;
	return( status );
	}

/****************************************************************************
*																			*
*									Fast Poll								*
*																			*
****************************************************************************/

/* Fast poll - not terribly useful.  SCO has a gettimeofday() prototype but
   no actual system call that implements it, and no getrusage() at all, so
   we use times() instead */

#if defined( _CRAY ) || defined( _M_XENIX )
  #include <sys/times.h>
#endif /* Systems without getrusage() */

void fastPoll( void )
	{
	RANDOM_STATE randomState;
	BYTE buffer[ RANDOM_BUFSIZE ];
#if !( defined( _CRAY ) || defined( _M_XENIX ) )
	struct timeval tv;
  #if !( defined( __QNX__ ) && OSVERSION <= 4 )
	struct rusage rusage;
  #endif /* !QNX 4.x */
#else
	struct tms tms;
#endif /* Systems without getrusage() */
#ifdef _AIX
	timebasestruct_t cpuClockInfo;
#endif /* Aches */
#if ( defined( sun ) && ( OSVERSION >= 5 ) )
	hrtime_t hrTime;
#endif /* Slowaris */
#if ( defined( __QNX__ ) && OSVERSION >= 5 )
	uint64_t clockCycles;
#endif /* QNX */

	initRandomData( randomState, buffer, RANDOM_BUFSIZE );

	/* Mix in the process ID.  This doesn't change per process but will
	   change if the process forks, ensuring that the parent and child data
	   differs from the parent */
	addRandomValue( randomState, getpid() );

#if !( defined( _CRAY ) || defined( _M_XENIX ) )
	gettimeofday( &tv, NULL );
	addRandomValue( randomState, tv.tv_sec );
	addRandomValue( randomState, tv.tv_usec );

	/* SunOS 5.4 has the function call but no prototypes for it, if you're
	   compiling this under 5.4 you'll have to copy the header files from 5.5
	   or something similar */
  #if !( defined( __QNX__ ) && OSVERSION <= 4 )
	getrusage( RUSAGE_SELF, &rusage );
	addRandomData( randomState, &rusage, sizeof( struct rusage ) );
  #endif /* !QNX 4.x */
#else
	/* Merely a subset of getrusage(), but it's the best that we can do.  On
	   Crays it provides access to the hardware clock, so the data is quite
	   good */
	times( &tms );
	addRandomData( randomState, &tms, sizeof( struct tms ) );
#endif /* Systems without getrusage() */
#ifdef _AIX
	/* Add the value of the nanosecond-level CPU clock or time base register */
	read_real_time( &cpuClockInfo, sizeof( timebasestruct_t ) );
	addRandomData( randomState, &cpuClockInfo, sizeof( timebasestruct_t ) );
#endif /* _AIX */
#if ( defined( sun ) && ( OSVERSION >= 5 ) )
	/* Read the Sparc %tick register (equivalent to the P5 TSC).  This is
	   only readable by the kernel by default, although Solaris 8 and newer
	   make it readable in user-space.  To do this portably, we use
	   gethrtime(), which does the same thing */
	hrTime = gethrtime();
	addRandomData( randomState, &hrTime, sizeof( hrtime_t ) );
#endif /* Slowaris */
#if ( defined( __QNX__ ) && OSVERSION >= 5 )
	/* Return the output of RDTSC or its equivalent on other systems.  We
	   don't worry about locking the thread to a CPU since we're not using
	   it for timing (in fact being run on another CPU helps the entropy) */
	clockCycles = ClockCycles();
	addRandomData( randomState, &clockCycles, sizeof( uint64_t ) );
#endif /* QNX */

	/* Flush any remaining data through */
	endRandomData( randomState, 0 );
	}

/****************************************************************************
*																			*
*									Slow Poll								*
*																			*
****************************************************************************/

/* Slowaris-specific slow poll using kstat, which provides kernel statistics.
   Since there can be a hundred or more of these, we use a larger-than-usual
   intermediate buffer to cut down on kernel traffic */

#if ( defined( sun ) && ( OSVERSION >= 5 ) )

#define USE_KSTAT
#include <kstat.h>

#define BIG_RANDOM_BUFSIZE	( RANDOM_BUFSIZE * 2 )

static int getKstatData( void )
	{
	kstat_ctl_t *kc;
	kstat_t *ksp;
	RANDOM_STATE randomState;
	BYTE buffer[ BIG_RANDOM_BUFSIZE ];
	int noEntries = 0, quality;

	/* Try and open a kernel stats handle */
	if( ( kc = kstat_open() ) == NULL )
		return( FALSE );

	initRandomData( randomState, buffer, BIG_RANDOM_BUFSIZE );

	/* Walk down the chain of stats reading each one.  Since some of the
	   stats can be rather lengthy, we optionally send them directly to
	   the randomness pool rather than using the accumulator */
	for( ksp = kc->kc_chain; ksp != NULL; ksp = ksp->ks_next )
		{
		if( kstat_read( kc, ksp, NULL ) == -1 || \
			!ksp->ks_data_size )
			continue;
		addRandomData( randomState, ksp, sizeof( kstat_t ) );
		if( ksp->ks_data_size > BIG_RANDOM_BUFSIZE )
			{
			RESOURCE_DATA msgData;

			setMessageData( &msgData, ksp->ks_data, ksp->ks_data_size );
			krnlSendMessage( SYSTEM_OBJECT_HANDLE, IMESSAGE_SETATTRIBUTE_S,
							 &msgData, CRYPT_IATTRIBUTE_ENTROPY );
			}
		else
			addRandomData( randomState, ksp->ks_data, ksp->ks_data_size );
		noEntries++;
		}
	kstat_close( kc );

	/* Flush any remaining data through and produce an estimate of its
	   value.  We require that we get at least 50 entries and give them a
	   maximum value of 35 to ensure that some data is still coming from
	   other sources */
	quality = ( noEntries > 50 ) ? 35 : 0;
	endRandomData( randomState, quality );

	return( quality );
	}
#endif /* Slowaris */

/* SYSV /proc interface, which provides assorted information that usually
   has to be obtained the hard way via a slow poll */

#if ( defined( sun ) && ( OSVERSION >= 5 ) ) || defined( __osf__ ) || \
	  defined( __alpha__ ) || defined( __linux__ )

#define USE_PROC
#include <sys/procfs.h>

static int getProcData( void )
	{
#ifdef PIOCSTATUS
	prstatus_t prStatus;
#endif /* PIOCSTATUS */
#ifdef PIOCPSINFO
	prpsinfo_t prMisc;
#endif /* PIOCPSINFO */
#ifdef PIOCUSAGE
	prusage_t prUsage;
#endif /* PIOCUSAGE */
	RANDOM_STATE randomState;
	BYTE buffer[ RANDOM_BUFSIZE ];
	char fileName[ 128 ];
	int fd, noEntries = 0, quality;

	/* Try and open the process info for this process */
	sPrintf( fileName, "/proc/%d", getpid() );
	if( ( fd = open( fileName, O_RDONLY ) ) == -1 )
		return;

	initRandomData( randomState, buffer, RANDOM_BUFSIZE );

	/* Get the process status information, misc information, and resource
	   usage */
#ifdef PIOCSTATUS
	if( ioctl( fd, PIOCSTATUS, &prStatus ) != -1 )
		{
#ifdef DEBUG_RANDOM
		printf( "rndunix: PIOCSTATUS contributed %d bytes.\n",
				sizeof( prstatus_t ) );
#endif /* DEBUG_RANDOM */
		addRandomData( randomState, &prStatus, sizeof( prstatus_t ) );
		noEntries++;
		}
#endif /* PIOCSTATUS */
#ifdef PIOCPSINFO
	if( ioctl( fd, PIOCPSINFO, &prMisc ) != -1 )
		{
#ifdef DEBUG_RANDOM
		printf( "rndunix: PIOCPSINFO contributed %d bytes.\n",
				sizeof( prpsinfo_t ) );
#endif /* DEBUG_RANDOM */
		addRandomData( randomState, &prMisc, sizeof( prpsinfo_t ) );
		noEntries++;
		}
#endif /* PIOCPSINFO */
#ifdef PIOCUSAGE
	if( ioctl( fd, PIOCUSAGE, &prUsage ) != -1 )
		{
#ifdef DEBUG_RANDOM
		printf( "rndunix: PIOCUSAGE contributed %d bytes.\n",
				sizeof( prusage_t ) );
#endif /* DEBUG_RANDOM */
		addRandomData( randomState, &prUsage, sizeof( prusage_t ) );
		noEntries++;
		}
#endif /* PIOCUSAGE */
	close( fd );

	/* Flush any remaining data through and produce an estimate of its
	   value.  We require that at least two of the sources exist and accesses
	   to them succeed, and give them a relatively low value since they're
	   returning information that has some overlap with that returned by the
	   general slow poll (although there's also a lot of low-level stuff
	   present that the slow poll doesn't get) */
	quality = ( noEntries > 2 ) ? 10 : 0;
	endRandomData( randomState, quality );

	return( quality );
	}
#endif /* Slowaris || OSF/1 || Linux */

/* /dev/random interface */

#define DEVRANDOM_BYTES		128

static int getDevRandomData( void )
	{
	RESOURCE_DATA msgData;
	BYTE buffer[ DEVRANDOM_BYTES ];
#if defined( __APPLE__ ) || ( defined( __FreeBSD__ ) && OSVERSION == 5 )
	static const int quality = 50;	/* See comment below */
#else
	static const int quality = 75;
#endif /* Mac OS X || FreeBSD 5.x */
	int randFD, noBytes;

	/* Check whether there's a /dev/random present */
	if( ( randFD = open( "/dev/urandom", O_RDONLY ) ) < 0 )
		return( 0 );

	/* Read data from /dev/urandom, which won't block (although the quality
	   of the noise is less).  We only assign this a 75% quality factor to
	   ensure that we still get randomness from other sources as well.  Under
	   FreeBSD 5.x and OS X, the /dev/random implementation is broken, using
	   a pretend dev-random implemented with Yarrow and a 160-bit pool, so
	   we only assign a 50% quality factor. These generators also lie about
	   entropy, with both /random and /urandom being the same PRNG-based
	   implementation */
	noBytes = read( randFD, buffer, DEVRANDOM_BYTES );
	close( randFD );
	if( noBytes < 1 )
		return( 0 );
#ifdef DEBUG_RANDOM
	printf( "rndunix: /dev/random contributed %d bytes.\n",
			DEVRANDOM_BYTES );
#endif /* DEBUG_RANDOM */
	setMessageData( &msgData, buffer, DEVRANDOM_BYTES );
	krnlSendMessage( SYSTEM_OBJECT_HANDLE, IMESSAGE_SETATTRIBUTE_S, &msgData,
					 CRYPT_IATTRIBUTE_ENTROPY );
	zeroise( buffer, DEVRANDOM_BYTES );
	if( noBytes < DEVRANDOM_BYTES )
		return( 0 );
	krnlSendMessage( SYSTEM_OBJECT_HANDLE, IMESSAGE_SETATTRIBUTE,
					 ( void * ) &quality, CRYPT_IATTRIBUTE_ENTROPY_QUALITY );
	return( quality );
	}

/* egd/prngd interface */

static int getEGDdata( void )
	{
	static const char *egdSources[] = {
		"/var/run/egd-pool", "/dev/egd-pool", "/etc/egd-pool", NULL };
	RESOURCE_DATA msgData;
	BYTE buffer[ DEVRANDOM_BYTES ];
	static const int quality = 75;
	int egdIndex, sockFD, noBytes = CRYPT_ERROR, status;

	/* Look for the egd/prngd output.  We re-search each time because,
	   unlike /dev/random, it's both a user-level process and a movable
	   feast, so it can disappear and reappear at a different location
	   between runs */
	sockFD = socket( AF_UNIX, SOCK_STREAM, 0 );
	if( sockFD < 0 )
		return( 0 );
	for( egdIndex = 0; egdSources[ egdIndex ] != NULL; egdIndex++ )
		{
		struct sockaddr_un sockAddr;

		memset( &sockAddr, 0, sizeof( struct sockaddr_un ) );
		sockAddr.sun_family = AF_UNIX;
		strcpy( sockAddr.sun_path, egdSources[ egdIndex ] );
		if( connect( sockFD, ( struct sockaddr * ) &sockAddr,
					 sizeof( struct sockaddr_un ) ) >= 0 )
			break;
		}
	if( egdSources[ egdIndex ] == NULL )
		{
		close( sockFD );
		return( 0 );
		}

	/* Read up to 128 bytes of data from the source:
		write:	BYTE 1 = read data nonblocking
				BYTE DEVRANDOM_BYTES = count
		read:	BYTE returned bytes
				BYTE[] data
	   As with /dev/random we only assign this a 75% quality factor to
	   ensure that we still get randomness from other sources as well */
	buffer[ 0 ] = 1;
	buffer[ 1 ] = DEVRANDOM_BYTES;
	status = write( sockFD, buffer, 2 );
	if( status == 2 )
		{
		status = read( sockFD, buffer, 1 );
		noBytes = buffer[ 0 ];
		if( status != 1 || noBytes < 0 || noBytes > DEVRANDOM_BYTES )
			status = -1;
		else
			status = read( sockFD, buffer, noBytes );
		}
	close( sockFD );
	if( ( status < 0 ) || ( status != noBytes ) )
		return( 0 );

	/* Send the data to the pool */
#ifdef DEBUG_RANDOM
	printf( "rndunix: EGD (%s) contributed %d bytes.\n",
			egdSources[ egdIndex ], DEVRANDOM_BYTES );
#endif /* DEBUG_RANDOM */
	setMessageData( &msgData, buffer, noBytes );
	krnlSendMessage( SYSTEM_OBJECT_HANDLE, IMESSAGE_SETATTRIBUTE_S, &msgData,
					 CRYPT_IATTRIBUTE_ENTROPY );
	zeroise( buffer, DEVRANDOM_BYTES );
	if( noBytes < DEVRANDOM_BYTES )
		return( 0 );
	krnlSendMessage( SYSTEM_OBJECT_HANDLE, IMESSAGE_SETATTRIBUTE,
					 ( void * ) &quality, CRYPT_IATTRIBUTE_ENTROPY_QUALITY );
	return( quality );
	}

/* Named process information /procfs interface */

static int getProcFSdata( void )
	{
	static const char *procSources[] = {
		"/proc/interrupts", "/proc/loadavg", "/proc/locks", "/proc/meminfo",
		"/proc/net/dev", "/proc/net/ipx", "/proc/net/netstat",
		"/proc/net/rt_cache_stat", "/proc/net/snmp",
		"/proc/net/softnet_stat", "/proc/net/tcp", "/proc/net/udp",
		"/proc/slabinfo", "/proc/stat", "/proc/sys/fs/inode-state",
		"/proc/sys/fs/file-nr", "/proc/sys/fs/dentry-state",
		"/proc/sysvipc/msg", "/proc/sysvipc/sem", "/proc/sysvipc/shm",
		NULL };
	RESOURCE_DATA msgData;
	BYTE buffer[ 1024 ];
	int procIndex, procFD, procCount = 0, quality;

	/* Read the first 1K of data from some of the more useful sources (most
	   of these produce far less than 1K output) */
	for( procIndex = 0; procSources[ procIndex ] != NULL; procIndex++ )
		{
		if( ( procFD = open( procSources[ procIndex ], O_RDONLY ) ) >= 0 )
			{
			const int count = read( procFD, buffer, 1024 );

			if( count > 16 )
				{
#ifdef DEBUG_RANDOM
				printf( "rndunix: %s contributed %d bytes.\n",
						procSources[ procIndex ], count );
#endif /* DEBUG_RANDOM */
				setMessageData( &msgData, buffer, count );
				krnlSendMessage( SYSTEM_OBJECT_HANDLE,
								 IMESSAGE_SETATTRIBUTE_S, &msgData,
								 CRYPT_IATTRIBUTE_ENTROPY );
				procCount++;
				}
			close( procFD );
			}
		}
	zeroise( buffer, 1024 );
	if( procCount < 5 )
		return( 0 );

	/* Produce an estimate of the data's value.  We require that we get at
	   least 5 entries and give them a maximum value of 50 to ensure that
	   some data is still coming from other sources */
	quality = min( procCount * 3, 50 );
	krnlSendMessage( SYSTEM_OBJECT_HANDLE, IMESSAGE_SETATTRIBUTE,
					 ( void * ) &quality, CRYPT_IATTRIBUTE_ENTROPY_QUALITY );
	return( quality );
	}

/* Get data from an entropy source */

static int getEntropySourceData( struct RI *dataSource, BYTE *bufPtr,
								 const int bufSize, int *bufPos )
	{
	int bufReadPos = 0, bufWritePos = 0;
	size_t noBytes;

	/* Try and get more data from the source.  If we get zero bytes, the
	   source has sent us all it has */
	if( ( noBytes = fread( bufPtr, 1, bufSize, dataSource->pipe ) ) <= 0 )
		{
		struct rusage rusage;
		int total = 0;

		/* If there's a problem, exit */
		if( my_pclose( dataSource, &rusage ) != 0 )
			return( 0 );

		/* Try and estimate how much entropy we're getting from a data
		   source */
		if( dataSource->usefulness != 0 )
			{
			if( dataSource->usefulness < 0 )
				/* Absolute rating, 1024 / -n */
				total = 1025 / -dataSource->usefulness;
			else
				/* Relative rating, 1024 * n */
				total = dataSource->length / dataSource->usefulness;
			}
#ifdef DEBUG_RANDOM
		printf( "rndunix: %s %s contributed %d bytes (compressed), "
				"usefulness = %d.\n", dataSource->path,
				( dataSource->arg != NULL ) ? dataSource->arg : "",
				dataSource->length, total );
#endif /* DEBUG_RANDOM */

		/* Copy in the last bit of entropy data, the resource usage of the
		   popen()ed child */
		if( sizeof( struct rusage ) < bufSize )
			{
			memcpy( bufPtr, &rusage, sizeof( struct rusage ) );
			*bufPos += sizeof( struct rusage );
			}

		return( total );
		}

	/* Run-length compress the input byte sequence */
	while( bufReadPos < noBytes )
		{
		const int ch = bufPtr[ bufReadPos ];

		/* If it's a single byte or we're at the end of the buffer, just
		   copy it over */
		if( bufReadPos >= bufSize - 1 || ch != bufPtr[ bufReadPos + 1 ] )
			{
			bufPtr[ bufWritePos++ ] = ch;
			bufReadPos++;
			}
		else
			{
			int count = 0;

			/* It's a run of repeated bytes, replace them with the byte
			   count mod 256 */
			while( bufReadPos < noBytes && ( ch == bufPtr[ bufReadPos ] ) )
				{
				count++;
				bufReadPos++;
				}
			bufPtr[ bufWritePos++ ] = count;
			}
		}

	/* Remember the number of (compressed) bytes of input that we obtained */
	*bufPos += bufWritePos;
	dataSource->length += noBytes;

	return( 0 );
	}

/* Unix slow poll.  If a few of the randomness sources create a large amount
   of output then the slowPoll() stops once the buffer has been filled (but
   before all the randomness sources have been sucked dry) so that the
   'usefulness' factor remains below the threshold.  For this reason the
   gatherer buffer has to be fairly sizeable on moderately loaded systems.

   An alternative strategy, suggested by Massimo Squillace, is to use a
   chunk of shared memory protected by a semaphore, with the first
   sizeof( int ) bytes at the start serving as a high-water mark.  The
   process forks and waitpid()'s for the child's pid.  The child forks all
   the entropy-gatherers and exits, allowing the parent to resume execution.
   The child's children are inherited by init (double-fork paradigm), when
   each one is finished it takes the semaphore, writes data to the shared
   memory segment at the given offset, updates the offset, releases the
   semaphore again, and exits, to be reaped by init.

   The parent monitors the shared memory offset and when enough data is
   available takes the semaphore, grabs the data, and releases the shared
   memory area and semaphore.  If any children are still running they'll get
   errors when they try to access the semaphore or shared memory and
   silently exit.

   This approach has the advantage that all of the forked processes are
   managed by init rather than having the parent have to wait for them, but
   the disadvantage that the end-of-job handling is rather less rigorous.
   An additional disadvantage is that the existing code has had a lot of
   real-world testing, which would have to be repeated for any new version */

#define SHARED_BUFSIZE		49152	/* Usually about 25K are filled */
#define SLOWPOLL_TIMEOUT	30		/* Time out after 30 seconds */

void slowPoll( void )
	{
	GATHERER_INFO *gathererInfo;
	BOOLEAN moreSources;
	struct timeval tv;
	struct rlimit rl = { 0, 0 };
	struct sigaction act;
	fd_set fds;
	time_t startTime;
#if defined( _CRAY ) || defined( __hpux ) || defined( _M_XENIX ) || \
	defined( __aux )
  #if defined( _SC_PAGESIZE )
	const int pageSize = sysconf( _SC_PAGESIZE );
  #elif defined( _SC_PAGE_SIZE )
	const int pageSize = sysconf( _SC_PAGE_SIZE );
  #else
	const int pageSize = 4096;
  #endif /* Systems without getpagesize() */
#else
	const int pageSize = getpagesize();
#endif /* Unix variant-specific brokenness */
#if defined( __hpux )
	size_t maxFD = 0;
#else
	int maxFD = 0;
#endif /* OS-specific brokenness */
	int extraEntropy = 0, usefulness = 0;
	int fd, bufPos, i, value;

	/* Make sure we don't start more than one slow poll at a time */
	krnlEnterMutex( MUTEX_RANDOMPOLLING );
	if( gathererProcess	)
		{
		krnlExitMutex( MUTEX_RANDOMPOLLING );
		return;
		}

	/* Some systems provide further info that we can grab before we start
	   the slow poll.  If this is sufficient to satisfy the polling
	   requirements, we don't have to try the full slow poll (a number of
	   these additional sources, things like procfs and kstats, duplicate
	   the sources polled in the slow poll anyway, so we're not adding
	   much by polling these extra sources if we've already got the data
	   directly) */
	extraEntropy += getDevRandomData();
	if( !access( "/proc/interrupts", R_OK ) )
		extraEntropy += getProcFSdata();
	extraEntropy += getEGDdata();
#ifdef USE_KSTAT
	extraEntropy += getKstatData();
#endif /* USE_KSTAT */
#ifdef USE_PROC
	extraEntropy += getProcData();
#endif /* USE_PROC */
#ifdef DEBUG_RANDOM
	printf( "rndunix: Got %d additional entropy from direct sources.\n",
			extraEntropy );
	if( extraEntropy >= 100 )
		puts( "  (Skipping full slowpoll since sufficient entropy is "
			  "available)." );
#endif /* DEBUG_RANDOM */
	if( extraEntropy >= 100 )
		{
		/* We got enough entropy from the additional sources, we don't
		   have to go through with the full (heavyweight) poll */
		krnlExitMutex( MUTEX_RANDOMPOLLING );
		return;
		}

	/* QNX 4.x doesn't have SYSV shared memory, so we can't go beyond this
	   point */
#if !( defined( __QNX__ ) && OSVERSION <= 4 )

	/* Reset the SIGCHLD handler to the system default.  This is necessary
	   because if the program that cryptlib is a part of installs its own
	   SIGCHLD handler, it will end up reaping the cryptlib children before
	   cryptlib can.  As a result, my_pclose() will call waitpid() on a
	   process that has already been reaped by the installed handler and
	   return an error, so the read data won't be added to the randomness
	   pool */
	memset( &act, 0, sizeof( act ) );
	act.sa_handler = SIG_DFL;
	sigemptyset( &act.sa_mask );
	if( sigaction( SIGCHLD, &act, &gathererOldHandler ) < 0 )
		{
		/* This assumes that stderr is open, i.e. that we're not a daemon
		   (this should be the case at least during the development/debugging
		   stage) */
		fprintf( stderr, "cryptlib: sigaction() failed, errno = %d, "
				 "file = %s, line = %d.\n", errno, __FILE__, __LINE__ );
		abort();
		}

	/* Check for handler override. */
	if( gathererOldHandler.sa_handler != SIG_DFL && \
		gathererOldHandler.sa_handler != SIG_IGN )
		{
#ifdef DEBUG_CONFLICTS
		/* We overwrote the caller's handler, warn them about this */
		fprintf( stderr, "cryptlib: Conflicting SIGCHLD handling detected "
				 "in randomness polling code,\nfile " __FILE__ ", line %d.  "
				 "See the source code for more\ninformation.\n", __LINE__ );
#endif /* DEBUG_CONFLICTS */
		}

	/* Set up the shared memory */
	gathererBufSize = ( SHARED_BUFSIZE / pageSize ) * ( pageSize + 1 );
	if( ( gathererMemID = shmget( IPC_PRIVATE, gathererBufSize,
								  IPC_CREAT | 0600 ) ) == -1 || \
		( gathererBuffer = ( BYTE * ) shmat( gathererMemID,
											 NULL, 0 ) ) == ( BYTE * ) -1 )
		{
		/* There was a problem obtaining the shared memory, warn the user
		   and exit */
#ifdef _CRAY
		if( errno == ENOSYS )
			/* Unicos supports shmget/shmat, but older Crays don't implement
			   it and return ENOSYS */
			fprintf( stderr, "cryptlib: SYSV shared memory required for "
					 "random number gathering isn't\n  supported on this "
					 "type of Cray hardware (ENOSYS),\n  file = %s, line = "
					 "%d.\n", __FILE__, __LINE__ );
#endif /* Cray */
#ifdef DEBUG_CONFLICTS
		fprintf( stderr, "cryptlib: shmget()/shmat() failed, errno = %d, "
				 "file = %s, line = %d.\n", errno, __FILE__, __LINE__ );
#endif /* DEBUG_CONFLICTS */
		if( gathererMemID != -1 )
			shmctl( gathererMemID, IPC_RMID, NULL );
		if( gathererOldHandler.sa_handler != SIG_DFL )
			sigaction( SIGCHLD, &gathererOldHandler, NULL );
		krnlExitMutex( MUTEX_RANDOMPOLLING );
		return; /* Something broke */
		}

	/* At this point we have a possible race condition since we need to set
	   the gatherer PID value inside the mutex but messing with mutexes
	   across a fork() is somewhat messy.  To resolve this, we set the PID
	   to a nonzero value (which marks it as busy) and exit the mutex, then
	   overwrite it with the real PID (also nonzero) from the fork */
	gathererProcess = -1;
	krnlExitMutex( MUTEX_RANDOMPOLLING );

	/* Fork off the gatherer, the parent process returns to the caller */
	if( ( gathererProcess = fork() ) != 0 )
		{
		/* If the fork() failed, clean up and reset the gatherer PID to make
		   sure that we're not locked out of retrying the poll later */
		if( gathererProcess == -1 )
			{
#ifdef DEBUG_CONFLICTS
			fprintf( stderr, "cryptlib: fork() failed, errno = %d, "
					 "file = %s, line = %d.\n", errno, __FILE__, __LINE__ );
#endif /* DEBUG_CONFLICTS */
			krnlEnterMutex( MUTEX_RANDOMPOLLING );
			shmctl( gathererMemID, IPC_RMID, NULL );
			sigaction( SIGCHLD, &gathererOldHandler, NULL );
			gathererProcess = 0;
			krnlExitMutex( MUTEX_RANDOMPOLLING );
			}
		return;	/* Error/parent process returns */
		}

	/* General housekeeping: Make sure that we can never dump core, and close
	   all inherited file descriptors.  We need to do this because if we
	   don't and the calling app has FILE *'s open, these will be flushed
	   when we call exit() in the child and again when the parent writes to
	   them or closes them, resulting in everything that was present in the
	   FILE * buffer at the time of the fork() being written twice.  An
	   alternative solution would be to call _exit() instead if exit() below,
	   but this is somewhat system-dependant and therefore a bit risky to
	   use.

	   In addition to this we should in theory call cryptEnd() since we
	   don't need any cryptlib objects beyond this point and it'd be a good
	   idea to clean them up to get rid of sensitive data held in memory.
	   However in some cases when using a crypto device or network interface
	   or similar item the shutdown in the child will also shut down the
	   parent item because while cryptlib objects are reference-counted the
	   external items aren't (they're beyond the control of cryptlib).  Even
	   destroying just contexts with keys loaded isn't possible because they
	   may be tied to a device that will propagate the shutdown from the
	   child to the parent via the device.

	   In general the child will be short-lived, and the use in its further
	   children of vfork() or the fact that many modern fork()s have copy-on-
	   write semantics even if no vfork() is available will mean that
	   cryptlib memory is never copied to the child and further children.  It
	   would, however, be better if there were some way to perform a neutron-
	   bomb type shutdown that only zeroises senstive information while
	   leaving structures intact */
	setrlimit( RLIMIT_CORE, &rl );
	for( fd = getdtablesize() - 1; fd > STDOUT_FILENO; fd-- )
		close( fd );
	fclose( stderr );	/* Arrghh!!  It's Stuart code!! */

	/* Fire up each randomness source */
	FD_ZERO( &fds );
	for( i = 0; dataSources[ i ].path != NULL; i++ )
		{
		/* Check for the end-of-lightweight-sources marker */
		if( dataSources[ i ].path[ 0 ] == '\0' )
			{
			/* If we're only polling lightweight sources because we've
			   already obtained entropy from additional sources,we're
			   done */
			if( extraEntropy >= 50 )
				{
#ifdef DEBUG_RANDOM
				puts( "rndunix: All lightweight sources polled, exiting "
					  "without polling\nheavyweight ones." );
#endif /* DEBUG_RANDOM */
				break;
				}

			/* We're polling all sources, continue with the heavyweight
			   ones */
			continue;
			}

		/* Since popen() is a fairly heavy function, we check to see whether
		   the executable exists before we try to run it */
		if( access( dataSources[ i ].path, X_OK ) )
			{
#ifdef DEBUG_RANDOM
			printf( "rndunix: %s not present%s.\n", dataSources[ i ].path,
					dataSources[ i ].hasAlternative ? \
						", has alternatives" : "" );
#endif /* DEBUG_RANDOM */
			dataSources[ i ].pipe = NULL;
			}
		else
			dataSources[ i ].pipe = my_popen( &dataSources[ i ] );
		if( dataSources[ i ].pipe != NULL )
			{
			dataSources[ i ].pipeFD = fileno( dataSources[ i ].pipe );
			if( dataSources[ i ].pipeFD > maxFD )
				maxFD = dataSources[ i ].pipeFD;
			fcntl( dataSources[ i ].pipeFD, F_SETFL, O_NONBLOCK );
			FD_SET( dataSources[ i ].pipeFD, &fds );
			dataSources[ i ].length = 0;

			/* If there are alternatives for this command, don't try and
			   execute them */
			while( dataSources[ i ].hasAlternative )
				{
#ifdef DEBUG_RANDOM
				printf( "rndunix: Skipping %s.\n",
						dataSources[ i + 1 ].path );
#endif /* DEBUG_RANDOM */
				i++;
				}
			}
		}
	gathererInfo = ( GATHERER_INFO * ) gathererBuffer;
	bufPos = sizeof( GATHERER_INFO );	/* Start of buf.has status info */

	/* Suck all the data that we can get from each of the sources */
	moreSources = TRUE;
	startTime = getTime();
	while( moreSources && bufPos < gathererBufSize )
		{
		/* Wait for data to become available from any of the sources, with a
		   timeout of 10 seconds.  This adds even more randomness since data
		   becomes available in a nondeterministic fashion.  Kudos to HP's QA
		   department for managing to ship a select() that breaks its own
		   prototype */
		tv.tv_sec = 10;
		tv.tv_usec = 0;
#if defined( __hpux ) && ( OSVERSION == 9 || OSVERSION == 0 )
		if( select( maxFD + 1, ( int * ) &fds, NULL, NULL, &tv ) == -1 )
#else
		if( select( maxFD + 1, &fds, NULL, NULL, &tv ) == -1 )
#endif /* __hpux */
			break;

		/* One of the sources has data available, read it into the buffer */
		for( i = 0; dataSources[ i ].path != NULL; i++ )
			if( dataSources[ i ].pipe != NULL && \
				FD_ISSET( dataSources[ i ].pipeFD, &fds ) )
				usefulness += getEntropySourceData( &dataSources[ i ],
													gathererBuffer + bufPos,
													gathererBufSize - bufPos,
													&bufPos );

		/* Check if there's more input available on any of the sources */
		moreSources = FALSE;
		FD_ZERO( &fds );
		for( i = 0; dataSources[ i ].path != NULL; i++ )
			if( dataSources[ i ].pipe != NULL )
				{
				FD_SET( dataSources[ i ].pipeFD, &fds );
				moreSources = TRUE;
				}

		/* If we've gone over our time limit, kill anything still hanging
		   around and exit.  This prevents problems with input from blocked
		   sources */
		if( getTime() > startTime + SLOWPOLL_TIMEOUT )
			{
			for( i = 0; dataSources[ i ].path != NULL; i++ )
				if( dataSources[ i ].pipe != NULL )
					{
#ifdef DEBUG_RANDOM
					printf( "rndunix: Aborting read of %s due to timeout.\n",
							dataSources[ i ].path );
#endif /* DEBUG_RANDOM */
					fclose( dataSources[ i ].pipe );
					kill( dataSources[ i ].pid, SIGKILL );
					dataSources[ i ].pipe = NULL;
					dataSources[ i ].pid = 0;
					}
			moreSources = FALSE;
#ifdef DEBUG_RANDOM
			puts( "rndunix: Poll timed out, probably due to blocked data "
				  "source." );
#endif /* DEBUG_RANDOM */
			}
		}
	gathererInfo->usefulness = usefulness;
	gathererInfo->noBytes = bufPos;
#ifdef DEBUG_RANDOM
	printf( "rndunix: Got %d bytes, usefulness = %d.\n", bufPos,
			usefulness );
#endif /* DEBUG_RANDOM */

	/* "Thou child of the daemon, ... wilt thou not cease...?"
	   -- Acts 13:10 */
	exit( 0 );
#endif /* !QNX 4.x */
	}

/* Wait for the randomness gathering to finish.  Cray Unicos doesn't have
   sched_yield() and OpenBSD has its sched_yield() in libpthread so it
   doesn't exist if we're not building with USE_THREADS, in which case we
   have to no-op it out */

#if defined( _CRAY ) || \
	( defined( __OpenBSD__ ) && !defined( USE_THREADS ) )
  #define sched_yield()
#endif /* Systems without sched_yield() */

void waitforRandomCompletion( const BOOLEAN force )
	{
	krnlEnterMutex( MUTEX_RANDOMPOLLING );
	if( gathererProcess	)
		{
		RESOURCE_DATA msgData;
		GATHERER_INFO *gathererInfo = ( GATHERER_INFO * ) gathererBuffer;
		int quality, status;

		/* If this is a forced shutdown, be fairly assertive with the
		   gathering process */
		if( force )
			{
			/* Politely ask the the gatherer to shut down and (try and)
			   yield our timeslice a few times so that the shutdown can
			   take effect. This is unfortunately somewhat implementation-
			   dependant in that in some cases it'll only yield the current
			   thread's timeslice, or if it's a high-priority thread it'll
			   be scheduled again before any lower-priority threads get to
			   run */
			kill( gathererProcess, SIGTERM );
			sched_yield();
			sched_yield();
			sched_yield();	/* Well, sync is done three times too... */

			/* If the gatherer is still running, ask again, less
			   politely this time */
#if 1
			if( kill( gathererProcess, 0 ) != -1 || errno != ESRCH )
#else
			if( getpgid( pid ) > 0 || errno == EPERM )
#endif /* 1 */
				kill( gathererProcess, SIGKILL );
			}

		/* Wait for the gathering process to finish, add the randomness it's
		   gathered, detach and delete the shared memory (the latter is
		   necessary because otherwise the unused ID hangs around until the
		   process terminates), and restore the original signal handler if we
		   replaced someone else's one.  We don't check for errors at this
		   point (except in the debug version) since this is an invisible
		   internal routine for which we can't easily recover from problems.
		   Any problems are caught at a higher level by the randomness-
		   quality checking */
		waitpid( gathererProcess, &status, 0 );
		if( gathererInfo->noBytes > 0 && !force )
			{
			quality = min( gathererInfo->usefulness * 5, 100 );	/* 0-20 -> 0-100 */
			setMessageData( &msgData, gathererBuffer, gathererInfo->noBytes );
			status = krnlSendMessage( SYSTEM_OBJECT_HANDLE,
									  IMESSAGE_SETATTRIBUTE_S,
									  &msgData, CRYPT_IATTRIBUTE_ENTROPY );
			assert( cryptStatusOK( status ) );
			if( quality > 0 )
				{
				/* On some very cut-down embedded systems the entropy
				   quality can be zero, so we only send a quality estimate
				   if there's actually something there */
				status = krnlSendMessage( SYSTEM_OBJECT_HANDLE,
										  IMESSAGE_SETATTRIBUTE,
										  &quality,
										  CRYPT_IATTRIBUTE_ENTROPY_QUALITY );
				assert( cryptStatusOK( status ) );
				}
			}
		zeroise( gathererBuffer, gathererBufSize );
#if !( defined( __QNX__ ) && OSVERSION <= 4 )
		shmdt( gathererBuffer );
		shmctl( gathererMemID, IPC_RMID, NULL );
		if( gathererOldHandler.sa_handler != SIG_DFL )
			{
			struct sigaction oact;

			/* We replaced someone else's handler for the slow poll,
			   reinstate the original one.  Since someone else could have in
			   turn replaced our handler, we check for this and warn the
			   user if necessary */
			sigaction( SIGCHLD, NULL, &oact );
			if( oact.sa_handler != SIG_DFL )
				{
#ifdef DEBUG_CONFLICTS
				/* The current handler isn't the one that we installed, warn
				   the user */
				fprintf( stderr, "cryptlib: SIGCHLD handler was replaced "
						 "while slow poll was in progress,\nfile " __FILE__
						 ", line %d.  See the source code for more\n"
						 "information.\n", __LINE__ );
#endif /* DEBUG_CONFLICTS */
				}
			else
				/* Our handler is still in place, replace it with the
				   original one */
				sigaction( SIGCHLD, &gathererOldHandler, NULL );
			}
#endif /* !QNX 4.x */
		gathererProcess = 0;
		}
	krnlExitMutex( MUTEX_RANDOMPOLLING );
	}

/* Check whether we've forked and we're the child.  The mechanism used varies
   depending on whether we're running in a single-threaded or multithreaded
   environment, for single-threaded we check whether the pid has changed
   since the last check, for multithreaded environments this isn't reliable
   since some systems have per-thread pid's so we need to use
   pthread_atfork() as a trigger to set the pid-changed flag.

   Under Aches, calling pthread_atfork() with any combination of arguments or
   circumstances produces a segfault, so we undefine USE_THREADS to force the
   use of the getpid()-based fork detection.  In addition some other
   environments don't support the call, so we exclude those as well.  FreeBSD
   is a particular pain because of its highly confusing use of -RELEASE,
   -STABLE, and -CURRENT while maintaining the same version, it's present in
   5.x-CURRENT but not 5.x-RELEASE or -STABLE, so we have to exclude it for
   all 5.x to be safe */

#if defined( USE_THREADS ) && \
	( defined( _AIX ) || defined( _CRAY ) || defined( __MVS__ ) || \
	  defined( _MPRAS ) || defined( __APPLE__ ) || \
	  ( defined( __FreeBSD__ ) && OSVERSION <= 5 ) )
  #undef USE_THREADS
#endif /* USE_THREADS && OSes without pthread_atfork() */

#ifdef USE_THREADS

static BOOLEAN forked = FALSE;

BOOLEAN checkForked( void )
	{
	BOOLEAN hasForked;

	/* Read the forked-t flag in a thread-safe manner */
	krnlEnterMutex( MUTEX_RANDOMPOLLING );
	hasForked = forked;
	forked = FALSE;
	krnlExitMutex( MUTEX_RANDOMPOLLING );

	return( hasForked );
	}

void setForked( void )
	{
	/* Set the forked-t flag in a thread-safe manner */
	krnlEnterMutex( MUTEX_RANDOMPOLLING );
	forked = TRUE;
	krnlExitMutex( MUTEX_RANDOMPOLLING );
	}

#else

BOOLEAN checkForked( void )
	{
	static pid_t originalPID = -1;

	/* Set the initial PID if necessary */
	if( originalPID == -1 )
		originalPID = getpid();

	/* If the pid has changed we've forked and we're the child, remember the
	   new pid */
	if( getpid() != originalPID )
		{
		originalPID = getpid();
		return( TRUE );
		}

	return( FALSE );
	}
#endif /* USE_THREADS */

/* Initialise and clean up any auxiliary randomness-related objects */

void initRandomPolling( void )
	{
	/* If it's multithreaded code, we need to ensure that we're signalled if
	   another thread calls fork().  Hardcoding in the Posix function name at
	   this point is safe because it also works for Solaris threads. We set
	   the forked flag in both the child and the parent to ensure that both
	   sides remix the pool thoroughly */
#ifdef USE_THREADS
	pthread_atfork( NULL, setForked, setForked );
#endif /* USE_THREADS */
	}
