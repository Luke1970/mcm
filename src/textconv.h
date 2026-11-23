/* textconv.h - HTML and Markdown message parts as plain text
 *
 * Both keep the text readable in a terminal: links become "text [n]" with a
 * numbered list of their addresses at the end, images their alt text.
 *
 * HTML: <style>, <script> and <head> are left out; paragraphs, line breaks,
 * headings (h1 underlined with =, h2 with -), bullet and numbered lists
 * (indented when nested), quotes ("> "), <pre> text exactly as written, and
 * tables: a simple data table is shown in aligned columns, a layout table
 * (nested, or one cell per row) as its text.
 *
 * Markdown: the markup characters are removed where they only mark style
 * (**bold**, *italic*, `code`, # headings), code blocks are kept as written
 * (indented), lists and quotes stay as they are.
 */
#ifndef MCM_TEXTCONV_H
#define MCM_TEXTCONV_H

#include "util.h"

/* Styles. While styles are on (textconv_styles(1); the message window
 * turns them on while it renders), md_to_text marks bold, italic,
 * struck-through, code and heading text with invisible marker characters
 * (U+F8E0 + 2 * style, + 1 where it ends); ts_strip takes them out again
 * and lists the styled spans. */
enum { TS_BOLD, TS_ITALIC, TS_STRIKE, TS_CODE, TS_HEAD, TS_N };
typedef struct {
	size_t off, len;
	unsigned style;          /* 1 << TS_BOLD | ... */
} ts_span;
void textconv_styles(int on);
/* Remove the markers from t (in place); returns the new length. *sp: the
 * spans (malloc'ed, in order, NULL when none), *nsp their number; *keep,
 * an offset into t ((size_t)-1: none), is moved along. */
size_t ts_strip(char *t, size_t len, ts_span **sp, int *nsp, size_t *keep);

void html_to_text(const char *s, size_t len, buf_t *out);
void md_to_text(const char *s, size_t len, buf_t *out);

/* Plain text that uses Markdown: at least two kinds of markup among
 * # headings (two count as two), **bold**, *italic*, `code`, [text](url)
 * links and ``` code blocks, outside quoted lines. */
int md_detect(const char *s, size_t len);

/* Append s without the characters that show nothing (zero-width space and
 * non-joiner, soft hyphen, BOM, ...); entities: also those written as HTML
 * entities (&#8204; &zwnj;), for plain text made from HTML. Spaces between
 * them shrink to one, and none are left at the end of a line. */
void strip_invisible(const char *s, size_t len, int entities, buf_t *out);

#endif
