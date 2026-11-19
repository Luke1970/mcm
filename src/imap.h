/* imap.h - minimal IMAP4rev1 client over TLS (IMAPS, port 993) */
#ifndef MCM_IMAP_H
#define MCM_IMAP_H

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

enum { FOLDER_SENT = 1, FOLDER_DRAFTS = 2, FOLDER_TRASH = 3 };

typedef struct {
	uint32_t seq, uid, size;
	int      seen, answered, flagged, deleted;
	int      att;      /* has an attachment (the @ in the list) */
	time_t   date;
	char    *from;     /* decoded display name or address */
	char    *subject;  /* decoded */
	char    *addr;     /* the sender's address (user@host), or NULL */
	char    *msgid;    /* Message-ID, <...> included, or NULL */
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
	int           html_inside;   /* text/plain that holds an HTML page (msgview.c) */
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
/* Copy, move or remove n messages of the selected mailbox (by uid). Move
 * uses MOVE (RFC 6851) when the server has it, else COPY and expunge.
 * imap_expunge removes only these messages (UID EXPUNGE, RFC 4315); without
 * UIDPLUS they are only marked \Deleted and 1 is returned (error set).
 * Expunges show up in imap_changes(). */
int  imap_copy(imap_t *im, const uint32_t *uids, int n, const char *mbox);
int  imap_move(imap_t *im, const uint32_t *uids, int n, const char *mbox);
int  imap_expunge(imap_t *im, const uint32_t *uids, int n);
int  imap_create(imap_t *im, const char *mbox);
/* Delete a folder (and unsubscribe), rename one (subfolders go along). */
int  imap_delete(imap_t *im, const char *mbox);
int  imap_rename(imap_t *im, const char *from, const char *to);
/* To undo the last imap_copy/imap_move: the uids of the copies (COPYUID,
 * RFC 4315; malloc'd), or, from a server without UIDPLUS, *uids NULL and
 * the UIDNEXT the mailbox had before (0: unknown); and its UIDVALIDITY. */
void imap_copied(imap_t *im, uint32_t **uids, int *n, uint32_t *next, uint32_t *validity);
/* UIDVALIDITY of the selected mailbox (0: the server did not say). */
uint32_t imap_uidvalidity(imap_t *im);
/* The uids from next on in the selected mailbox (malloc'd). */
int  imap_uids_from(imap_t *im, uint32_t next, uint32_t **uids, int *n);
/* The Message-IDs of n messages of the selected mailbox ("" for one
 * without), and whose they are (uid); malloc'd, in no particular order. */
int  imap_message_ids(imap_t *im, const uint32_t *uids, int n, char ***ids, uint32_t **iduids, int *nids);
/* Remove flags (e.g. "\\Deleted") from n messages of the selected mailbox. */
int  imap_unflag(imap_t *im, const uint32_t *uids, int n, const char *flags);
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
char *mutf7_encode(const char *s);

#endif
