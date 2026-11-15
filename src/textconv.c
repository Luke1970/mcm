/* textconv.c - HTML and Markdown message parts as plain text (see textconv.h) */
#include "textconv.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <wchar.h>

/* ------------------------------------------------------------------ */
/* Shared helpers                                                      */

typedef struct {
	char **url;
	int n;
} links_t;

#define MAX_LINKS 999

/* Number of url in the list (1-based), added when new; 0 when the list is full. */
static int link_add(links_t *l, const char *url)
{
	for (int i = 0; i < l->n; i++)
		if (!strcmp(l->url[i], url)) return i + 1;
	if (l->n >= MAX_LINKS) return 0;
	l->url = xrealloc(l->url, sizeof *l->url * (size_t)(l->n + 1));
	l->url[l->n++] = xstrdup(url);
	return l->n;
}

static void ensure_nl(buf_t *out)
{
	if (out->len && out->d[out->len - 1] != '\n') buf_addc(out, '\n');
}

static void ensure_para(buf_t *out)
{
	ensure_nl(out);
	if (out->len >= 2 && out->d[out->len - 2] != '\n') buf_addc(out, '\n');
}

static void links_out(links_t *l, buf_t *out)
{
	if (l->n) {
		ensure_para(out);
		buf_adds(out, "Links:\n");
		for (int i = 0; i < l->n; i++) buf_printf(out, "[%d] %s\n", i + 1, l->url[i]);
	}
	for (int i = 0; i < l->n; i++) free(l->url[i]);
	free(l->url);
	l->url = NULL;
	l->n = 0;
}

/* Screen columns of n bytes of UTF-8. */
static size_t text_width(const char *s, size_t n)
{
	size_t w = 0, i = 0;
	while (i < n) {
		size_t cl;
		wchar_t c = utf8_next(s + i, n - i, &cl);
		int cw = wcwidth(c);
		w += cw > 0 ? (size_t)cw : 0;
		i += cl;
	}
	return w;
}

static void add_repeat(buf_t *out, char c, size_t n)
{
	for (size_t i = 0; i < n; i++) buf_addc(out, c);
}

/* Width of the last line in out, from offset start on. */
static size_t last_line_width(buf_t *out, size_t start)
{
	size_t b = out->len;
	while (b > start && out->d[b - 1] != '\n') b--;
	return text_width(out->d + b, out->len - b);
}

/* Clean up: no spaces at line ends, at most one empty line in a row, no
 * empty lines at the start. */
static void tidy(buf_t *out, size_t start)
{
	size_t w = start, nl = 2;
	for (size_t r = start; r < out->len; r++) {
		char c = out->d[r];
		if (c == '\n') {
			while (w > start && out->d[w - 1] == ' ') w--;
			if (nl >= 2) continue;
			nl++;
		} else {
			nl = 0;
		}
		out->d[w++] = c;
	}
	out->len = w;
}

/* ------------------------------------------------------------------ */
/* HTML                                                                */

static const struct { const char *name; uint32_t cp; } entities[] = {
	{"amp", '&'}, {"lt", '<'}, {"gt", '>'}, {"quot", '"'}, {"apos", '\''},
	{"nbsp", ' '}, {"ensp", ' '}, {"emsp", ' '}, {"thinsp", ' '},
	{"copy", 0xA9}, {"reg", 0xAE}, {"trade", 0x2122},
	{"mdash", 0x2014}, {"ndash", 0x2013}, {"hellip", 0x2026}, {"euro", 0x20AC},
	{"pound", 0xA3}, {"yen", 0xA5}, {"cent", 0xA2}, {"sect", 0xA7}, {"para", 0xB6},
	{"deg", 0xB0}, {"times", 0xD7}, {"divide", 0xF7}, {"plusmn", 0xB1},
	{"frac12", 0xBD}, {"frac14", 0xBC}, {"frac34", 0xBE}, {"micro", 0xB5},
	{"lsquo", 0x2018}, {"rsquo", 0x2019}, {"ldquo", 0x201C}, {"rdquo", 0x201D},
	{"sbquo", 0x201A}, {"bdquo", 0x201E}, {"prime", 0x2032},
	{"bull", 0x2022}, {"middot", 0xB7}, {"laquo", 0xAB}, {"raquo", 0xBB},
	{"larr", 0x2190}, {"rarr", 0x2192}, {"uarr", 0x2191}, {"darr", 0x2193},
	{"iexcl", 0xA1}, {"iquest", 0xBF},
	{"auml", 0xE4}, {"ouml", 0xF6}, {"uuml", 0xFC}, {"Auml", 0xC4}, {"Ouml", 0xD6},
	{"Uuml", 0xDC}, {"euml", 0xEB}, {"iuml", 0xEF}, {"Euml", 0xCB}, {"Iuml", 0xCF},
	{"szlig", 0xDF}, {"aacute", 0xE1}, {"eacute", 0xE9}, {"iacute", 0xED},
	{"oacute", 0xF3}, {"uacute", 0xFA}, {"Aacute", 0xC1}, {"Eacute", 0xC9},
	{"agrave", 0xE0}, {"egrave", 0xE8}, {"igrave", 0xEC}, {"ograve", 0xF2},
	{"ugrave", 0xF9}, {"Agrave", 0xC0}, {"Egrave", 0xC8},
	{"acirc", 0xE2}, {"ecirc", 0xEA}, {"icirc", 0xEE}, {"ocirc", 0xF4}, {"ucirc", 0xFB},
	{"ccedil", 0xE7}, {"Ccedil", 0xC7}, {"ntilde", 0xF1}, {"Ntilde", 0xD1},
	{"atilde", 0xE3}, {"otilde", 0xF5}, {"aring", 0xE5}, {"Aring", 0xC5},
	{"aelig", 0xE6}, {"AElig", 0xC6}, {"oslash", 0xF8}, {"Oslash", 0xD8},
	{"zwnj", 0}, {"zwj", 0}, {"shy", 0}, {"lrm", 0}, {"rlm", 0},
};

/* An entity at s ("&...;"): append it to out and return its length, or 0. */
static size_t entity(const char *s, size_t len, buf_t *out)
{
	size_t j = 1;
	while (j < len && j < 12 && s[j] != ';' && !isspace((unsigned char)s[j]) && s[j] != '&') j++;
	if (j >= len || s[j] != ';') return 0;
	char ent[16];
	snprintf(ent, sizeof ent, "%.*s", (int)(j - 1), s + 1);
	long cp = -1;
	if (ent[0] == '#') {
		cp = (ent[1] == 'x' || ent[1] == 'X') ? strtol(ent + 2, NULL, 16) : strtol(ent + 1, NULL, 10);
		if (cp == 160) cp = ' ';
		if (cp >= 0x80 && cp < 0xA0) cp = (long)cp1252_char((uint32_t)cp);   /* as HTML5: &#146; is ’ */
		if (cp == 0) return j + 1;
	} else {
		for (size_t e = 0; e < sizeof entities / sizeof *entities; e++)
			if (!strcmp(ent, entities[e].name)) { cp = (long)entities[e].cp; break; }
	}
	if (cp < 0) return 0;
	if (cp > 0 && cp <= 0x10FFFF) utf8_put(out, (uint32_t)cp);
	return j + 1;
}

/* Value of attribute name in the tag text t[0..n) ("<a href=...>"), with
 * entities decoded; NULL when absent. */
static char *attr_get(const char *t, size_t n, const char *name)
{
	size_t i = 1, nl = strlen(name);
	while (i < n && (isalnum((unsigned char)t[i]) || t[i] == '/')) i++;   /* tag name */
	while (i < n) {
		while (i < n && (isspace((unsigned char)t[i]) || t[i] == '/')) i++;
		if (i >= n || t[i] == '>') break;
		size_t a = i;
		while (i < n && !isspace((unsigned char)t[i]) && t[i] != '=' && t[i] != '>' && t[i] != '/') i++;
		size_t al = i - a;
		while (i < n && isspace((unsigned char)t[i])) i++;
		const char *v = NULL;
		size_t vl = 0;
		if (i < n && t[i] == '=') {
			i++;
			while (i < n && isspace((unsigned char)t[i])) i++;
			if (i < n && (t[i] == '"' || t[i] == '\'')) {
				char q = t[i++];
				v = t + i;
				while (i < n && t[i] != q) i++;
				vl = (size_t)(t + i - v);
				if (i < n) i++;
			} else {
				v = t + i;
				while (i < n && !isspace((unsigned char)t[i]) && t[i] != '>') i++;
				vl = (size_t)(t + i - v);
			}
		}
		if (al == nl && !strncasecmp(t + a, name, nl)) {
			buf_t b = {0};
			for (size_t k = 0; k < vl;) {
				size_t e = v[k] == '&' ? entity(v + k, vl - k, &b) : 0;
				if (e) { k += e; continue; }
				buf_addc(&b, v[k++]);
			}
			char *r = buf_steal(&b);
			return r ? r : xstrdup("");
		}
	}
	return NULL;
}

/* Tag name at s ("<name" or "</name"), lower case; *close set for "</". */
static void tag_name(const char *s, size_t len, char *name, size_t nsize, int *close)
{
	size_t j = 1, k = 0;
	*close = 0;
	if (j < len && s[j] == '/') { *close = 1; j++; }
	while (j < len && isalnum((unsigned char)s[j]) && k + 1 < nsize)
		name[k++] = (char)tolower((unsigned char)s[j++]);
	name[k] = '\0';
}

static int in_list(const char *t, const char *const *list)
{
	for (int i = 0; list[i]; i++)
		if (!strcmp(t, list[i])) return 1;
	return 0;
}

static const char *const para_tags[] = {"p", "h3", "h4", "h5", "h6", "table", "dl",
                                        "figure", NULL};
static const char *const line_tags[] = {"div", "tr", "section", "article", "header",
                                        "footer", "dt", "dd", "nav", "aside", "center",
                                        "form", "address", "main", "figcaption", NULL};

#define MAX_NEST 10

typedef struct {
	buf_t *out;
	links_t *links;
	char skip[16];           /* inside <style>, <script>, <head>, <title> */
	int pre, pre_start, sp;
	int nlist;
	struct { int ol, count; } list[MAX_NEST];
	int nquote;
	size_t quote[MAX_NEST];
	int head;                /* 1 or 2 inside <h1>/<h2> */
	size_t head_start;
	char *href;              /* inside <a href> */
	size_t a_start;
} html_t;

static void render_html(const char *s, size_t len, buf_t *out, links_t *links);

/* Text character c (whitespace collapsed outside <pre>). */
static void put_char(html_t *h, char c)
{
	buf_t *out = h->out;
	if (h->pre) {
		if (c == '\r') return;
		if (h->pre_start) {                    /* a newline right after <pre> */
			h->pre_start = 0;
			if (c == '\n') return;
		}
		buf_addc(out, c);
		return;
	}
	if (isspace((unsigned char)c)) { h->sp = 1; return; }
	if (h->sp && out->len && out->d[out->len - 1] != '\n' && out->d[out->len - 1] != ' ')
		buf_addc(out, ' ');
	h->sp = 0;
	buf_addc(out, c);
}

static void put_str(html_t *h, const char *s)
{
	while (*s) put_char(h, *s++);
}

/* "> " before every line from start on (">" before empty ones); empty
 * lines at the end are left out. */
static void quote_lines(buf_t *out, size_t start)
{
	while (out->len > start + 1 && out->d[out->len - 1] == '\n' && out->d[out->len - 2] == '\n')
		out->len--;
	buf_t q = {0};
	int bol = 1;
	for (size_t i = start; i < out->len; i++) {
		if (bol) buf_adds(&q, out->d[i] == '\n' ? ">" : "> ");
		buf_addc(&q, out->d[i]);
		bol = out->d[i] == '\n';
	}
	out->len = start;
	buf_add(out, q.d, q.len);
	buf_free(&q);
}

/* Find "<" + name or "</" + name (case-insensitive, whole tag name) from i. */
static size_t find_tag(const char *s, size_t len, size_t i, const char *name, int *close)
{
	size_t nl = strlen(name);
	for (; i < len; i++) {
		if (s[i] != '<') continue;
		size_t j = i + 1;
		int c = 0;
		if (j < len && s[j] == '/') { c = 1; j++; }
		if (j + nl <= len && !strncasecmp(s + j, name, nl) &&
		    (j + nl == len || !isalnum((unsigned char)s[j + nl]))) {
			if (close) *close = c;
			return i;
		}
	}
	return len;
}

/* A data table as aligned columns. s + pos is "<table"; on success the text
 * is added and *end is set past "</table>". Returns 0 for tables that are
 * better shown as text (layout tables). */
static int table_aligned(html_t *h, const char *s, size_t len, size_t pos, size_t *end)
{
	/* the matching </table>; give up on nested tables */
	int close;
	size_t t = find_tag(s, len, pos + 1, "table", &close);
	if (t >= len || !close) return 0;
	const char *e = memchr(s + t, '>', len - t);
	size_t tend = e ? (size_t)(e - s) + 1 : len;

	enum { MAXR = 200, MAXC = 10 };
	char *cell[MAXR][MAXC];
	int ncells[MAXR], th_row[MAXR], nrows = -1, ok = 1;
	memset(ncells, 0, sizeof ncells);
	size_t i = pos + 1;
	while (ok) {
		size_t lt = i;
		while (lt < t && s[lt] != '<') lt++;
		if (lt >= t) break;
		char name[8];
		tag_name(s + lt, t - lt, name, sizeof name, &close);
		const char *ge = memchr(s + lt, '>', t - lt);
		size_t after = ge ? (size_t)(ge - s) + 1 : t;
		if (!close && !strcmp(name, "tr")) {
			if (++nrows >= MAXR) { ok = 0; break; }
			th_row[nrows] = 1;
		} else if (!close && (!strcmp(name, "td") || !strcmp(name, "th"))) {
			if (nrows < 0) { nrows = 0; th_row[0] = 1; }
			if (ncells[nrows] >= MAXC) { ok = 0; break; }
			if (strcmp(name, "th")) th_row[nrows] = 0;
			/* the cell ends at the next cell, row or the table end */
			size_t ce = after;
			for (; ce < t; ce++) {
				if (s[ce] != '<') continue;
				char n2[8];
				int c2;
				tag_name(s + ce, t - ce, n2, sizeof n2, &c2);
				if (!strcmp(n2, "td") || !strcmp(n2, "th") || !strcmp(n2, "tr")) break;
			}
			buf_t tmp = {0};
			render_html(s + after, ce - after, &tmp, h->links);
			/* one line: whitespace collapsed */
			buf_t one = {0};
			int sp = 0;
			for (size_t k = 0; k < tmp.len; k++) {
				char c = tmp.d[k];
				if (isspace((unsigned char)c)) { sp = 1; continue; }
				if (sp && one.len) buf_addc(&one, ' ');
				sp = 0;
				buf_addc(&one, c);
			}
			buf_free(&tmp);
			char *str = buf_steal(&one);
			cell[nrows][ncells[nrows]++] = str ? str : xstrdup("");
			after = ce;
		}
		i = after;
	}
	nrows++;
	if (nrows > MAXR) nrows = MAXR;

	/* only real data tables: at least 2 rows and 2 columns, narrow enough */
	int ncols = 0;
	size_t width[MAXC] = {0};
	for (int r = 0; r < nrows; r++) {
		if (ncells[r] > ncols) ncols = ncells[r];
		for (int c = 0; c < ncells[r]; c++) {
			size_t w = text_width(cell[r][c], strlen(cell[r][c]));
			if (w > width[c]) width[c] = w;
			if (w > 40) ok = 0;
		}
	}
	size_t total = 0;
	for (int c = 0; c < ncols; c++) total += width[c] + 2;
	if (nrows < 2 || ncols < 2 || total > 110) ok = 0;

	if (ok) {
		buf_t *out = h->out;
		ensure_para(out);
		for (int r = 0; r < nrows; r++) {
			if (!ncells[r]) continue;
			for (int c = 0; c < ncells[r]; c++) {
				buf_adds(out, cell[r][c]);
				if (c + 1 < ncells[r])
					add_repeat(out, ' ', width[c] - text_width(cell[r][c], strlen(cell[r][c])) + 2);
			}
			buf_addc(out, '\n');
			if (r == 0 && th_row[0]) {
				for (int c = 0; c < ncols; c++) {
					add_repeat(out, '-', width[c]);
					if (c + 1 < ncols) buf_adds(out, "  ");
				}
				buf_addc(out, '\n');
			}
		}
		ensure_para(out);
		h->sp = 0;
		*end = tend;
	}
	for (int r = 0; r < nrows && r < MAXR; r++)
		for (int c = 0; c < ncells[r]; c++) free(cell[r][c]);
	return ok;
}

static void open_tag(html_t *h, const char *name, const char *t, size_t tl)
{
	buf_t *out = h->out;
	if (!strcmp(name, "br")) {
		buf_addc(out, '\n');
		h->sp = 0;
	} else if (!strcmp(name, "h1") || !strcmp(name, "h2")) {
		ensure_para(out);
		h->head = name[1] - '0';
		h->head_start = out->len;
		h->sp = 0;
	} else if (!strcmp(name, "ul") || !strcmp(name, "ol")) {
		if (h->nlist) ensure_nl(out);
		else ensure_para(out);
		if (h->nlist < MAX_NEST) {
			h->list[h->nlist].ol = name[0] == 'o';
			char *st = attr_get(t, tl, "start");
			h->list[h->nlist].count = st ? atoi(st) - 1 : 0;
			free(st);
		}
		h->nlist++;
		h->sp = 0;
	} else if (!strcmp(name, "li")) {
		ensure_nl(out);
		int d = h->nlist ? h->nlist : 1;
		add_repeat(out, ' ', 2 * (size_t)(d < MAX_NEST ? d : MAX_NEST));
		if (h->nlist && h->nlist <= MAX_NEST && h->list[h->nlist - 1].ol)
			buf_printf(out, "%d. ", ++h->list[h->nlist - 1].count);
		else
			buf_adds(out, "* ");
		h->sp = 0;
	} else if (!strcmp(name, "blockquote")) {
		ensure_para(out);
		if (h->nquote < MAX_NEST) h->quote[h->nquote] = out->len;
		h->nquote++;
		h->sp = 0;
	} else if (!strcmp(name, "pre")) {
		ensure_para(out);
		h->pre++;
		h->pre_start = 1;
	} else if (!strcmp(name, "hr")) {
		ensure_nl(out);
		buf_adds(out, "----------------\n");
		h->sp = 0;
	} else if (!strcmp(name, "a")) {
		free(h->href);
		h->href = attr_get(t, tl, "href");
		if (h->href && (h->href[0] == '#' || !*h->href || !strncasecmp(h->href, "javascript:", 11))) {
			free(h->href);
			h->href = NULL;
		}
		h->a_start = out->len;
	} else if (!strcmp(name, "img")) {
		char *alt = attr_get(t, tl, "alt");
		char *a = alt ? str_trim(alt) : NULL;
		if (a && *a) {
			put_str(h, "[image: ");
			put_str(h, a);
			put_str(h, "]");
		}
		free(alt);
	} else if (!strcmp(name, "td") || !strcmp(name, "th")) {
		if (out->len && out->d[out->len - 1] != '\n') buf_adds(out, "  ");
		h->sp = 0;
	} else if (in_list(name, para_tags)) {
		ensure_para(out);
		h->sp = 0;
	} else if (in_list(name, line_tags)) {
		ensure_nl(out);
		h->sp = 0;
	}
}

static void close_tag(html_t *h, const char *name)
{
	buf_t *out = h->out;
	if ((!strcmp(name, "h1") || !strcmp(name, "h2")) && h->head) {
		size_t w = last_line_width(out, h->head_start);
		if (w) {
			ensure_nl(out);
			add_repeat(out, h->head == 1 ? '=' : '-', w < 72 ? w : 72);
		}
		h->head = 0;
		ensure_para(out);
		h->sp = 0;
	} else if (!strcmp(name, "ul") || !strcmp(name, "ol")) {
		if (h->nlist) h->nlist--;
		if (h->nlist) ensure_nl(out);
		else ensure_para(out);
		h->sp = 0;
	} else if (!strcmp(name, "blockquote")) {
		if (h->nquote) {
			h->nquote--;
			ensure_nl(out);
			if (h->nquote < MAX_NEST) quote_lines(out, h->quote[h->nquote]);
		}
		ensure_para(out);
		h->sp = 0;
	} else if (!strcmp(name, "pre")) {
		if (h->pre) h->pre--;
		ensure_para(out);
		h->sp = 0;
	} else if (!strcmp(name, "a")) {
		if (h->href) {
			/* the link text, trimmed; no number when it is the address */
			size_t a = h->a_start, b = out->len;
			while (a < b && isspace((unsigned char)out->d[a])) a++;
			while (b > a && isspace((unsigned char)out->d[b - 1])) b--;
			const char *u = h->href;
			size_t ul = strlen(u);
			const char *bare = !strncasecmp(u, "mailto:", 7) ? u + 7 : u;
			int same = (b - a == ul && !memcmp(out->d + a, u, ul)) ||
			           (b - a == strlen(bare) && !memcmp(out->d + a, bare, b - a));
			if (!same) {
				int n = link_add(h->links, u);
				if (n) {
					if (b > h->a_start && out->len && out->d[out->len - 1] != ' ' &&
					    out->d[out->len - 1] != '\n')
						buf_addc(out, ' ');
					buf_printf(out, "[%d]", n);
				}
			}
			free(h->href);
			h->href = NULL;
		}
	} else if (in_list(name, para_tags)) {
		ensure_para(out);
		h->sp = 0;
	} else if (in_list(name, line_tags) || !strcmp(name, "li")) {
		ensure_nl(out);
		h->sp = 0;
	}
}

static void render_html(const char *s, size_t len, buf_t *out, links_t *links)
{
	html_t h = {0};
	h.out = out;
	h.links = links;
	size_t i = 0;
	while (i < len) {
		char c = s[i];
		if (c == '<' && i + 1 < len && (isalpha((unsigned char)s[i + 1]) || s[i + 1] == '/' ||
		                                s[i + 1] == '!')) {
			if (len - i >= 4 && !memcmp(s + i, "<!--", 4)) {
				const char *e = memmem(s + i + 4, len - i - 4, "-->", 3);
				i = e ? (size_t)(e - s) + 3 : len;
				continue;
			}
			char name[16];
			int close;
			tag_name(s + i, len - i, name, sizeof name, &close);
			const char *e = memchr(s + i, '>', len - i);
			size_t tend = e ? (size_t)(e - s) + 1 : len;
			if (*h.skip) {
				/* <head> without </head> ends at <body> */
				if ((close && !strcmp(name, h.skip)) ||
				    (!close && !strcmp(h.skip, "head") && !strcmp(name, "body")))
					*h.skip = '\0';
			} else if (!close && (!strcmp(name, "style") || !strcmp(name, "script") ||
			                      !strcmp(name, "head") || !strcmp(name, "title"))) {
				snprintf(h.skip, sizeof h.skip, "%s", name);
			} else if (!close && !strcmp(name, "table") && table_aligned(&h, s, len, i, &tend)) {
				/* shown as columns */
			} else if (close) {
				close_tag(&h, name);
			} else {
				open_tag(&h, name, s + i, tend - i);
			}
			i = tend;
			continue;
		}
		if (*h.skip) { i++; continue; }
		if (c == '&') {
			buf_t ent = {0};
			size_t n = entity(s + i, len - i, &ent);
			if (n) {
				for (size_t k = 0; k < ent.len; k++) put_char(&h, ent.d[k]);
				buf_free(&ent);
				i += n;
				continue;
			}
			buf_free(&ent);
		}
		put_char(&h, c);
		i++;
	}
	free(h.href);
	while (h.nquote > 0) {                    /* unclosed <blockquote> */
		h.nquote--;
		ensure_nl(out);
		if (h.nquote < MAX_NEST) quote_lines(out, h.quote[h.nquote]);
	}
}

void html_to_text(const char *s, size_t len, buf_t *out)
{
	links_t links = {0};
	size_t start = out->len;
	render_html(s, len, out, &links);
	ensure_nl(out);
	links_out(&links, out);
	tidy(out, start);
	ensure_nl(out);
}

/* ------------------------------------------------------------------ */
/* Markdown                                                            */

static void md_inline(const char *s, size_t n, buf_t *out, links_t *links, int depth);

/* "[text](url "title")" at s: lengths of text and the whole; url copied. */
static int md_link(const char *s, size_t n, size_t *tlen, size_t *total, char *url, size_t usize)
{
	if (!n || s[0] != '[') return 0;
	int d = 0;
	size_t i = 0;
	for (; i < n; i++) {
		if (s[i] == '\\') { i++; continue; }
		if (s[i] == '[') d++;
		else if (s[i] == ']' && --d == 0) break;
	}
	if (i >= n || i + 1 >= n || s[i + 1] != '(') return 0;
	*tlen = i - 1;
	size_t u = i + 2;
	while (u < n && s[u] == ' ') u++;
	size_t ue;
	if (u < n && s[u] == '<') {
		u++;
		ue = u;
		while (ue < n && s[ue] != '>') ue++;
	} else {
		ue = u;
		int pd = 0;
		while (ue < n && s[ue] != ' ' && !(s[ue] == ')' && pd == 0)) {
			if (s[ue] == '(') pd++;
			else if (s[ue] == ')') pd--;
			ue++;
		}
	}
	size_t close = ue;
	while (close < n && s[close] != ')') {
		if (s[close] == '"') {                  /* title: skip to its end */
			close++;
			while (close < n && s[close] != '"') close++;
		}
		close++;
	}
	if (close >= n) return 0;
	snprintf(url, usize, "%.*s", (int)(ue - u), s + u);
	*total = close + 1;
	return 1;
}

static void md_inline(const char *s, size_t n, buf_t *out, links_t *links, int depth)
{
	size_t i = 0;
	while (i < n) {
		char c = s[i];
		if (c == '\\' && i + 1 < n && ispunct((unsigned char)s[i + 1])) {
			buf_addc(out, s[i + 1]);
			i += 2;
			continue;
		}
		if (c == '`') {                         /* code span: as written */
			size_t r = 0;
			while (i + r < n && s[i + r] == '`') r++;
			size_t j = i + r;
			for (; j + r <= n; j++) {
				size_t k = 0;
				while (k < r && s[j + k] == '`') k++;
				if (k == r && (j + r == n || s[j + r] != '`')) break;
			}
			if (j + r <= n) {
				size_t a = i + r, b = j;
				if (b - a >= 2 && s[a] == ' ' && s[b - 1] == ' ') { a++; b--; }
				buf_add(out, s + a, b - a);
				i = j + r;
				continue;
			}
			buf_add(out, s + i, r);
			i += r;
			continue;
		}
		if ((c == '!' && i + 1 < n && s[i + 1] == '[') || c == '[') {
			int img = c == '!';
			size_t tl, total;
			char url[2048];
			if (md_link(s + i + img, n - i - img, &tl, &total, url, sizeof url)) {
				const char *text = s + i + img + 1;
				if (img) {
					buf_adds(out, "[image: ");
					md_inline(text, tl, out, links, depth + 1);
					buf_addc(out, ']');
				} else {
					size_t before = out->len;
					md_inline(text, tl, out, links, depth + 1);
					size_t ul = strlen(url);
					if (!(out->len - before == ul && !memcmp(out->d + before, url, ul)) && *url &&
					    url[0] != '#') {
						int k = link_add(links, url);
						if (k) buf_printf(out, " [%d]", k);
					}
				}
				i += img + total;
				continue;
			}
		}
		if (c == '<') {                         /* <https://...> autolink */
			const char *e = memchr(s + i, '>', n - i);
			size_t l = e ? (size_t)(e - (s + i)) - 1 : 0;
			if (e && l && !memchr(s + i + 1, ' ', l) &&
			    (!strncasecmp(s + i + 1, "http", 4) || !strncasecmp(s + i + 1, "mailto:", 7) ||
			     memchr(s + i + 1, '@', l))) {
				buf_add(out, s + i + 1, l);
				i += l + 2;
				continue;
			}
		}
		if ((c == '*' || c == '_') && depth < 8) {   /* emphasis: drop the markers */
			size_t r = 0;
			while (i + r < n && s[i + r] == c && r < 3) r++;
			int can_open = i + r < n && !isspace((unsigned char)s[i + r]) &&
			               !(c == '_' && i > 0 && isalnum((unsigned char)s[i - 1]));
			if (can_open) {
				size_t j = i + r;
				for (; j + r <= n; j++) {
					size_t k = 0;
					while (k < r && s[j + k] == c) k++;
					if (k == r && !isspace((unsigned char)s[j - 1]) &&
					    !(c == '_' && j + r < n && isalnum((unsigned char)s[j + r])))
						break;
				}
				if (j + r <= n && j > i + r) {
					md_inline(s + i + r, j - i - r, out, links, depth + 1);
					i = j + r;
					continue;
				}
			}
			buf_add(out, s + i, r);
			i += r;
			continue;
		}
		buf_addc(out, c);
		i++;
	}
}

void md_to_text(const char *s, size_t len, buf_t *out)
{
	links_t links = {0};
	size_t start = out->len, i = 0;
	char fence = 0;
	while (i < len) {
		const char *ln = s + i;
		const char *eol = memchr(ln, '\n', len - i);
		size_t ll = eol ? (size_t)(eol - ln) : len - i, n = ll;
		i += ll + (eol ? 1 : 0);
		if (n && ln[n - 1] == '\r') n--;

		size_t k = 0;
		while (k < n && k < 3 && ln[k] == ' ') k++;
		if (n - k >= 3 && (ln[k] == '`' || ln[k] == '~') && ln[k + 1] == ln[k] && ln[k + 2] == ln[k]) {
			if (!fence) { fence = ln[k]; continue; }      /* ``` opens a code block */
			if (ln[k] == fence) { fence = 0; continue; }
		}
		if (fence || (n >= 4 && !memcmp(ln, "    ", 4)) || (n && ln[0] == '\t')) {
			if (fence) buf_adds(out, "    ");
			buf_add(out, ln, n);                     /* code: as written */
			buf_addc(out, '\n');
			continue;
		}
		if (k < n && ln[k] == '#') {               /* # heading */
			size_t lvl = 0;
			while (k + lvl < n && ln[k + lvl] == '#') lvl++;
			if (lvl <= 6 && (k + lvl == n || ln[k + lvl] == ' ')) {
				size_t a = k + lvl, b = n;
				while (a < b && ln[a] == ' ') a++;
				while (b > a && (ln[b - 1] == '#' || ln[b - 1] == ' ')) b--;
				size_t hs = out->len;
				md_inline(ln + a, b - a, out, &links, 0);
				size_t w = text_width(out->d + hs, out->len - hs);
				buf_addc(out, '\n');
				if (lvl <= 2 && w) {
					add_repeat(out, lvl == 1 ? '=' : '-', w < 72 ? w : 72);
					buf_addc(out, '\n');
				}
				continue;
			}
		}
		md_inline(ln, n, out, &links, 0);
		buf_addc(out, '\n');
	}
	links_out(&links, out);
	tidy(out, start);
	ensure_nl(out);
}
