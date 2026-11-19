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

void html_to_text(const char *s, size_t len, buf_t *out);
void md_to_text(const char *s, size_t len, buf_t *out);

/* Append s without the characters that show nothing (zero-width space and
 * non-joiner, soft hyphen, BOM, ...); entities: also those written as HTML
 * entities (&#8204; &zwnj;), for plain text made from HTML. Spaces between
 * them shrink to one, and none are left at the end of a line. */
void strip_invisible(const char *s, size_t len, int entities, buf_t *out);

#endif
