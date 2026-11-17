/* keys.c - what a key does on the main screen, a click in a window, and
 * the help window
 */
#include "mcm.h"
#include "helptext.h"   /* the help texts, from the files in help/ */

void show_help(void)
{
	int n = 0;
	while (HELP_KEYS[n]) n++;
	char **l = xcalloc((size_t)n + 1, sizeof *l);
	for (int i = 0; i < n; i++) {
		buf_t b = {0};
		const char *s = HELP_KEYS[i], *v;
		while ((v = strstr(s, "@VERSION@"))) {
			buf_add(&b, s, (size_t)(v - s));
			buf_adds(&b, MCM_VERSION);
			s = v + 9;
		}
		buf_adds(&b, s);
		l[i] = buf_steal(&b);
	}
	ui_text_dialog("Help", (const char *const *)l, n, draw_bg);
	for (int i = 0; i < n; i++) free(l[i]);
	free(l);
}

void focus_to(int col)
{
	if (col < COL_FOLDERS || col > COL_ACCTS) col = COL_LIST;
	if (col == COL_ACCTS && A.focus != COL_ACCTS) A.asel = (int)(ac - accts);   /* start on this one */
	A.focus = col;
	if (col != COL_MSG) A.left = col;       /* two columns: show it on the left */
	if (col == COL_FOLDERS || col == COL_ACCTS) A.side = col;   /* three columns: the first */
	if (col == COL_MSG && A.pending_load) load_selected();
}

/* Mouse click at x, y inside the windows (below the menu bar, above the
 * bottom borders): select the email or folder under it and give that window
 * the focus; a second click on the same row within 400 ms opens it (as Enter). */
void click_window(const cols_t *cl, int x, int y)
{
	static struct timespec last;
	static int last_col = -1, last_idx = -1;
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	long ms = (now.tv_sec - last.tv_sec) * 1000L + (now.tv_nsec - last.tv_nsec) / 1000000L;

	for (int k = 0; k < cl->n; k++) {
		if (x <= cl->x[k] || x >= cl->x[k] + cl->w[k] - 1) continue;   /* not inside its borders */
		int col = cl->col[k], idx = -1, r = y - 2;
		if (col == COL_LIST && A.mtop + r < nrows()) {
			idx = A.mtop + r;
			focus_to(COL_LIST);
			select_msg(idx);
		} else if (col == COL_FOLDERS && A.ftop + r < A.nfolders) {
			idx = A.ftop + r;
			focus_to(COL_FOLDERS);
			A.fsel = idx;
			A.ffree = 0;
		} else if (col == COL_ACCTS && A.atop + r <= naccts) {
			idx = A.atop + r;
			focus_to(COL_ACCTS);
			A.asel = idx;
		} else if (col == COL_MSG) {
			focus_to(COL_MSG);
		}
		if (idx >= 0 && col == last_col && idx == last_idx && ms >= 0 && ms < 400) {
			if (col == COL_LIST) focus_to(COL_MSG);              /* double click: open */
			else if (col == COL_ACCTS) account_open(idx);
			else if (!A.folders[idx].noselect) {
				if (idx != A.cur_folder) open_folder(idx);
				if (A.cur_folder == idx) focus_to(COL_LIST);
			}
			idx = -1;                                            /* a third click starts over */
		}
		last = now;
		last_col = col;
		last_idx = idx;
		return;
	}
}

/* Returns 0 to quit. */
int handle_key(int ch)
{
	set_status("%s", "");
	/* a key brings lists the wheel scrolled back to their selection */
	if (ch != KEY_RESIZE) A.mfree = A.ffree = A.afree = 0;

	switch (ch) {
	case 'q': case 'Q':
		return 0;
	case KEY_F(1): case '?':
		show_help();
		return 1;
	case '\t': case KEY_BTAB:
		/* three columns: the first one switches between folders and accounts;
		 * two columns: the left one goes emails -> folders -> accounts ->
		 * emails (Shift-Tab the other way) */
		if (full()) return 1;                    /* all four windows are shown */
		if (wide()) focus_to(A.side == COL_FOLDERS ? COL_ACCTS : COL_FOLDERS);
		else if (ch == '\t')
			focus_to(A.left == COL_LIST ? COL_FOLDERS : A.left == COL_FOLDERS ? COL_ACCTS : COL_LIST);
		else
			focus_to(A.left == COL_LIST ? COL_ACCTS : A.left == COL_ACCTS ? COL_FOLDERS : COL_LIST);
		return 1;
	case 12:   /* ^L */
		clearok(curscr, TRUE);
		return 1;
	case KEY_RESIZE:
		return 1;
	case KEY_F(5):
		refresh_folder(0);
		return 1;
	case 'h': case 'H':
		A.show_headers = !A.show_headers;
		A.hex = 0;
		A.vtop = 0;
		A.text_dirty = 1;
		return 1;
	case 'x': case 'X':
		A.hex = !A.hex;
		A.vtop = 0;
		return 1;
	case 'w': case 'W':                 /* wrap the message's lines, or not */
		handle_key_wrap();
		return 1;
	case ':':
		if (A.focus == COL_MSG) return msg_colon();
		return 1;
	case 's': case 'S': save_message(0); return 1;
	case 'p': case 'P': save_message(1); return 1;
	case 't': case 'T': attachments(); return 1;
	case 'm': case 'M': compose_start('m'); return 1;
	case 'r': case 'R': compose_start('r'); return 1;
	case 'f': case 'F': compose_start('f'); return 1;
	case 'c': case 'C': config_menu(); return 1;
	case 'a': case 'A': account_menu(); return 1;
	case 'l': case 'L':
		if (A.cur_folder >= 0) filter_edit();
		return 1;
	}
	if (ch == ui_key_cleft || ch == ui_key_cright) {   /* Ctrl-arrows: column border */
		/* the border right of the folders, or the one left of the message */
		int b = full() ? (A.focus == COL_ACCTS ? 1 : A.focus == COL_FOLDERS ? 2 : 3)
		      : wide() && (A.focus == COL_FOLDERS || A.focus == COL_ACCTS) ? 1 : wide() ? 2 : 1;
		cols_t c;
		get_cols(&c);
		int *pct = border_pct(b), before = *pct, step = COLS / 50 > 1 ? COLS / 50 : 1;
		for (int k = step; *pct == before && k <= 4 * step + 4; k++)   /* at least 1% */
			set_border(b, c.x[b] + (ch == ui_key_cright ? k : -k));
		A.text_dirty = 1;
		config_save();
		return 1;
	}

	/* Space pages the message down from every window; Shift-, Ctrl- and
	 * Alt-Space and Backspace page it up, and B in the message window.
	 * (Many terminals send Shift-Space as a plain space; Ctrl-Space arrives
	 * as NUL, Alt-Space as Esc Space.) */
	if (ch == ' ' || ch == ui_key_sspace || ch == 0 || ch == KEY_BACKSPACE || ch == 127 || ch == 8 ||
	    (A.focus == COL_MSG && (ch == 'b' || ch == 'B'))) {
		int page = A.vrows > 1 ? A.vrows - 1 : 1;
		A.vtop += ch == ' ' ? page : -page;
		return 1;
	}

	if (A.focus == COL_ACCTS) {
		switch (ch) {
		case KEY_UP: A.asel--; break;
		case KEY_DOWN: A.asel++; break;
		case KEY_HOME: case KEY_PPAGE: A.asel = 0; break;
		case KEY_END: case KEY_NPAGE: A.asel = naccts; break;
		case '\n': case '\r': case KEY_ENTER: account_open(A.asel); break;
		case KEY_RIGHT: focus_to(COL_FOLDERS); break;   /* back: a -> f -> l -> e */
		case 27: focus_to(COL_LIST); break;
		}
		ensure_visible(&A.asel, &A.atop, A.arows, naccts + 1);
	} else if (A.focus == COL_FOLDERS) {
		switch (ch) {
		case KEY_UP: A.fsel--; break;
		case KEY_DOWN: A.fsel++; break;
		case KEY_HOME: A.fsel = 0; break;
		case KEY_END: A.fsel = A.nfolders - 1; break;
		case '\n': case '\r': case KEY_ENTER: case KEY_RIGHT:
			if (A.fsel != A.cur_folder) open_folder(A.fsel);
			if (A.cur_folder == A.fsel) focus_to(COL_LIST);
			break;
		case KEY_LEFT: focus_to(COL_ACCTS); break;      /* e -> l -> f -> a */
		}
		ensure_visible(&A.fsel, &A.ftop, A.frows, A.nfolders);
	} else if (A.focus == COL_LIST) {
		int page = A.mrows > 1 ? A.mrows - 1 : 1;
		switch (ch) {
		case KEY_UP: select_msg(A.msel - 1); break;
		case KEY_DOWN: select_msg(A.msel + 1); break;
		case KEY_PPAGE: select_msg(A.msel - page); break;
		case KEY_NPAGE: select_msg(A.msel + page); break;
		case KEY_HOME: select_msg(0); break;
		case KEY_END: select_msg(nrows() - 1); break;
		case '\n': case '\r': case KEY_ENTER: case KEY_RIGHT:
			if (nrows()) focus_to(COL_MSG);
			break;
		case 27: if (A.fon) filter_leave(); break;
		case KEY_LEFT: focus_to(wide() && !full() ? A.side : COL_FOLDERS); break;
		}
	} else {
		switch (ch) {
		case KEY_UP: A.vtop--; break;
		case KEY_DOWN: A.vtop++; break;
		case KEY_HOME: A.vtop = 0; break;
		case KEY_END: A.vtop = A.nlines; break;
		/* not wrapped: Left and Right move the text sideways; Left at the
		 * start goes to the window on the left */
		case KEY_RIGHT: if (A.nowrap && !A.hex) A.hoff += HSTEP; break;
		case KEY_LEFT:
			if (A.nowrap && !A.hex && A.hoff > 0) A.hoff = A.hoff > HSTEP ? A.hoff - HSTEP : 0;
			else focus_to(wide() ? COL_LIST : A.left);
			break;
		case 27: focus_to(COL_LIST); break;
		}
	}
	return 1;
}
