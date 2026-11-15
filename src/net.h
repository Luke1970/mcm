/* net.h - TCP connections with TLS (implicit or STARTTLS) and certificate trust
 *
 * Shared by the IMAP and SMTP clients. A certificate that fails normal
 * verification (system CAs + host name) is passed to a callback that may
 * accept it anyway; nothing is sent over the connection before that decision.
 */
#ifndef MCM_NET_H
#define MCM_NET_H

#include <stddef.h>

#include "util.h"

/* A server certificate that failed verification. */
typedef struct {
	const char *host, *port;
	const char *problem;          /* e.g. "self-signed certificate; hostname mismatch" */
	char subject[256], issuer[256];
	char names[256];              /* DNS/IP names the certificate is valid for */
	char not_before[16], not_after[16];   /* YYYY-MM-DD */
	char sha256[100];             /* fingerprint, AB:CD:... */
} net_certinfo;

/* Return 1 to accept the certificate anyway, 0 to refuse. */
typedef int (*net_cert_fn)(const net_certinfo *ci, void *ctx);

typedef struct {
	net_cert_fn fn;
	void       *ctx;
	char       *accepted;   /* "host port sha256" accepted earlier (owned) */
} net_trust;

typedef struct net net_t;

/* Plain TCP connection. On failure returns NULL with a message in err. */
net_t *net_open(const char *host, const char *port, int timeout_s, char *err, size_t errlen);

/* TLS handshake on the connection (implicit TLS, or after STARTTLS).
 * An unverified certificate is accepted if it equals trust->accepted, or,
 * when interactive, if trust->fn accepts it (it is then remembered there).
 * Fails if the server sent data ahead of the handshake (STARTTLS injection). */
int  net_tls(net_t *n, net_trust *trust, int interactive);
int  net_is_tls(const net_t *n);

int  net_write(net_t *n, const void *p, size_t len);
int  net_line(net_t *n, buf_t *b);              /* append one line incl. LF */
int  net_bytes(net_t *n, buf_t *b, size_t len); /* append exactly len bytes */
void net_set_timeout(net_t *n, int seconds);
/* Local address as an SMTP address literal: "[192.0.2.1]" or "[IPv6:...]". */
void net_local_literal(const net_t *n, char *out, size_t size);

const char *net_error(const net_t *n);
void net_close(net_t *n);                       /* NULL is fine */

void net_trust_free(net_trust *t);

#endif
