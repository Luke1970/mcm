/* pop3.h - minimal POP3 client (RFC 1939) over TLS
 *
 * Port 995 (or any port but 110) is POP3 over TLS from the start; on port
 * 110, or when a server greets in plain text, STLS (RFC 2595) is required:
 * the password never travels unencrypted. Messages are left on the server.
 *
 * A POP3 session sees the mailbox as it was at login; pop3_reconnect()
 * starts a new session to see new mail. Message numbers can change between
 * sessions, so callers identify messages by their UIDL and check
 * pop3_generation() to know when numbers must be looked up again.
 */
#ifndef MCM_POP3_H
#define MCM_POP3_H

#include <stddef.h>
#include <stdint.h>

#include "net.h"
#include "util.h"

typedef struct pop3 pop3_t;

typedef struct {
	int      num;          /* message number in this session */
	uint32_t size;         /* octets, as the server counts them (CRLF lines) */
	char     uidl[72];     /* unique id, stable across sessions */
} pop3_msg;

pop3_t     *pop3_new(void);
void        pop3_free(pop3_t *p);
const char *pop3_error(pop3_t *p);
void        pop3_set_cert_check(pop3_t *p, net_cert_fn fn, void *ctx);

int  pop3_connect(pop3_t *p, const char *host, const char *port);
int  pop3_login(pop3_t *p, const char *user, const char *pass);
/* New session (QUIT, connect, log in again): shows new mail. */
int  pop3_reconnect(pop3_t *p);
void pop3_quit(pop3_t *p);
/* Mark message num for removal; the server removes it at QUIT (pop3_quit,
 * pop3_reconnect), and not at all when the connection breaks first. */
int  pop3_dele(pop3_t *p, int num);
/* Incremented by every new session. */
unsigned    pop3_generation(pop3_t *p);
const char *pop3_password(pop3_t *p);

/* All messages: numbers, sizes and unique ids (LIST + UIDL). */
int  pop3_scan(pop3_t *p, pop3_msg **out, int *n);
/* Header block of a message (TOP n 0), or the whole message (RETR n),
 * appended to out with CRLF line ends and dot-stuffing removed. */
int  pop3_top(pop3_t *p, int num, buf_t *out);
int  pop3_retr(pop3_t *p, int num, buf_t *out);
/* A new session if the connection was lost (check pop3_generation()). */
int  pop3_ensure(pop3_t *p);
/* Keep the session alive. */
int  pop3_noop(pop3_t *p);

#endif
