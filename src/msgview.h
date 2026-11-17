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

#define MV_NO_MORE ((size_t)-1)

typedef struct {
	mpart_t *part;     /* not owned (points into root) */
	buf_t    raw;      /* encoded bytes fetched so far */
	int      done;     /* everything fetched */
} mv_leaf;

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
	size_t    more_off;      /* offset of the first "more" marker, or MV_NO_MORE */
	int       flowed;        /* a shown text part was format=flowed (unwrapped) */
	size_t    hdr_end;       /* text before this offset is the header section */

	/* hex window */
	size_t    hex_off;
	buf_t     hex;
} msgview_t;

int  mv_open(msgview_t *mv, store_t *im, uint32_t uid, uint32_t size);
void mv_close(msgview_t *mv);

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
/* Fetch the whole (still encoded) content of part p into out. */
int  mv_fetch_part(msgview_t *mv, store_t *im, const mpart_t *p, buf_t *out);

/* Cached raw bytes [off, off+len), or NULL if not in the window. */
const unsigned char *mv_hex_peek(msgview_t *mv, size_t off, size_t len);
/* Load the window around [off, off+len). */
int  mv_hex_load(msgview_t *mv, store_t *im, size_t off, size_t len);

#endif
