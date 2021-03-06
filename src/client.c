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
 *
 *   In addition, as a special exception, Michal Trojnara gives
 *   permission to link the code of this program with the OpenSSL
 *   library (or with modified versions of OpenSSL that use the same
 *   license as OpenSSL), and distribute linked combinations including
 *   the two.  You must obey the GNU General Public License in all
 *   respects for all of the code used other than OpenSSL.  If you modify
 *   this file, you may extend this exception to your version of the
 *   file, but you are not obligated to do so.  If you do not wish to
 *   do so, delete this exception statement from your version.
 */

/* Undefine if you have problems with make_sockets() */
#define INET_SOCKET_PAIR

#include "common.h"
#include "prototypes.h"

#ifndef SHUT_RD
#define SHUT_RD 0
#endif
#ifndef SHUT_WR
#define SHUT_WR 1
#endif
#ifndef SHUT_RDWR
#define SHUT_RDWR 2
#endif

/* TCP wrapper */
#ifdef USE_LIBWRAP
#include <tcpd.h>
int allow_severity=LOG_NOTICE;
int deny_severity=LOG_WARNING;
#endif

#if SSLEAY_VERSION_NUMBER >= 0x0922
static unsigned char *sid_ctx=(unsigned char *)"stunnel SID";
    /* const allowed here */
#endif

static void do_client(CLI *);
static void run_client(CLI *);
static void init_local(CLI *);
static void init_remote(CLI *);
static void init_ssl(CLI *);
static void transfer(CLI *);
static void parse_socket_error(CLI *, const char *);

static void print_cipher(CLI *);
static void auth_libwrap(CLI *);
static void auth_user(CLI *);
static int connect_local(CLI *);
#ifndef USE_WIN32
static void make_sockets(CLI *, int [2]);
#endif
static int connect_remote(CLI *);
static void connect_wait(CLI *);
static void reset(int, char *);

int max_clients;
#ifndef USE_WIN32
int max_fds;
#endif

/* Allocate local data structure for the new thread */
void *alloc_client_session(LOCAL_OPTIONS *opt, int rfd, int wfd) {
    CLI *c;

    c=calloc(1, sizeof(CLI));
    if(!c) {
        s_log(LOG_ERR, "Memory allocation failed");
        return NULL;
    }
    c->opt=opt;
    c->local_rfd.fd=rfd;
    c->local_wfd.fd=wfd;
    return c;
}

void *client(void *arg) {
    CLI *c=arg;

#ifdef DEBUG_STACK_SIZE
    stack_info(1); /* initialize */
#endif
    s_log(LOG_DEBUG, "%s started", c->opt->servname);
#ifndef USE_WIN32
    if(c->opt->option.remote && c->opt->option.program) {
            /* connect and exec options specified together */
            /* -> spawn a local program instead of stdio */
        while((c->local_rfd.fd=c->local_wfd.fd=connect_local(c))>=0) {
            run_client(c);
            sleep(1); /* FIXME: not a good idea in ucontext threading */
        }
    } else
#endif
    {
        if(alloc_fd(c->local_rfd.fd))
            return NULL;
        if(c->local_wfd.fd!=c->local_rfd.fd)
            if(alloc_fd(c->local_wfd.fd))
                return NULL;
        run_client(c);
    }
    free(c);
#ifdef DEBUG_STACK_SIZE
    stack_info(0); /* display computed value */
#endif
#if defined(USE_WIN32) && !defined(_WIN32_WCE)
    _endthread();
#endif
#ifdef USE_UCONTEXT
    s_log(LOG_DEBUG, "Context %ld closed", ready_head->id);
    s_poll_wait(NULL, 0); /* wait on poll() */
    s_log(LOG_ERR, "INTERNAL ERROR: failed to drop context");
#endif
    return NULL;
}

static void run_client(CLI *c) {
    int error;

    c->remote_fd.fd=-1;
    c->fd=-1;
    c->ssl=NULL;
    c->sock_bytes=c->ssl_bytes=0;

    error=setjmp(c->err);
    if(!error)
        do_client(c);

    s_log(LOG_NOTICE,
        "Connection %s: %d bytes sent to SSL, %d bytes sent to socket",
         error ? "reset" : "closed", c->ssl_bytes, c->sock_bytes);

        /* Cleanup IDENT socket */
    if(c->fd>=0)
        closesocket(c->fd);

        /* Cleanup SSL */
    if(c->ssl) { /* SSL initialized */
        SSL_set_shutdown(c->ssl, SSL_SENT_SHUTDOWN|SSL_RECEIVED_SHUTDOWN);
        SSL_free(c->ssl);
        ERR_remove_state(0);
    }

        /* Cleanup remote socket */
    if(c->remote_fd.fd>=0) { /* Remote socket initialized */
        if(error && c->remote_fd.is_socket)
            reset(c->remote_fd.fd, "linger (remote)");
        closesocket(c->remote_fd.fd);
    }

        /* Cleanup local socket */
    if(c->local_rfd.fd>=0) { /* Local socket initialized */
        if(c->local_rfd.fd==c->local_wfd.fd) {
            if(error && c->local_rfd.is_socket)
                reset(c->local_rfd.fd, "linger (local)");
            closesocket(c->local_rfd.fd);
        } else { /* STDIO */
            if(error && c->local_rfd.is_socket)
                reset(c->local_rfd.fd, "linger (local_rfd)");
            if(error && c->local_wfd.is_socket)
                reset(c->local_wfd.fd, "linger (local_wfd)");
       }
    }
#ifdef USE_FORK
    if(!c->opt->option.remote) /* 'exec' specified */
        child_status(); /* null SIGCHLD handler was used */
#else
    enter_critical_section(CRIT_CLIENTS); /* for multi-cpu machines */
    s_log(LOG_DEBUG, "%s finished (%d left)", c->opt->servname,
        --num_clients);
    leave_critical_section(CRIT_CLIENTS);
#endif
}

static void do_client(CLI *c) {
    init_local(c);
    if(!c->opt->option.client && !c->opt->protocol) {
        /* Server mode and no protocol negotiation needed */
        init_ssl(c);
        init_remote(c);
    } else {
        init_remote(c);
        negotiate(c);
        init_ssl(c);
    }
    transfer(c);
}

static void init_local(CLI *c) {
    SOCKADDR_UNION addr;
    socklen_t addrlen;

    addrlen=sizeof(SOCKADDR_UNION);
    if(getpeername(c->local_rfd.fd, &addr.sa, &addrlen)<0) {
        strcpy(c->accepting_address, "NOT A SOCKET");
        c->local_rfd.is_socket=0;
        c->local_wfd.is_socket=0; /* TODO: It's not always true */
#ifdef USE_WIN32
        if(get_last_socket_error()!=ENOTSOCK) {
#else
        if(c->opt->option.transparent || get_last_socket_error()!=ENOTSOCK) {
#endif
            sockerror("getpeerbyname");
            longjmp(c->err, 1);
        }
        /* Ignore ENOTSOCK error so 'local' doesn't have to be a socket */
    } else { /* success */
        /* copy addr to c->peer_addr */
        memcpy(&c->peer_addr.addr[0], &addr, sizeof(SOCKADDR_UNION));
        c->peer_addr.num=1;
        s_ntop(c->accepting_address, &c->peer_addr.addr[0]);
        c->local_rfd.is_socket=1;
        c->local_wfd.is_socket=1; /* TODO: It's not always true */
        /* It's a socket: lets setup options */
        if(set_socket_options(c->local_rfd.fd, 1)<0)
            longjmp(c->err, 1);
        auth_libwrap(c);
        auth_user(c);
        s_log(LOG_NOTICE, "%s connected from %s",
            c->opt->servname, c->accepting_address);
    }
}

static void init_remote(CLI *c) {
    /* create connection to host/service */
    if(c->opt->source_addr.num)
        memcpy(&c->bind_addr, &c->opt->source_addr, sizeof(SOCKADDR_LIST));
#ifndef USE_WIN32
    else if(c->opt->option.transparent)
        memcpy(&c->bind_addr, &c->peer_addr, sizeof(SOCKADDR_LIST));
#endif
    else {
        c->bind_addr.num=0; /* don't bind connecting socket */
    }

    /* setup c->remote_fd, now */
    if(c->opt->option.remote) {
        c->remote_fd.fd=connect_remote(c);
    } else /* NOT in remote mode */
        c->remote_fd.fd=connect_local(c);
    c->remote_fd.is_socket=1; /* Always! */
#ifndef USE_WIN32
    if(c->remote_fd.fd>=max_fds) {
        s_log(LOG_ERR, "Remote file descriptor out of range (%d>=%d)",
            c->remote_fd.fd, max_fds);
        longjmp(c->err, 1);
    }
#endif
    s_log(LOG_DEBUG, "Remote FD=%d initialized", c->remote_fd.fd);
    if(set_socket_options(c->remote_fd.fd, 2)<0)
        longjmp(c->err, 1);
}

static char *get_cfg_name(CLI *c) {
    char *cfg_addr, *port;
    cfg_addr = strdup(c->opt->remote_address);
    if(!cfg_addr) return(0);
    port = strrchr(cfg_addr, ':');
    if(port)
        *port++ = '\0';

    return(cfg_addr);
}

static int verify_cert_hostname(X509 *cert, const char *hostname) {
    int ok = 0;

    { // check against subjectAltName
        GENERAL_NAMES *subjectAltNames;
        int i;

        if(subjectAltNames = X509_get_ext_d2i(cert, NID_subject_alt_name, 0, 0)) {
            for(i = 0; !ok && i < sk_GENERAL_NAME_num(subjectAltNames); ++i) {
                GENERAL_NAME *name;
                unsigned char *dns;
                int dnsLen;

                name = sk_GENERAL_NAME_value(subjectAltNames, i);
                if(name->type != GEN_DNS) continue;
                dnsLen = ASN1_STRING_to_UTF8(&dns, name->d.dNSName);

                s_log(LOG_INFO, "Post check: subjectAltName: \"%s\","
                    "length: %d, strlen: %d", dns, dnsLen, strlen(dns));

                if(strlen(hostname) == dnsLen)
                    if(!strcasecmp(dns, hostname)) ok = 1;

                OPENSSL_free(dns);
            }
        }
    }

    { // check against CommonName
        X509_NAME *subj;
        int cnIdx;

        if(!ok
            && (subj = X509_get_subject_name(cert))
            && (-1 != (cnIdx = X509_NAME_get_index_by_NID(subj, NID_commonName, -1)))) {
            X509_NAME_ENTRY *cnEntry;
            ASN1_STRING *cnASN1;
            int cnLen;
            unsigned char *cn;

            cnEntry = X509_NAME_get_entry(subj, cnIdx);
            cnASN1 = X509_NAME_ENTRY_get_data(cnEntry);
            cnLen = ASN1_STRING_to_UTF8(&cn, cnASN1);

            s_log(LOG_INFO, "Post check: commonName: \"%s\","
                "length: %d, strlen: %d", cn, cnLen, strlen(cn));

            /* only if the lengths are equal do we need to perform a comparison,
             * also, if there are embedded NULLs in cn then cnLen > strlen(cn) */
            if(strlen(hostname) == cnLen)
                if(!strcasecmp(cn, hostname)) ok = 1;
            OPENSSL_free(cn);
        }

        return ok;
    }
}

static int post_connection_check(CLI *c) {
    int ok = 0;
    char *cfg_name = 0;
    X509 *cert = 0;

    if(c->opt->verify_level <= SSL_VERIFY_PEER) {
        s_log(LOG_INFO, "Post check: verification level is low, skipping check");
        return X509_V_OK;
    }

    if(!(cert = SSL_get_peer_certificate(c->ssl))) {
        s_log(LOG_INFO, "Post check: No peer certificate!");
        return X509_V_ERR_APPLICATION_VERIFICATION;
    }

    cfg_name = get_cfg_name(c);
    s_log(LOG_INFO, "Post check: Config hostname: %s", cfg_name);
    if(cfg_name && verify_cert_hostname(cert, cfg_name))
        ok = 1;

    if(cfg_name) free(cfg_name);
    X509_free(cert);

    if(ok)
        return SSL_get_verify_result(c->ssl);
    else
        return X509_V_ERR_APPLICATION_VERIFICATION;
}

static void init_ssl(CLI *c) {
    int i, err;
    SSL_SESSION *old_session;

    if(!(c->ssl=SSL_new(c->opt->ctx))) {
        sslerror("SSL_new");
        longjmp(c->err, 1);
    }
    SSL_set_ex_data(c->ssl, cli_index, c); /* for verify callback */
#if SSLEAY_VERSION_NUMBER >= 0x0922
    SSL_set_session_id_context(c->ssl, sid_ctx, strlen(sid_ctx));
#endif
    if(c->opt->option.client) {
        if(c->opt->session) {
            enter_critical_section(CRIT_SESSION);
            SSL_set_session(c->ssl, c->opt->session);
            leave_critical_section(CRIT_SESSION);
        }
        SSL_set_fd(c->ssl, c->remote_fd.fd);
        SSL_set_connect_state(c->ssl);
    } else {
        if(c->local_rfd.fd==c->local_wfd.fd)
            SSL_set_fd(c->ssl, c->local_rfd.fd);
        else {
           /* Does it make sence to have SSL on STDIN/STDOUT? */
            SSL_set_rfd(c->ssl, c->local_rfd.fd);
            SSL_set_wfd(c->ssl, c->local_wfd.fd);
        }
        SSL_set_accept_state(c->ssl);
    }

    /* Setup some values for transfer() function */
    if(c->opt->option.client) {
        c->sock_rfd=&(c->local_rfd);
        c->sock_wfd=&(c->local_wfd);
        c->ssl_rfd=c->ssl_wfd=&(c->remote_fd);
    } else {
        c->sock_rfd=c->sock_wfd=&(c->remote_fd);
        c->ssl_rfd=&(c->local_rfd);
        c->ssl_wfd=&(c->local_wfd);
    }

    while(1) {
        if(c->opt->option.client)
            i=SSL_connect(c->ssl);
        else
            i=SSL_accept(c->ssl);
        err=SSL_get_error(c->ssl, i);
        if(err==SSL_ERROR_NONE) {
            if(post_connection_check(c) != X509_V_OK) {
                s_log(LOG_NOTICE, "Post connection cert verification failed");
                longjmp(c->err, 1); // bail
            }
            break; /* ok -> done */
        }
        if(err==SSL_ERROR_WANT_READ || err==SSL_ERROR_WANT_WRITE) {
            s_poll_zero(&c->fds);
            s_poll_add(&c->fds, c->ssl_rfd->fd,
                err==SSL_ERROR_WANT_READ,
                err==SSL_ERROR_WANT_WRITE);
            switch(s_poll_wait(&c->fds, c->opt->timeout_busy)) {
            case -1:
                sockerror("init_ssl: s_poll_wait");
                longjmp(c->err, 1);
            case 0:
                s_log(LOG_INFO, "init_ssl: s_poll_wait timeout");
                longjmp(c->err, 1);
            case 1:
                break; /* OK */
            default:
                s_log(LOG_ERR, "init_ssl: s_poll_wait unknown result");
                longjmp(c->err, 1);
            }
            continue; /* ok -> retry */
        }
        if(err==SSL_ERROR_SYSCALL) {
            switch(get_last_socket_error()) {
            case EINTR:
            case EAGAIN:
                continue;
            }
        }
        if(c->opt->option.client)
            sslerror("SSL_connect");
        else
            sslerror("SSL_accept");
        longjmp(c->err, 1);
    }
    if(SSL_session_reused(c->ssl)) {
        s_log(LOG_INFO, "SSL %s: previous session reused",
            c->opt->option.client ? "connected" : "accepted");
    } else { /* a new session was negotiated */
        if(c->opt->option.client) {
            s_log(LOG_INFO, "SSL connected: new session negotiated");
            enter_critical_section(CRIT_SESSION);
            old_session=c->opt->session;
            c->opt->session=SSL_get1_session(c->ssl); /* store it */
            if(old_session)
                SSL_SESSION_free(old_session); /* release the old one */
            leave_critical_section(CRIT_SESSION);
        } else
            s_log(LOG_INFO, "SSL accepted: new session negotiated");
        print_cipher(c);
    }
}

/****************************** some defines for transfer() */
/* is socket/SSL open for read/write? */
#define sock_rd (c->sock_rfd->rd)
#define sock_wr (c->sock_wfd->wr)
#define ssl_rd  (c->ssl_rfd->rd)
#define ssl_wr  (c->ssl_wfd->wr)

/* is socket/SSL ready for read/write? */
#define sock_can_rd (s_poll_canread(&c->fds, c->sock_rfd->fd))
#define sock_can_wr (s_poll_canwrite(&c->fds, c->sock_wfd->fd))
#define ssl_can_rd  (s_poll_canread(&c->fds, c->ssl_rfd->fd))
#define ssl_can_wr  (s_poll_canwrite(&c->fds, c->ssl_wfd->fd))
/* NOTE: above defines are related to the logical data stream,
 * no longer to the underlying file descriptors */

/* does the underlaying file descriptor want to read/write? */
#define want_rd     (SSL_want_read(c->ssl))
#define want_wr     (SSL_want_write(c->ssl))

/****************************** transfer data */
static void transfer(CLI *c) {
    int num, err;
    int check_SSL_pending;
    enum {CL_OPEN, CL_INIT, CL_RETRY, CL_CLOSED} ssl_closing=CL_OPEN;
    int watchdog=0; /* a counter to detect an infinite loop */

    c->sock_ptr=c->ssl_ptr=0;
    sock_rd=sock_wr=ssl_rd=ssl_wr=1;

    do { /* main loop */
        /* set flag to try and read any buffered SSL data
         * if we made room in the buffer by writing to the socket */
        check_SSL_pending=0;

        /****************************** setup c->fds structure */
        s_poll_zero(&c->fds); /* Initialize the structure */
        if(sock_rd && c->sock_ptr<BUFFSIZE) /* socket input buffer not full*/
            s_poll_add(&c->fds, c->sock_rfd->fd, 1, 0);
        if((ssl_rd && c->ssl_ptr<BUFFSIZE) || /* SSL input buffer not full */
                ((c->sock_ptr || ssl_closing==CL_RETRY) && want_rd))
                /* want to SSL_write or SSL_shutdown but read from the
                 * underlying socket needed for the SSL protocol */
            s_poll_add(&c->fds, c->ssl_rfd->fd, 1, 0);
        if(c->ssl_ptr) /* SSL input buffer not empty */
            s_poll_add(&c->fds, c->sock_wfd->fd, 0, 1);
        if(c->sock_ptr || /* socket input buffer not empty */
                ssl_closing==CL_INIT /* need to send close_notify */ ||
                ((c->ssl_ptr<BUFFSIZE || ssl_closing==CL_RETRY) && want_wr))
                /* want to SSL_read or SSL_shutdown but write to the
                 * underlying socket needed for the SSL protocol */
            s_poll_add(&c->fds, c->ssl_wfd->fd, 0, 1);

        /****************************** wait for an event */
        err=s_poll_wait(&c->fds, (sock_rd && ssl_rd) /* both peers open */ ||
            c->ssl_ptr /* data buffered to write to socket */ ||
            c->sock_ptr /* data buffered to write to SSL */ ?
            c->opt->timeout_idle : c->opt->timeout_close);
        switch(err) {
        case -1:
            sockerror("transfer: s_poll_wait");
            longjmp(c->err, 1);
        case 0: /* timeout */
            if((sock_rd && ssl_rd) || c->ssl_ptr || c->sock_ptr) {
                s_log(LOG_INFO, "s_poll_wait timeout: connection reset");
                longjmp(c->err, 1);
            } else { /* already closing connection */
                s_log(LOG_INFO, "s_poll_wait timeout: connection close");
                return; /* OK */
            }
        }
        if(!(sock_can_rd || sock_can_wr || ssl_can_rd || ssl_can_wr)) {
            s_log(LOG_ERR, "INTERNAL ERROR: "
                "s_poll_wait returned %d, but no descriptor is ready", err);
            longjmp(c->err, 1);
        }

        /****************************** send SSL close_notify message */
        if(ssl_closing==CL_INIT || (ssl_closing==CL_RETRY &&
                ((want_rd && ssl_can_rd) || (want_wr && ssl_can_wr)))) {
            switch(SSL_shutdown(c->ssl)) { /* Send close_notify */
            case 1: /* the shutdown was successfully completed */
                s_log(LOG_INFO, "SSL_shutdown successfully sent close_notify");
                ssl_closing=CL_CLOSED; /* done! */
                break;
            case 0: /* the shutdown is not yet finished */
                s_log(LOG_DEBUG, "SSL_shutdown retrying");
                ssl_closing=CL_RETRY; /* retry next time */
                break;
            case -1: /* a fatal error occurred */
                sslerror("SSL_shutdown");
                longjmp(c->err, 1);
            }
        }

        /****************************** write to socket */
        if(sock_wr && sock_can_wr) {
            num=writesocket(c->sock_wfd->fd, c->ssl_buff, c->ssl_ptr);
            switch(num) {
            case -1: /* error */
                parse_socket_error(c, "writesocket");
                break;
            case 0:
                s_log(LOG_DEBUG, "No data written to the socket: retrying");
                break;
            default:
                memmove(c->ssl_buff, c->ssl_buff+num, c->ssl_ptr-num);
                if(c->ssl_ptr==BUFFSIZE) /* buffer was previously full */
                    check_SSL_pending=1; /* check for data buffered by SSL */
                c->ssl_ptr-=num;
                c->sock_bytes+=num;
                watchdog=0; /* reset watchdog */
            }
        }

        /****************************** write to SSL */
        if(ssl_wr && c->sock_ptr && ( /* output buffer not empty */
                ssl_can_wr || (want_rd && ssl_can_rd)
                /* SSL_write wants to read from the underlying descriptor */
                )) {
            num=SSL_write(c->ssl, c->sock_buff, c->sock_ptr);
            switch(err=SSL_get_error(c->ssl, num)) {
            case SSL_ERROR_NONE:
                memmove(c->sock_buff, c->sock_buff+num, c->sock_ptr-num);
                c->sock_ptr-=num;
                c->ssl_bytes+=num;
                watchdog=0; /* reset watchdog */
                break;
            case SSL_ERROR_WANT_WRITE:
                s_log(LOG_DEBUG, "SSL_write returned WANT_WRITE: retrying");
                break;
            case SSL_ERROR_WANT_READ:
                s_log(LOG_DEBUG, "SSL_write returned WANT_READ: retrying");
                break;
            case SSL_ERROR_WANT_X509_LOOKUP:
                s_log(LOG_DEBUG,
                    "SSL_write returned WANT_X509_LOOKUP: retrying");
                break;
            case SSL_ERROR_SYSCALL: /* really an error */
                if(num)
                    parse_socket_error(c, "SSL_write");
                break;
            case SSL_ERROR_ZERO_RETURN: /* close_notify received */
                s_log(LOG_DEBUG, "SSL closed on SSL_write");
                ssl_rd=0;
                break;
            case SSL_ERROR_SSL:
                sslerror("SSL_write");
                longjmp(c->err, 1);
            default:
                s_log(LOG_ERR, "SSL_write/SSL_get_error returned %d", err);
                longjmp(c->err, 1);
            }
        }

        /****************************** read from socket */
        if(sock_rd && sock_can_rd) {
            num=readsocket(c->sock_rfd->fd,
                c->sock_buff+c->sock_ptr, BUFFSIZE-c->sock_ptr);
            switch(num) {
            case -1:
                parse_socket_error(c, "readsocket");
                break;
            case 0: /* close */
                s_log(LOG_DEBUG, "Socket closed on read");
                sock_rd=0;
                break;
            default:
                c->sock_ptr+=num;
                watchdog=0; /* reset watchdog */
            }
        }

        /****************************** read from SSL */
        if(ssl_rd && c->ssl_ptr<BUFFSIZE  && ( /* input buffer not full */
                ssl_can_rd || (want_wr && ssl_can_wr) ||
                /* SSL_read wants to write to the underlying descriptor */
                (check_SSL_pending && SSL_pending(c->ssl))
                /* write made space from full buffer */
                )) {
            num=SSL_read(c->ssl, c->ssl_buff+c->ssl_ptr, BUFFSIZE-c->ssl_ptr);
            switch(err=SSL_get_error(c->ssl, num)) {
            case SSL_ERROR_NONE:
                c->ssl_ptr+=num;
                watchdog=0; /* reset watchdog */
                break;
            case SSL_ERROR_WANT_WRITE:
                s_log(LOG_DEBUG, "SSL_read returned WANT_WRITE: retrying");
                break;
            case SSL_ERROR_WANT_READ:
                s_log(LOG_DEBUG, "SSL_read returned WANT_READ: retrying");
                break;
            case SSL_ERROR_WANT_X509_LOOKUP:
                s_log(LOG_DEBUG,
                    "SSL_read returned WANT_X509_LOOKUP: retrying");
                break;
            case SSL_ERROR_SYSCALL:
                if(!num) { /* EOF */
                    if(c->sock_ptr) {
                        s_log(LOG_ERR,
                            "SSL socket closed with %d byte(s) in buffer",
                            c->sock_ptr);
                        longjmp(c->err, 1); /* reset the socket */
                    }
                    s_log(LOG_DEBUG, "SSL socket closed on SSL_read");
                    ssl_rd=ssl_wr=0; /* buggy or SSLv2 peer: no close_notify */
                    ssl_closing=CL_CLOSED; /* don't try to send it back */
                } else
                    parse_socket_error(c, "SSL_read");
                break;
            case SSL_ERROR_ZERO_RETURN: /* close_notify received */
                s_log(LOG_DEBUG, "SSL closed on SSL_read");
                ssl_rd=0;
                break;
            case SSL_ERROR_SSL:
                sslerror("SSL_read");
                longjmp(c->err, 1);
            default:
                s_log(LOG_ERR, "SSL_read/SSL_get_error returned %d", err);
                longjmp(c->err, 1);
            }
        }

        /****************************** check write shutdown conditions */
        if(sock_wr && !ssl_rd && !c->ssl_ptr) {
            s_log(LOG_DEBUG, "Socket write shutdown");
            sock_wr=0; /* no further write allowed */
            shutdown(c->sock_wfd->fd, SHUT_WR); /* send TCP FIN */
        }
        if(ssl_wr && (!sock_rd || SSL_get_shutdown(c->ssl)) && !c->sock_ptr) {
            s_log(LOG_DEBUG, "SSL write shutdown");
            ssl_wr=0; /* no further write allowed */
            if(strcmp(SSL_get_version(c->ssl), "SSLv2")) { /* SSLv3, TLSv1 */
                ssl_closing=CL_INIT; /* initiate close_notify */
            } else { /* no alerts in SSLv2 including close_notify alert */
                shutdown(c->sock_rfd->fd, SHUT_RD); /* notify the kernel */
                shutdown(c->sock_wfd->fd, SHUT_WR); /* send TCP FIN */
                SSL_set_shutdown(c->ssl, /* notify the OpenSSL library */
                    SSL_SENT_SHUTDOWN|SSL_RECEIVED_SHUTDOWN);
                ssl_rd=0; /* no further read allowed */
                ssl_closing=CL_CLOSED; /* closed */
            }
        }
        if(ssl_closing==CL_RETRY) { /* SSL shutdown */
            if(!want_rd && !want_wr) { /* close_notify alert was received */
                s_log(LOG_DEBUG, "SSL doesn't need to read or write");
                ssl_closing=CL_CLOSED;
            }
            if(watchdog>5) {
                s_log(LOG_NOTICE, "Too many retries on SSL shutdown");
                ssl_closing=CL_CLOSED;
            }
        }

        /****************************** check watchdog */
        if(++watchdog>100) { /* loop executes without transferring any data */
            s_log(LOG_ERR,
                "transfer() loop executes not transferring any data");
            s_log(LOG_ERR,
                "please report the problem to Michal.Trojnara@mirt.net");
            s_log(LOG_ERR, "socket open: rd=%s wr=%s, ssl open: rd=%s wr=%s",
                sock_rd ? "yes" : "no", sock_wr ? "yes" : "no",
                ssl_rd ? "yes" : "no", ssl_wr ? "yes" : "no");
            s_log(LOG_ERR, "socket ready: rd=%s wr=%s, ssl ready: rd=%s wr=%s",
                sock_can_rd ? "yes" : "no", sock_can_wr ? "yes" : "no",
                ssl_can_rd ? "yes" : "no", ssl_can_wr ? "yes" : "no");
            s_log(LOG_ERR, "ssl want: rd=%s wr=%s",
                want_rd ? "yes" : "no", want_wr ? "yes" : "no");
            s_log(LOG_ERR, "socket input buffer: %d byte(s), "
                "ssl input buffer: %d byte(s)", c->sock_ptr, c->ssl_ptr);
            s_log(LOG_ERR, "check_SSL_pending=%d, ssl_closing=%d",
                check_SSL_pending, ssl_closing);
            longjmp(c->err, 1);
        }

    } while(sock_wr || ssl_closing!=CL_CLOSED);
}

static void parse_socket_error(CLI *c, const char *text) {
    switch(get_last_socket_error()) {
    case EINTR:
        s_log(LOG_DEBUG, "%s interrupted by a signal: retrying", text);
        return;
    case EWOULDBLOCK:
        s_log(LOG_NOTICE, "%s would block: retrying", text);
        sleep(1); /* Microsoft bug KB177346 */
        return;
#if EAGAIN!=EWOULDBLOCK
    case EAGAIN:
        s_log(LOG_DEBUG, "%s temporary lack of resources: retrying", text);
        return;
#endif
    default:
        sockerror(text);
        longjmp(c->err, 1);
    }
}

static void print_cipher(CLI *c) { /* print negotiated cipher */
#if SSLEAY_VERSION_NUMBER <= 0x0800
    s_log(LOG_INFO, "%s opened with SSLv%d, cipher %s",
        c->opt->servname, ssl->session->ssl_version, SSL_get_cipher(c->ssl));
#else
    SSL_CIPHER *cipher;
    char buf[STRLEN];
    int len;

    cipher=SSL_get_current_cipher(c->ssl);
    SSL_CIPHER_description(cipher, buf, STRLEN);
    len=strlen(buf);
    if(len>0)
        buf[len-1]='\0';
    s_log(LOG_INFO, "Negotiated ciphers: %s", buf);
#endif
}

static void auth_libwrap(CLI *c) {
#ifdef USE_LIBWRAP
    struct request_info request;
    int fd[2];
    int result=0; /* deny by default */

    if(pipe(fd)<0) {
        ioerror("pipe");
        longjmp(c->err, 1);
    }
    if(alloc_fd(fd[0]) || alloc_fd(fd[1]))
        longjmp(c->err, 1);
    switch(fork()) {
    case -1:    /* error */
        close(fd[0]);
        close(fd[1]);
        ioerror("fork");
        longjmp(c->err, 1);
    case  0:    /* child */
        close(fd[0]); /* read side */
        request_init(&request,
            RQ_DAEMON, c->opt->servname, RQ_FILE, c->local_rfd.fd, 0);
        fromhost(&request);
        result=hosts_access(&request);
        write_blocking(c, fd[1], (u8 *)&result, sizeof(result));
            /* ignore the returned error */
        close(fd[1]); /* write side */
        _exit(0);
    default:    /* parent */
        close(fd[1]); /* write side */
        read_blocking(c, fd[0], (u8 *)&result, sizeof(result));
            /* ignore the returned error */
        close(fd[0]); /* read side */
        /* no need to wait() for zombies here:
         *  - in UCONTEXT/PTHREAD mode they're removed using the signal pipe
         *  - in FORK mode they're removed with the client process */
    }

    if(!result) {
        s_log(LOG_WARNING, "Connection from %s REFUSED by libwrap",
            c->accepting_address);
        s_log(LOG_DEBUG, "See hosts_access(5) manual for details");
        longjmp(c->err, 1);
    }
    s_log(LOG_DEBUG, "Connection from %s permitted by libwrap",
        c->accepting_address);
#endif
}

static void auth_user(CLI *c) {
#ifndef _WIN32_WCE
    struct servent *s_ent;    /* structure for getservbyname */
#endif
    SOCKADDR_UNION ident;     /* IDENT socket name */
    char name[STRLEN];
    int error;

    if(!c->opt->username)
        return; /* -u option not specified */
    if((c->fd=
            socket(c->peer_addr.addr[0].sa.sa_family, SOCK_STREAM, 0))<0) {
        sockerror("socket (auth_user)");
        longjmp(c->err, 1);
    }
    if(alloc_fd(c->fd))
        longjmp(c->err, 1);
    memcpy(&ident, &c->peer_addr.addr[0], sizeof(SOCKADDR_UNION));
#ifndef _WIN32_WCE
    s_ent=getservbyname("auth", "tcp");
    if(s_ent) {
        ident.in.sin_port=s_ent->s_port;
    } else
#endif
    {
        s_log(LOG_WARNING, "Unknown service 'auth': using default 113");
        ident.in.sin_port=htons(113);
    }
    if(connect(c->fd, &ident.sa, addr_len(ident))) {
        error=get_last_socket_error();
        if(error!=EINPROGRESS && error!=EWOULDBLOCK) {
            sockerror("ident connect (auth_user)");
            longjmp(c->err, 1);
        }
        connect_wait(c);
    }
    s_log(LOG_DEBUG, "IDENT server connected");
    fdprintf(c, c->fd, "%u , %u",
        ntohs(c->peer_addr.addr[0].in.sin_port),
        ntohs(c->opt->local_addr.addr[0].in.sin_port));
    if(fdscanf(c, c->fd, "%*[^:]: USERID :%*[^:]:%s", name)!=1) {
        s_log(LOG_ERR, "Incorrect data from IDENT server");
        longjmp(c->err, 1);
    }
    closesocket(c->fd);
    c->fd=-1; /* avoid double close at cleanup */
    if(strcmp(name, c->opt->username)) {
        safestring(name);
        s_log(LOG_WARNING, "Connection from %s REFUSED by IDENT (user %s)",
            c->accepting_address, name);
        longjmp(c->err, 1);
    }
    s_log(LOG_INFO, "IDENT authentication passed");
}

static int connect_local(CLI *c) { /* spawn local process */
#if defined (USE_WIN32) || defined (__vms)
    s_log(LOG_ERR, "LOCAL MODE NOT SUPPORTED ON WIN32 and OpenVMS PLATFORM");
    longjmp(c->err, 1);
    return -1; /* some C compilers require a return value */
#else /* USE_WIN32, __vms */
    char env[3][STRLEN], name[STRLEN], *portname;
    int fd[2], pid;
    X509 *peer;
#ifdef HAVE_PTHREAD_SIGMASK
    sigset_t newmask;
#endif

    if (c->opt->option.pty) {
        char tty[STRLEN];

        if(pty_allocate(fd, fd+1, tty, STRLEN))
            longjmp(c->err, 1);
        s_log(LOG_DEBUG, "%s allocated", tty);
    } else
        make_sockets(c, fd);
    pid=fork();
    c->pid=(unsigned long)pid;
    switch(pid) {
    case -1:    /* error */
        closesocket(fd[0]);
        closesocket(fd[1]);
        ioerror("fork");
        longjmp(c->err, 1);
    case  0:    /* child */
        closesocket(fd[0]);
        dup2(fd[1], 0);
        dup2(fd[1], 1);
        if(!options.option.foreground)
            dup2(fd[1], 2);
        closesocket(fd[1]);
        safecopy(env[0], "REMOTE_HOST=");
        safeconcat(env[0], c->accepting_address);
        portname=strrchr(env[0], ':');
        if(portname) /* strip the port name */
            *portname='\0';
        putenv(env[0]);
        if(c->opt->option.transparent) {
            putenv("LD_PRELOAD=" LIBDIR "/libstunnel.so");
            /* For Tru64 _RLD_LIST is used instead */
            putenv("_RLD_LIST=" LIBDIR "/libstunnel.so:DEFAULT");
        }
        if(c->ssl) {
            peer=SSL_get_peer_certificate(c->ssl);
            if(peer) {
                safecopy(env[1], "SSL_CLIENT_DN=");
                X509_NAME_oneline(X509_get_subject_name(peer), name, STRLEN);
                safestring(name);
                safeconcat(env[1], name);
                putenv(env[1]);
                safecopy(env[2], "SSL_CLIENT_I_DN=");
                X509_NAME_oneline(X509_get_issuer_name(peer), name, STRLEN);
                safestring(name);
                safeconcat(env[2], name);
                putenv(env[2]);
                X509_free(peer);
            }
        }
#ifdef HAVE_PTHREAD_SIGMASK
        sigemptyset(&newmask);
        sigprocmask(SIG_SETMASK, &newmask, NULL);
#endif
        execvp(c->opt->execname, c->opt->execargs);
        ioerror(c->opt->execname); /* execv failed */
        _exit(1);
    default:
        break;
    }
    /* parent */
    s_log(LOG_INFO, "Local mode child started (PID=%lu)", c->pid);
    closesocket(fd[1]);
#ifdef FD_CLOEXEC
    fcntl(fd[0], F_SETFD, FD_CLOEXEC);
#endif
    return fd[0];
#endif /* USE_WIN32,__vms */
}

#ifndef USE_WIN32

static void make_sockets(CLI *c, int fd[2]) { /* make a pair of connected sockets */
#ifdef INET_SOCKET_PAIR
    SOCKADDR_UNION addr;
    socklen_t addrlen;
    int s; /* temporary socket awaiting for connection */

    if((s=socket(AF_INET, SOCK_STREAM, 0))<0) {
        sockerror("socket#1");
        longjmp(c->err, 1);
    }
    if((fd[1]=socket(AF_INET, SOCK_STREAM, 0))<0) {
        sockerror("socket#2");
        longjmp(c->err, 1);
    }
    addrlen=sizeof(SOCKADDR_UNION);
    memset(&addr, 0, addrlen);
    addr.in.sin_family=AF_INET;
    addr.in.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    addr.in.sin_port=0; /* dynamic port allocation */
    if(bind(s, &addr.sa, addrlen))
        log_error(LOG_DEBUG, get_last_socket_error(), "bind#1");
    if(bind(fd[1], &addr.sa, addrlen))
        log_error(LOG_DEBUG, get_last_socket_error(), "bind#2");
    if(listen(s, 5)) {
        sockerror("listen");
        longjmp(c->err, 1);
    }
    if(getsockname(s, &addr.sa, &addrlen)) {
        sockerror("getsockname");
        longjmp(c->err, 1);
    }
    if(connect(fd[1], &addr.sa, addrlen)) {
        sockerror("connect");
        longjmp(c->err, 1);
    }
    if((fd[0]=accept(s, &addr.sa, &addrlen))<0) {
        sockerror("accept");
        longjmp(c->err, 1);
    }
    closesocket(s); /* don't care about the result */
#else
    if(socketpair(AF_UNIX, SOCK_STREAM, 0, fd)) {
        sockerror("socketpair");
        longjmp(c->err, 1);
    }
#endif
}
#endif

static int connect_remote(CLI *c) { /* connect to remote host */
    SOCKADDR_UNION bind_addr, addr;
    SOCKADDR_LIST resolved_list, *address_list;
    int error, fd;
    u16 i;

    /* setup address_list */
    if(c->opt->option.delayed_lookup) {
        resolved_list.num=0;
        if(!name2addrlist(&resolved_list,
                c->opt->remote_address, DEFAULT_LOOPBACK)){
            s_log(LOG_ERR, "No host resolved");
            longjmp(c->err, 1);
        }
        address_list=&resolved_list;
    } else /* use pre-resolved addresses */
        address_list=&c->opt->remote_addr;

    /* try to connect each host from the list */
    for(i=0; i<address_list->num; i++) {
        memcpy(&addr, address_list->addr + address_list->cur,
            sizeof(SOCKADDR_UNION));
        address_list->cur=(address_list->cur+1)%address_list->num;
        /* race condition is possible, but harmless in this case */

        if((c->fd=socket(addr.sa.sa_family, SOCK_STREAM, 0))<0) {
            sockerror("remote socket");
            longjmp(c->err, 1);
        }
        if(alloc_fd(c->fd))
            longjmp(c->err, 1);

        if(c->bind_addr.num) { /* explicit local bind or transparent proxy */
            memcpy(&bind_addr, &c->bind_addr.addr[0], sizeof(SOCKADDR_UNION));
            if(bind(c->fd, &bind_addr.sa, addr_len(bind_addr))<0) {
                sockerror("bind transparent");
                longjmp(c->err, 1);
            }
        }

        /* try to connect for the 1st time */
        s_ntop(c->connecting_address, &addr);
        s_log(LOG_DEBUG, "%s connecting %s",
            c->opt->servname, c->connecting_address);
        if(!connect(c->fd, &addr.sa, addr_len(addr))) {
            fd=c->fd;
            c->fd=-1;
            return fd; /* no error -> success (should not be possible) */
        }
        error=get_last_socket_error();
        if(error!=EINPROGRESS && error!=EWOULDBLOCK) {
            s_log(LOG_ERR, "remote connect (%s): %s (%d)",
                c->connecting_address, my_strerror(error), error);
            closesocket(c->fd);
            c->fd=-1;
            continue; /* next IP */
        }
        connect_wait(c);
        fd=c->fd;
        c->fd=-1;
        return fd; /* success! */
    }
    longjmp(c->err, 1);
    return -1; /* some C compilers require a return value */
}

    /* wait for the result of a non-blocking connect */
    /* file descriptor : c->fd                       */
    /* timeout         : c->opt->timeout_connect     */
static void connect_wait(CLI *c) {
    int error;
    socklen_t optlen;

    s_log(LOG_DEBUG, "connect_wait: waiting %d seconds",
        c->opt->timeout_connect);
    s_poll_zero(&c->fds);
    s_poll_add(&c->fds, c->fd, 1, 1);
    switch(s_poll_wait(&c->fds, c->opt->timeout_connect)) {
    case -1:
        sockerror("connect_wait: s_poll_wait");
        longjmp(c->err, 1);
    case 0:
        s_log(LOG_INFO, "connect_wait: s_poll_wait timeout");
        longjmp(c->err, 1);
    default:
        if(s_poll_canread(&c->fds, c->fd)) {
            /* just connected socket should not be ready for read */
            /* get the resulting error code, now */
            optlen=sizeof(error);
            if(!getsockopt(c->fd, SOL_SOCKET, SO_ERROR, (void *)&error, &optlen))
                errno=error;
            if(errno) { /* really an error? */
                sockerror("connect_wait: getsockopt");
                longjmp(c->err, 1);
            }
        }
        if(s_poll_canwrite(&c->fds, c->fd)) {
            s_log(LOG_DEBUG, "connect_wait: connected");
            return; /* success */
        }
        s_log(LOG_ERR, "connect_wait: unexpected s_poll_wait result");
        longjmp(c->err, 1);
    }
    longjmp(c->err, 1); /* should not be possible */
}

static void reset(int fd, char *txt) {
    /* Set lingering on a socket if needed*/
    struct linger l;

    l.l_onoff=1;
    l.l_linger=0;
    if(setsockopt(fd, SOL_SOCKET, SO_LINGER, (void *)&l, sizeof(l)))
        log_error(LOG_DEBUG, get_last_socket_error(), txt);
}

/* End of client.c */
