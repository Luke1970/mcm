/* msgwin.c - the message window: its text (wrapped or not, all headers,
 * hex), the ':' commands (as in vim), saving and printing the message
 */
#include "mcm.h"

static void push_line(size_t off, size_t len);
static void index_lines(const char *t, size_t n, int width);
static void build_text(int width);
static int hex_per_line(int width);
static void draw_text_line(int y, int x, int w, const line_t *l);
static void draw_hex(int x, int y, int w, int rows);
static int load_whole_message(void);
static int line_at_off(size_t off);
static void view_in_editor(void);
static void write_message_to(const char *name, int force);
static void search_message(const char *pat, int back);
/* a link in the message text (see collect_stops) */
typedef struct stop {
	size_t off, len;        /* in A.mv.text */
	const char *url;        /* points into the links list or the text; an
	                         * attachment: "name  (type, size)" */
	size_t ulen;
	int att;                /* 0 a link, k+1 the k-th "[Attachment: ...]" */
} stop_t;

/* Where the stops are on the screen (drawn last), for mouse clicks. */
static struct { int y, x0, x1, stop; } hits[512];
static int nhits;
static void stops_reset(void);
static int get_stops(stop_t **v);

/* ------------------------------------------------------------------ */
/* Message pane text                                                   */

static void push_line(size_t off, size_t len)
{
	if (A.nlines == A.lines_cap) {
		A.lines_cap = A.lines_cap ? A.lines_cap * 2 : 256;
		A.lines = xrealloc(A.lines, sizeof *A.lines * (size_t)A.lines_cap);
	}
	A.lines[A.nlines].off = (uint32_t)off;
	A.lines[A.nlines].len = (uint32_t)len;
	A.nlines++;
}

/* Word-wrap the message text into display lines: only offsets are stored. */
static void index_lines(const char *t, size_t n, int width)
{
	size_t i = 0;
	A.nlines = 0;
	A.maxw = 0;
	if (width <= 0) width = INT_MAX;        /* not wrapped */
	while (i < n) {
		size_t j = i;
		while (j < n && t[j] != '\n') j++;
		if (j == i) push_line(i, 0);

		size_t s = i;
		while (s < j) {
			int col = 0;
			size_t k = s, lastsp = (size_t)-1, first_len = 1;
			while (k < j) {
				size_t clen;
				wchar_t c = utf8_next(t + k, j - k, &clen);
				int cw = c == '\t' ? 8 - col % 8 : ui_wcwidth(c);
				/* the active link's > and < take a column each */
				if (A.lk_url && (k == A.lk_off || k == A.lk_off + A.lk_len)) cw++;
				if (k == s) first_len = clen;
				if (col + cw > width) break;
				if (c == ' ' || c == '\t') lastsp = k;
				col += cw;
				k += clen;
			}
			if (col > A.maxw) A.maxw = col;
			if (k >= j) { push_line(s, j - s); break; }
			size_t brk = (lastsp != (size_t)-1 && lastsp > s) ? lastsp : k;
			if (brk == s) brk = s + first_len;
			push_line(s, brk - s);
			s = brk;
			while (s < j && t[s] == ' ') s++;
		}
		i = j + 1;
	}
}

static void build_text(int width)
{
	if (!A.mv.open) { A.nlines = 0; return; }
	if (!A.text_dirty && !A.reindex && A.lines_width == width) return;
	if (A.text_dirty) {
		mv_render(&A.mv, A.show_headers);
		stops_reset();
		/* the active link stays only where the text still has it */
		if (A.lk_url && (A.lk_off + A.lk_len > A.mv.textlen ||
		                 memcmp(A.mv.text + A.lk_off, A.lk_text, A.lk_len)))
			link_clear();
	}
	A.text_dirty = 0;
	A.reindex = 0;
	index_lines(A.mv.text, A.mv.textlen, width);
	A.lines_width = width;

	if (A.keep_line) {                     /* W: stay at the same text */
		int i = 0;
		while (i + 1 < A.nlines && A.lines[i + 1].off < (uint32_t)A.keep_line) i++;
		A.vtop = i;
		A.keep_line = 0;
	}

	A.more_line = -1;
	if (A.mv.more_off != MV_NO_MORE)
		for (int i = 0; i < A.nlines && A.lines[i].off <= A.mv.more_off; i++) A.more_line = i;
}

void close_message(void)
{
	link_clear();
	stops_reset();
	mv_close(&A.mv);
	A.nlines = 0;
	A.vtop = 0;
	A.hoff = 0;
	A.keep_line = 0;
	A.text_dirty = 1;
	A.lines_width = -1;
}

static int hex_per_line(int width)
{
	int per = 16;
	while (per > 4 && 11 + per * 4 > width) per /= 2;
	return per;
}

static void draw_text_line(int y, int x, int w, const line_t *l)
{
	const char *t = A.mv.text + l->off;
	wchar_t *e = xmalloc((l->len * 8 + 3) * sizeof *e);
	size_t k = 0;
	int col = 0, h0 = -1, h1 = -1;           /* the active link: columns [h0, h1) */
	size_t a = A.lk_off, b = A.lk_off + A.lk_len;
	int on = A.lk_url && a < l->off && b > l->off;   /* it started on a line above */
	if (on) h0 = 0;
	/* the column of byte i: cb before a > or < put in front of it, ca after */
	int *cb = xmalloc(sizeof *cb * (l->len + 1)), *ca = xmalloc(sizeof *ca * (l->len + 1));
	for (size_t i = 0;;) {
		size_t at = l->off + i;
		cb[i] = col;
		if (A.lk_url && at == a && i < l->len) {
			h0 = col;
			e[k++] = '>';
			col++;
			on = 1;
		}
		/* < after it; also at the end of the line when only the spaces
		 * wrapping left out are between */
		int end = on && at == b;
		if (on && !end && i == l->len && b > at) {
			end = 1;
			for (size_t q = at; q < b && end; q++) end = A.mv.text[q] == ' ' || A.mv.text[q] == '\n';
		}
		if (end) {
			e[k++] = '<';
			col++;
			h1 = col;
			on = 0;
		}
		ca[i] = col;
		if (i == l->len) break;
		size_t clen;
		wchar_t c = utf8_next(t + i, l->len - i, &clen);
		if (!clen) clen = 1;
		for (size_t q = 1; q < clen && i + q <= l->len; q++) cb[i + q] = ca[i + q] = col;
		i += clen;
		if (c == '\t') {
			do { e[k++] = ' '; col++; } while (col % 8);
		} else {
			e[k++] = c;
			col += ui_wcwidth(c);
		}
	}
	if (on) h1 = col;                        /* it goes on below */
	e[k] = 0;
	/* scrolled sideways: leave out the first A.hoff columns (half of a
	 * wide character becomes a space) */
	wchar_t *p = e;
	for (col = 0; *p && col < A.hoff; p++) col += ui_wcwidth(*p);
	if (col > A.hoff) *--p = ' ';
	short base = l->off < A.mv.hdr_end ? CP_HEADER : CP_NORMAL;
	ui_putw(y, x, p, w, 0, base);
	/* the links that can be chosen (Up/Down, Enter) underlined, as under a
	 * mouse pointer */
	if (!A.hex && l->off >= A.mv.hdr_end) {
		stop_t *v;
		int n = get_stops(&v);
		size_t lo = l->off, hi = l->off + l->len;
		for (int i = 0; i < n && v[i].off <= hi; i++) {
			size_t so = v[i].off, eo = v[i].off + v[i].len;
			if (eo <= lo || so >= hi) continue;
			int u0 = so <= lo ? 0 : ca[so - lo], u1 = eo >= hi ? cb[l->len] : cb[eo - lo];
			int h0c = u0 - A.hoff < 0 ? 0 : u0 - A.hoff, h1c = u1 - A.hoff > w ? w : u1 - A.hoff;
			if (h1c > h0c && nhits < (int)(sizeof hits / sizeof *hits)) {
				hits[nhits].y = y;
				hits[nhits].x0 = x + h0c;
				hits[nhits].x1 = x + h1c;
				hits[nhits++].stop = i;
			}
			int c = 0;
			for (wchar_t *q = e; *q; c += ui_wcwidth(*q), q++) {
				int cw = ui_wcwidth(*q);
				if (c < u0 || c >= u1 || c < A.hoff || cw <= 0) continue;
				if (c - A.hoff + cw > w) break;
				wchar_t one[2] = {*q, 0};
				ui_putw(y, x + c - A.hoff, one, cw, A_UNDERLINE, base);
			}
		}
	}
	if (h0 >= 0) {                           /* the active link in the highlight color */
		int c = 0;
		for (wchar_t *q = e; *q; c += ui_wcwidth(*q), q++) {
			int cw = ui_wcwidth(*q);
			if (c < h0 || c >= h1 || c < A.hoff || cw <= 0) continue;
			if (c - A.hoff + cw > w) break;
			wchar_t one[2] = {*q, 0};
			ui_putw(y, x + c - A.hoff, one, cw, A_BOLD | A_UNDERLINE, CP_HIGHLIGHT);
		}
	}

	/* after :/ or :? the matches (any case) in the highlight color */
	if (A.hls && *A.srch) {
		wchar_t *pw;
		size_t pl = utf8_to_wcs(A.srch, strlen(A.srch), &pw);
		int c0 = 0;                       /* column of e[i] */
		for (size_t i = 0; pl && i + pl <= k; c0 += ui_wcwidth(e[i]), i++) {
			size_t j = 0;
			while (j < pl && towlower((wint_t)e[i + j]) == towlower((wint_t)pw[j])) j++;
			if (j < pl) continue;
			/* draw e[i .. i+pl) where it is visible */
			int c = c0;
			for (size_t q = i; q < i + pl; c += ui_wcwidth(e[q]), q++) {
				int cw = ui_wcwidth(e[q]);
				if (c < A.hoff || cw <= 0) continue;
				if (c - A.hoff + cw > w) break;
				wchar_t one[2] = {e[q], 0};
				ui_putw(y, x + c - A.hoff, one, cw, 0, CP_HIGHLIGHT);
			}
			for (size_t q = i; q + 1 < i + pl; q++) c0 += ui_wcwidth(e[q]);   /* past it */
			i += pl - 1;
		}
		free(pw);
	}
	free(e);
	free(cb);
	free(ca);
}

static void draw_hex(int x, int y, int w, int rows)
{
	int per = hex_per_line(w);
	size_t off0 = (size_t)A.vtop * (size_t)per;
	const unsigned char *p = mv_hex_peek(&A.mv, off0, (size_t)rows * (size_t)per);
	if (!p) {
		A.need_hex = 1;
		A.need_hex_off = off0;
		A.need_hex_len = (size_t)rows * (size_t)per;
		for (int r = 0; r < rows; r++)
			ui_puts(y + r, x, r == 0 ? "…" : "", w, A_DIM, CP_NORMAL);
		return;
	}
	for (int r = 0; r < rows; r++) {
		size_t off = off0 + (size_t)r * (size_t)per;
		if (off >= A.mv.size) break;
		char tmp[128];
		int n = snprintf(tmp, sizeof tmp, "%08zx  ", off);
		const unsigned char *b = p + (size_t)r * (size_t)per;
		for (int k = 0; k < per; k++)
			n += off + (size_t)k < A.mv.size ? snprintf(tmp + n, sizeof tmp - (size_t)n, "%02x ", b[k])
			                                 : snprintf(tmp + n, sizeof tmp - (size_t)n, "   ");
		tmp[n++] = ' ';
		for (int k = 0; k < per && off + (size_t)k < A.mv.size; k++)
			tmp[n++] = (b[k] >= 32 && b[k] < 127) ? (char)b[k] : '.';
		tmp[n] = '\0';
		ui_puts(y + r, x, tmp, w, 0, CP_NORMAL);
	}
}

/* Does the open message have attachments (T inverted)? Its structure
 * says so; without one (local mail) the list's @ does. */
static int open_has_att(void)
{
	static uint32_t uid;
	static const void *root;
	static int has;
	if (!A.mv.open) return 0;
	if (A.mv.uid == uid && A.mv.root == root) return has;
	uid = A.mv.uid;
	root = A.mv.root;
	mpart_t **v;
	has = mv_attachments(&A.mv, &v) > 0;
	free(v);
	if (!has && !A.mv.root)
		for (int i = 0; i < nrows(); i++) {
			const msg_t *m = msg_at(i);
			if (m && m->uid == uid) { has = m->att; break; }
		}
	return has;
}

void draw_message(int x, int y, int w, int h, int focused)
{
	int rows = h - 2, inner = w - 2;
	A.vrows = rows;

	if (A.mv.open) {
		if (A.hex) {
			int per = hex_per_line(inner);
			A.nlines = (int)((A.mv.size + (uint32_t)per - 1) / (uint32_t)per);
			A.lines_width = -1;   /* text index must be rebuilt when leaving hex */
		} else {
			/* W: wrapped at the window, at most 76 columns; else not at all */
			build_text(A.nowrap ? 0 : inner < WRAP_VIEW ? inner : WRAP_VIEW);
		}
	}
	int hmax = A.mv.open && !A.hex && A.nowrap ? A.maxw - inner : 0;
	if (A.hoff > hmax) A.hoff = hmax;
	if (A.hoff < 0) A.hoff = 0;
	if (A.vtop > A.nlines - rows) A.vtop = A.nlines - rows;
	if (A.vtop < 0) A.vtop = 0;

	char title[128];
	const char *what = A.hex ? "Hex" : A.show_headers ? "Message, all headers" : "Message";
	if (A.mv.open && A.nlines > rows)
		snprintf(title, sizeof title, " %s  %d%% ", what, (A.vtop + rows) * 100 / A.nlines);
	else
		snprintf(title, sizeof title, " %s ", what);
	ui_box(y, x, h, w, CP_NORMAL, title, A_BOLD, focused ? CP_TITLE : CP_NORMAL);
	draw_scrollbar(COL_MSG, x + w - 1, y + 1, rows, A.mv.open && !A.hex ? A.nlines : 0, &A.vtop);

	/* bottom border: H (all headers), W (wrap), X (hex), inverted when on,
	 * S (save), P (print), T (attachments), K (links); then a horizontal
	 * scroll bar for long lines, or the paging hint */
	int by = y + h - 1, bx = x + 16;
	if (w >= 20) {
		const int on[7] = {A.show_headers, !A.nowrap, A.hex, 0, 0, open_has_att(), 0};
		for (int i = 0; i < 7; i++)
			ui_puts(by, x + 2 + 2 * i, (const char[]){HWXSP[i], 0}, 1,
			        A_BOLD | (on[i] ? A_REVERSE : 0), CP_NORMAL);
		A.hwx_y = by;
		A.hwx_x = x + 2;
	} else bx = x + 1;
	draw_hscrollbar(SB_HMSG, bx, by, x + w - 1 - bx, A.mv.open && !A.hex && A.nowrap ? A.maxw : 0,
	                inner, &A.hoff);
	static const char hint[] = " Spc=PgDn B=PgUp ";
	if (!sbs[SB_HMSG].h && (int)strlen(hint) + 2 <= x + w - 1 - bx)
		ui_puts(by, x + w - 1 - (int)strlen(hint), hint, (int)strlen(hint), 0, CP_NORMAL);

	if (!A.mv.open) {
		const char *m = A.pending_load ? "Loading..." : "(no message selected)";
		ui_puts(y + 1, x + 2, m, inner - 2, A_DIM, CP_NORMAL);
		return;
	}
	if (A.hex) {
		draw_hex(x + 1, y + 1, inner, rows);
		return;
	}
	nhits = 0;
	for (int r = 0; r < rows && A.vtop + r < A.nlines; r++)
		draw_text_line(y + 1 + r, x + 1, inner, &A.lines[A.vtop + r]);
	/* the reader reached the end of what is loaded: fetch more */
	if (A.more_line >= 0 && A.vtop + rows > A.more_line) A.need_more = 1;
}

/* Load the rest of every text part of the open message, so that the whole
 * text is in A.mv.text (rendered as shown: H = all headers). 0 = done. */
static int load_whole_message(void)
{
	for (int i = 0; i < 1000000; i++) {
		mv_render(&A.mv, A.show_headers);
		stops_reset();
		A.text_dirty = 1;                       /* the display index is rebuilt */
		if (A.mv.more_off == MV_NO_MORE) return 0;
		if (i == 0) now_status("Loading the whole message...");
		if (mv_more(&A.mv, A.im) != 0) {
			set_status("Error: %s", store_error(A.im));
			return -1;
		}
	}
	set_status("%s", "Error: the message does not end");
	return -1;
}

/* Append s to b for a file name: no control characters, no bidi controls
 * (U+202A-202E, U+2066-2069, U+200E/F, U+061C: they can make "x<RLO>fdp.exe"
 * look like "xexe.pdf") and no / \ : * ? " < > | (all become _), at most
 * max bytes, never half a UTF-8 character. */
void name_part(buf_t *b, const char *s, size_t max)
{
	size_t start = b->len;
	while (*s == ' ') s++;
	while (*s) {
		unsigned char c = (unsigned char)*s;
		size_t n = 1;
		if (c >= 0xC0)
			while (n < 4 && ((unsigned char)s[n] & 0xC0) == 0x80) n++;
		if (b->len - start + n > max) break;
		const unsigned char *u = (const unsigned char *)s;
		int bidi = n == 3 && u[0] == 0xE2 &&
		           ((u[1] == 0x80 && ((u[2] >= 0xAA && u[2] <= 0xAE) || u[2] == 0x8E || u[2] == 0x8F)) ||
		            (u[1] == 0x81 && u[2] >= 0xA6 && u[2] <= 0xA9));
		if (n == 2 && u[0] == 0xD8 && u[1] == 0x9C) bidi = 1;          /* U+061C */
		if (bidi || c < 32 || c == 127 || (c < 128 && strchr("/\\:*?\"<>|", c))) buf_addc(b, '_');
		else buf_add(b, s, n);
		s += n;
	}
	while (b->len > start && (b->d[b->len - 1] == ' ' || b->d[b->len - 1] == '.')) b->len--;
	if (b->len == start) buf_adds(b, "none");
}

/* ~/Downloads, or ~ without it (Windows: %USERPROFILE%). 0 = found. */
int downloads_dir(char *dir, size_t n)
{
	const char *home = getenv("HOME");
#ifdef _WIN32
	if (!home || !*home) home = getenv("USERPROFILE");
#endif
	if (!home || !*home) return -1;
	struct stat st;
	snprintf(dir, n, "%s/Downloads", home);
	if (stat(dir, &st) != 0 || !S_ISDIR(st.st_mode)) snprintf(dir, n, "%s", home);
	return 0;
}

/* A new file (mode 0600: private mail) named path + ext; if that exists
 * path-2 + ext, -3 ... path holds the name in the end (also on failure). */
FILE *new_file(buf_t *path, const char *ext)
{
	size_t base = path->len;
	FILE *f = NULL;
	for (int i = 1; i < 1000 && !f; i++) {
		path->len = base;
		if (i > 1) buf_printf(path, "-%d", i);
		buf_adds(path, ext);
		buf_cstr(path);
		int fd = open(path->d, O_WRONLY | O_CREAT | O_EXCL, 0600);
		if (fd >= 0) f = fdopen(fd, "wb");
		else if (errno != EEXIST) break;
	}
	return f;
}

/* S: save the open message as text (as shown: H = with all headers) in
 * ~/Downloads, or ~ without it, as mcm--FROM--YYYY-MM-DD--SUBJECT.txt.
 * P (print = 1): send that text to the printer (lp; Windows: Notepad). */
void save_message(int print)
{
	if (A.pending_load) load_selected();
	if (!A.mv.open) {
		set_status("%s", "No message is open");
		return;
	}
	if (load_whole_message() != 0) return;
	if (print) now_status("%s", "Printing...");
	/* only now: a redraw renders the text again (a new buffer) */
	const char *text = A.mv.text;
	size_t len = A.mv.textlen;

	if (print) {
#ifdef _WIN32
		char tmp[MAX_PATH + 32], cmd[MAX_PATH + 64];
		DWORD tn = GetTempPathA(MAX_PATH, tmp);
		if (!tn || tn > MAX_PATH) { set_status("%s", "Error: no folder for temporary files"); return; }
		snprintf(tmp + tn, sizeof tmp - tn, "mcm-print-%lu.txt", (unsigned long)GetCurrentProcessId());
		FILE *f = fopen(tmp, "wb");
		int bad = !f || fwrite(text, 1, len, f) != len;
		if (f && fclose(f) != 0) bad = 1;
		if (bad) { set_status("Error: cannot write %s", tmp); remove(tmp); return; }
		snprintf(cmd, sizeof cmd, "notepad.exe /p \"%s\"", tmp);
		int rc = win_run(cmd);
		remove(tmp);
		if (rc != 0) set_status("%s", "Error: printing with Notepad failed");
		else set_status("%s", "Sent to the printer");
#else
		FILE *f = popen("exec lp -s >/dev/null 2>&1", "w");
		if (!f) { set_status("Error: cannot run lp: %s", strerror(errno)); return; }
		size_t w = fwrite(text, 1, len, f);
		int st = pclose(f);
		if (st == -1 || !WIFEXITED(st) || WEXITSTATUS(st) == 127)
			set_status("%s", "Error: lp is not installed (CUPS)");
		else if (WEXITSTATUS(st) != 0 || w != len)
			set_status("Error: lp failed (exit status %d); is a printer set up?", WEXITSTATUS(st));
		else set_status("%s", "Sent to the printer (lp)");
#endif
		return;
	}

	/* the folder: ~/Downloads, else ~ */
	char dir[1024];
	if (downloads_dir(dir, sizeof dir) != 0) { set_status("%s", "Error: no home folder (HOME)"); return; }

	/* the name: sender's address, date (from the Date header), subject */
	char *from = mime_header(A.mv.hdr, A.mv.hlen, "From");
	char *date = mime_header(A.mv.hdr, A.mv.hlen, "Date");
	char *subj = mime_header(A.mv.hdr, A.mv.hlen, "Subject");
	const char *addr = from ? from : "";
	char *lt = from ? strrchr(from, '<') : NULL, *gt = lt ? strchr(lt, '>') : NULL;
	if (lt && gt) { *gt = '\0'; addr = lt + 1; }
	char ymd[16] = "nodate";
	time_t t = date ? mime_parse_date(date) : 0;
	struct tm tm;
	if (t > 0 && localtime_r(&t, &tm)) strftime(ymd, sizeof ymd, "%Y-%m-%d", &tm);
	buf_t b = {0};
	buf_printf(&b, "%s/mcm--", dir);
	name_part(&b, addr, 100);
	buf_printf(&b, "--%s--", ymd);
	name_part(&b, subj ? subj : "", 120);
	free(from);
	free(date);
	free(subj);

	FILE *f = new_file(&b, ".txt");
	if (!f) {
		set_status("Error: cannot save %s: %s", buf_cstr(&b), strerror(errno));
		buf_free(&b);
		return;
	}
	int bad = fwrite(text, 1, len, f) != len || (len && text[len - 1] != '\n' && fputc('\n', f) == EOF);
	if (fclose(f) != 0) bad = 1;
	if (bad) set_status("Error: writing %s failed", b.d);
	else set_status("Saved %s", b.d);
	buf_free(&b);
}

#ifndef O_BINARY
#define O_BINARY 0
#endif

/* W: wrap the message's lines, or not; also back from all headers and
 * hex to the message text. */
void handle_key_wrap(void)
{
	if (A.show_headers || A.hex) {
		A.vtop = 0;
		A.text_dirty = 1;
	} else if (A.mv.open && A.vtop < A.nlines) {
		A.keep_line = (int)A.lines[A.vtop].off + 1;   /* stay at the same text */
	}
	A.show_headers = A.hex = 0;
	A.nowrap = !A.nowrap;
	A.hoff = 0;
	config_save();
}

/* ------------------------------------------------------------------ */
/* ':' commands in the message window (as in vim, as far as they fit)  */

/* The display line that holds byte off of the message text. */
static int line_at_off(size_t off)
{
	build_text(A.lines_width);
	int i = 0;
	while (i + 1 < A.nlines && A.lines[i + 1].off <= off) i++;
	return i;
}

/* :e - the message, as shown, in $VISUAL/$EDITOR (vi -R: read only). */
static void view_in_editor(void)
{
	if (A.pending_load) load_selected();
	if (!A.mv.open) {
		set_status("%s", "No message is open");
		return;
	}
	if (load_whole_message() != 0) return;
	char path[1100];
	int fd = -1;
#ifdef _WIN32
	char tmp[MAX_PATH + 1];
	DWORD tn = GetTempPathA(MAX_PATH, tmp);
	if (!tn || tn > MAX_PATH) snprintf(tmp, sizeof tmp, ".\\");
	for (int i = 0; i < 100 && fd < 0; i++) {
		snprintf(path, sizeof path, "%smcm-view-%lu-%d.txt", tmp, (unsigned long)GetCurrentProcessId(), i);
		fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_BINARY, 0600);
	}
#else
	const char *tmp = getenv("TMPDIR");
	snprintf(path, sizeof path, "%.1000s/mcm-view-XXXXXX.txt", tmp && *tmp ? tmp : "/tmp");
	fd = mkstemps(path, 4);
#endif
	if (fd < 0) {
		set_status("Error: cannot create a temporary file: %s", strerror(errno));
		return;
	}
	ssize_t w = write(fd, A.mv.text, A.mv.textlen);
	close(fd);
	if (w != (ssize_t)A.mv.textlen) {
		unlink(path);
		set_status("%s", "Error: cannot write the temporary file");
		return;
	}
	int rc = ui_run_editor(path, 1);
	unlink(path);
	clearok(curscr, TRUE);
	if (rc != 0) set_status("%s", "Error: the editor ($VISUAL/$EDITOR) did not run or failed");
}

/* :w FILE - the message, as shown, to FILE (:w! replaces it). */
static void write_message_to(const char *name, int force)
{
	if (A.pending_load) load_selected();
	if (!A.mv.open) {
		set_status("%s", "No message is open");
		return;
	}
	if (load_whole_message() != 0) return;
	char path[1024];
	expand_home(path, sizeof path, name);
	int fd = open(path, O_WRONLY | O_CREAT | (force ? O_TRUNC : O_EXCL) | O_BINARY, 0600);
	if (fd < 0) {
		if (errno == EEXIST) set_status("%.300s exists (add ! to replace it: :w! %.300s)", path, name);
		else set_status("Error: %.300s: %s", path, strerror(errno));
		return;
	}
	ssize_t w = write(fd, A.mv.text, A.mv.textlen);
	if (close(fd) != 0 || w != (ssize_t)A.mv.textlen) set_status("Error: cannot write %.300s", path);
	else set_status("Saved: %.300s", path);
}

/* :/text and :?text - the next (previous) line with text, any case,
 * around the end (start) of the message; "" repeats the last search. */
static void search_message(const char *pat, int back)
{
	char *last = A.srch;
	if (*pat) snprintf(A.srch, sizeof A.srch, "%s", pat);
	A.hls = *last != 0;                   /* matches highlighted, as vim's hlsearch */
	if (!*last) { set_status("%s", "No previous search"); return; }
	if (!A.mv.open || A.hex) { set_status("%s", "No message text to search"); return; }
	build_text(A.lines_width);
	const char *t = A.mv.text;
	size_t n = A.mv.textlen, pl = strlen(last);
	size_t here = A.vtop < A.nlines ? A.lines[A.vtop].off : 0;
	size_t found = (size_t)-1;
	for (size_t k = 0; k < n && found == (size_t)-1; k++) {
		/* forward: from the line after the top one; back: before it */
		size_t i = back ? (here + n - 1 - k) % n : (here + 1 + k) % n;
		if (i + pl > n) continue;
		size_t j = 0;
		while (j < pl && tolower((unsigned char)t[i + j]) == tolower((unsigned char)last[j])) j++;
		if (j == pl) found = i;
	}
	if (found == (size_t)-1) {
		set_status("Not found: %.100s", last);
		return;
	}
	int ln = line_at_off(found);
	if (ln == A.vtop && !back) {          /* on the top line already: the next one */
		size_t from = A.vtop + 1 < A.nlines ? A.lines[A.vtop + 1].off : n;
		for (size_t i = from; i + pl <= n; i++) {
			size_t j = 0;
			while (j < pl && tolower((unsigned char)t[i + j]) == tolower((unsigned char)last[j])) j++;
			if (j == pl) { ln = line_at_off(i); break; }
		}
	}
	A.vtop = ln;
	A.hoff = 0;
	if (A.nowrap) {                       /* bring the text into view sideways */
		int col = 0;
		for (size_t i = A.lines[ln].off; i < found; i++)
			if (((unsigned char)t[i] & 0xC0) != 0x80) col++;
		A.hoff = col > 8 ? col - 8 : 0;
	}
}

/* ':' in the message window: a command at the bottom line. Returns 0 to
 * quit mcm. */
int msg_colon(void) { return msg_colon_with(""); }

/* The : line, starting with init typed (Email > Find: "/"). */
int msg_colon_with(const char *init)
{
	char cmd[256];
	snprintf(cmd, sizeof cmd, "%s", init);
	size_t n = strlen(cmd);
	curs_set(1);
	for (;;) {
		ui_fill(LINES - 1, 0, 1, COLS, CP_MENU);
		ui_puts(LINES - 1, 0, ":", 1, A_BOLD, CP_MENU);
		ui_puts(LINES - 1, 1, cmd, COLS - 2, 0, CP_MENU);
		move(LINES - 1, 1 + (int)n < COLS - 1 ? 1 + (int)n : COLS - 1);
		refresh();
		int ch = ui_getch();
		if (ch == ERR) continue;
		if (ch == KEY_RESIZE) { draw(); continue; }
		if (ch == 27) { curs_set(0); return 1; }
		if (ch == '\n' || ch == '\r' || ch == KEY_ENTER) break;
		if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
			if (!n) { curs_set(0); return 1; }
			while (n && ((unsigned char)cmd[n - 1] & 0xC0) == 0x80) n--;   /* a whole character */
			if (n) n--;
			cmd[n] = '\0';
			continue;
		}
		if (ch >= 32 && ch < 256 && ch != 127 && n < sizeof cmd - 1) {
			cmd[n++] = (char)ch;
			cmd[n] = '\0';
		}
	}
	curs_set(0);
	char *c = str_trim(cmd);
	if (*c == ':') c = str_trim(c + 1);
	if (!*c) return 1;
	if (*c == '/' || *c == '?') {
		char *p = c + 1;
		size_t pl = strlen(p);
		if (pl && p[pl - 1] == *c) p[pl - 1] = '\0';     /* :/text/ */
		search_message(p, *c == '?');
		return 1;
	}
	/* the command word and its argument */
	char *arg = c + strcspn(c, " \t");
	int bang = 0;
	if (*arg) *arg++ = '\0';
	arg = str_trim(arg);
	size_t cl = strlen(c);
	if (cl > 1 && c[cl - 1] == '!') { bang = 1; c[cl - 1] = '\0'; }
#define IS(full, min) (strlen(c) >= (min) && !strncmp(c, full, strlen(c)))
	if (IS("quit", 1) || IS("qall", 2) || IS("quitall", 5) || IS("xit", 1) || IS("exit", 3) ||
	    !strcmp(c, "wq") || !strcmp(c, "wqa") || !strcmp(c, "xa"))
		return 0;
	if (IS("edit", 1) || IS("view", 3) || IS("vi", 2) || !strcmp(c, "vim")) {
		view_in_editor();
		return 1;
	}
	if (IS("write", 1) || IS("saveas", 3) || IS("update", 2)) {
		if (*arg) write_message_to(arg, bang);
		else save_message(0);
		return 1;
	}
	if (IS("hardcopy", 2)) { save_message(1); return 1; }
	if (IS("help", 1)) { show_help(); return 1; }
	if (IS("nohlsearch", 3)) { A.hls = 0; return 1; }
	if (IS("set", 2) || IS("setlocal", 4)) {
		int wrap = -1;
		if (!strcmp(arg, "wrap")) wrap = 1;
		else if (!strcmp(arg, "nowrap")) wrap = 0;
		else if (!strcmp(arg, "wrap!") || !strcmp(arg, "invwrap")) wrap = A.nowrap;
		if (wrap < 0) {
			set_status("Only :set wrap, :set nowrap, :set wrap! here (not '%.60s')", arg);
			return 1;
		}
		if (wrap == A.nowrap) handle_key_wrap();
		return 1;
	}
	if (!strcmp(c, "$")) {
		A.vtop = A.nlines;                 /* draw_message keeps it in range */
		return 1;
	}
	/* :N, :+N, :-N - a line of the message text (not of the wrapped lines) */
	char *end;
	long v = strtol(c, &end, 10);
	if (*c && !*end && A.mv.open) {
		if (A.hex) {
			A.vtop = *c == '+' ? A.vtop + (int)v : *c == '-' ? A.vtop + (int)v : (int)v - 1;
			return 1;
		}
		build_text(A.lines_width);
		long cur = 1;                      /* text line of the top display line */
		size_t top = A.vtop < A.nlines ? A.lines[A.vtop].off : 0;
		for (size_t i = 0; i < top && i < A.mv.textlen; i++) cur += A.mv.text[i] == '\n';
		long want = *c == '+' || *c == '-' ? cur + v : v;
		if (want < 1) want = 1;
		size_t off = 0;
		for (long l = 1; l < want && off < A.mv.textlen; off++)
			if (A.mv.text[off] == '\n') l++;
		A.vtop = line_at_off(off);
		A.hoff = 0;
		return 1;
	}
#undef IS
	set_status("Unknown :%.60s  (:e vim  :q quit  :w [file]  :N line  :$ end  :/text  :set wrap  :h)", c);
	return 1;
}

/* ------------------------------------------------------------------ */
/* Links (K, Enter or a number in the message window)                  */

typedef struct {
	int   num;
	char *url;
} link_t;

static int add_link(link_t **v, int *n, int num, const char *s, size_t len)
{
	for (int i = 0; i < *n; i++)
		if (strlen((*v)[i].url) == len && !memcmp((*v)[i].url, s, len)) return 0;
	*v = xrealloc(*v, sizeof **v * (size_t)(*n + 1));
	(*v)[(*n)++] = (link_t){num, xstrndup(s, len)};
	return 1;
}

/* An http(s) address written out at t[i]: where it ends, or 0. */
static size_t url_at(const char *t, size_t len, size_t i)
{
	size_t sl = len - i > 8 && !strncasecmp(t + i, "https://", 8) ? 8
	          : len - i > 7 && !strncasecmp(t + i, "http://", 7) ? 7 : 0;
	if (!sl || (i && isalnum((unsigned char)t[i - 1]))) return 0;
	size_t j = i + sl, par = 0;
	while (j < len && (unsigned char)t[j] > ' ' && t[j] != 127 && !strchr("<>\"'`", t[j])) {
		if (t[j] == '(') par++;
		else if (t[j] == ')') {
			if (!par) break;          /* (see https://...) and [text](https://...) */
			par--;
		}
		j++;
	}
	while (j > i + sl && strchr(".,;:!?]*", t[j - 1])) j--;
	return j > i + sl ? j : 0;
}

/* The links in the message text: the numbered ones of the "Links:" lists
 * that HTML and Markdown parts end with ("[n] URL"), then the http(s)
 * addresses written out in the text, numbered after them. */
static int collect_links(const char *t, size_t len, link_t **out)
{
	link_t *v = NULL;
	int n = 0, max = 0, in_list = 0;
	for (size_t i = 0; i < len;) {
		const char *e = memchr(t + i, '\n', len - i);
		size_t ll = e ? (size_t)(e - (t + i)) : len - i;
		const char *l = t + i;
		if (ll == 6 && !memcmp(l, "Links:", 6)) {
			in_list = 1;
		} else if (in_list && ll > 3 && l[0] == '[' && isdigit((unsigned char)l[1])) {
			size_t k = 1;
			int num = 0;
			while (k < ll && isdigit((unsigned char)l[k]) && num < 100000) num = num * 10 + (l[k++] - '0');
			if (k + 1 < ll && l[k] == ']' && l[k + 1] == ' ') {
				k += 2;
				size_t b = ll;
				while (b > k && (l[b - 1] == ' ' || l[b - 1] == '\r')) b--;
				if (b > k) {
					add_link(&v, &n, num, l + k, b - k);
					if (num > max) max = num;
				}
			} else in_list = 0;
		} else in_list = 0;
		i += ll + 1;
	}
	for (size_t i = 0; i + 8 < len; i++) {
		size_t j = url_at(t, len, i);
		if (!j) continue;
		if (add_link(&v, &n, max + 1, t + i, j - i)) max++;
		i = j - 1;
	}
	*out = v;
	return n;
}

/* Open url in the desktop's browser (mailto: its mail program). Only
 * http, https, ftp and mailto: a link in a mail must not run a program or
 * open a local file. */
static void open_link(const char *url)
{
	static const char *const ok[] = {"http://", "https://", "ftp://", "mailto:"};
	int good = 0;
	for (size_t i = 0; i < sizeof ok / sizeof *ok; i++)
		if (!strncasecmp(url, ok[i], strlen(ok[i]))) good = 1;
	buf_t b = {0};
	for (const unsigned char *s = (const unsigned char *)url; *s; s++) {
		if (*s < ' ' || *s == 127) good = 0;
		if (*s == ' ') buf_adds(&b, "%20");
		else buf_addc(&b, (char)*s);
	}
	if (!good) set_status("Not opened: mcm opens only http, https, ftp and mailto links");
	else if (open_with_viewer(b.d) != 0) set_status("Error: cannot open %s", b.d);
	else set_status("Opened %s", b.d);
	buf_free(&b);
}

/* The open message's links in a window: Up/Down or a number chooses one,
 * Enter opens it. key: a digit already typed (or 0). */
void links_window(int key)
{
	if (A.pending_load) load_selected();
	if (!A.mv.open) {
		set_status("%s", "No message is open");
		return;
	}
	if (load_whole_message() != 0) return;
	size_t from = A.mv.hdr_end < A.mv.textlen ? A.mv.hdr_end : 0;
	link_t *v;
	int n = collect_links(A.mv.text + from, A.mv.textlen - from, &v);
	/* the attachments follow the links, numbered on from the last link */
	char **att;
	int na = att_collect(&att);
	if (!n && !na) {
		set_status("%s", "This message has no links or attachments");
		free(v);
		return;
	}
	int top = 0;
	for (int i = 0; i < n; i++)
		if (v[i].num > top) top = v[i].num;
	char **items = xcalloc((size_t)(n + na), sizeof *items);
	int *nums = xcalloc((size_t)(n + na), sizeof *nums);
	for (int i = 0; i < n; i++) {
		buf_t b = {0};
		buf_printf(&b, "[%d] %s", v[i].num, v[i].url);
		items[i] = buf_steal(&b);
		nums[i] = v[i].num;
	}
	for (int i = 0; i < na; i++) {
		buf_t b = {0};
		nums[n + i] = top + 1 + i;
		buf_printf(&b, "[%d] Attachment: %s", nums[n + i], att[i]);
		items[n + i] = buf_steal(&b);
	}
	char title[64];
	if (!na) snprintf(title, sizeof title, "Links (%d)", n);
	else if (!n) snprintf(title, sizeof title, "Attachments (%d)", na);
	else snprintf(title, sizeof title, "Links (%d) and attachments (%d)", n, na);
	set_status("%s", "");
	int r = ui_list_dialog_num(title, (const char *const *)items, nums, n + na, key,
	                           "Up/Down or a number, Enter=Open, Esc=Close", draw_bg);
	if (r >= 0 && r < n) open_link(v[r].url);
	else if (r >= n) att_choose(r - n, att[r - n]);
	for (int i = 0; i < n + na; i++) free(items[i]);
	for (int i = 0; i < n; i++) free(v[i].url);
	for (int i = 0; i < na; i++) free(att[i]);
	free(att);
	att_release();
	free(items);
	free(nums);
	free(v);
}

/* ------------------------------------------------------------------ */
/* The active link: Up / Down walk over the links in the text            */


static int stop_cmp(const void *x, const void *y)
{
	const stop_t *a = x, *b = y;
	return a->off < b->off ? -1 : a->off > b->off;
}

/* Where the links are in the loaded text: the [n] marks of the numbered
 * links (not in the "Links:" lists themselves) and every http(s) address
 * written out, in text order. */
static int collect_stops(stop_t **out)
{
	const char *t = A.mv.text;
	size_t len = A.mv.textlen, from = A.mv.hdr_end < len ? A.mv.hdr_end : 0;
	struct { int num; const char *url; size_t ulen; } *nl = NULL;
	size_t (*sect)[2] = NULL;               /* the Links: lists, [start, end) */
	int nn = 0, ns = 0, in_list = 0;
	for (size_t i = from; i < len;) {
		const char *e = memchr(t + i, '\n', len - i);
		size_t ll = e ? (size_t)(e - (t + i)) : len - i;
		const char *l = t + i;
		if (ll == 6 && !memcmp(l, "Links:", 6)) {
			sect = xrealloc(sect, sizeof *sect * (size_t)(ns + 1));
			sect[ns][0] = i;
			sect[ns++][1] = i + ll + 1;
			in_list = 1;
		} else if (in_list && ll > 3 && l[0] == '[' && isdigit((unsigned char)l[1])) {
			size_t k = 1;
			int num = 0;
			while (k < ll && isdigit((unsigned char)l[k]) && num < 100000) num = num * 10 + (l[k++] - '0');
			size_t b = ll;
			while (b > k + 2 && (l[b - 1] == ' ' || l[b - 1] == '\r')) b--;
			if (k + 2 < b && l[k] == ']' && l[k + 1] == ' ') {
				nl = xrealloc(nl, sizeof *nl * (size_t)(nn + 1));
				nl[nn].num = num;
				nl[nn].url = l + k + 2;
				nl[nn++].ulen = b - k - 2;
				sect[ns - 1][1] = i + ll + 1;
			} else in_list = 0;
		} else in_list = 0;
		i += ll + 1;
	}
	stop_t *v = NULL;
	int n = 0, si = 0;
	for (size_t i = from; i < len; i++) {
		while (si < ns && i >= sect[si][1]) si++;
		int in_sect = si < ns && i >= sect[si][0];
		size_t j = url_at(t, len, i);
		if (j) {
			v = xrealloc(v, sizeof *v * (size_t)(n + 1));
			v[n++] = (stop_t){i, j - i, t + i, j - i, 0};
			i = j - 1;
			continue;
		}
		if (t[i] != '[' || in_sect || i + 2 >= len || !isdigit((unsigned char)t[i + 1])) continue;
		size_t k = i + 1;
		int num = 0;
		while (k < len && isdigit((unsigned char)t[k]) && num < 100000) num = num * 10 + (t[k++] - '0');
		if (k >= len || t[k] != ']') continue;
		for (int q = 0; q < nn; q++)
			if (nl[q].num == num) {
				v = xrealloc(v, sizeof *v * (size_t)(n + 1));
				v[n++] = (stop_t){i, k + 1 - i, nl[q].url, nl[q].ulen, 0};
				break;
			}
		i = k;
	}
	/* "[Attachment: name  (type, size)]" lines, as the text shows them */
	int k = 0;
	for (size_t i = from; i < len;) {
		const char *e = memchr(t + i, '\n', len - i);
		size_t ll = e ? (size_t)(e - (t + i)) : len - i;
		if (ll > 14 && !memcmp(t + i, "[Attachment: ", 13) && t[i + ll - 1] == ']') {
			v = xrealloc(v, sizeof *v * (size_t)(n + 1));
			v[n++] = (stop_t){i, ll, t + i + 13, ll - 14, ++k};
		}
		i += ll + 1;
	}
	free(nl);
	free(sect);
	if (n) qsort(v, (size_t)n, sizeof *v, stop_cmp);
	*out = v;
	return n;
}

void link_clear(void)
{
	if (!A.lk_url) return;
	free(A.lk_url);
	free(A.lk_text);
	A.lk_url = A.lk_text = NULL;
	A.lk_off = A.lk_len = 0;
	A.lk_att = 0;
	A.reindex = 1;
}

static int active_visible(void)
{
	if (!A.lk_url) return 0;
	int li = line_at_off(A.lk_off);
	return li >= A.vtop && li < A.vtop + A.vrows;
}

/* The links of the text as now rendered (collect_stops), kept until the
 * text is rendered again. */
static stop_t *stops;
static int     nstops = -1;

static void stops_reset(void)
{
	free(stops);
	stops = NULL;
	nstops = -1;
}

static int get_stops(stop_t **v)
{
	if (nstops < 0) nstops = A.mv.open && A.mv.text ? collect_stops(&stops) : 0;
	*v = stops;
	return nstops;
}

/* The text on the screen: bytes [*lo, *hi) of A.mv.text. */
static void screen_range(size_t *lo, size_t *hi)
{
	int top = A.vtop < A.nlines ? A.vtop : A.nlines - 1;
	if (top < 0) top = 0;
	int last = top + A.vrows < A.nlines ? top + A.vrows : A.nlines;
	*lo = A.lines[top].off;
	*hi = last < A.nlines ? A.lines[last].off : A.mv.textlen;
}

/* Up (dir -1) / Down (1) in the message window: to the previous / next
 * link while there is one on the screen (it becomes the active one, between
 * > and <), else scroll a line. A link scrolled off the screen stops being
 * the active one. */
void link_step(int dir)
{
	if (A.pending_load) load_selected();
	if (!A.mv.open || A.hex || !A.nlines) {
		A.vtop += dir;
		return;
	}
	build_text(A.lines_width);
	clamp_top(&A.vtop, A.vrows, A.nlines);
	stop_t *v;
	int n = get_stops(&v), pick = -1;
	size_t lo, hi;
	screen_range(&lo, &hi);
	int vis = A.lk_url && A.lk_off >= lo && A.lk_off < hi;
	if (dir > 0) {
		for (int i = 0; i < n && pick < 0; i++)
			if (vis ? v[i].off > A.lk_off : v[i].off >= lo) pick = i;
		if (pick >= 0 && v[pick].off >= hi) pick = -1;
	} else {
		for (int i = n - 1; i >= 0 && pick < 0; i--)
			if (vis ? v[i].off < A.lk_off : v[i].off < hi) pick = i;
		if (pick >= 0 && v[pick].off < lo) pick = -1;
	}
	if (pick >= 0) {
		link_clear();
		A.lk_off = v[pick].off;
		A.lk_len = v[pick].len;
		A.lk_text = xstrndup(A.mv.text + v[pick].off, v[pick].len);
		A.lk_url = xstrndup(v[pick].url, v[pick].ulen);
		A.lk_att = v[pick].att;
		A.reindex = 1;
		if (A.nowrap) A.hoff = 0;
		return;                          /* the status line shows it (screen.c) */
	}
	A.vtop += dir;
	clamp_top(&A.vtop, A.vrows, A.nlines);
	screen_range(&lo, &hi);
	if (A.lk_url && (A.lk_off < lo || A.lk_off >= hi)) link_clear();
}

/* The active link is on the screen (the status line shows it then). */
int link_active_visible(void)
{
	return active_visible();
}

/* Enter in the message window: open the active link, if it is in view. */
int link_open_active(void)
{
	if (!active_visible()) return 0;
	if (A.lk_att) {
		/* the name: what comes before the last "  (" */
		char *nm = xstrdup(A.lk_url), *q = NULL;
		for (char *f = nm; (f = strstr(f, "  (")); f++) q = f;
		if (q) *q = '\0';
		att_open_shown(A.lk_att - 1, nm, A.lk_url);
		free(nm);
	} else {
		open_link(A.lk_url);
	}
	return 1;
}

/* A click in the message text: on a link or an attachment, that one
 * becomes the active one and is opened. 1 = it was on one. */
int msg_click(int x, int y)
{
	stop_t *v;
	int n = get_stops(&v);
	for (int h = 0; h < nhits; h++) {
		if (y != hits[h].y || x < hits[h].x0 || x >= hits[h].x1 || hits[h].stop >= n) continue;
		const stop_t *p = &v[hits[h].stop];
		link_clear();
		A.lk_off = p->off;
		A.lk_len = p->len;
		A.lk_text = xstrndup(A.mv.text + p->off, p->len);
		A.lk_url = xstrndup(p->url, p->ulen);
		A.lk_att = p->att;
		A.reindex = 1;
		build_text(A.lines_width);
		draw();
		link_open_active();
		return 1;
	}
	return 0;
}
