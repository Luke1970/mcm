/* ui.c - ncurses drawing primitives and dialogs */
#include "ui.h"

#include <ctype.h>
#include "util.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <signal.h>
#include <sys/stat.h>
#ifndef _WIN32
#include <termios.h>
#endif
#include <time.h>
#include <unistd.h>
#ifndef _WIN32
#include <sys/wait.h>
#endif

#define KEY_ESC 27

int ui_key_cleft, ui_key_cright, ui_key_sspace, ui_key_senter;

static void (*quit_fn)(void);

void ui_set_quit(void (*fn)(void)) { quit_fn = fn; }

#define CTRL_Q 17

#ifdef _WIN32
/* PDCurses: keypad keys come as their own codes, Ctrl-C as a key (raw
 * mode), and after a resize the screen size must be read again. */
static int win_key(int ch)
{
	switch (ch) {
	case PADENTER: return '\n';
	case PADSLASH: return '/';
	case PADSTAR: return '*';
	case PADMINUS: return '-';
	case PADPLUS: return '+';
	case KEY_RESIZE: resize_term(0, 0); return ch;
	case 3: quit_requested = 1; return ch;
	}
	return ch;
}
#endif

int ui_getch(void)
{
	if (quit_requested && quit_fn) quit_fn();
	int ch = getch();
#ifdef _WIN32
	ch = win_key(ch);
#endif
	if ((ch == CTRL_Q || quit_requested) && quit_fn) quit_fn();
	return ch;
}

int ui_get_wch(wint_t *wc)
{
	if (quit_requested && quit_fn) quit_fn();
	int r = get_wch(wc);
#ifdef _WIN32
	if (r == KEY_CODE_YES) {
		int k = win_key((int)*wc);
		if (k < KEY_MIN) r = OK;          /* a keypad key that is a character */
		*wc = (wint_t)k;
	} else if (r == OK && *wc == 3) {
		quit_requested = 1;
	}
#endif
	if (((r == OK && *wc == CTRL_Q) || quit_requested) && quit_fn) quit_fn();
	return r;
}

#ifndef _WIN32

/* The terminal settings from before mcm started (for a second Ctrl-C). */
static struct termios shell_tio;
static int have_shell_tio;

/* Ctrl-C: ask to quit. A second one leaves at once, restoring the terminal
 * with what may be done in a signal handler. */
static void on_sigint(int sig)
{
	(void)sig;
	if (quit_requested) {
		static const char reset[] = "\033[?1000l\033[?1002l\033[?1003l\033[?1006l"
		                            "\033[0m\033[?25h\033[?1049l\r\n";
		ssize_t r = write(STDOUT_FILENO, reset, sizeof reset - 1);
		(void)r;
		if (have_shell_tio) tcsetattr(STDIN_FILENO, TCSANOW, &shell_tio);
		_exit(130);
	}
	quit_requested = 1;
}
#endif

/* release: end of a drag; buttons 4 and 5: the wheel */
#define MOUSE_MASK (BUTTON1_PRESSED | BUTTON1_RELEASED | BUTTON4_PRESSED | BUTTON5_PRESSED)

void ui_mouse_drag(int on)
{
	/* xterm 1002: also report movement while a button is held. Only during
	 * a drag, so the rest of the time no movement events arrive. ncurses may
	 * switch on 1003 (all movement) for REPORT_MOUSE_POSITION; afterwards
	 * both are switched off and 1000 (clicks only) is set again. */
	mousemask(on ? MOUSE_MASK | REPORT_MOUSE_POSITION : MOUSE_MASK, NULL);
#ifndef _WIN32
	doupdate();                     /* ncurses' own output first, then ours */
	const char *seq = on ? "\033[?1002h" : "\033[?1003l\033[?1002l\033[?1000h";
	ssize_t r = write(STDOUT_FILENO, seq, strlen(seq));
	(void)r;
#endif
}

#ifdef _WIN32
void ui_init(const theme_t *t)
{
	initscr();
	raw();                    /* Ctrl-C, Ctrl-Q and Ctrl-S are keys */
	PDC_set_title("Mail Commander");
	PDC_return_key_modifiers(FALSE);
	noecho();
	keypad(stdscr, TRUE);
	curs_set(0);
	mousemask(MOUSE_MASK, NULL);
	mouseinterval(0);
	ui_key_cleft = CTL_LEFT;
	ui_key_cright = CTL_RIGHT;
	ui_key_sspace = KEY_MAX + 3;   /* not reported by the Windows console */
	ui_key_senter = KEY_MAX + 4;   /* neither is Shift-Enter */
	/* PDCurses switches the console to VT mode (256 colors) here */
	if (has_colors()) {
		start_color();
		PDC_set_blink(FALSE);
	}
	theme_apply(t);
	bkgdset(' ' | COLOR_PAIR(CP_NORMAL));
}
#else

/* Key code of an extended key (terminfo name such as kLFT5), or define
 * the usual xterm sequence for it. */
static int ext_key(const char *cap, const char *seq, int code)
{
	char *s = tigetstr(cap);
	if (s && s != (char *)-1) {
		int k = key_defined(s);
		if (k > 0) return k;
	}
	int k = key_defined(seq);
	if (k > 0) return k;
	define_key(seq, code);
	return code;
}

/* A terminal that says it does 24-bit color (COLORTERM=truecolor) while TERM
 * is xterm-256color: use the xterm-direct description so #RRGGBB theme colors
 * are sent as such. Anything else: the terminal's own TERM. */
static void start_curses(void)
{
	const char *term = getenv("TERM"), *ct = getenv("COLORTERM");
	if (term && ct && !getenv("MCM_NO_DIRECT") && !getenv("EMSG_NO_DIRECT") &&
	    !strcmp(term, "xterm-256color") &&
	    (!strcmp(ct, "truecolor") || !strcmp(ct, "24bit"))) {
		if (newterm("xterm-direct", stdout, stdin)) return;   /* NULL: not installed */
	}
	initscr();
}

void ui_init(const theme_t *t)
{
	have_shell_tio = tcgetattr(STDIN_FILENO, &shell_tio) == 0;
	start_curses();
	cbreak();
	/* Ctrl-Q and Ctrl-S are keys here, not XON/XOFF flow control
	 * (ncurses restores the terminal's own setting at exit) */
	struct termios tio;
	if (tcgetattr(STDIN_FILENO, &tio) == 0) {
		tio.c_iflag &= ~(tcflag_t)(IXON | IXOFF);
		tcsetattr(STDIN_FILENO, TCSANOW, &tio);
		def_prog_mode();
	}
	/* Ctrl-C interrupts waits (no SA_RESTART) and quits */
	struct sigaction sa;
	memset(&sa, 0, sizeof sa);
	sa.sa_handler = on_sigint;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGINT, &sa, NULL);
	noecho();
	keypad(stdscr, TRUE);
	set_escdelay(25);
	curs_set(0);
	mousemask(MOUSE_MASK, NULL);
	ui_key_cleft = ext_key("kLFT5", "\033[1;5D", KEY_MAX + 1);
	ui_key_cright = ext_key("kRIT5", "\033[1;5C", KEY_MAX + 2);
	/* Shift-, Ctrl- and Alt-Space (all: page up): kitty / fixterms CSI u and
	 * xterm modifyOtherKeys (modifier 2 Shift, 3 Alt, 5 Ctrl); Alt-Space is
	 * otherwise sent as Esc Space. Ctrl-Space usually arrives as NUL (0). */
	ui_key_sspace = KEY_MAX + 3;
	static const char *const sspace[] = {
		"\033[32;2u", "\033[27;2;32~", "\033[32;3u", "\033[27;3;32~",
		"\033[32;5u", "\033[27;5;32~", "\033 ",
	};
	for (size_t i = 0; i < sizeof sspace / sizeof *sspace; i++) define_key(sspace[i], ui_key_sspace);
	/* Shift-Enter: the same two ways (most terminals send a plain Enter) */
	ui_key_senter = KEY_MAX + 4;
	define_key("\033[13;2u", ui_key_senter);
	define_key("\033[27;2;13~", ui_key_senter);
	mouseinterval(0);   /* report presses immediately, no click detection delay */
	theme_apply(t);
	bkgdset(' ' | COLOR_PAIR(CP_NORMAL));
}
#endif

void ui_end(void)
{
#ifndef _WIN32
	/* A terminal without an alternate screen (the Linux console, TERM=linux)
	 * would keep mcm's screen and colors after it ends: clear it in the
	 * terminal's own colors first. (xterm and the like switch back to the
	 * screen they had before.) */
	const char *rmcup = tigetstr("rmcup");
	if (!rmcup || rmcup == (char *)-1 || !*rmcup) {
		attrset(A_NORMAL);
		bkgdset(' ' | A_NORMAL);              /* color pair 0: the terminal's colors */
		clear();
		refresh();
	}
#endif
	curs_set(1);
	endwin();
}

/* Bidi controls that reorder the text after them: the terminal would show
 * "x\u202Efdp.exe" as "xexe.pdf". Shown as a visible mark instead. */
static int bidi_control(wchar_t c)
{
	return (c >= 0x202A && c <= 0x202E) || (c >= 0x2066 && c <= 0x2069);
}

int ui_wcwidth(wchar_t c)
{
	if (c < 32 || c == 0x7F || bidi_control(c)) return 1;
	int w = wcwidth(c);
	return w < 0 ? 1 : w;
}

int ui_putw(int y, int x, const wchar_t *s, int width, attr_t attr, short pair)
{
	if (width <= 0 || y < 0 || y >= LINES) return 0;
	attr_t a = attr | COLOR_PAIR(pair);
	int col = 0;

	attron(a);
	move(y, x);
	for (; s && *s; s++) {
		wchar_t c = *s;
		int cw;
		if (c < 32 || c == 0x7F) { c = ' '; cw = 1; }
		else if (bidi_control(c)) { c = '?'; cw = 1; }
		else {
			cw = wcwidth(c);
			if (cw < 0) { c = '?'; cw = 1; }
		}
		if (col + cw > width) break;
		if (cw == 0 && col == 0) continue;
		addnwstr(&c, 1);
		col += cw;
	}
	for (; col < width; col++) addch(' ');
	attroff(a);
	return col;
}

int ui_puts(int y, int x, const char *s, int width, attr_t attr, short pair)
{
	wchar_t *w;
	utf8_to_wcs(s ? s : "", s ? strlen(s) : 0, &w);
	int r = ui_putw(y, x, w, width, attr, pair);
	free(w);
	return r;
}

void ui_fill(int y, int x, int h, int w, short pair)
{
	for (int r = 0; r < h; r++) ui_puts(y + r, x, "", w, 0, pair);
}

void ui_box(int y, int x, int h, int w, short pair, const char *title, attr_t tattr, short tpair)
{
	if (h < 2 || w < 2) return;
	ui_fill(y, x, h, w, pair);
	attron(COLOR_PAIR(pair));
	mvadd_wch(y, x, WACS_ULCORNER);
	mvhline_set(y, x + 1, WACS_HLINE, w - 2);
	mvadd_wch(y, x + w - 1, WACS_URCORNER);
	mvvline_set(y + 1, x, WACS_VLINE, h - 2);
	mvvline_set(y + 1, x + w - 1, WACS_VLINE, h - 2);
	mvadd_wch(y + h - 1, x, WACS_LLCORNER);
	mvhline_set(y + h - 1, x + 1, WACS_HLINE, w - 2);
	mvadd_wch(y + h - 1, x + w - 1, WACS_LRCORNER);
	attroff(COLOR_PAIR(pair));

	if (title && *title && w > 6) {
		wchar_t *wt;
		size_t n = utf8_to_wcs(title, strlen(title), &wt);
		int tw = 0;
		for (size_t i = 0; i < n; i++) tw += ui_wcwidth(wt[i]);
		if (tw > w - 4) tw = w - 4;
		ui_putw(y, x + 2, wt, tw, tattr, tpair);
		free(wt);
	}
}

void ui_box_hint(int y, int x, int h, int w, const char *hint, short pair)
{
	wchar_t *wt;
	size_t n = utf8_to_wcs(hint, strlen(hint), &wt);
	int tw = 0;
	for (size_t i = 0; i < n; i++) tw += ui_wcwidth(wt[i]);
	if (h >= 2 && tw + 4 <= w) ui_putw(y + h - 1, x + w - 2 - tw, wt, tw, 0, pair);
	free(wt);
}

/* Same layout as ui_menubar: items start at column 1, separated by 3 spaces. */
int ui_menu_hit(const ui_menuitem *items, int n, int x)
{
	int pos = 1;
	for (int i = 0; i < n && pos < COLS; i++) {
		int len = (int)strlen(items[i].label);
		if (pos + len + 2 > COLS) break;
		if (x >= pos - 1 && x <= pos + len) return items[i].key;
		pos += len + 2;
	}
	return 0;
}

void ui_menubar(const ui_menuitem *items, int n, unsigned active)
{
	ui_fill(0, 0, 1, COLS, CP_MENU);
	int x = 1;
	for (int i = 0; i < n && x < COLS; i++) {
		const char *l = items[i].label;
		int len = (int)strlen(l);
		if (x + len + 2 > COLS) break;
		attr_t a = i < 32 && (active >> i & 1u) ? A_REVERSE : 0;
		if (a) ui_puts(0, x - 1, " ", 1, a, CP_MENU);
		ui_puts(0, x, l, len, a, CP_MENU);
		if (a) ui_puts(0, x + len, " ", 1, a, CP_MENU);
		for (int k = 0; k < len; k++)
			if (k == items[i].hot || (items[i].hot == UI_HOT_CAPS && l[k] >= 'A' && l[k] <= 'Z') ||
			    (items[i].hot == UI_HOT_KEYNAME && k < (int)strcspn(l, "="))) {
				char h[2] = {l[k], 0};
				ui_puts(0, x + k, h, 1, a | A_BOLD, CP_HOTKEY);
			}
		x += len + 2;              /* two spaces between the items */
	}

	char clk[64];
	time_t now = time(NULL);
	struct tm tm;
	localtime_r(&now, &tm);
	strftime(clk, sizeof clk, "%a %d %b %Y  %H:%M:%S", &tm);
	int cw = (int)strlen(clk);
	/* x is 2 past the last item: the clock at least 2 columns after it,
	 * one column from the right edge, or against it when that is short */
	int cx = COLS - cw - 1;
	if (cx < x) cx = COLS - cw;
	if (cx >= x) ui_puts(0, cx, clk, cw, 0, CP_MENU);
}

/* ------------------------------------------------------------------ */
/* Dialogs                                                             */

static void dialog_frame(int h, int w, const char *title, int *y0, int *x0, ui_bg_fn bg)
{
	if (bg) bg();
	if (w > COLS) w = COLS;
	if (h > LINES) h = LINES;
	*y0 = (LINES - h) / 2;
	*x0 = (COLS - w) / 2;
	char t[128];
	snprintf(t, sizeof t, " %s ", title);
	ui_box(*y0, *x0, h, w, CP_MENU, t, A_BOLD, CP_MENU);
}

static int is_backspace(int ch) { return ch == KEY_BACKSPACE || ch == 127 || ch == 8; }

/* Remove the last UTF-8 character from s. */
static void utf8_pop(char *s)
{
	size_t n = strlen(s);
	if (!n) return;
	n--;
	while (n && ((unsigned char)s[n] & 0xC0) == 0x80) n--;
	s[n] = '\0';
}

int ui_input_dialog(const char *title, ui_field *f, int nf, const char *msg, ui_bg_fn bg)
{
	int cur = 0;
	for (int i = 0; i < nf; i++)
		if (!*f[i].buf && !f[i].optional) { cur = i; break; }

	int labw = 11;                       /* label column: the longest label */
	for (int i = 0; i < nf; i++)
		if ((int)strlen(f[i].label) > labw) labw = (int)strlen(f[i].label);
	timeout(1000);   /* keep the clock ticking */
	for (;;) {
		int h = nf * 2 + 6, w = 49 + labw, y0, x0;
		dialog_frame(h, w, title, &y0, &x0, bg);
		if (w > COLS) w = COLS;
		int lx = x0 + 3, fx = x0 + 4 + labw, fw = w - 7 - labw, cy = 0, cx = 0;

		for (int i = 0; i < nf; i++) {
			int y = y0 + 2 + i * 2;
			ui_puts(y, lx, f[i].label, labw, 0, CP_MENU);
			const char *shown = f[i].buf;
			char stars[256];
			if (f[i].secret) {
				size_t n = 0;
				for (const char *c = f[i].buf; *c && n < sizeof stars - 1; c++)
					if (((unsigned char)*c & 0xC0) != 0x80) stars[n++] = '*';
				stars[n] = '\0';
				shown = stars;
			}
			/* show the tail if it does not fit */
			wchar_t *ws;
			size_t wn = utf8_to_wcs(shown, strlen(shown), &ws);
			size_t start = 0;
			int tot = 0;
			for (size_t k = 0; k < wn; k++) tot += ui_wcwidth(ws[k]);
			while (tot > fw - 1 && start < wn) tot -= ui_wcwidth(ws[start++]);
			ui_putw(y, fx, ws + start, fw, 0, i == cur ? CP_HIGHLIGHT : CP_NORMAL);
			free(ws);
			if (i == cur) { cy = y; cx = fx + tot; }
		}
		if (msg && *msg) ui_puts(y0 + h - 3, lx, msg, w - 6, A_BOLD, CP_MENU);
		ui_puts(y0 + h - 2, lx, "Tab/↓ next field   Enter OK   Esc cancel", w - 6, 0, CP_MENU);

		move(cy, cx);
		curs_set(1);
		refresh();

		int ch = ui_getch();
		if (ch == ERR) continue;
		msg = NULL;
		if (ch == KEY_ESC) { curs_set(0); return -1; }
		if (ch == KEY_RESIZE) continue;
		if (ch == '\t' || ch == KEY_DOWN) cur = (cur + 1) % nf;
		else if (ch == KEY_BTAB || ch == KEY_UP) cur = (cur + nf - 1) % nf;
		else if (ch == '\n' || ch == '\r' || ch == KEY_ENTER) {
			int empty = -1;
			for (int i = 0; i < nf; i++)
				if (!*f[i].buf && !f[i].optional) { empty = i; break; }
			if (empty < 0) { curs_set(0); return 0; }
			cur = empty;
		} else if (is_backspace(ch)) {
			utf8_pop(f[cur].buf);
		} else if (ch == 21) {               /* ^U clears the field */
			f[cur].buf[0] = '\0';
		} else if (ch >= 32 && ch < 256 && ch != 127) {
			size_t n = strlen(f[cur].buf);
			if (n + 1 < f[cur].size) {
				f[cur].buf[n] = (char)ch;
				f[cur].buf[n + 1] = '\0';
			}
		}
	}
}

void ui_progress(const char *msg, ui_bg_fn bg)
{
	int w = (int)strlen(msg) + 8, y0, x0;
	if (w < 30) w = 30;
	dialog_frame(5, w, "Please wait", &y0, &x0, bg);
	ui_puts(y0 + 2, x0 + 3, msg, w - 6, 0, CP_MENU);
	refresh();
}

static int wait_key(void)
{
	int ch;
	timeout(-1);
	for (;;) {
		ch = ui_getch();
		if (ch == KEY_RESIZE || ch == ERR) continue;
		if (ch == KEY_MOUSE) {                       /* only a click counts, not a release */
			MEVENT ev;
			if (getmouse(&ev) != OK || !(ev.bstate & BUTTON1_PRESSED)) continue;
		}
		return ch;
	}
}

void ui_message(const char *title, const char *msg, ui_bg_fn bg)
{
	int w = (int)strlen(msg) + 8, y0, x0;
	if (w < 34) w = 34;
	if (w > COLS - 2) w = COLS - 2;
	dialog_frame(6, w, title, &y0, &x0, bg);
	ui_puts(y0 + 2, x0 + 3, msg, w - 6, 0, CP_MENU);
	ui_puts(y0 + 3, x0 + 3, "Press any key", w - 6, A_DIM, CP_MENU);
	refresh();
	wait_key();
}

int ui_confirm(const char *title, const char *msg, ui_bg_fn bg)
{
	int w = (int)strlen(msg) + 8, y0, x0;
	if (w < 30) w = 30;
	dialog_frame(6, w, title, &y0, &x0, bg);
	ui_puts(y0 + 2, x0 + 3, msg, w - 6, 0, CP_MENU);
	ui_puts(y0 + 3, x0 + 3, "[Y]es / [N]o", w - 6, A_BOLD, CP_MENU);
	refresh();
	int ch = wait_key();
	return ch == 'y' || ch == 'Y' || ch == '\n' || ch == '\r';
}

/* Scroll bar of the text dialog: in its right border, rows y .. y+h-1. */
typedef struct { int x, y, h, t0, tlen, th, tsize; } dlg_sbar;

static void dlg_scrollbar(dlg_sbar *b, int x, int y, int h, int n, int rows, int top)
{
	b->x = x;
	b->y = y;
	b->h = h;
	b->t0 = y + 1;
	b->tlen = h - 2;
	b->tsize = b->tlen * rows / n;
	if (b->tsize < 1) b->tsize = 1;
	int range = n - rows, free_rows = b->tlen - b->tsize;
	b->th = b->t0 + (range > 0 ? (top * free_rows + range / 2) / range : 0);
	ui_puts(y, x, "▲", 1, 0, CP_MENU);
	ui_puts(y + h - 1, x, "▼", 1, 0, CP_MENU);
	for (int r = 0; r < b->tlen; r++) {
		int on = b->t0 + r >= b->th && b->t0 + r < b->th + b->tsize;
		ui_puts(b->t0 + r, x, on ? "█" : "░", 1, 0, CP_MENU);
	}
}

void ui_text_dialog(const char *title, const char *const *lines, int n, ui_bg_fn bg)
{
	int w = 0, top = 0, grab = -1;   /* grab: dragging the thumb (row offset), -1 not */
	dlg_sbar sb = {0};
	for (int i = 0; i < n; i++) {
		wchar_t *ws;
		size_t wn = utf8_to_wcs(lines[i], strlen(lines[i]), &ws);
		int cw = 0;
		for (size_t k = 0; k < wn; k++) cw += ui_wcwidth(ws[k]);
		free(ws);
		if (cw > w) w = cw;
	}
	w += 6;
	timeout(1000);   /* keep the clock ticking */
	for (;;) {
		/* longer than the screen: it scrolls, with a scroll bar */
		int rows = n, maxrows = LINES - 6;
		if (maxrows < 1) maxrows = 1;
		if (rows > maxrows) rows = maxrows;
		int scroll = rows < n;
		if (top > n - rows) top = n - rows;
		if (top < 0) top = 0;
		int y0, x0;
		dialog_frame(rows + 4, w, title, &y0, &x0, bg);
		for (int i = 0; i < rows; i++)
			ui_puts(y0 + 2 + i, x0 + 3, lines[top + i], w - 6, 0, CP_MENU);
		sb.h = 0;
		if (scroll) {
			char hint[80];
			snprintf(hint, sizeof hint, " %d-%d of %d  ↑↓ PgUp PgDn  Esc close ", top + 1,
			         top + rows, n);
			ui_box_hint(y0, x0, rows + 4, w, hint, CP_MENU);
			if (rows + 2 >= 4) {
				int bw = w > COLS ? COLS : w;
				dlg_scrollbar(&sb, x0 + bw - 1, y0 + 1, rows + 2, n, rows, top);
			}
		}
		refresh();
		int ch = ui_getch();
		if (ch == ERR || ch == KEY_RESIZE) continue;
		int page = rows > 1 ? rows - 1 : 1;
		if (ch == KEY_MOUSE) {
			MEVENT ev;
			if (getmouse(&ev) != OK) continue;
			if (grab >= 0) {                     /* the thumb follows the mouse */
				int fr = sb.tlen - sb.tsize, p = ev.y - grab - sb.t0;
				if (p < 0) p = 0;
				if (p > fr) p = fr;
				top = fr > 0 ? (p * (n - rows) + fr / 2) / fr : 0;
				if (ev.bstate & BUTTON1_RELEASED) {
					grab = -1;
					ui_mouse_drag(0);
				}
				continue;
			}
			if (ev.bstate & BUTTON4_PRESSED) { top -= 3; continue; }
			if (ev.bstate & BUTTON5_PRESSED) { top += 3; continue; }
			if (!(ev.bstate & BUTTON1_PRESSED)) continue;
			if (sb.h && ev.x == sb.x && ev.y >= sb.y && ev.y < sb.y + sb.h) {
				if (ev.y == sb.y) top--;
				else if (ev.y == sb.y + sb.h - 1) top++;
				else if (ev.y < sb.th) top -= page;
				else if (ev.y >= sb.th + sb.tsize) top += page;
				else {
					grab = ev.y - sb.th;
					ui_mouse_drag(1);
				}
				continue;
			}
			break;                               /* a click elsewhere closes it */
		}
		if (!scroll) break;
		if (ch == KEY_UP || ch == 'k') top--;
		else if (ch == KEY_DOWN || ch == 'j') top++;
		else if (ch == KEY_PPAGE || ch == KEY_BACKSPACE || ch == 127) top -= page;
		else if (ch == KEY_NPAGE || ch == ' ') top += page;
		else if (ch == KEY_HOME) top = 0;
		else if (ch == KEY_END) top = n;
		else break;
	}
	if (grab >= 0) ui_mouse_drag(0);
	timeout(-1);
}

int ui_choice_dialog(const char *title, const char *const *lines, int n,
                     const ui_menuitem *btn, int nb, int def, int warn, ui_bg_fn bg)
{
	int sel = def;
	for (;;) {
		int w = 0, bw = 0, y0, x0;
		for (int i = 0; i < n; i++)
			if ((int)strlen(lines[i]) > w) w = (int)strlen(lines[i]);
		for (int i = 0; i < nb; i++) bw += (int)strlen(btn[i].label) + 4 + (i ? 2 : 0);
		if (bw > w) w = bw;
		w += 6;
		if (w > COLS - 2) w = COLS - 2;

		if (bg) bg();
		int h = n + 6;
		if (h > LINES) h = LINES;
		y0 = (LINES - h) / 2;
		x0 = (COLS - w) / 2;
		char t[128];
		snprintf(t, sizeof t, " %s ", title);
		ui_box(y0, x0, h, w, CP_MENU, t, A_BOLD, warn ? CP_HIGHLIGHT : CP_MENU);
		for (int i = 0; i < n && y0 + 2 + i < y0 + h - 3; i++)
			ui_puts(y0 + 2 + i, x0 + 3, lines[i], w - 6, 0, CP_MENU);

		int by = y0 + h - 2, bx = x0 + (w - bw) / 2;
		int bxs[16] = {0};
		for (int i = 0; i < nb && i < 16; i++) {
			const char *l = btn[i].label;
			int len = (int)strlen(l);
			short pair = i == sel ? CP_HIGHLIGHT : CP_MENU;
			bxs[i] = bx;
			ui_puts(by, bx, "[ ", 2, 0, pair);
			ui_puts(by, bx + 2, l, len, 0, pair);
			ui_puts(by, bx + 2 + len, " ]", 2, 0, pair);
			if (btn[i].hot >= 0 && btn[i].hot < len) {
				char hc[2] = {l[btn[i].hot], 0};
				ui_puts(by, bx + 2 + btn[i].hot, hc, 1, A_BOLD,
				        i == sel ? CP_HOTKEY_HL : CP_HOTKEY);
			}
			bx += len + 6;
		}
		move(by, bxs[sel] + 2);
		refresh();

		timeout(-1);
		int ch = ui_getch();
		switch (ch) {
		case KEY_LEFT: case KEY_BTAB: case KEY_UP:
			sel = (sel + nb - 1) % nb;
			break;
		case KEY_RIGHT: case '\t': case KEY_DOWN:
			sel = (sel + 1) % nb;
			break;
		case '\n': case '\r': case KEY_ENTER:
			return sel;
		case 27:
			return -1;
		case KEY_MOUSE: {
			MEVENT ev;
			if (getmouse(&ev) != OK || !(ev.bstate & BUTTON1_PRESSED) || ev.y != by) break;
			for (int i = 0; i < nb && i < 16; i++)
				if (ev.x >= bxs[i] && ev.x < bxs[i] + (int)strlen(btn[i].label) + 4) return i;
			break;
		}
		default:
			for (int i = 0; i < nb; i++)
				if (btn[i].hot >= 0 && ch < 256 && ch > 0 &&
				    tolower(ch) == tolower((unsigned char)btn[i].label[btn[i].hot]))
					return i;
		}
	}
}

static int list_dialog(const char *title, const char *const *items, const int *nums, int n, int def,
                       int key, const char *hint, ui_bg_fn bg);

int ui_list_dialog(const char *title, const char *const *items, int n, int def, const char *hint,
                   ui_bg_fn bg)
{
	return list_dialog(title, items, NULL, n, def, 0, hint, bg);
}

int ui_list_dialog_num(const char *title, const char *const *items, const int *nums, int n,
                       int key, const char *hint, ui_bg_fn bg)
{
	return list_dialog(title, items, nums, n, 0, key, hint, bg);
}

static int list_dialog(const char *title, const char *const *items, const int *nums, int n, int def,
                       int key, const char *hint, ui_bg_fn bg)
{
	int sel = def >= 0 && def < n ? def : 0, top = 0;
	char num[8] = "";                  /* the number being typed */
	for (;;) {
		int w = (int)strlen(hint ? hint : "") + 6;
		for (int i = 0; i < n; i++)
			if ((int)strlen(items[i]) + 10 > w) w = (int)strlen(items[i]) + 10;
		if (w < 40) w = 40;
		if (w > COLS - 2) w = COLS - 2;
		int rows = n, maxrows = LINES - 8;
		if (maxrows < 1) maxrows = 1;
		if (rows > maxrows) rows = maxrows;
		int h = rows + 5, y0, x0;
		dialog_frame(h, w, title, &y0, &x0, bg);
		if (sel < top) top = sel;
		if (sel >= top + rows) top = sel - rows + 1;
		for (int r = 0; r < rows; r++) {
			int i = top + r;
			char line[512];
			snprintf(line, sizeof line, " %s", items[i]);
			ui_puts(y0 + 2 + r, x0 + 3, line, w - 6, 0, i == sel ? CP_HIGHLIGHT : CP_MENU);
		}
		if (hint) ui_puts(y0 + h - 2, x0 + 3, hint, w - 6, A_DIM, CP_MENU);
		if (*num) {
			char t[32];
			snprintf(t, sizeof t, " %s_ ", num);
			ui_puts(y0 + h - 1, x0 + w - 3 - (int)strlen(t), t, (int)strlen(t), A_BOLD, CP_MENU);
		}
		refresh();

		int ch = key;
		key = 0;
		if (!ch) {
			timeout(-1);
			ch = ui_getch();
		}
		if (nums && ch >= '0' && ch <= '9') {
			/* digits: the item with that number; a number no item has
			 * starts again at this digit */
			size_t l = strlen(num);
			if (l + 1 < sizeof num) {
				num[l] = (char)ch;
				num[l + 1] = '\0';
			}
			for (int pass = 0; pass < 2; pass++) {
				int want = atoi(num), hit = -1;
				for (int i = 0; i < n && hit < 0; i++)
					if (nums[i] == want) hit = i;
				if (hit >= 0) {
					sel = hit;
					break;
				}
				num[0] = (char)ch;
				num[1] = '\0';
			}
			continue;
		}
		if (nums && (ch == KEY_BACKSPACE || ch == 127 || ch == 8) && *num) {
			num[strlen(num) - 1] = '\0';
			continue;
		}
		num[0] = '\0';
		switch (ch) {
		case KEY_UP: if (sel > 0) sel--; break;
		case KEY_DOWN: case '\t': if (sel < n - 1) sel++; break;
		case KEY_HOME: case KEY_PPAGE: sel = 0; break;
		case KEY_END: case KEY_NPAGE: sel = n - 1; break;
		case '\n': case '\r': case KEY_ENTER: return sel;
		case 27: return -1;
		case KEY_MOUSE: {
			MEVENT ev;
			if (getmouse(&ev) != OK || !(ev.bstate & BUTTON1_PRESSED)) break;
			int r = ev.y - (y0 + 2);
			if (r >= 0 && r < rows && ev.x >= x0 && ev.x < x0 + w) return top + r;
			break;
		}
		}
	}
}

/* ------------------------------------------------------------------ */
/* File picker                                                         */

typedef struct {
	char  *name;
	int    dir;
	off_t  size;
} fentry;

static int fentry_cmp(const void *a, const void *b)
{
	const fentry *x = a, *y = b;
	if (x->dir != y->dir) return y->dir - x->dir;     /* directories first */
	return strcasecmp(x->name, y->name);
}

static void human(char *o, size_t n, off_t s)
{
	if (s < 1024) snprintf(o, n, "%lld B", (long long)s);
	else if (s < 1024 * 1024) snprintf(o, n, "%.1f KB", s / 1024.0);
	else if (s < 1024LL * 1024 * 1024) snprintf(o, n, "%.1f MB", s / (1024.0 * 1024));
	else snprintf(o, n, "%.1f GB", s / (1024.0 * 1024 * 1024));
}

char *ui_file_dialog(const char *title, char *dir, size_t dirsize, ui_bg_fn bg)
{
	if (!*dir && !getcwd(dir, dirsize)) snprintf(dir, dirsize, "/");
#ifdef _WIN32
	for (char *c = dir; *c; c++)
		if (*c == '\\') *c = '/';
#endif
	for (;;) {
		DIR *d = opendir(dir);
		fentry *e = NULL;
		int n = 0;
		if (d) {
			struct dirent *de;
			while ((de = readdir(d))) {
				if (de->d_name[0] == '.') continue;      /* hidden: type the path */
				char *p;
				if (asprintf(&p, "%s/%s", strcmp(dir, "/") ? dir : "", de->d_name) < 0) continue;
				struct stat st;
				int ok = stat(p, &st) == 0;
				free(p);
				if (!ok || !(S_ISDIR(st.st_mode) || S_ISREG(st.st_mode))) continue;
				e = xrealloc(e, sizeof *e * (size_t)(n + 1));
				e[n++] = (fentry){xstrdup(de->d_name), S_ISDIR(st.st_mode), st.st_size};
			}
			closedir(d);
		}
		if (n) qsort(e, (size_t)n, sizeof *e, fentry_cmp);

		/* items: type a path, .., then the entries */
		int ni = n + 2;
		char **txt = xcalloc((size_t)ni, sizeof *txt);
		txt[0] = xstrdup("[ Type a path ... ]");
		txt[1] = xstrdup("../");
		for (int i = 0; i < n; i++) {
			/* name (folders end in /) cut or padded to NAMEW screen columns, then the size */
			enum { NAMEW = 44 };
			buf_t b = {0};
			wchar_t *w;
			size_t wn = utf8_to_wcs(e[i].name, strlen(e[i].name), &w);
			int col = 0, limit = NAMEW - (e[i].dir ? 1 : 0), total = 0;
			for (size_t k = 0; k < wn; k++) total += ui_wcwidth(w[k]);
			int room = total <= limit ? limit : limit - 1;     /* too long: end in … */
			for (size_t k = 0; k < wn && col + ui_wcwidth(w[k]) <= room; k++) {
				utf8_put(&b, (uint32_t)w[k]);
				col += ui_wcwidth(w[k]);
			}
			if (total > limit) { utf8_put(&b, 0x2026); col++; }
			free(w);
			if (e[i].dir) { buf_addc(&b, '/'); col++; }
			while (col < NAMEW) { buf_addc(&b, ' '); col++; }
			char sz[32] = "";
			if (!e[i].dir) human(sz, sizeof sz, e[i].size);
			buf_printf(&b, " %9s", sz);
			txt[i + 2] = buf_steal(&b);
		}
		char t[300];
		snprintf(t, sizeof t, "%s: %.240s", title, dir);
		int r = ui_list_dialog(t, (const char *const *)txt, ni, n ? 2 : 1,
		                       "Enter: open folder / choose file   Esc: cancel", bg);
		char *pick = NULL;
		int is_dir = 0;
		if (r == 0) {
			char path[1024];
			snprintf(path, sizeof path, "%s/", strcmp(dir, "/") ? dir : "");
			ui_field f[] = {{"Path", path, sizeof path, 0, 0}};
			if (ui_input_dialog(title, f, 1, "A file, or a folder to look in (~ = home)", bg) == 0) {
				const char *home = getenv("HOME");
				if (path[0] == '~' && (path[1] == '/' || !path[1]) && home) {
					if (asprintf(&pick, "%s%s", home, path + 1) < 0) pick = NULL;
				} else {
					pick = xstrdup(path);
				}
				struct stat st;
				if (pick && stat(pick, &st) == 0) is_dir = S_ISDIR(st.st_mode);
				else if (pick) {
					ui_message(title, "No such file or folder", bg);
					free(pick);
					pick = NULL;
				}
			}
		} else if (r == 1) {
			pick = xstrdup(dir);
			char *sl = strrchr(pick, '/');
#ifdef _WIN32
			if (sl && sl == pick + 2 && pick[1] == ':') sl[1] = '\0';   /* C:/x -> C:/ */
			else if (sl && sl[1] && sl != pick) *sl = '\0';
			else if (!sl) snprintf(pick, strlen(pick) + 1, "/");
#else
			if (sl && sl != pick) *sl = '\0';
			else snprintf(pick, strlen(pick) + 1, "/");
#endif
			is_dir = 1;
		} else if (r >= 2) {
			if (asprintf(&pick, "%s/%s", strcmp(dir, "/") ? dir : "", e[r - 2].name) < 0) pick = NULL;
			is_dir = e[r - 2].dir;
		}
		for (int i = 0; i < ni; i++) free(txt[i]);
		free(txt);
		for (int i = 0; i < n; i++) free(e[i].name);
		free(e);
		if (r < 0) return NULL;
		if (!pick) continue;
		char real[4096];
		if (realpath(pick, real)) {
			free(pick);
			pick = xstrdup(real);
		}
		if (!is_dir) {
			/* remember the folder for next time */
			char *sl = strrchr(pick, '/');
			if (sl) snprintf(dir, dirsize, "%.*s", sl == pick ? 1 : (int)(sl - pick), pick);
			return pick;
		}
		snprintf(dir, dirsize, "%s", pick);
		free(pick);
	}
}

char cfg_editor[256];

/* The editor: $VISUAL, $EDITOR, editor in mcm.conf, else vi (Windows:
 * Notepad). */
const char *ui_editor_cmd(void)
{
	const char *ed = getenv("VISUAL");
	if (!ed || !*ed) ed = getenv("EDITOR");
	if (!ed || !*ed) ed = cfg_editor;
#ifdef _WIN32
	if (!*ed) ed = "notepad";
#else
	if (!*ed) ed = "vi";
#endif
	return ed;
}

/* Run the editor (ui_editor_cmd) on the file at path, with curses put
 * aside meanwhile. readonly: vi, vim, nvim and view get -R.
 * Returns the editor's exit status, -1 when it could not run. */
int ui_run_editor(const char *path, int readonly)
{
	const char *ed = ui_editor_cmd();
	/* the program's name: the first word of the command, without its folder */
	const char *name = ed, *sp = ed + strcspn(ed, " \t");
	for (const char *c = ed; c < sp; c++)
		if (*c == '/' || *c == '\\') name = c + 1;
	size_t nl = (size_t)(sp - name);
	int vi = (nl == 2 && !strncmp(name, "vi", 2)) || (nl == 3 && !strncmp(name, "vim", 3)) ||
	         (nl == 4 && (!strncmp(name, "nvim", 4) || !strncmp(name, "view", 4))) ||
	         (nl == 7 && !strncasecmp(name, "vim.exe", 7));
	return ui_run_program(ed, path, readonly && vi ? "-R" : "");
}

/* Run the command cmd (it may have arguments) with flag and path after it,
 * in the terminal, curses put aside meanwhile; waits for it. Returns its
 * exit status, -1 when it could not run. */
/* The sh -c script that runs cmd on the file "$1" ($0 is cmd, $2 flag):
 * cmd may contain arguments, which the shell splits; when it contains %s,
 * it is a shell command line (pipes allowed) with "$1" for each %s. */
void viewer_script(char *out, size_t n, const char *cmd, const char *flag)
{
	(void)flag;
	if (!strstr(cmd, "%s")) {
		snprintf(out, n, "exec $0 $2 \"$1\"");
		return;
	}
	size_t o = 0;
	for (const char *c = cmd; *c && o + 5 < n; c++) {
		if (c[0] == '%' && c[1] == 's') {
			memcpy(out + o, "\"$1\"", 4);
			o += 4;
			c++;
		} else out[o++] = *c;
	}
	out[o] = 0;
}

int ui_run_program(const char *ed, const char *path, const char *flag)
{
	def_prog_mode();
	endwin();
#ifdef _WIN32
	/* $EDITOR may contain arguments: it is the start of the command line */
	char cmd[1200];
	snprintf(cmd, sizeof cmd, "%s %s \"%s\"", ed, flag, path);
	int rc = win_run(cmd);
	reset_prog_mode();
	refresh();
	return rc;
#else
	/* while the editor runs, Ctrl-C is its own, not "quit mcm" */
	struct sigaction ign, old;
	memset(&ign, 0, sizeof ign);
	ign.sa_handler = SIG_IGN;
	sigemptyset(&ign.sa_mask);
	sigaction(SIGINT, &ign, &old);
	pid_t pid = fork();
	if (pid == 0) {
		signal(SIGINT, SIG_DFL);
		char script[1100];
		viewer_script(script, sizeof script, ed, flag);
		execl("/bin/sh", "sh", "-c", script, ed, path, flag, (char *)NULL);
		_exit(127);
	}
	int st = 0;
	if (pid > 0)
		while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
	sigaction(SIGINT, &old, NULL);
	reset_prog_mode();
	refresh();
	return pid < 0 || !WIFEXITED(st) ? -1 : WEXITSTATUS(st);
#endif
}
