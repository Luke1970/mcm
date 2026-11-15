/* imap.h - minimal IMAP4rev1 client over TLS (IMAPS, port 993) */
#ifndef EMSG_IMAP_H
#define EMSG_IMAP_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "net.h"
#include "util.h"

typedef struct imap imap_t;

typedef struct {
	char *name;      /* raw (modified UTF-7) mailbox name, used in commands */
	char *display;   /* last hierarchy component, decoded to UTF-8 */
	char  delim;
	int   depth;
	int   noselect;
	int   special;   /* FOLDER_SENT / FOLDER_DRAFTS from RFC 6154 attributes, or 0 */
} folder_t;

enum { FOLDER_SENT = 1, FOLDER_DRAFTS = 2 };

typedef struct {
	uint32_t seq, uid, size;
	int      seen, answered, flagged, deleted;
	time_t   date;
	char    *from;     /* decoded display name or address */
	char    *subject;  /* decoded */
} msg_t;

/* One node of a message's MIME structure (from BODYSTRUCTURE). */
typedef struct mpart {
	char          section[64];   /* IMAP part number: "1", "2.1", ... */
	char          type[32];      /* lowercase: text, multipart, image, ... */
	char          subtype[64];   /* lowercase: plain, html, mixed, ... */
	char          charset[48];
	char          cte[32];       /* content-transfer-encoding, lowercase */
	char         *name;          /* decoded file name, or NULL */
	int           attachment;    /* Content-Disposition: attachment */
	int           flowed, delsp; /* text/plain; format=flowed; delsp=yes (RFC 3676) */
	uint32_t      size;          /* encoded size in octets */
	struct mpart *kids;          /* children of a multipart */
	int           nkids;
} mpart_t;

/* A request for part of a message: BODY.PEEK[section]<off.len>. */
typedef struct {
	const char *section;   /* "" = whole message, "1.2", "TEXT", ... */
	size_t      off, len;
	buf_t      *out;       /* data is appended here */
	size_t      got;       /* set by imap_fetch_partial */
} imap_partial_t;

imap_t     *imap_new(void);
void        imap_free(imap_t *im);
const char *imap_error(imap_t *im);

/* Called during imap_connect() when the certificate is not trusted; see
 * net.h. An accepted certificate is also accepted on automatic reconnects
 * to the same server; a different one is refused there without asking. */
void imap_set_cert_check(imap_t *im, net_cert_fn fn, void *ctx);

int  imap_connect(imap_t *im, const char *host, const char *port);
int  imap_login(imap_t *im, const char *user, const char *pass);
void imap_logout(imap_t *im);

int  imap_list(imap_t *im, folder_t **out, int *n);
int  imap_select(imap_t *im, const char *mbox, uint32_t *exists);
/* Envelopes for sequence numbers from..to; out[i] is seq from+i (uid 0 if missing). */
int  imap_fetch_headers(imap_t *im, uint32_t from, uint32_t to, msg_t **out, int *n);
/* The same for n sequence numbers (ascending); out[i] is seqs[i]. */
int  imap_fetch_set(imap_t *im, const uint32_t *seqs, int n, msg_t **out, int *nout);
int  imap_status_unseen(imap_t *im, const char *mbox, uint32_t *unseen);
/* Header block and MIME structure (root may be NULL if the server sent none). */
int  imap_fetch_structure(imap_t *im, uint32_t uid, char **hdr, size_t *hlen, mpart_t **root);
int  imap_fetch_partial(imap_t *im, uint32_t uid, imap_partial_t *req, int n);
int  imap_mark_seen(imap_t *im, uint32_t uid);
int  imap_add_flags(imap_t *im, uint32_t uid, const char *flags);   /* e.g. "\\Answered" */
/* The login password (kept in memory for reconnects; offered for SMTP). */
const char *imap_password(imap_t *im);
int  imap_noop(imap_t *im);
/* Mailbox changes since the last call: returns 1 if anything changed. Expunged
 * sequence numbers are in server order, valid until the next call. */
int  imap_changes(imap_t *im, uint32_t *exists, const uint32_t **expunged, int *n);

void mpart_free(mpart_t *p);
void folders_free(folder_t *f, int n);
void msgs_free(msg_t *m, int n);

/* Sequence numbers (ascending, malloc'd) of the messages in the selected
 * mailbox whose Subject or From contains text (case-insensitive). */
int imap_search(imap_t *im, const char *text, uint32_t **out, int *n);

/* Store a message (CRLF line ends) in a mailbox; flags like "\\Seen". */
int imap_append(imap_t *im, const char *mbox, const char *flags, const char *data, size_t len);

/* Decode an IMAP modified UTF-7 mailbox name to UTF-8 (malloc'd). */
char *mutf7_decode(const char *s);

#endif
