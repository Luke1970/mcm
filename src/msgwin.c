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
	if (!A.text_dirty && A.lines_width == width) return;
	if (A.text_dirty) mv_render(&A.mv, A.show_headers);
	A.text_dirty = 0;
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
	wchar_t *ws;
	size_t n = utf8_to_wcs(A.mv.text + l->off, l->len, &ws);
	wchar_t *e = xmalloc((n * 8 + 1) * sizeof *e);
	size_t k = 0;
	int col = 0;
	for (size_t i = 0; i < n; i++) {
		if (ws[i] == '\t') {
			do { e[k++] = ' '; col++; } while (col % 8);
		} else {
			e[k++] = ws[i];
			col += ui_wcwidth(ws[i]);
		}
	}
	e[k] = 0;
	/* scrolled sideways: leave out the first A.hoff columns (half of a
	 * wide character becomes a space) */
	wchar_t *p = e;
	for (col = 0; *p && col < A.hoff; p++) col += ui_wcwidth(*p);
	if (col > A.hoff) *--p = ' ';
	ui_putw(y, x, p, w, 0, l->off < A.mv.hdr_end ? CP_HEADER : CP_NORMAL);

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
	free(ws);
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
	 * S (save), P (print), T (attachments); then a horizontal scroll bar
	 * for long lines, or the paging hint */
	int by = y + h - 1, bx = x + 14;
	if (w >= 18) {
		const int on[6] = {A.show_headers, !A.nowrap, A.hex, 0, 0, 0};
		for (int i = 0; i < 6; i++)
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
int msg_colon(void)
{
	char cmd[256] = "";
	size_t n = 0;
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
