/* mime.h - RFC 2047 header decoding and MIME message rendering */
#ifndef EMSG_MIME_H
#define EMSG_MIME_H

#include <stddef.h>
#include <time.h>

/* Convert text in `charset` to UTF-8. Always returns a malloc'd NUL-terminated string. */
char *conv_to_utf8(const char *charset, const char *in, size_t len, size_t *outlen);

/* Decode RFC 2047 encoded-words (=?utf-8?Q?...?=) into UTF-8. */
char *mime_decode_words(const char *s);

/* Return the (unfolded, decoded) value of a top-level header, or NULL. */
char *mime_header(const char *raw, size_t len, const char *name);

/* Unfolded but undecoded value of a top-level header, or NULL. */
char *mime_header_raw(const char *raw, size_t len, const char *name);

/* Render a raw RFC 822 message as plain UTF-8 text for display.
 * all_headers: show the complete raw header block instead of a summary. */
char *mime_render(const char *raw, size_t len, int all_headers);

/* Summary (From, To, Cc, Date, Subject) of a header block, ending in a blank line. */
char *mime_summary(const char *hdr, size_t len);

/* Decode one body part (transfer encoding + charset) to UTF-8; HTML and
 * Markdown are turned into plain text (textconv.h). */
enum { MIME_PLAIN, MIME_HTML, MIME_MARKDOWN };
char *mime_decode_text(const char *data, size_t len, int kind, const char *cte,
                       const char *charset, size_t *outlen);
/* text/markdown or text/x-markdown */
int   mime_is_markdown(const char *type);

/* Undo format=flowed (RFC 3676): join soft-broken lines (ending in a space)
 * into paragraphs, remove space-stuffing; with delsp the joining space is
 * dropped too. Quoted lines come out as ">> text". malloc'd, NUL-terminated. */
char *mime_unflow(const char *s, size_t n, int delsp, size_t *outlen);

void  mime_human_size(char *dst, size_t n, size_t bytes);

/* RFC 2822 date ("Thu, 12 Nov 2026 23:10:05 +0100") or IMAP INTERNALDATE
 * ("12-Nov-2026 23:10:05 +0100") as a time_t; 0 if it cannot be read. */
time_t mime_parse_date(const char *s);

/* Display form of a From: header value: the decoded name, else the address. */
char *mime_addr_display(const char *v);

#endif
