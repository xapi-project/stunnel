/*
 *   stunnel       Universal SSL tunnel
 *   Copyright (c) 1998-2006 Michal Trojnara <Michal.Trojnara@mirt.net>
 *                 All Rights Reserved
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#ifndef COMMON_H
#define COMMON_H

/* For FormatGuard */
/* #define __NO_FORMATGUARD_ */

/* #define DEBUG_STACK_SIZE */

/**************************************** Platform */

#ifdef USE_WIN32
#define USE_IPv6
#endif

#ifdef _WIN32_WCE
#define USE_WIN32
typedef int socklen_t;
#define EINTR WSAEINTR
#define EMFILE WSAEMFILE
#endif

#ifdef USE_WIN32
#define HAVE_OPENSSL
#define HAVE_OSSL_ENGINE_H
#endif

/**************************************** Common constants */

#ifndef VERSION
#define VERSION "4.15"
#endif

/* CPU stack size */
#define STACK_SIZE 65536

/* I/O buffer size */
#define BUFFSIZE        16384

/* Length of strings (including the terminating '\0' character) */
#define STRLEN          256

/* IP address and TCP port textual representation length */
#define IPLEN           128

/* How many bytes of random input to read from files for PRNG */
/* OpenSSL likes at least 128 bits, so 64 bytes seems plenty. */
#define RANDOM_BYTES    64

/**************************************** Generic headers */

#ifdef __vms
#include <starlet.h>
#endif /* __vms */

/* For nsr-tandem-nsk architecture */
#ifdef __TANDEM
#include <floss.h>
#endif

/* threads model */
#ifdef USE_UCONTEXT
#define __MAKECONTEXT_V2_SOURCE
#include <ucontext.h>
#endif

#ifdef USE_PTHREAD
#define THREADS
#define _REENTRANT
#define _THREAD_SAFE
#include <pthread.h>
#endif

/* TCP wrapper */
#if HAVE_TCPD_H && HAVE_LIBWRAP
#define USE_LIBWRAP
#endif

/* Must be included before sys/stat.h for Ultrix */
#include <sys/types.h>   /* u_short, u_long */
/* General headers */
#include <stdio.h>
/* Must be included before sys/stat.h for Ultrix */
#include <errno.h>
#include <stdlib.h>
#include <stdarg.h>      /* va_ */
#include <string.h>
#include <ctype.h>       /* isalnum */
#include <time.h>
#include <sys/stat.h>    /* stat */
#include <setjmp.h>

/**************************************** WIN32 headers */

#ifdef USE_WIN32

#ifndef HOST
#ifdef __MINGW32__
#define HOST "x86-pc-mingw32-gnu"
#else
#define HOST "x86-pc-unknown"
#endif
#endif

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned long u32;

#define HAVE_SNPRINTF
#define snprintf _snprintf
#define HAVE_VSNPRINTF
#define vsnprintf _vsnprintf
#define strcasecmp _stricmp
#define strncasecmp _strnicmp
#define sleep(c) Sleep(1000*(c))

#define exit(c) exit_stunnel(c)

#define get_last_socket_error() WSAGetLastError()
#define get_last_error()        GetLastError()
#define readsocket(s,b,n)       recv((s),(b),(n),0)
#define writesocket(s,b,n)      send((s),(b),(n),0)

/* #define FD_SETSIZE 4096 */
/* #define Win32_Winsock */
#define __USE_W32_SOCKETS

/* Winsock2 header for IPv6 definitions */
#ifdef _WIN32_WCE
#include <winsock.h>
#else
#include <winsock2.h>
#include <ws2tcpip.h>
#endif
#include <windows.h>

#define ECONNRESET WSAECONNRESET
#define ENOTSOCK WSAENOTSOCK
#define ENOPROTOOPT WSAENOPROTOOPT
#define EINPROGRESS WSAEINPROGRESS
#define EWOULDBLOCK WSAEWOULDBLOCK
#define EISCONN WSAEISCONN
#undef EINVAL
#define EINVAL WSAEINVAL

#include <process.h>     /* _beginthread */
#include <tchar.h>

#define NO_IDEA
#define OPENSSL_NO_IDEA

/**************************************** non-WIN32 headers */

#else /* USE_WIN32 */

#if SIZEOF_UNSIGNED_CHAR == 1
typedef unsigned char u8;
#endif

#if SIZEOF_UNSIGNED_SHORT == 2
typedef unsigned short u16;
#else
typedef unsigned int u16;
#endif

#if SIZEOF_UNSIGNED_INT == 4
typedef unsigned int u32;
#else
typedef unsigned long u32;
#endif

#define get_last_socket_error() errno
#define get_last_error()        errno
#define readsocket(s,b,n)       read((s),(b),(n))
#define writesocket(s,b,n)      write((s),(b),(n))
#define closesocket(s)          close(s)
#define ioctlsocket(a,b,c)      ioctl((a),(b),(c))

    /* OpenVMS compatibility */
#ifdef __vms
#define libdir "__NA__"
#define PIDFILE "SYS$LOGIN:STUNNEL.PID"
#ifdef __alpha
#define HOST "alpha-openvms"
#else
#define HOST "vax-openvms"
#endif
#include <inet.h>
#include <unistd.h>
#else   /* __vms */
#include <syslog.h>
#endif  /* __vms */

    /* Unix-specific headers */
#include <signal.h>      /* signal */
#include <sys/wait.h>    /* wait */
#ifdef HAVE_SYS_RESOURCE_H
#include <sys/resource.h> /* getrlimit */
#endif
#ifdef HAVE_UNISTD_H
#include <unistd.h>      /* getpid, fork, execvp, exit */
#endif
#ifdef HAVE_STROPTS_H
#include <stropts.h>
#endif
#ifdef HAVE_SYS_SELECT_H
#include <sys/select.h>  /* for aix */
#endif

#ifndef BROKEN_POLL
#ifdef HAVE_POLL_H
#include <poll.h>
#define USE_POLL
#else /* HAVE_POLL_H */
#ifdef HAVE_SYS_POLL_H
#include <sys/poll.h>
#define USE_POLL
#endif /* HAVE_SYS_POLL_H */
#endif /* HAVE_POLL_H */
#endif /* BROKEN_POLL */

#ifdef HAVE_SYS_FILIO_H
#include <sys/filio.h>   /* for FIONBIO */
#endif
#include <pwd.h>
#ifdef HAVE_GRP_H
#include <grp.h>
#endif
#ifdef __BEOS__
#include <posix/grp.h>
#endif
#include <fcntl.h>

#include <netinet/in.h>  /* struct sockaddr_in */
#include <sys/socket.h>  /* getpeername */
#include <arpa/inet.h>   /* inet_ntoa */
#include <sys/time.h>    /* select */
#include <sys/ioctl.h>   /* ioctl */
#include <netinet/tcp.h>
#include <netdb.h>
#ifndef INADDR_ANY
#define INADDR_ANY       (u32)0x00000000
#endif
#ifndef INADDR_LOOPBACK
#define INADDR_LOOPBACK  (u32)0x7F000001
#endif

#if defined(HAVE_WAITPID)
/* For SYSV systems */
#define wait_for_pid(a, b, c) waitpid((a), (b), (c))
#define HAVE_WAIT_FOR_PID 1
#elif defined(HAVE_WAIT4)
/* For BSD systems */
#define wait_for_pid(a, b, c) wait4((a), (b), (c), NULL)
#define HAVE_WAIT_FOR_PID 1
#endif

/* SunOS 4 */
#if defined(sun) && !defined(__svr4__) && !defined(__SVR4)
#define atexit(a) on_exit((a), NULL)
extern int sys_nerr;
extern char *sys_errlist[];
#define strerror(num) ((num)==0 ? "No error" : \
    ((num)>=sys_nerr ? "Unknown error" : sys_errlist[num]))
#endif /* SunOS 4 */

#endif /* USE_WIN32 */

/**************************************** OpenSSL headers */

#ifdef HAVE_OPENSSL
#include <openssl/lhash.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/crypto.h> /* for CRYPTO_* and SSLeay_version */
#include <openssl/rand.h>
#include <openssl/conf.h>
#include <openssl/x509v3.h>
#ifdef HAVE_OSSL_ENGINE_H
#include <openssl/engine.h>
#endif
#else
#include <lhash.h>
#include <ssl.h>
#include <err.h>
#include <crypto.h> /* for CRYPTO_* and SSLeay_version */
#endif

/**************************************** Other defines */

/* Safe copy for strings declarated as char[STRLEN] */
#define safecopy(dst, src) \
    (dst[STRLEN-1]='\0', strncpy((dst), (src), STRLEN-1))
#define safeconcat(dst, src) \
    (dst[STRLEN-1]='\0', strncat((dst), (src), STRLEN-strlen(dst)-1))
/* change all non-printable characters to '.' */
#define safestring(s) \
    do {char *p; for(p=(s); *p; p++) if(!isprint((int)*p)) *p='.';} while(0)
/* change all unsafe characters to '.' */
#define safename(s) \
    do {char *p; for(p=(s); *p; p++) if(!isalnum((int)*p)) *p='.';} while(0)

/* Some definitions for IPv6 support */
#if defined(USE_IPv6)
#define addr_len(x) ((x).sa.sa_family==AF_INET ? \
    sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6))
#else
#define addr_len(x) (sizeof(struct sockaddr_in))
#endif

/* Always use IPv4 defaults! */
#define DEFAULT_LOOPBACK "127.0.0.1"
#define DEFAULT_ANY "0.0.0.0"
#if 0
#define DEFAULT_LOOPBACK "::1"
#define DEFAULT_ANY "::"
#endif

#if defined (USE_WIN32) || defined (__vms)
#define LOG_EMERG       0
#define LOG_ALERT       1
#define LOG_CRIT        2
#define LOG_ERR         3
#define LOG_WARNING     4
#define LOG_NOTICE      5
#define LOG_INFO        6
#define LOG_DEBUG       7
#endif /* defined (USE_WIN32) || defined (__vms) */

#endif /* defined COMMON_H */

/* End of common.h */
