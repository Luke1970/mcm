/* editor.c - full-screen vim-like message editor (see editor.h) */
#include "editor.h"
#include "helptext.h"   /* help/editor.txt */
#include "mime.h"
#include "theme.h"
#include "ui.h"
#include "util.h"

#include <curses.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifndef _WIN32
#include <sys/wait.h>
#endif
#include <time.h>
#include <unistd.h>

#define TABW      8
/* body lines are wrapped at this column while typing (format=flowed) */
#define WRAP_COL  (E && E->cfg->wrap_width >= 20 ? E->cfg->wrap_width : 72)
#define MAX_UNDO  100

typedef struct {
	char  *s;
	size_t len;
} line_t;

/* A window of text: the header window or the body. */
typedef struct {
	line_t *l;
	int     n;
	int     cy;            /* cursor line */
	size_t  cx;            /* cursor byte offset in the line */
	int     want_col;      /* display column kept on vertical moves */
	int     top;           /* first line shown */
	int     y0, x0, rows, cols;   /* text area on screen */
	/* the header window is a form: one row per line, "Name:" fixed in a
	 * column of its own, the field after it scrolled sideways */
	int     field;
	int     hoff, hoff_line;      /* first column of the cursor line's field shown */
	/* the message with unWrap: lines not wrapped on screen, all of them
	 * scrolled sideways by hoff columns */
	int     nowrap;
} pane_t;

/* Horizontal scroll bar on the message window's bottom border (unWrap). */
typedef struct {
	int y, x, len;         /* where it is drawn (len 0: not shown) */
	int th, tsize;         /* thumb: first column and length */
	int n;                 /* columns of the text (widest line, cursor room) */
	int grab;              /* dragging the thumb: column offset, -1 not */
} hbar_t;

enum { M_HDR, M_BODY, M_CMD };   /* header window, body editing, command mode */

typedef struct {
	const editor_cfg *cfg;
	pane_t  hp, bp;        /* header and body */
	pane_t *p;             /* the active one */
	int     mode;
	int     insert;
	int     modified;
	char    msg[512];
	int     msg_err;
	char   *yank;          /* yanked lines, '\n'-separated */
	int     yank_lines;
	char   *undo[MAX_UNDO];
	int     nundo;
	int     pending;       /* first key of a two-key command: 'd', 'y', 'g', 'Z' */
	int     wrap;          /* Wrap: wrap body lines at WRAP_COL while typing */
	buf_t   hidden;        /* header lines not shown (From, Date, ...), '\n'-ended */
	int     nhidden;       /* how many */
	int     nhdr;          /* header lines in the text, shown and hidden */
	int     want_line;     /* set_text: header line (of all) to put the cursor on */
	int     want_vis;      /* ... its index in the header window, or -1 */
	hbar_t  hb;
} ed_t;

static ed_t *E;   /* for the dialog background callback */

/* ------------------------------------------------------------------ */
/* Text                                                                */

static void pane_clear(pane_t *p)
{
	for (int i = 0; i < p->n; i++) free(p->l[i].s);
	free(p->l);
	p->l = NULL;
	p->n = 0;
}

static void pane_add(pane_t *p, const char *s, size_t n)
{
	p->l = xrealloc(p->l, sizeof *p->l * (size_t)(p->n + 1));
	p->l[p->n].s = xstrndup(s, n);
	p->l[p->n].len = n;
	p->n++;
}

static void pane_fix(pane_t *p)
{
	if (p->n == 0) pane_add(p, "", 0);
	if (p->cy >= p->n) p->cy = p->n - 1;
	if (p->cy < 0) p->cy = 0;
	if (p->cx > p->l[p->cy].len) p->cx = p->l[p->cy].len;
}

static int is_blank(char c) { return c == ' ' || c == '\t'; }

/* Header window: length of "Name:" at the start of line l, 0 if it has no
 * name (a continuation line, or a new line whose name is being typed). */
static size_t hdr_name_len(const line_t *l)
{
	if (!l->len || is_blank(l->s[0])) return 0;
	for (size_t i = 0; i < l->len; i++) {
		unsigned char c = (unsigned char)l->s[i];
		if (c == ':') return i > 0 ? i + 1 : 0;
		if (c <= ' ' || c > 126) return 0;
	}
	return 0;
}

/* Where the editable field starts: after "Name:" and its blanks (the name
 * cannot be edited); after the indent of a continuation line; else 0. */
static size_t hdr_min(const line_t *l)
{
	size_t n = hdr_name_len(l);
	if (!n && !(l->len && is_blank(l->s[0]))) return 0;
	while (n < l->len && is_blank(l->s[n])) n++;
	return n;
}

/* "Name:value" becomes "Name: value", so the field starts after a space. */
static void hdr_space(line_t *l)
{
	size_t n = hdr_name_len(l);
	if (!n || (n < l->len && is_blank(l->s[n]))) return;
	l->s = xrealloc(l->s, l->len + 2);
	memmove(l->s + n + 1, l->s + n, l->len - n + 1);
	l->s[n] = ' ';
	l->len++;
}

/* The header window shows only the headers the writer fills in: To, Cc,
 * Bcc and Subject, and the Attach: lines of attached files. The others
 * (From, Date, In-Reply-To, References, ...) are kept as they are and sent
 * along; :e shows them in the external editor. An empty From: is shown
 * as well, to be filled in (an account without an address). */
static int shown_header(const char *s, size_t n, int prev_shown)
{
	static const char *const show[] = {"To", "Cc", "Bcc", "Subject", "Attach", NULL};
	if (n && (s[0] == ' ' || s[0] == '\t')) return prev_shown;   /* continuation line */
	const char *c = memchr(s, ':', n);
	if (!c) return 1;                                             /* not a header (yet) */
	size_t k = (size_t)(c - s);
	for (int i = 0; show[i]; i++)
		if (strlen(show[i]) == k && !strncasecmp(s, show[i], k)) return 1;
	if (k == 4 && !strncasecmp(s, "From", 4)) {
		const char *v = c + 1, *end = s + n;
		while (v < end && is_blank(*v)) v++;
		return v == end;
	}
	return 0;
}

/* Split the text into the header window (up to the first empty line) and
 * the body; headers that are not shown go to e->hidden. */
static void set_text(ed_t *e, const char *text, size_t len)
{
	pane_clear(&e->hp);
	pane_clear(&e->bp);
	buf_free(&e->hidden);
	e->nhidden = e->nhdr = 0;
	e->want_vis = -1;
	pane_t *to = &e->hp;
	int shown = 1;
	size_t p = 0;
	for (;;) {
		const char *nl = memchr(text + p, '\n', len - p);
		size_t n = nl ? (size_t)(nl - (text + p)) : len - p;
		size_t m = n;
		if (m && text[p + m - 1] == '\r') m--;
		if (to == &e->hp && m == 0) to = &e->bp;     /* the separating empty line */
		else if (to == &e->hp && !(shown = shown_header(text + p, m, shown))) {
			buf_add(&e->hidden, text + p, m);
			buf_addc(&e->hidden, '\n');
			e->nhidden++;
			e->nhdr++;
		} else {
			if (to == &e->hp && e->nhdr++ == e->want_line) e->want_vis = e->hp.n;
			pane_add(to, text + p, m);
		}
		if (!nl) break;
		p += n + 1;
	}
	for (int i = 0; i < e->hp.n; i++) hdr_space(&e->hp.l[i]);
	pane_fix(&e->hp);
	pane_fix(&e->bp);
}

/* The hidden header lines, the shown ones (empty ones left out), an empty
 * line, the body. */
static char *get_text(const ed_t *e, size_t *len)
{
	buf_t b = {0};
	if (e->hidden.len) buf_add(&b, e->hidden.d, e->hidden.len);
	for (int i = 0; i < e->hp.n; i++) {
		if (!e->hp.l[i].len) continue;
		buf_add(&b, e->hp.l[i].s, e->hp.l[i].len);
		buf_addc(&b, '\n');
	}
	buf_addc(&b, '\n');
	for (int i = 0; i < e->bp.n; i++) {
		buf_add(&b, e->bp.l[i].s, e->bp.l[i].len);
		buf_addc(&b, '\n');
	}
	*len = b.len;
	return buf_steal(&b);
}

static void snapshot(ed_t *e)
{
	size_t len;
	if (e->nundo == MAX_UNDO) {
		free(e->undo[0]);
		memmove(e->undo, e->undo + 1, sizeof *e->undo * (MAX_UNDO - 1));
		e->nundo--;
	}
	char *t = get_text(e, &len);
	if (len) t[len - 1] = '\0';          /* drop the final newline */
	e->undo[e->nundo++] = t;
}

static void undo(ed_t *e)
{
	if (!e->nundo) {
		snprintf(e->msg, sizeof e->msg, "Nothing to undo");
		return;
	}
	char *t = e->undo[--e->nundo];
	set_text(e, t, strlen(t));
	free(t);
	e->modified = 1;
}

static void ins_line(ed_t *e, int at, const char *s, size_t n)
{
	e->p->l = xrealloc(e->p->l, sizeof *e->p->l * (size_t)(e->p->n + 1));
	memmove(e->p->l + at + 1, e->p->l + at, sizeof *e->p->l * (size_t)(e->p->n - at));
	e->p->l[at].s = xstrndup(s, n);
	e->p->l[at].len = n;
	e->p->n++;
}

static void del_line(ed_t *e, int at)
{
	free(e->p->l[at].s);
	memmove(e->p->l + at, e->p->l + at + 1, sizeof *e->p->l * (size_t)(e->p->n - at - 1));
	e->p->n--;
	if (e->p->n == 0) ins_line(e, 0, "", 0);
	if (e->p->cy >= e->p->n) e->p->cy = e->p->n - 1;
}

static void ins_bytes(ed_t *e, const char *s, size_t n)
{
	line_t *l = &e->p->l[e->p->cy];
	l->s = xrealloc(l->s, l->len + n + 1);
	memmove(l->s + e->p->cx + n, l->s + e->p->cx, l->len - e->p->cx + 1);
	memcpy(l->s + e->p->cx, s, n);
	l->len += n;
	e->p->cx += n;
	e->modified = 1;
}

static size_t prev_char(const line_t *l, size_t x)
{
	if (x == 0) return 0;
	x--;
	while (x > 0 && ((unsigned char)l->s[x] & 0xC0) == 0x80) x--;
	return x;
}

static size_t next_char(const line_t *l, size_t x)
{
	if (x >= l->len) return l->len;
	size_t cl;
	utf8_next(l->s + x, l->len - x, &cl);
	return x + cl;
}

static void del_range(ed_t *e, size_t a, size_t b)
{
	line_t *l = &e->p->l[e->p->cy];
	memmove(l->s + a, l->s + b, l->len - b + 1);
	l->len -= b - a;
	e->modified = 1;
}

/* "-- " (signature separator) keeps its trailing space (RFC 3676 4.3). */
static int is_sig_sep(const line_t *l) { return l->len == 3 && !memcmp(l->s, "-- ", 3); }

/* Enter. In the body a line ending in a space is a soft break
 * (format=flowed), so trailing blanks before a hard break are removed. */
static void split_line(ed_t *e)
{
	line_t *l = &e->p->l[e->p->cy];
	ins_line(e, e->p->cy + 1, l->s + e->p->cx, l->len - e->p->cx);
	l = &e->p->l[e->p->cy];
	if (e->p == &e->bp) {
		size_t end = e->p->cx;
		while (end && (l->s[end - 1] == ' ' || l->s[end - 1] == '\t')) end--;
		if (!(end == 2 && e->p->cx == 3 && !memcmp(l->s, "-- ", 3))) e->p->cx = end;
	}
	l->len = e->p->cx;
	l->s[l->len] = '\0';
	e->p->cy++;
	e->p->cx = 0;
	e->modified = 1;
}

static void join_next(ed_t *e, int sep)
{
	if (e->p->cy + 1 >= e->p->n) return;
	line_t *a = &e->p->l[e->p->cy], *b = &e->p->l[e->p->cy + 1];
	size_t skip = 0;
	if (sep) while (skip < b->len && (b->s[skip] == ' ' || b->s[skip] == '\t')) skip++;
	e->p->cx = a->len;
	int space = sep && a->len && b->len > skip;
	a->s = xrealloc(a->s, a->len + space + b->len - skip + 1);
	if (space) a->s[a->len++] = ' ';
	memcpy(a->s + a->len, b->s + skip, b->len - skip + 1);
	a->len += b->len - skip;
	del_line(e, e->p->cy + 1);
	e->modified = 1;
}

/* ------------------------------------------------------------------ */
/* Columns                                                             */

static int char_w(wchar_t w, int col)
{
	if (w == '\t') return TABW - col % TABW;
	int cw = ui_wcwidth(w);
	return cw < 0 ? 1 : cw;
}

/* Display column (unwrapped) of byte offset x. */
static int col_of(const line_t *l, size_t x)
{
	int col = 0;
	for (size_t i = 0; i < x && i < l->len;) {
		size_t cl;
		wchar_t w = utf8_next(l->s + i, l->len - i, &cl);
		col += char_w(w, col);
		i += cl;
	}
	return col;
}

static size_t x_of_col(const line_t *l, int want)
{
	int col = 0;
	size_t i = 0;
	while (i < l->len) {
		size_t cl;
		wchar_t w = utf8_next(l->s + i, l->len - i, &cl);
		int cw = char_w(w, col);
		if (col + cw > want) break;
		col += cw;
		i += cl;
	}
	return i;
}

/* ------------------------------------------------------------------ */
/* Hard wrap at WRAP_COL with format=flowed soft breaks                 */

/* Break body line i after the last blank that keeps it within WRAP_COL
 * columns; the blank stays at the line end (soft break). The cursor moves
 * along with its text. Returns 1 if the line was broken. Quoted lines
 * ('>') and words longer than a line are left alone. */
static int wrap_line(ed_t *e, int i)
{
	pane_t *pn = &e->bp;
	line_t *l = &pn->l[i];
	if (!l->len || l->s[0] == '>' || col_of(l, l->len) <= WRAP_COL) return 0;
	size_t fit = x_of_col(l, WRAP_COL);              /* bytes that fit in WRAP_COL */
	size_t k = fit < l->len ? fit : l->len - 1;
	while (k > 0 && !(l->s[k] == ' ' && !is_blank(l->s[k - 1]))) k--;
	if (k == 0) return 0;
	size_t cut = k + 1;
	while (cut < l->len && is_blank(l->s[cut])) cut++;   /* keep one space at the end */
	if (cut >= l->len) return 0;                      /* only blanks follow */

	/* A soft-broken line (ends in a space) continues on the next line:
	 * the overflow goes to the front of that line, which is wrapped next. */
	line_t *nx = i + 1 < pn->n ? &pn->l[i + 1] : NULL;
	int join = l->s[l->len - 1] == ' ' && nx && nx->len && nx->s[0] != '>' && !is_blank(nx->s[0]) &&
	           !is_sig_sep(nx);
	size_t rl = l->len - cut;
	if (join) {
		nx->s = xrealloc(nx->s, nx->len + rl + 1);
		memmove(nx->s + rl, nx->s, nx->len + 1);
		memcpy(nx->s, l->s + cut, rl);
		nx->len += rl;
		if (pn->cy == i + 1) pn->cx += rl;
	} else {
		pane_t *save = e->p;
		e->p = pn;
		ins_line(e, i + 1, l->s + cut, rl);
		e->p = save;
		if (pn->cy > i) pn->cy++;
	}
	l = &pn->l[i];
	l->len = k + 1;
	l->s[l->len] = '\0';
	if (pn->cy == i && pn->cx >= cut) {
		pn->cy = i + 1;
		pn->cx -= cut;
	} else if (pn->cy == i && pn->cx > k + 1) {
		pn->cx = k + 1;
	}
	e->modified = 1;
	return 1;
}

/* After typing in the body: wrap the cursor line (and what was pushed on). */
static void hard_wrap(ed_t *e)
{
	if (e->p != &e->bp || !e->wrap) return;
	for (int i = e->p->cy, n = 0; n < 1000 && wrap_line(e, i); i++, n++) {}
}

/* gq: join the paragraph around the cursor (lines up to an empty line,
 * a quoted line or the signature) and wrap it again at WRAP_COL. */
static int reflow(ed_t *e)
{
	if (e->p != &e->bp) return -1;
	pane_t *pn = e->p;
	#define IN_PARA(j) (pn->l[j].len && pn->l[j].s[0] != '>' && !is_sig_sep(&pn->l[j]))
	if (!IN_PARA(pn->cy)) return -1;
	int a = pn->cy, b = pn->cy;
	while (a > 0 && IN_PARA(a - 1)) a--;
	while (b + 1 < pn->n && IN_PARA(b + 1)) b++;
	#undef IN_PARA
	buf_t t = {0};
	for (int j = a; j <= b; j++) {
		const line_t *l = &pn->l[j];
		size_t s0 = 0, e0 = l->len;
		if (j > a) while (s0 < e0 && is_blank(l->s[s0])) s0++;   /* first line keeps its indent */
		while (e0 > s0 && is_blank(l->s[e0 - 1])) e0--;
		if (e0 == s0) continue;
		if (t.len) buf_addc(&t, ' ');
		buf_add(&t, l->s + s0, e0 - s0);
	}
	snapshot(e);
	for (int j = b; j > a; j--) del_line(e, j);
	line_t *l = &pn->l[a];
	free(l->s);
	l->len = t.len;
	l->s = buf_steal(&t);
	if (!l->s) l->s = xstrdup("");
	pn->cy = a;
	pn->cx = 0;
	int i = a;
	while (wrap_line(e, i)) i++;
	pn->cy = i;                                       /* end of the paragraph */
	pn->cx = 0;
	e->modified = 1;
	return 0;
}

/* Word wrap: does the word starting at byte i (screen column c) have to
 * go to the next row? Words longer than a row are broken at the edge. */
static int word_break(const pane_t *pn, const line_t *l, size_t i, int c)
{
	if (c == 0 || is_blank(l->s[i]) || (i > 0 && !is_blank(l->s[i - 1]))) return 0;
	int ww = 0;
	for (size_t j = i; j < l->len && !is_blank(l->s[j]) && ww <= pn->cols;) {
		size_t cl;
		wchar_t w = utf8_next(l->s + j, l->len - j, &cl);
		ww += char_w(w, ww);
		j += cl;
	}
	return c + ww > pn->cols && ww <= pn->cols;
}

/* Wrapped position (row, column) of byte offset x when lines wrap at word
 * boundaries within the pane's width. Tabs expand relative to the screen row. */
static void pos_of(const pane_t *pn, const line_t *l, size_t x, int *row, int *col)
{
	if (pn->field || pn->nowrap) {         /* header window, unWrap: one row per line */
		*row = 0;
		*col = col_of(l, x);
		return;
	}
	int r = 0, c = 0;
	for (size_t i = 0; i < x && i < l->len;) {
		size_t cl;
		wchar_t w = utf8_next(l->s + i, l->len - i, &cl);
		if (word_break(pn, l, i, c)) {
			r++;
			c = 0;
		}
		int cw = char_w(w, c);
		if (c + cw > pn->cols && c > 0) {
			r++;
			c = 0;
			cw = char_w(w, 0);
		}
		c += cw;
		i += cl;
	}
	if (c >= pn->cols) {
		r++;
		c = 0;
	}
	*row = r;
	*col = c;
}

/* Byte offset at wrapped position (row, col), or the line end. */
static size_t x_at(const pane_t *pn, const line_t *l, int row, int col)
{
	size_t i = 0;
	while (i < l->len) {
		int r, c;
		pos_of(pn, l, next_char(l, i), &r, &c);
		if (r > row || (r == row && c > col)) break;
		i = next_char(l, i);
	}
	return i;
}

/* Screen rows a line takes when wrapped. */
static int line_rows(const pane_t *pn, const line_t *l)
{
	if (pn->field || pn->nowrap) return 1;
	int r, c;
	pos_of(pn, l, l->len, &r, &c);
	return r + 1;
}

/* ------------------------------------------------------------------ */
/* Drawing                                                             */

/* Header window: width of the name column ("Subject: "), at most half. */
static int name_w(const pane_t *pn)
{
	int w = 0;
	for (int i = 0; i < pn->n; i++) {
		size_t n = hdr_name_len(&pn->l[i]);
		int cw = n ? col_of(&pn->l[i], n) + 1 : 0;
		if (cw > w) w = cw;
	}
	return w > pn->cols / 2 ? pn->cols / 2 : w;
}

/* Header line i: the column its field starts at (a line whose name is
 * still being typed starts at the left), and the field as a line. */
static int field_of(const pane_t *pn, int i, int nw, line_t *v)
{
	const line_t *l = &pn->l[i];
	size_t vs = hdr_min(l);
	v->s = l->s + vs;
	v->len = l->len - vs;
	return hdr_name_len(l) || (l->len && is_blank(l->s[0])) ? nw : 0;
}

/* Keep the cursor in its field's view: scroll that field sideways. */
static void field_scroll(pane_t *pn)
{
	if (pn->hoff_line != pn->cy) {
		pn->hoff = 0;
		pn->hoff_line = pn->cy;
	}
	line_t v;
	int vx = field_of(pn, pn->cy, name_w(pn), &v);
	int avail = pn->cols - vx > 1 ? pn->cols - vx : 1;
	int cc = col_of(&v, pn->cx - (size_t)(v.s - pn->l[pn->cy].s));
	if (cc < pn->hoff) pn->hoff = cc;
	if (cc >= pn->hoff + avail) pn->hoff = cc - avail + 1;
}

/* Draw header line i at row y: the name bold, the field after it (the
 * cursor line from its scroll position); "…" where a field goes on. */
static void draw_field(const pane_t *pn, int y, int i, int nw, int *cur_y, int *cur_x)
{
	const line_t *l = &pn->l[i];
	size_t nl = hdr_name_len(l);
	if (nl) {
		char *name = xstrndup(l->s, nl);
		ui_puts(y, pn->x0, name, nw > 1 ? nw - 1 : 1, A_BOLD, CP_NORMAL);
		free(name);
	}
	line_t v;
	int vx = field_of(pn, i, nw, &v);
	int avail = pn->cols - vx > 1 ? pn->cols - vx : 1;
	int hoff = i == pn->cy ? pn->hoff : 0, x = pn->x0 + vx;
	int c = 0;
	for (size_t p = 0; p < v.len;) {
		size_t cl;
		wchar_t w = utf8_next(v.s + p, v.len - p, &cl);
		int cw = char_w(w, c);
		if (c >= hoff) {
			if (c - hoff + cw > avail) {                 /* more than fits */
				ui_puts(y, x + avail - 1, "…", 1, A_BOLD, CP_NORMAL);
				break;
			}
			char tmp[8];
			memcpy(tmp, v.s + p, cl);
			tmp[cl] = '\0';
			if (w == '\t' || w < 0x20 || (w >= 0x7F && w < 0xA0)) strcpy(tmp, w == '\t' ? " " : "?");
			ui_puts(y, x + c - hoff, tmp, cw, 0, CP_NORMAL);
		} else if (c + cw > hoff) {                      /* half of a wide character */
			ui_puts(y, x, "", c + cw - hoff, 0, CP_NORMAL);
		}
		c += cw;
		p += cl;
	}
	if (cur_y && i == pn->cy) {
		*cur_y = y;
		*cur_x = x + col_of(&v, pn->cx - (size_t)(v.s - l->s)) - hoff;
	}
}

/* unWrap: line i of the message at row y, from column pn->hoff. */
static void draw_hline(const pane_t *pn, int y, int i, int *cur_y, int *cur_x)
{
	const line_t *l = &pn->l[i];
	int c = 0, x = pn->x0;
	for (size_t p = 0; p < l->len;) {
		size_t cl;
		wchar_t w = utf8_next(l->s + p, l->len - p, &cl);
		int cw = char_w(w, c);
		if (c >= pn->hoff) {
			if (c - pn->hoff + cw > pn->cols) break;
			char tmp[8];
			memcpy(tmp, l->s + p, cl);
			tmp[cl] = '\0';
			if (w == '\t') tmp[0] = '\0';                 /* blanks (ui_puts pads) */
			else if (w < 0x20 || (w >= 0x7F && w < 0xA0)) strcpy(tmp, "?");
			ui_puts(y, x + c - pn->hoff, tmp, cw, 0, CP_NORMAL);
		} else if (c + cw > pn->hoff) {                    /* half of a wide character */
			ui_puts(y, x, "", c + cw - pn->hoff, 0, CP_NORMAL);
		}
		c += cw;
		p += cl;
	}
	if (cur_y && i == pn->cy) {
		*cur_y = y;
		*cur_x = x + col_of(l, pn->cx) - pn->hoff;
	}
}

/* unWrap: keep the cursor a few columns away from the window's left and
 * right edge by moving the text sideways. */
static int hmargin(const pane_t *pn) { return pn->cols >= 40 ? 8 : pn->cols / 5; }

static void body_scroll(pane_t *pn)
{
	if (!pn->nowrap) { pn->hoff = 0; return; }
	int cc = col_of(&pn->l[pn->cy], pn->cx), m = hmargin(pn);
	if (cc < pn->hoff + m) pn->hoff = cc - m;
	if (cc > pn->hoff + pn->cols - 1 - m) pn->hoff = cc - (pn->cols - 1 - m);
	if (pn->hoff < 0) pn->hoff = 0;
}

/* After the scroll bar moved the text: the cursor into the visible part. */
static void cursor_into_view(pane_t *pn)
{
	int cc = col_of(&pn->l[pn->cy], pn->cx), m = hmargin(pn);
	int lo = pn->hoff ? pn->hoff + m : 0, hi = pn->hoff + pn->cols - 1 - m;
	if (cc < lo) cc = lo;
	if (cc > hi) cc = hi;
	pn->cx = x_of_col(&pn->l[pn->cy], cc);
}

/* The scroll bar under the message (unWrap), when a line does not fit. */
static void draw_hbar(ed_t *e, int y)
{
	pane_t *pn = &e->bp;
	hbar_t *b = &e->hb;
	b->len = 0;
	if (!pn->nowrap) return;
	int n = 0;
	for (int i = 0; i < pn->n; i++) {
		int w = col_of(&pn->l[i], pn->l[i].len) + 1;   /* + room for the cursor */
		if (w > n) n = w;
	}
	if (n < pn->hoff + pn->cols) n = pn->hoff + pn->cols;
	if (n <= pn->cols || pn->cols < 8) return;
	b->n = n;
	b->y = y;
	b->x = pn->x0 + 1;
	b->len = pn->cols - 2;
	int tlen = b->len - 2, range = n - pn->cols;
	b->tsize = tlen * pn->cols / n;
	if (b->tsize < 1) b->tsize = 1;
	b->th = b->x + 1 + (range > 0 ? (pn->hoff * (tlen - b->tsize) + range / 2) / range : 0);
	ui_puts(y, b->x, "◄", 1, 0, CP_NORMAL);
	ui_puts(y, b->x + b->len - 1, "►", 1, 0, CP_NORMAL);
	for (int k = 0; k < tlen; k++) {
		int c = b->x + 1 + k, on = c >= b->th && c < b->th + b->tsize;
		ui_puts(y, c, on ? "█" : "░", 1, 0, CP_NORMAL);
	}
}

/* Thumb dragged so that its left end is at screen column c. */
static void hbar_drag_to(ed_t *e, int c)
{
	hbar_t *b = &e->hb;
	pane_t *pn = &e->bp;
	int tlen = b->len - 2, fr = tlen - b->tsize, p = c - (b->x + 1);
	if (p < 0) p = 0;
	if (p > fr) p = fr;
	pn->hoff = fr > 0 ? (p * (b->n - pn->cols) + fr / 2) / fr : 0;
	cursor_into_view(pn);
}

/* Draw line i of a pane at screen row y; header names are bold. */
static void draw_line(const ed_t *e, const pane_t *pn, int y, int i, int *cur_y, int *cur_x)
{
	const line_t *l = &pn->l[i];
	const char *cp = pn == &e->hp ? strchr(l->s, ':') : NULL;
	size_t colon = cp ? (size_t)(cp - l->s) + 1 : 0;
	int ymax = pn->y0 + pn->rows;
	int r = 0, c = 0;
	size_t p = 0;
	while (p < l->len) {
		size_t cl;
		wchar_t w = utf8_next(l->s + p, l->len - p, &cl);
		if (word_break(pn, l, p, c)) {
			r++;
			c = 0;
		}
		int cw = char_w(w, c);
		if (c + cw > pn->cols && c > 0) {
			r++;
			c = 0;
			cw = char_w(w, 0);
		}
		if (y + r >= ymax) break;
		attr_t a = p < colon ? A_BOLD : 0;
		if (w == '\t') {
			for (int k = 0; k < cw && c + k < pn->cols; k++)
				ui_puts(y + r, pn->x0 + c + k, " ", 1, a, CP_NORMAL);
		} else {
			char tmp[8];
			memcpy(tmp, l->s + p, cl);
			tmp[cl] = '\0';
			if (w < 0x20 || (w >= 0x7F && w < 0xA0)) strcpy(tmp, "?");
			ui_puts(y + r, pn->x0 + c, tmp, cw, a, CP_NORMAL);
		}
		c += cw;
		p += cl;
	}
	if (cur_y && i == pn->cy) {
		pos_of(pn, l, pn->cx, &r, &c);
		*cur_y = y + r;
		*cur_x = pn->x0 + c;
	}
}

static void ensure_visible(pane_t *pn)
{
	if (pn->cy < pn->top) pn->top = pn->cy;
	for (;;) {
		int used = 0;
		for (int i = pn->top; i < pn->cy; i++) used += line_rows(pn, &pn->l[i]);
		int cr, cc;
		pos_of(pn, &pn->l[pn->cy], pn->cx, &cr, &cc);
		if (used + cr < pn->rows || pn->top == pn->cy) break;
		pn->top++;
	}
}

static void draw_pane(const ed_t *e, const pane_t *pn, int *cur_y, int *cur_x)
{
	int y = pn->y0;
	if (pn->nowrap) {
		for (int i = pn->top; i < pn->n && y < pn->y0 + pn->rows; i++, y++)
			draw_hline(pn, y, i, cur_y, cur_x);
		return;
	}
	if (pn->field) {
		int nw = name_w(pn);
		for (int i = pn->top; i < pn->n && y < pn->y0 + pn->rows; i++, y++)
			draw_field(pn, y, i, nw, cur_y, cur_x);
		return;
	}
	for (int i = pn->top; i < pn->n && y < pn->y0 + pn->rows; i++) {
		draw_line(e, pn, y, i, cur_y, cur_x);
		y += line_rows(pn, &pn->l[i]);
	}
}

static ui_menuitem CMD_MENU[] = {
	{"EdIt", UI_HOT_CAPS, 'i'}, {"Send", 0, 'S'}, {"Keep", 0, 'K'}, {"Header", 0, 'H'},
	{"Wrap", 0, 'W'}, {"aTtach", 1, 'T'}, {"Open", 0, 'O'},
};
#define MENU_WRAP 4
#define NCMD ((int)(sizeof CMD_MENU / sizeof *CMD_MENU))

/* While editing the message: the same commands for the mouse (typed letters
 * are text), and Esc to leave editing. */
static ui_menuitem EDIT_MENU[] = {
	{"Esc=leaveEdit", UI_HOT_KEYNAME, 27},
	{"EdIt", UI_HOT_CAPS, 'i'}, {"Send", 0, 'S'}, {"Keep", 0, 'K'}, {"Header", 0, 'H'},
	{"Wrap", 0, 'W'}, {"aTtach", 1, 'T'}, {"Open", 0, 'O'},
};
#define NEDIT ((int)(sizeof EDIT_MENU / sizeof *EDIT_MENU))

static void draw_menu(ed_t *e)
{
	/* W names what it does: "unWrap" while lines are wrapped, "Wrap" while
	 * they are not */
	for (int k = 0; k < 2; k++) {
		ui_menuitem *it = k ? &EDIT_MENU[MENU_WRAP + 1] : &CMD_MENU[MENU_WRAP];
		it->label = e->wrap ? "unWrap" : "Wrap";
		it->hot = e->wrap ? 2 : 0;
	}
	if (e->mode == M_CMD) ui_menubar(CMD_MENU, NCMD, 0);
	else ui_menubar(EDIT_MENU, NEDIT, 0);   /* header or message */
}

/* Screen layout: menu, header window, body window, status line. The header
 * window is as tall as its text (more while it is being edited). */
static void layout(ed_t *e)
{
	int cols = COLS - 2 < 10 ? 10 : COLS - 2;
	e->hp.cols = e->bp.cols = cols;
	e->hp.x0 = e->bp.x0 = 1;
	int need = 0;
	for (int i = 0; i < e->hp.n; i++) need += line_rows(&e->hp, &e->hp.l[i]);
	int avail = LINES - 1 - 1 - 4;              /* menu, status, two box frames */
	int max = e->mode == M_HDR ? avail - 3 : avail / 3;
	if (e->mode == M_HDR) need++;              /* room to add a header */
	if (need > max) need = max;
	if (need < 1) need = 1;
	e->hp.y0 = 2;
	e->hp.rows = need;
	e->bp.y0 = e->hp.y0 + need + 2;
	e->bp.rows = LINES - 2 - e->bp.y0;
	if (e->bp.rows < 1) e->bp.rows = 1;
}

static void draw(void)
{
	ed_t *e = E;
	bkgdset(' ' | COLOR_PAIR(CP_NORMAL));
	erase();
	layout(e);
	/* the cursor never stands in a header name */
	size_t hmin = hdr_min(&e->hp.l[e->hp.cy]);
	if (e->hp.cx < hmin) e->hp.cx = hmin;
	ensure_visible(&e->hp);
	field_scroll(&e->hp);
	e->bp.nowrap = !e->wrap;
	ensure_visible(&e->bp);
	body_scroll(&e->bp);
	draw_menu(e);

	int hdr = e->mode == M_HDR;
	ui_box(1, 0, e->hp.rows + 2, COLS, CP_NORMAL,
	       hdr ? " Header - Tab: write the message " : " Header ", A_BOLD,
	       hdr ? CP_TITLE : CP_NORMAL);
	char title[128];
	snprintf(title, sizeof title, " %s%s ", e->cfg->title, e->modified ? " *" : "");
	ui_box(e->bp.y0 - 1, 0, e->bp.rows + 2, COLS, CP_NORMAL, title, A_BOLD,
	       hdr ? CP_NORMAL : CP_TITLE);

	int cur_y = e->p->y0, cur_x = e->p->x0;
	draw_pane(e, &e->hp, hdr ? &cur_y : NULL, &cur_x);
	draw_pane(e, &e->bp, hdr ? NULL : &cur_y, &cur_x);
	draw_hbar(e, e->bp.y0 + e->bp.rows);

	/* status line */
	ui_fill(LINES - 1, 0, 1, COLS, CP_MENU);
	if (*e->msg) {
		ui_puts(LINES - 1, 1, e->msg, COLS - 2, e->msg_err ? A_BOLD : 0,
		        e->msg_err ? CP_HIGHLIGHT : CP_MENU);
	} else if (e->mode == M_CMD) {
		ui_puts(LINES - 1, 1, HELP_EDITOR[0] ? HELP_EDITOR[0] : "", COLS - 2, 0, CP_MENU);
	} else {
		const char *help = e->mode == M_HDR
			? "-- HEADER --   Esc command mode   Tab or Enter on Subject: write the message   arrows move   Enter new header line"
			: "-- EDIT --   Esc command mode   arrows move (↑ at the top: header)   Enter new line";
		char pos[64];
		snprintf(pos, sizeof pos, "Ln %d/%d  Col %d", e->p->cy + 1, e->p->n,
		         col_of(&e->p->l[e->p->cy], e->p->cx) + 1);
		int pw = (int)strlen(pos);
		ui_puts(LINES - 1, 1, help, COLS - pw - 4, 0, CP_MENU);
		if (COLS > pw + 2) ui_puts(LINES - 1, COLS - pw - 1, pos, pw, 0, CP_MENU);
	}
	if (cur_y >= e->p->y0 + e->p->rows) cur_y = e->p->y0 + e->p->rows - 1;
	move(cur_y, cur_x);
}

void editor_redraw(void)
{
	if (E) draw();
}

/* ------------------------------------------------------------------ */
/* Actions                                                             */

static void set_mode(ed_t *e, int mode)
{
	e->mode = mode;
	e->insert = mode != M_CMD;
	e->p = mode == M_HDR ? &e->hp : &e->bp;
}

static int run_action(ed_t *e, editor_fn fn)
{
	size_t len;
	char *t = get_text(e, &len);
	int line = -1;
	char msg[512] = "";
	curs_set(0);
	int done = fn(e->cfg->ctx, t, len, &line, msg, sizeof msg);
	curs_set(1);
	free(t);
	snprintf(e->msg, sizeof e->msg, "%s", msg);
	e->msg_err = !done && *msg;
	if (line >= 0) {
		/* the text had the hidden header lines, the non-empty shown ones, an
		 * empty line, the body; an error in a hidden header is only reported */
		if (line < e->nhidden) return done;
		int k = e->nhidden;
		for (int i = 0; i < e->hp.n; i++) {
			if (!e->hp.l[i].len) continue;
			if (k++ == line) {
				set_mode(e, M_HDR);
				e->hp.cy = i;
				e->hp.cx = e->hp.l[i].len;
				return done;
			}
		}
		line -= k + 1;
		if (line >= 0 && line < e->bp.n) {
			e->bp.cy = line;
			e->bp.cx = 0;
		}
	}
	return done;
}

static void yank_lines(ed_t *e, int from, int count, int del)
{
	if (from + count > e->p->n) count = e->p->n - from;
	buf_t b = {0};
	for (int i = 0; i < count; i++) {
		buf_add(&b, e->p->l[from + i].s, e->p->l[from + i].len);
		buf_addc(&b, '\n');
	}
	free(e->yank);
	e->yank = buf_steal(&b);
	e->yank_lines = count;
	if (del) {
		snapshot(e);
		for (int i = 0; i < count; i++) del_line(e, from);
		e->p->cx = 0;
		e->modified = 1;
		snprintf(e->msg, sizeof e->msg, "%d line%s deleted", count, count == 1 ? "" : "s");
	} else {
		snprintf(e->msg, sizeof e->msg, "%d line%s yanked (p puts below, P above)", count,
		         count == 1 ? "" : "s");
	}
}

static void put_lines(ed_t *e, int below)
{
	if (!e->yank) {
		snprintf(e->msg, sizeof e->msg, "Nothing yanked yet (Y yanks a line)");
		return;
	}
	snapshot(e);
	int at = below ? e->p->cy + 1 : e->p->cy;
	const char *p = e->yank;
	int k = 0;
	while (*p) {
		const char *nl = strchr(p, '\n');
		size_t n = nl ? (size_t)(nl - p) : strlen(p);
		ins_line(e, at + k++, p, n);
		p += n + (nl ? 1 : 0);
	}
	e->p->cy = at;
	e->p->cx = 0;
	e->modified = 1;
}

/* Write the text to a temporary file, run $VISUAL/$EDITOR on it, read it back. */
static void external_editor(ed_t *e)
{
	const char *ed = getenv("VISUAL");
	if (!ed || !*ed) ed = getenv("EDITOR");
#ifdef _WIN32
	if (!ed || !*ed) ed = "notepad";
	const char *tmp = getenv("TEMP");
	char path[512];
	int fd = -1;
	for (int i = 0; i < 100 && fd < 0; i++) {
		snprintf(path, sizeof path, "%s/mcm-%d-%d.txt", tmp && *tmp ? tmp : ".", (int)getpid(), i);
		fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_BINARY, 0600);
	}
#else
	if (!ed || !*ed) ed = "vi";
	const char *tmp = getenv("TMPDIR");
	char path[512];
	snprintf(path, sizeof path, "%s/mcm-XXXXXX.eml", tmp && *tmp ? tmp : "/tmp");
	int fd = mkstemps(path, 4);
#endif
	if (fd < 0) {
		snprintf(e->msg, sizeof e->msg, "Cannot create a temporary file: %s", strerror(errno));
		e->msg_err = 1;
		return;
	}
	size_t len;
	char *t = get_text(e, &len);
	ssize_t w = write(fd, t, len);
	free(t);
	close(fd);
	if (w != (ssize_t)len) {
		unlink(path);
		snprintf(e->msg, sizeof e->msg, "Cannot write the temporary file");
		e->msg_err = 1;
		return;
	}
	struct stat before;
	stat(path, &before);

	int rc = ui_run_editor(path, 0);

	FILE *f = fopen(path, "r");
	buf_t b = {0};
	if (f) {
		char chunk[8192];
		size_t n;
		while ((n = fread(chunk, 1, sizeof chunk, f)) > 0) buf_add(&b, chunk, n);
		fclose(f);
	}
	unlink(path);
	if (rc != 0 || !f) {
		snprintf(e->msg, sizeof e->msg, "%s failed; text unchanged", ed);
		e->msg_err = 1;
		buf_free(&b);
		return;
	}
	{   /* Windows editors may write CR LF */
		size_t k = 0;
		for (size_t i = 0; i < b.len; i++)
			if (!(b.d[i] == '\r' && i + 1 < b.len && b.d[i + 1] == '\n')) b.d[k++] = b.d[i];
		b.len = k;
	}
	while (b.len && b.d[b.len - 1] == '\n') b.len--;
	snapshot(e);
	set_text(e, b.d ? b.d : "", b.len);
	buf_free(&b);
	e->modified = 1;
	snprintf(e->msg, sizeof e->msg, "Text from %s loaded", ed);
}

/* Q / :q: returns 1 to leave the editor */
static int quit(ed_t *e, int force)
{
	if (!e->modified || force) return 1;
	const char *lines[] = {"The message has not been sent.", ""};
	static const ui_menuitem btn[] = {
		{"Continue editing", 0, 'c'}, {"Keep as draft", 0, 'k'}, {"Discard", 0, 'd'},
	};
	int c = ui_choice_dialog("Leave editor", lines, 1, btn, 3, 0, 0, draw);
	if (c == 2) return 1;
	if (c == 1) return run_action(e, e->cfg->keep);
	return 0;
}

/* ':' command line */
static int colon_cmd(ed_t *e)
{
	char cmd[64] = "";
	size_t n = 0;
	for (;;) {
		ui_fill(LINES - 1, 0, 1, COLS, CP_MENU);
		ui_puts(LINES - 1, 0, ":", 1, 0, CP_MENU);
		ui_puts(LINES - 1, 1, cmd, COLS - 2, 0, CP_MENU);
		move(LINES - 1, 1 + (int)n);
		refresh();
		int ch = ui_getch();
		if (ch == ERR || ch == KEY_RESIZE) continue;
		if (ch == 27) return 0;
		if (ch == '\n' || ch == '\r' || ch == KEY_ENTER) break;
		if ((ch == KEY_BACKSPACE || ch == 127 || ch == 8)) {
			if (!n) return 0;
			cmd[--n] = '\0';
			continue;
		}
		if (ch >= 32 && ch < 127 && n < sizeof cmd - 1) {
			cmd[n++] = (char)ch;
			cmd[n] = '\0';
		}
	}
	if (!strcmp(cmd, "q")) return quit(e, 0);
	if (!strcmp(cmd, "q!")) return 1;
	if (!strcmp(cmd, "w") || !strcmp(cmd, "keep")) return run_action(e, e->cfg->keep);
	if (!strcmp(cmd, "wq") || !strcmp(cmd, "x") || !strcmp(cmd, "send")) return run_action(e, e->cfg->send);
	if (!strcmp(cmd, "e") || !strcmp(cmd, "edit")) {
		external_editor(e);
		return 0;
	}
	char *end;
	long ln = strtol(cmd, &end, 10);
	if (*cmd && !*end) {
		e->p->cy = ln < 1 ? 0 : ln > e->p->n ? e->p->n - 1 : (int)ln - 1;
		e->p->cx = 0;
		return 0;
	}
	snprintf(e->msg, sizeof e->msg, "Unknown command :%s  (:q :q! :w keep :x send :e $EDITOR :N line)", cmd);
	e->msg_err = 1;
	return 0;
}

static int is_word(char c) { return c && c != ' ' && c != '\t'; }

static void word_fwd(ed_t *e)
{
	line_t *l = &e->p->l[e->p->cy];
	size_t x = e->p->cx;
	while (x < l->len && is_word(l->s[x])) x++;
	while (x < l->len && !is_word(l->s[x])) x++;
	if (x >= l->len && e->p->cy + 1 < e->p->n) {
		e->p->cy++;
		e->p->cx = 0;
		l = &e->p->l[e->p->cy];
		while (e->p->cx < l->len && !is_word(l->s[e->p->cx])) e->p->cx++;
	} else {
		e->p->cx = x;
	}
}

static void word_back(ed_t *e)
{
	line_t *l = &e->p->l[e->p->cy];
	size_t x = e->p->cx;
	if (x == 0) {
		if (e->p->cy > 0) {
			e->p->cy--;
			e->p->cx = e->p->l[e->p->cy].len;
		}
		return;
	}
	while (x > 0 && !is_word(l->s[x - 1])) x--;
	while (x > 0 && is_word(l->s[x - 1])) x--;
	e->p->cx = x;
}

/* Go to the other window with Up at the top of the message or Down at the end
 * of the header: the cursor keeps its column, and that window is edited. */
static void to_pane(ed_t *e, int hdr)
{
	int col = e->p->want_col;
	if (e->mode == M_CMD) snapshot(e);
	set_mode(e, hdr ? M_HDR : M_BODY);
	e->msg[0] = '\0';
	pane_t *p = e->p;
	p->cy = hdr ? p->n - 1 : 0;
	p->cx = x_of_col(&p->l[p->cy], col);
	p->want_col = col;
}

/* Movement shared by both modes. Returns 1 if the key was a movement. */
static int movement(ed_t *e, int ch)
{
	line_t *l = &e->p->l[e->p->cy];
	int vert = 0;
	if (ch && (ch == ui_key_cright || ch == ui_key_cleft)) {   /* Ctrl-arrows: word by word */
		if (ch == ui_key_cright) word_fwd(e);
		else word_back(e);
		e->p->want_col = col_of(&e->p->l[e->p->cy], e->p->cx);
		return 1;
	}
	switch (ch) {
	case KEY_LEFT:  e->p->cx = prev_char(l, e->p->cx); break;
	case KEY_RIGHT: e->p->cx = next_char(l, e->p->cx); break;
	case KEY_UP:
		if (e->p->cy > 0) e->p->cy--;
		else if (e->p == &e->bp) { to_pane(e, 1); return 1; }
		vert = 1;
		break;
	case KEY_DOWN:
		if (e->p->cy + 1 < e->p->n) e->p->cy++;
		else if (e->p == &e->hp) { to_pane(e, 0); return 1; }
		vert = 1;
		break;
	case KEY_HOME:  e->p->cx = 0; break;
	case KEY_END:   e->p->cx = l->len; break;
	case KEY_PPAGE: e->p->cy -= e->p->rows - 1; if (e->p->cy < 0) e->p->cy = 0; vert = 1; break;
	case KEY_NPAGE: e->p->cy += e->p->rows - 1; if (e->p->cy >= e->p->n) e->p->cy = e->p->n - 1; vert = 1; break;
	default: return 0;
	}
	if (vert) {
		e->p->cx = x_of_col(&e->p->l[e->p->cy], e->p->want_col);
	} else {
		e->p->want_col = col_of(&e->p->l[e->p->cy], e->p->cx);
	}
	return 1;
}

/* Click on the menu (command mode, or editing the message) gives its key in
 * *ch. A click in a window (its text, empty part or frame) makes it the
 * window being edited, with the cursor on the clicked line (below the text:
 * the last line). */
static void mouse(ed_t *e, int *ch)
{
	MEVENT ev;
	*ch = 0;
	if (getmouse(&ev) != OK) return;
	hbar_t *b = &e->hb;
	if (b->grab >= 0) {                     /* dragging the scroll bar's thumb */
		hbar_drag_to(e, ev.x - b->grab);
		if (ev.bstate & BUTTON1_RELEASED) {
			b->grab = -1;
			ui_mouse_drag(0);
		}
		return;
	}
	if (!(ev.bstate & BUTTON1_PRESSED)) return;
	if (b->len && ev.y == b->y && ev.x >= b->x && ev.x < b->x + b->len) {
		pane_t *pn = &e->bp;
		int page = pn->cols - 1;
		if (ev.x == b->x) pn->hoff -= 8;
		else if (ev.x == b->x + b->len - 1) pn->hoff += 8;
		else if (ev.x < b->th) pn->hoff -= page;
		else if (ev.x >= b->th + b->tsize) pn->hoff += page;
		else {
			b->grab = ev.x - b->th;
			ui_mouse_drag(1);
			return;
		}
		if (pn->hoff > b->n - pn->cols) pn->hoff = b->n - pn->cols;
		if (pn->hoff < 0) pn->hoff = 0;
		cursor_into_view(pn);
		return;
	}
	if (ev.y == 0) {
		if (e->mode == M_CMD) *ch = ui_menu_hit(CMD_MENU, NCMD, ev.x);
		else *ch = ui_menu_hit(EDIT_MENU, NEDIT, ev.x);
		return;
	}
	pane_t *panes[2] = {&e->hp, &e->bp};
	for (int k = 0; k < 2; k++) {
		pane_t *pn = panes[k];
		if (ev.y < pn->y0 - 1 || ev.y > pn->y0 + pn->rows) continue;   /* outside its box */
		int want = pn == &e->hp ? M_HDR : M_BODY;
		if (e->mode != want) {
			if (e->mode == M_CMD) snapshot(e);
			set_mode(e, want);
			e->msg[0] = '\0';
		}
		if (pn->field) {                   /* header: row = line, the field from its column */
			int i = pn->top + ev.y - pn->y0;
			if (ev.y < pn->y0 || i >= pn->n) i = pn->n - 1;
			line_t v;
			int vx = field_of(pn, i, name_w(pn), &v), col = ev.x - pn->x0 - vx;
			int hoff = i == pn->cy ? pn->hoff : 0;
			size_t vs = (size_t)(v.s - pn->l[i].s);
			pn->cy = i;
			pn->cx = ev.y < pn->y0 + pn->rows && ev.y >= pn->y0 && i == pn->top + ev.y - pn->y0
			         ? vs + (col < 0 ? 0 : x_of_col(&v, col + hoff)) : pn->l[i].len;
			pn->want_col = col_of(&pn->l[i], pn->cx);
			return;
		}
		int y = pn->y0;
		for (int i = pn->top; i < pn->n && y < pn->y0 + pn->rows; i++) {
			int h = line_rows(pn, &pn->l[i]);
			if (ev.y >= y && ev.y < y + h) {
				pn->cy = i;
				int col = ev.x - pn->x0;
				if (pn->nowrap) col += pn->hoff;
				pn->cx = x_at(pn, &pn->l[i], ev.y - y, col < 0 ? 0 : col);
				pn->want_col = col_of(&pn->l[i], pn->cx);
				return;
			}
			y += h;
		}
		if (ev.y >= y && ev.y >= pn->y0) {   /* below the text */
			pn->cy = pn->n - 1;
			pn->cx = pn->l[pn->cy].len;
			pn->want_col = col_of(&pn->l[pn->cy], pn->cx);
		}
		return;
	}
}

/* Enter in the header window: on Subject to the message; else a new empty
 * line below for another header ("Reply-To: ..."); on an empty line the
 * next line (after the last one: the message). */
static void hdr_enter(ed_t *e)
{
	pane_t *p = &e->hp;
	line_t *l = &p->l[p->cy];
	size_t n = hdr_name_len(l);
	if (n == 8 && !strncasecmp(l->s, "Subject:", 8)) {
		set_mode(e, M_BODY);
		return;
	}
	if (!l->len) {
		if (p->cy + 1 < p->n) {
			p->cy++;
			p->cx = p->l[p->cy].len;
		} else {
			set_mode(e, M_BODY);
		}
		return;
	}
	ins_line(e, p->cy + 1, "", 0);
	p->cy++;
	p->cx = 0;
	e->modified = 1;
}

static void insert_key(ed_t *e, wint_t wc, int is_key)
{
	if (is_key) {
		switch (wc) {
		case KEY_BACKSPACE:
			if (e->p == &e->hp && e->p->cx <= hdr_min(&e->p->l[e->p->cy]) &&
			    (e->p->cx > 0 || hdr_name_len(&e->p->l[e->p->cy])))
				break;                                /* not into the header's name */
			if (e->p->cx > 0) {
				size_t p = prev_char(&e->p->l[e->p->cy], e->p->cx);
				del_range(e, p, e->p->cx);
				e->p->cx = p;
			} else if (e->p->cy > 0) {
				e->p->cy--;
				join_next(e, 0);
			}
			break;
		case KEY_DC:
			if (e->p->cx < e->p->l[e->p->cy].len) del_range(e, e->p->cx, next_char(&e->p->l[e->p->cy], e->p->cx));
			else if (e->p != &e->hp) join_next(e, 0);   /* the next header keeps its own line */
			break;
		case KEY_ENTER:
			if (e->p == &e->hp) hdr_enter(e);
			else split_line(e);
			break;
		default:
			movement(e, (int)wc);
			return;
		}
		e->p->want_col = col_of(&e->p->l[e->p->cy], e->p->cx);
		return;
	}
	if (wc == 27 || wc == 3) {                       /* Esc, Ctrl-C */
		set_mode(e, M_CMD);                          /* leave editing (header or message) */
		if (e->p->cx > 0) e->p->cx = prev_char(&e->p->l[e->p->cy], e->p->cx);
		return;
	}
	if (wc == 127 || wc == 8) { insert_key(e, KEY_BACKSPACE, 1); return; }
	if (wc == '\n' || wc == '\r') {
		if (e->p == &e->hp) hdr_enter(e);
		else split_line(e);
		e->p->want_col = col_of(&e->p->l[e->p->cy], e->p->cx);
		return;
	}
	if (wc == '\t' && e->mode == M_HDR) { set_mode(e, M_BODY); return; }   /* Tab: to the message */
	if (wc < 0x20 && wc != '\t') return;
	uint32_t cp = (uint32_t)wc;
	if (!utf16_join(&cp)) return;                     /* first half of a Windows pair */
	buf_t b = {0};
	utf8_put(&b, cp);
	ins_bytes(e, b.d, b.len);
	buf_free(&b);
	if (e->p == &e->hp && cp == ':' && hdr_name_len(&e->p->l[e->p->cy]) == e->p->cx) {
		hdr_space(&e->p->l[e->p->cy]);         /* a new header's name is done: "Name: " */
		e->p->cx++;
	}
	if (wc != ' ' && wc != '\t') hard_wrap(e);
	e->p->want_col = col_of(&e->p->l[e->p->cy], e->p->cx);
}

static void enter_insert(ed_t *e)
{
	snapshot(e);
	set_mode(e, M_BODY);
	e->msg[0] = '\0';
}

/* Choose a file and add an "Attach: PATH" line to the header; deleting that
 * line (dd in the header window) removes the attachment again. */
static void attach_file(ed_t *e)
{
	static char dir[1024];
	char *path = ui_file_dialog("Attach a file", dir, sizeof dir, editor_redraw);
	if (!path) {
		e->msg[0] = '\0';
		return;
	}
	snapshot(e);
	buf_t l = {0};
	const char *home = getenv("HOME");
	size_t hl = home ? strlen(home) : 0;
	if (hl > 1 && !strncmp(path, home, hl) && path[hl] == '/')
		buf_printf(&l, "Attach: ~%s", path + hl);      /* shorter; Attach: understands ~ */
	else
		buf_printf(&l, "Attach: %s", path);
	pane_t *keep = e->p;
	e->p = &e->hp;
	int at = e->hp.n;
	while (at > 0 && !e->hp.l[at - 1].len) at--;    /* after the last header line */
	ins_line(e, at, l.d, l.len);
	e->p = keep;
	e->modified = 1;
	const char *base = strrchr(path, '/');
	snprintf(e->msg, sizeof e->msg, "Attached %.200s - to remove it, delete its Attach: line (H, dd)",
	         base ? base + 1 : path);
	buf_free(&l);
	free(path);
}

/* Open: is t (n bytes, '\n' line ends) an email message? A header block
 * ("Name: value" lines and their continuations; an mbox "From " line before
 * it is skipped) that ends in an empty line, with at least one of From, To,
 * Subject, Date, Message-ID. *start: where the message starts. */
static int is_message(const char *t, size_t n, size_t *start)
{
	size_t p = 0;
	if (n > 5 && !memcmp(t, "From ", 5)) {
		const char *nl = memchr(t, '\n', n);
		if (!nl) return 0;
		p = (size_t)(nl - t) + 1;
	}
	*start = p;
	int lines = 0, known = 0;
	while (p < n) {
		const char *nl = memchr(t + p, '\n', n - p);
		size_t len = nl ? (size_t)(nl - (t + p)) : n - p;
		if (!len) return lines > 0 && known;        /* the end of the header */
		line_t l = {(char *)t + p, len};
		size_t nm = hdr_name_len(&l);
		if (nm) {
			static const char *const k[] = {"From:", "To:", "Subject:", "Date:", "Message-ID:", NULL};
			for (int i = 0; k[i]; i++)
				if (nm == strlen(k[i]) && !strncasecmp(l.s, k[i], nm)) known = 1;
		} else if (!lines || !is_blank(l.s[0])) {
			return 0;                                   /* neither a header nor its continuation */
		}
		lines++;
		if (!nl) return 0;                              /* no body separator */
		p += len + 1;
	}
	return 0;
}

/* Index of the header window line "Name: ...", or -1. */
static int hdr_find(const ed_t *e, const char *name)
{
	size_t nl = strlen(name);
	for (int i = 0; i < e->hp.n; i++)
		if (hdr_name_len(&e->hp.l[i]) == nl + 1 && !strncasecmp(e->hp.l[i].s, name, nl)) return i;
	return -1;
}

static int hdr_filled(const ed_t *e, const char *name)
{
	int i = hdr_find(e, name);
	if (i < 0) return 0;
	const line_t *l = &e->hp.l[i];
	for (size_t k = hdr_min(l); k < l->len; k++)
		if (!is_blank(l->s[k])) return 1;
	return 0;
}

/* Set header line "Name: value" (added after the last line if missing). */
static void hdr_set(ed_t *e, const char *name, const char *value)
{
	buf_t b = {0};
	buf_printf(&b, "%s: %s", name, value);
	int i = hdr_find(e, name);
	pane_t *keep = e->p;
	e->p = &e->hp;
	if (i < 0) {
		int at = e->hp.n;
		while (at > 0 && !e->hp.l[at - 1].len) at--;
		ins_line(e, at, b.d, b.len);
	} else {
		free(e->hp.l[i].s);
		e->hp.l[i].len = b.len;
		e->hp.l[i].s = buf_steal(&b);
	}
	e->p = keep;
	buf_free(&b);
}

/* Put text into the message window: it replaces an empty message, else it
 * goes in below the cursor line. */
static void body_insert(ed_t *e, const char *t, size_t n)
{
	pane_t *keep = e->p, *bp = &e->bp;
	e->p = bp;
	while (n && t[n - 1] == '\n') n--;
	int empty = 1;
	for (int i = 0; i < bp->n && empty; i++)
		if (bp->l[i].len) empty = 0;
	if (empty) {
		pane_clear(bp);
		bp->cy = bp->top = 0;
	}
	int at = empty ? 0 : bp->cy + 1, first = at;
	for (size_t p = 0;;) {
		const char *nl = memchr(t + p, '\n', n - p);
		size_t len = nl ? (size_t)(nl - (t + p)) : n - p;
		ins_line(e, at++, t + p, len);
		if (!nl) break;
		p += len + 1;
	}
	pane_fix(bp);
	bp->cy = first;                                    /* on its first line */
	bp->cx = 0;
	e->p = keep;
}

/* O / Open: load a text file. An email message, while To, Cc, Bcc and
 * Subject are all empty: those four headers go to the header window and
 * its text to the message. Anything else: the whole file to the message. */
static void open_file(ed_t *e)
{
	static char dir[1024];
	char *path = ui_file_dialog("Open a text file", dir, sizeof dir, editor_redraw);
	if (!path) {
		e->msg[0] = '\0';
		return;
	}
	FILE *f = fopen(path, "rb");
	buf_t raw = {0};
	char chunk[65536];
	size_t r;
	while (f && (r = fread(chunk, 1, sizeof chunk, f)) > 0 && raw.len < 16u << 20)
		buf_add(&raw, chunk, r);
	int err = !f || ferror(f);
	if (f) fclose(f);
	const char *base = path;
	for (const char *c = path; *c; c++)
		if (*c == '/' || *c == '\\') base = c + 1;
	if (err || raw.len >= 16u << 20 || (raw.len && memchr(raw.d, '\0', raw.len))) {
		snprintf(e->msg, sizeof e->msg, err ? "Cannot read %.200s: %s"
		         : raw.len >= 16u << 20 ? "%.200s is too large (16 MB at most)%s"
		         : "%.200s is not a text file%s", base, err ? strerror(errno) : "");
		e->msg_err = 1;
		buf_free(&raw);
		free(path);
		return;
	}
	/* lines end in \n */
	size_t n = 0;
	for (size_t i = 0; i < raw.len; i++)
		if (!(raw.d[i] == '\r' && i + 1 < raw.len && raw.d[i + 1] == '\n')) raw.d[n++] = raw.d[i];
	raw.len = n;
	buf_cstr(&raw);

	snapshot(e);
	size_t start;
	static const char *const H[] = {"To", "Cc", "Bcc", "Subject"};
	int filled = 0;
	for (int i = 0; i < 4; i++) filled |= hdr_filled(e, H[i]);
	if (!filled && is_message(raw.d, raw.len, &start)) {
		const char *m = raw.d + start;
		size_t ml = raw.len - start;
		for (int i = 0; i < 4; i++) {
			char *v = mime_header(m, ml, H[i]);          /* unfolded, decoded */
			if (v) {
				for (char *c = v; *c; c++)
					if ((unsigned char)*c < 0x20) *c = ' ';
				hdr_set(e, H[i], v);
				free(v);
			}
		}
		char *body = mime_render_body(m, ml);
		body_insert(e, body, strlen(body));
		free(body);
		snprintf(e->msg, sizeof e->msg, "Opened %.200s: its To, Cc, Bcc and Subject in the header, its text in the message", base);
	} else {
		size_t cl;
		char *t = utf8_clean(raw.d, raw.len, &cl);   /* other encodings: as Windows-1252 */
		body_insert(e, t, cl);
		free(t);
		snprintf(e->msg, sizeof e->msg, "Opened %.200s in the message", base);
	}
	e->modified = 1;
	buf_free(&raw);
	free(path);
}

/* Ctrl-Q / Ctrl-C: mcm quits at once; the message being written is not lost but saved
 * as a text file (~/.local/share/mcm/unsent-DATE.txt). */
int editor_save_unsent(char *path, size_t n)
{
	*path = '\0';
	if (!E || !E->modified) return 0;
	size_t len;
	char *t = get_text(E, &len);
	const char *xdg = getenv("XDG_DATA_HOME"), *home = getenv("HOME");
	char dir[900];
	if (xdg && *xdg) snprintf(dir, sizeof dir, "%.800s/mcm", xdg);
	else snprintf(dir, sizeof dir, "%.800s/.local/share/mcm", home ? home : ".");
	for (char *s = dir + 1; *s; s++)
		if (*s == '/') { *s = '\0'; mkdir(dir, 0700); *s = '/'; }
	mkdir(dir, 0700);
	char stamp[32];
	time_t now = time(NULL);
	struct tm tm;
	localtime_r(&now, &tm);
	strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &tm);
	snprintf(path, n, "%s/unsent-%s.txt", dir, stamp);
	int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
	int ok = fd >= 0 && write(fd, t, len) == (ssize_t)len;
	if (fd >= 0 && close(fd) != 0) ok = 0;
	free(t);
	if (!ok) {
		*path = '\0';
		return -1;
	}
	return 0;
}

/* Command mode key. Returns 1 to leave the editor. */
static int command_key(ed_t *e, int ch)
{
	line_t *l = &e->p->l[e->p->cy];
	int pend = e->pending;
	e->pending = 0;

	if (pend == 'd' && ch == 'd') { yank_lines(e, e->p->cy, 1, 1); return 0; }
	if (pend == 'y' && ch == 'y') { yank_lines(e, e->p->cy, 1, 0); return 0; }
	if (pend == 'g' && ch == 'g') { e->p->cy = 0; e->p->cx = 0; return 0; }
	if (pend == 'g' && ch == 'q') {
		if (reflow(e) != 0) snprintf(e->msg, sizeof e->msg, "gq: put the cursor in a paragraph of the message");
		return 0;
	}
	if (pend == 'Z' && ch == 'Q') return 1;
	if (pend == 'Z' && ch == 'Z') return quit(e, 0);

	if (movement(e, ch)) return 0;
	switch (ch) {
	/* menu: EdIt Send Keep Header */
	/* the menu letters work in both cases (so h and k are not vim moves) */
	case 'E': case 'e': case 'I': case 'i': enter_insert(e); break;
	case 'S': case 's': return run_action(e, e->cfg->send);
	case 'K': case 'k': return run_action(e, e->cfg->keep);
	case 'H': case 'h':
		snapshot(e);
		set_mode(e, M_HDR);
		e->msg[0] = '\0';
		break;
	/* vim */
	case 'Y': yank_lines(e, e->p->cy, 1, 0); break;
	case 'a': enter_insert(e); e->p->cx = next_char(l, e->p->cx); break;
	case 'A': enter_insert(e); e->p->cx = l->len; break;
	case 'o':                                 /* (O is Open) */
		enter_insert(e);
		e->p->cy++;
		ins_line(e, e->p->cy, "", 0);
		e->p->cx = 0;
		e->modified = 1;
		break;
	case ' ': movement(e, KEY_RIGHT); break;
	case '0': case '^': e->p->cx = 0; e->p->want_col = 0; break;
	case '$': e->p->cx = l->len; e->p->want_col = col_of(l, e->p->cx); break;
	case 'T': case 't': attach_file(e); break;
	case 'O': open_file(e); break;
	case 'W': case 'w':
		e->wrap = !e->wrap;
		if (e->wrap)
			snprintf(e->msg, sizeof e->msg, "Wrap on: lines are wrapped at %d columns while typing (gq: re-wrap)", WRAP_COL);
		else
			snprintf(e->msg, sizeof e->msg, "unWrap: lines are not wrapped; long lines scroll sideways (gq still works)");
		break;
	case 'b': word_back(e); break;
	case 'G': e->p->cy = e->p->n - 1; e->p->cx = 0; break;
	case 6: movement(e, KEY_NPAGE); break;    /* Ctrl-F */
	case 2: movement(e, KEY_PPAGE); break;    /* Ctrl-B */
	case 'x': case KEY_DC:
		if (l->len) {
			snapshot(e);
			del_range(e, e->p->cx, next_char(l, e->p->cx));
			if (e->p->cx >= l->len && e->p->cx > 0) e->p->cx = prev_char(l, e->p->cx);
		}
		break;
	case 'D':
		snapshot(e);
		del_range(e, e->p->cx, l->len);
		break;
	case 'J':
		if (e->p == &e->hp) break;            /* header lines stay apart */
		snapshot(e);
		join_next(e, 1);
		break;
	case 'p': put_lines(e, 1); break;
	case 'P': put_lines(e, 0); break;
	case 'u': undo(e); break;
	case 'd': case 'y': case 'g': case 'Z': e->pending = ch; break;
	case ':': return colon_cmd(e);
	case 'Q': return quit(e, 0);
	case 27:                                  /* Esc: leave the editor (a pending d/y/g/Z: cancel it) */
		if (pend) break;
		return quit(e, 0);
	case 12: clearok(curscr, TRUE); break;    /* Ctrl-L */
	default: break;
	}
	return 0;
}

void editor_run(const editor_cfg *cfg, const char *text, int start_line)
{
	ed_t e = {.cfg = cfg, .wrap = cfg->wrap};
	e.hp.field = 1;
	e.hp.hoff_line = -1;
	e.hb.grab = -1;
	ed_t *prev = E;
	E = &e;
	e.want_line = start_line;
	set_text(&e, text, strlen(text));

	/* start in the header window, at start_line if it is a shown header line
	 * (else at the first one, To:); the body cursor goes to start_line if it
	 * is there */
	e.hp.cy = e.want_vis >= 0 ? e.want_vis : 0;
	e.hp.cx = e.hp.l[e.hp.cy].len;
	int bl = start_line - e.nhdr - 1;
	e.bp.cy = bl >= 0 && bl < e.bp.n ? bl : 0;
	e.bp.cx = e.bp.l[e.bp.cy].len;
	set_mode(&e, M_HDR);
	e.hp.want_col = col_of(&e.hp.l[e.hp.cy], e.hp.cx);
	curs_set(1);

	for (;;) {
		draw();
		refresh();
		timeout(1000);              /* keeps the clock running */
		wint_t wc;
		int r = ui_get_wch(&wc);
		if (r == ERR) continue;
		if (r == KEY_CODE_YES && wc == KEY_RESIZE) continue;
		int menu = 0;
		if (r == KEY_CODE_YES && wc == KEY_MOUSE) {
			int ch;
			mouse(&e, &ch);
			if (!ch) continue;           /* also the button release: keeps the message */
			r = OK;
			wc = (wint_t)ch;
			menu = 1;
		}
		if (!e.insert) e.msg[0] = '\0';
		e.msg_err = 0;
		if (menu && e.insert) {          /* menu click while editing the message */
			if (e.mode == M_BODY) insert_key(&e, 27, 0);   /* leave editing, as Esc */
			else set_mode(&e, M_CMD);                      /* from the header window */
			if (wc == 27) continue;
		}
		if (e.insert) {
			insert_key(&e, wc, r == KEY_CODE_YES);
			continue;
		}
		int ch = r == KEY_CODE_YES ? (int)wc : (wc < 0x80 ? (int)wc : 0);
		if (command_key(&e, ch)) break;
	}

	if (e.hb.grab >= 0) ui_mouse_drag(0);
	curs_set(0);
	pane_clear(&e.hp);
	pane_clear(&e.bp);
	free(e.yank);
	buf_free(&e.hidden);
	for (int i = 0; i < e.nundo; i++) free(e.undo[i]);
	E = prev;
}
