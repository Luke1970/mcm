/* msgview.h - an open message, loaded piece by piece from the server
 *
 * Only what is shown is downloaded: the header block, the MIME structure,
 * and the first block of each displayed text part. More text is fetched
 * when the reader scrolls to the end of what has been loaded; attachments
 * are never downloaded just to display a message. The hex view keeps a
 * single small window of raw bytes around the visible range.
 */
#ifndef MCM_MSGVIEW_H
#define MCM_MSGVIEW_H

#include <stddef.h>
#include <stdint.h>

#include "store.h"
#include "util.h"
#include "textconv.h"

#define MV_NO_MORE ((size_t)-1)

typedef struct {
	mpart_t *part;     /* not owned (points into root) */
	buf_t    raw;      /* encoded bytes fetched so far */
	int      done;     /* everything fetched */
} mv_leaf;

/* PGP (set by the program while the account has it on; NULL: off).
 * from: the sender's address. Both return a note for the reader. */
typedef struct {
	/* a detached signature sig over data */
	char *(*verify)(const char *data, size_t n, const char *sig, size_t sn, const char *from);
	/* an OpenPGP message or clearsigned text: *out its content (empty
	 * when it could not be had); *encrypted: it was encrypted */
	char *(*decrypt)(const char *in, size_t n, buf_t *out, int *encrypted, const char *from);
} mv_pgp_t;
extern const mv_pgp_t *mv_pgp;

/* An inline PGP block in the text and what it became. */
typedef struct {
	char  *armor;        /* the block as it appears in the decoded text (no CR) */
	char  *result;       /* "[PGP: note]\n" and its content */
} mv_pgpblk;

typedef struct {
	int       open;
	uint32_t  uid, size;     /* size = RFC822.SIZE of the whole message */
	char     *hdr;
	size_t    hlen;
	mpart_t  *root;
	mpart_t   fallback;      /* used when the server sent no BODYSTRUCTURE */
	mv_leaf  *leaves;
	int       nleaves;

	/* rendered text */
	char     *text;
	size_t    textlen;
	ts_span  *sty;           /* styled spans of text (Markdown: bold, italic, ...) */
	int       nsty;
	size_t    more_off;      /* offset of the first "more" marker, or MV_NO_MORE */
	int       flowed;        /* a shown text part was format=flowed (unwrapped) */
	size_t    hdr_end;       /* text before this offset is the header section */
	int       plain;         /* T: the plain-text version as sent, not mcm's choice */
	int       plain_changed;

	/* PGP */
	char     *pgp_note;      /* PGP/MIME: what the signature check or decryption said */
	int       pgp_enc;       /* something in it was encrypted (a reply should be too) */
	int       pgp_decrypted; /* hdr + the fallback leaf are the decrypted message */
	mv_pgpblk *pgp_blk;      /* inline blocks done */
	int       npgp_blk;

	/* hex window */
	size_t    hex_off;
	buf_t     hex;
} msgview_t;

int  mv_open(msgview_t *mv, store_t *im, uint32_t uid, uint32_t size);
void mv_close(msgview_t *mv);
/* T: show the plain-text version of the message as sent (on), or mcm's
 * choice again (off). 0 done, 1 the message has no plain-text version,
 * -1 error (fetching). mv_render afterwards. */
int  mv_set_plain(msgview_t *mv, store_t *im, int on);
int  mv_repick(msgview_t *mv, store_t *im);

/* (Re)build mv->text for display. */
void mv_render(msgview_t *mv, int all_headers);

/* The body text loaded so far, without headers (for reply/forward). */
char *mv_body(msgview_t *mv);

/* Fetch the next block of the first incompletely loaded part. */
int  mv_more(msgview_t *mv, store_t *im);

/* Attachments: the parts shown as "[Attachment: ...]", from the server's
 * structure (IMAP). Returns how many (*out: malloc'd array of pointers into
 * the structure), 0 without a structure (then the raw message is parsed,
 * mime_attachments). */
int  mv_attachments(msgview_t *mv, mpart_t ***out);
/* The whole raw message (the decrypted one, if it was encrypted). */
int  mv_raw(msgview_t *mv, store_t *im, buf_t *out);
/* Fetch the whole (still encoded) content of part p into out. */
int  mv_fetch_part(msgview_t *mv, store_t *im, const mpart_t *p, buf_t *out);

/* Cached raw bytes [off, off+len), or NULL if not in the window. */
const unsigned char *mv_hex_peek(msgview_t *mv, size_t off, size_t len);
/* Load the window around [off, off+len). */
int  mv_hex_load(msgview_t *mv, store_t *im, size_t off, size_t len);

#endif
