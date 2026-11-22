/* compose.h - turn the editor text into an Internet message
 *
 * The editor text is a header block ("Name: value" lines, continuation lines
 * start with white space), an empty line, and the body. compose_build()
 * checks the header syntax and produces a message that follows
 *
 *   RFC 5322  Internet Message Format (address lists, date-time, msg-id,
 *             line length <= 998, folding at 78)
 *   RFC 2045-2049  MIME: text/plain, charset, 7bit/8bit/quoted-printable,
 *             RFC 2047 encoded-words for non-ASCII header text
 *   RFC 6532  UTF-8 header fields, used only when an address itself is
 *             non-ASCII; such a message needs SMTPUTF8 (RFC 6531)
 *   RFC 6854  group syntax in From:/Sender:
 *   RFC 3676  text/plain; format=flowed: a line ending in a space is a
 *             soft break (the editor wraps at 72), space-stuffing
 *   RFC 2046/2183/2231  attachments: each "Attach: PATH" line in the header
 *             block adds that file (base64) to a multipart/mixed message,
 *             with its file name (RFC 2231 encoded when not plain ASCII)
 *
 * and the SMTP envelope (MAIL FROM / RCPT TO) that goes with it.
 */
#ifndef MCM_COMPOSE_H
#define MCM_COMPOSE_H

#include <stddef.h>
#include <time.h>

#include "util.h"

typedef struct {
	const char *user_agent;   /* value of User-Agent: */
	const char *auto_date;    /* the template's Date value: replaced by the send time */
	int smtputf8;             /* UTF-8 addresses/headers may be used (RFC 6531/6532) */
	int eightbit;             /* body may be 8bit (8BITMIME) */
	int draft;                /* for Drafts: recipients optional, bad addresses kept as text */
	/* PGP: line (sign, encrypt): allowed for this account; pgp, when set,
	 * turns the body entity (CRLF) into a multipart/signed or
	 * multipart/encrypted one (out); NULL: only checked. rcpt: To and Cc,
	 * hidden: Bcc. 0 = done, else err says why. */
	int pgp_ok;
	int (*pgp)(void *ctx, int how, const buf_t *entity, const char *const *rcpt, int nrcpt,
	           const char *const *hidden, int nhidden, buf_t *out, char *err, size_t errlen);
	void *pgp_ctx;
} compose_opts;

enum { COMPOSE_SIGN = 1, COMPOSE_ENCRYPT = 2 };
/* A PGP: value ("sign", "encrypt", "sign encrypt", empty) as COMPOSE_*
 * bits; -1 when it has another word. */
int compose_pgp_parse(const char *v, int *how);

typedef struct {
	buf_t  msg;               /* message to transmit (without Bcc:), CRLF line ends */
	buf_t  copy;              /* copy for the Sent folder (with Bcc:) */
	char  *mail_from;         /* envelope sender */
	char **rcpt;              /* envelope recipients (To, Cc, Bcc) */
	int    nrcpt;
	int    smtputf8;          /* message needs SMTPUTF8 (non-ASCII address) */
	int    eightbit;          /* body is sent as 8bit: BODY=8BITMIME */
	int    no_subject;        /* Subject: missing or empty */
	char  *subject;           /* decoded subject, for messages to the user */
	int    nattach;           /* files attached (Attach: lines) */
	size_t attach_bytes;      /* their size together */
	int    pgp;               /* the PGP: line asks for COMPOSE_SIGN / COMPOSE_ENCRYPT */
	int    pgp_done;          /* and the message was signed / encrypted */
	int    err_line;          /* 0-based line of the error in the text, or -1 */
	char   err[512];
} compose_out;

int   compose_build(const char *text, size_t len, const compose_opts *o, compose_out *out);
void  compose_out_free(compose_out *out);

/* RFC 5322 date-time in English with the local zone, e.g.
 * "Fri, 13 Nov 2026 14:05:09 +0100". Returns a static buffer. */
const char *compose_date(time_t t);

/* Parse an address list (raw header text, encoded-words allowed) and return
 * the mailboxes in editor form "Name <addr>, addr2", or NULL if empty/bad.
 * `skip` (may be NULL) is an address to leave out (case-insensitive). */
char *compose_addrs(const char *raw, const char *skip);
/* The same, leaving out every address of skip_list (raw header text, may be
 * NULL) and addresses that appear twice. */
char *compose_addrs_skip(const char *raw, const char *skip_list);

/* Just the addr-spec of the first mailbox in raw, or NULL. */
char *compose_first_addr(const char *raw);

#endif
