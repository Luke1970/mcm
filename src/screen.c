/* screen.c - drawing the main screen: the windows, the column layout and
 * its borders, scroll bars, the status line
 */
#include "mcm.h"

static void side_hint(int y, int x, int h, int w, const char *tab);
static void draw_folders(int x, int y, int w, int h, int focused);
static void draw_accounts(int x, int y, int w, int h, int focused);
static int str_width(const char *s);
static void draw_filter_bar(int y, int x, int w);
static void draw_list(int x, int y, int w, int h, int focused);
static void sb_moved(int col);
static void draw_column(int col, int x, int y, int w, int h);
static void draw_statusbar(void);
static int clampi(int v, int lo, int hi);

/* ------------------------------------------------------------------ */
/* Drawing                                                             */

/* Keep a list's first visible row in range (rows rows, n entries). */
void clamp_top(int *top, int rows, int n)
{
	if (*top > n - rows) *top = n - rows;
	if (*top < 0) *top = 0;
}

void ensure_visible(int *sel, int *top, int rows, int n)
{
	if (n <= 0) { *sel = 0; *top = 0; return; }
	if (*sel >= n) *sel = n - 1;
	if (*sel < 0) *sel = 0;
	if (rows < 1) rows = 1;
	if (*sel < *top) *top = *sel;
	if (*sel >= *top + rows) *top = *sel - rows + 1;
	if (*top > n - rows) *top = n - rows;
	if (*top < 0) *top = 0;
}

/* Bottom border of the folders or accounts window: "Enter=Select  TAB=..."
 * when it fits, else the Tab part alone, else Enter=Select. tab is where
 * Tab | Shift-Tab go (two columns), or the other window (three columns). */
static void side_hint(int y, int x, int h, int w, const char *tab)
{
	if (!tab) {                                  /* all windows shown: no Tab */
		ui_box_hint(y, x, h, w, " Enter=Select ", CP_NORMAL);
		return;
	}
	char full[80], part[64];
	snprintf(full, sizeof full, " Enter=Select  TAB=%s ", tab);
	snprintf(part, sizeof part, " TAB=%s ", tab);
	const char *hint = (int)strlen(full) + 4 <= w ? full : (int)strlen(part) + 4 <= w ? part : " Enter=Select ";
	ui_box_hint(y, x, h, w, hint, CP_NORMAL);
}

static void draw_folders(int x, int y, int w, int h, int focused)
{
	ui_box(y, x, h, w, CP_NORMAL, " Folders ", A_BOLD, focused ? CP_TITLE : CP_NORMAL);
	/* a narrow window shows Enter=Select only (Tab is also on the bottom line) */
	side_hint(y, x, h, w, full() ? NULL : wide() ? "Accounts" : "Account|Email");
	int rows = h - 2;
	A.frows = rows;
	if (A.ffree) clamp_top(&A.ftop, rows, A.nfolders);
	else ensure_visible(&A.fsel, &A.ftop, rows, A.nfolders);
	draw_scrollbar(COL_FOLDERS, x + w - 1, y + 1, rows, A.nfolders, &A.ftop);

	for (int r = 0; r < rows; r++) {
		int i = A.ftop + r;
		if (i >= A.nfolders) break;
		folder_t *f = &A.folders[i];
		int sel = i == A.fsel;
		short pair = sel && focused ? CP_HIGHLIGHT : CP_NORMAL;
		attr_t a = (sel && !focused ? A_REVERSE : 0) | (i == A.cur_folder ? A_BOLD : 0) |
		           (f->noselect ? A_DIM : 0);
		char line[512];
		snprintf(line, sizeof line, "%c%*s%s", i == A.cur_folder ? '>' : ' ',
		         f->depth * 2, "", f->display);
		ui_puts(y + 1 + r, x + 1, line, w - 2, a, pair);
	}
	if (A.nfolders == 0) ui_puts(y + 1, x + 2, "(no folders)", w - 4, A_DIM, CP_NORMAL);
}

/* Window a: every account, the current one marked '>' and bold; the last
 * row makes a new one. Enter opens the selected account (switch_account). */
static void draw_accounts(int x, int y, int w, int h, int focused)
{
	ui_box(y, x, h, w, CP_NORMAL, " Accounts ", A_BOLD, focused ? CP_TITLE : CP_NORMAL);
	side_hint(y, x, h, w, full() ? NULL : wide() ? "Folders" : "Email|Folder");
	int rows = h - 2, n = naccts + 1;
	A.arows = rows;
	if (A.afree) clamp_top(&A.atop, rows, n);
	else ensure_visible(&A.asel, &A.atop, rows, n);
	draw_scrollbar(COL_ACCTS, x + w - 1, y + 1, rows, n, &A.atop);
	int cur = (int)(ac - accts);
	for (int r = 0; r < rows; r++) {
		int i = A.atop + r;
		if (i >= n) break;
		int sel = i == A.asel;
		short pair = sel && focused ? CP_HIGHLIGHT : CP_NORMAL;
		attr_t a = (sel && !focused ? A_REVERSE : 0) | (i == cur ? A_BOLD : 0);
		char line[700];
		if (i == naccts) {
			snprintf(line, sizeof line, " New account ...");
			if (!sel) a |= A_DIM;
		} else {
			const account_t *t = &accts[i];
			char where[600];
			if (t->type == STORE_LOCAL) snprintf(where, sizeof where, "%.500s", t->path);
			else snprintf(where, sizeof where, "%.250s@%.250s%s", t->user, t->host,
			              t->type == STORE_POP3 ? " (POP3)" : "");
			/* the name, and where the mail is when that says something else */
			if (strcmp(t->name, where))
				snprintf(line, sizeof line, "%c%.64s  %s", i == cur ? '>' : ' ', t->name, where);
			else
				snprintf(line, sizeof line, "%c%.64s", i == cur ? '>' : ' ', t->name);
		}
		ui_puts(y + 1 + r, x + 1, line, w - 2, a, pair);
	}
}

static int str_width(const char *s)
{
	wchar_t *w;
	size_t n = utf8_to_wcs(s, strlen(s), &w);
	int tw = 0;
	for (size_t i = 0; i < n; i++) tw += ui_wcwidth(w[i]);
	free(w);
	return tw;
}

/* The filter bar in the bottom border of the email list:
 *   " Filter: text_  Enter=Apply Esc=Leave "   while typing
 *   " Filter: text   L=Edit Esc=Leave "        while it is applied */
static void draw_filter_bar(int y, int x, int w)
{
	const char *keys = A.fedit ? " Enter=Apply  Esc=Leave " : " L=Edit  Esc=Leave ";
	int kw = (int)strlen(keys);
	int avail = w - 10;                        /* right of "─L─O─U─", one line before the corner */
	if (avail < 10) return;
	if (avail - 9 - kw < 8) { keys = ""; kw = 0; }   /* narrow: the text only */
	int fw = avail - 9 - kw - (kw ? 1 : 0);    /* the text field */
	int x0 = x + 8;
	ui_puts(y, x0, " Filter:", 8, A_BOLD, CP_NORMAL);
	ui_puts(y, x0 + 8, " ", 1, 0, CP_NORMAL);
	/* the end of the text that fits (one column for the cursor) */
	const char *t = A.filter;
	while (*t && str_width(t) > fw - 1) {
		t++;
		while ((*t & 0xC0) == 0x80) t++;
	}
	ui_puts(y, x0 + 9, t, fw, A.fedit ? 0 : A_BOLD, CP_HIGHLIGHT);
	A.fcur_x = x0 + 9 + str_width(t);
	A.fesc_x1 = 0;
	if (kw) {
		int kx = x + w - 2 - kw;
		ui_puts(y, kx, keys, kw, 0, CP_NORMAL);
		const char *e = strstr(keys, "Esc");
		A.fesc_x0 = kx + (int)(e - keys);
		A.fesc_x1 = A.fesc_x0 + 9;
		ui_puts(y, A.fesc_x0, "Esc", 3, A_BOLD, CP_HOTKEY);
		const char *k2 = A.fedit ? strstr(keys, "Enter") : strstr(keys, "L=");
		ui_puts(y, kx + (int)(k2 - keys), A.fedit ? "Enter" : "L", A.fedit ? 5 : 1, A_BOLD, CP_HOTKEY);
	}
}

static void draw_list(int x, int y, int w, int h, int focused)
{
	char title[300];
	const char *fname = A.cur_folder >= 0 ? A.folders[A.cur_folder].display : "";
	char picked[32] = "";
	if (pick_count()) snprintf(picked, sizeof picked, ", %d selected", pick_count());
	if (A.fon)
		snprintf(title, sizeof title, " %s filtered  %d of %d msgs%s ", fname, A.nf, A.nmsgs, picked);
	else if (A.unseen >= 0)
		snprintf(title, sizeof title, " %s  %d msgs, %ld new%s ", fname, A.nmsgs, A.unseen, picked);
	else
		snprintf(title, sizeof title, " %s  %d msgs%s ", fname, A.nmsgs, picked);
	ui_box(y, x, h, w, CP_NORMAL, title, A_BOLD, focused ? CP_TITLE : CP_NORMAL);
	A.fbar_y = y + h - 1;
	A.fbar_x = x;
	A.fbar_w = w;
	/* L (filter) and O (the selected messages) at the left of the bottom
	 * border, inverted while the filter is on / messages are selected; U
	 * only while the last copy or move can be undone */
	if (w >= 10) {
		ui_puts(y + h - 1, x + 2, "L", 1, A_BOLD | (A.fedit || A.fon ? A_REVERSE : 0), CP_NORMAL);
		ui_puts(y + h - 1, x + 4, "O", 1, A_BOLD | (pick_count() ? A_REVERSE : 0), CP_NORMAL);
		if (pick_can_undo()) ui_puts(y + h - 1, x + 6, "U", 1, A_BOLD, CP_NORMAL);
	}
	if (A.fedit || A.fon) {
		draw_filter_bar(y + h - 1, x, w);
	} else {
		A.fesc_x1 = 0;
		/* two columns: Tab goes to the folders, Shift-Tab to the accounts */
		const char *hint = wide() ? " PgUp/PgDn " : " TAB=Folder|Accounts  PgUp/PgDn ";
		int tw = (int)strlen(hint);
		if (tw + 6 > w) {
			hint = wide() ? " PgUp/PgDn " : " TAB=Folder|Accounts  PgUp/PgDn ";
			tw = (int)strlen(hint);
		}
		if (tw + 6 > w && !wide()) {
			hint = " TAB=Folder|Accounts ";
			tw = (int)strlen(hint);
		}
		if (tw + 6 <= w) ui_box_hint(y, x, h, w, hint, CP_NORMAL);
	}

	int rows = h - 2, inner = w - 2;
	A.mrows = rows;
	if (A.mfree) clamp_top(&A.mtop, rows, nrows());
	else ensure_visible(&A.msel, &A.mtop, rows, nrows());
	draw_scrollbar(COL_LIST, x + w - 1, y + 1, rows, nrows(), &A.mtop);

	/* columns: flags (N A D or *, @ attachment, + selected) | subject | from |
	 * date (YYMMDD.HH:MM) */
	int datew = 12;
	int fromw = inner >= 70 ? 22 : inner >= 48 ? 16 : 0;
	int subjw = inner - 3 - 1 - datew - (fromw ? fromw + 1 : 0);
	if (subjw < 4) { fromw = 0; subjw = inner - 4 - datew; }

	for (int r = 0; r < rows; r++) {
		int i = A.mtop + r;
		if (i >= nrows()) break;
		msg_t *m = msg_at(i);
		int sel = i == A.msel, yy = y + 1 + r, xx = x + 1;
		short pair = sel && focused ? CP_HIGHLIGHT : CP_NORMAL;

		if (!m || !m->uid) {
			attr_t a = A_DIM | (sel && !focused ? A_REVERSE : 0);
			ui_puts(yy, xx, m ? "  (not available)" : "  …", inner, a, pair);
			continue;
		}
		attr_t a = (!m->seen ? A_BOLD : 0) | (sel && !focused ? A_REVERSE : 0);
		int picked = pick_has(m->uid);         /* selected (Enter): + and the hotkey color */
		if (picked && pair == CP_NORMAL) pair = CP_PICK;
		char flag[4] = {m->deleted ? 'D' : !m->seen ? 'N' : m->answered ? 'A' : m->flagged ? '*' : ' ',
		                m->att ? '@' : ' ', picked ? '+' : ' ', 0};
		char date[32] = "";
		if (m->date) {
			struct tm tm;
			localtime_r(&m->date, &tm);
			strftime(date, sizeof date, "%y%m%d.%H:%M", &tm);
		}
		ui_puts(yy, xx, flag, 3, a, pair);
		xx += 3;
		ui_puts(yy, xx, *m->subject ? m->subject : "(no subject)", subjw, a, pair);
		xx += subjw;
		ui_puts(yy, xx++, "", 1, a, pair);
		if (fromw) {
			ui_puts(yy, xx, m->from, fromw, a, pair);
			xx += fromw;
			ui_puts(yy, xx++, "", 1, a, pair);
		}
		ui_puts(yy, xx, date, datew, a, pair);
	}
	if (nrows() == 0)
		ui_puts(y + 1, x + 2, A.fon ? "(no messages match the filter)" : "(no messages)", w - 4, A_DIM,
		        CP_NORMAL);

	/* ask for headers when rows within half a page of the view are missing */
	for (int i = A.mtop - rows / 2; i < A.mtop + rows + rows / 2; i++)
		if (i >= 0 && i < nrows() && !msg_at(i)) { A.need_list = 1; break; }
}

/* A scroll bar moved window col: lists keep their selection (as the wheel
 * does) until a key or click moves it. */
static void sb_moved(int col)
{
	sbar_t *b = &sbs[col];
	if (*b->top > b->n - b->rows) *b->top = b->n - b->rows;
	if (*b->top < 0) *b->top = 0;
	if (b->horiz) return;
	if (col == COL_LIST) A.mfree = 1;
	else if (col == COL_FOLDERS) A.ffree = 1;
	else if (col == COL_ACCTS) A.afree = 1;
}

/* Scroll bar in the right border of window col (x, rows y .. y+rows-1):
 * ▲, a track with the thumb, ▼; only when n lines do not fit in rows. */
void draw_scrollbar(int col, int x, int y, int rows, int n, int *top)
{
	sbar_t *b = &sbs[col];
	b->h = 0;
	if (n <= rows || rows < 4) return;
	b->horiz = 0;
	b->x = x;
	b->y = y;
	b->h = rows;
	b->n = n;
	b->rows = rows;
	b->top = top;
	b->t0 = y + 1;
	b->tlen = rows - 2;
	b->tsize = b->tlen * rows / n;
	if (b->tsize < 1) b->tsize = 1;
	int range = n - rows, free_rows = b->tlen - b->tsize, t = *top;
	if (t > range) t = range;
	if (t < 0) t = 0;
	b->th = b->t0 + (range > 0 ? (t * free_rows + range / 2) / range : 0);
	ui_puts(y, x, "▲", 1, 0, CP_NORMAL);
	ui_puts(y + rows - 1, x, "▼", 1, 0, CP_NORMAL);
	for (int r = 0; r < b->tlen; r++) {
		int on = b->t0 + r >= b->th && b->t0 + r < b->th + b->tsize;
		ui_puts(b->t0 + r, x, on ? "█" : "░", 1, 0, CP_NORMAL);
	}
}

/* Horizontal scroll bar on row y, columns x .. x+len-1: ◄, track, ►, for
 * n columns of which cols are shown from *left. */
void draw_hscrollbar(int col, int x, int y, int len, int n, int cols, int *left)
{
	sbar_t *b = &sbs[col];
	b->h = 0;
	if (n <= cols || len < 4) return;
	b->horiz = 1;
	b->x = x;
	b->y = y;
	b->h = len;
	b->n = n;
	b->rows = cols;
	b->top = left;
	b->t0 = x + 1;
	b->tlen = len - 2;
	b->tsize = b->tlen * cols / n;
	if (b->tsize < 1) b->tsize = 1;
	int range = n - cols, free_cols = b->tlen - b->tsize, t = *left;
	if (t > range) t = range;
	if (t < 0) t = 0;
	b->th = b->t0 + (range > 0 ? (t * free_cols + range / 2) / range : 0);
	ui_puts(y, x, "◄", 1, 0, CP_NORMAL);
	ui_puts(y, x + len - 1, "►", 1, 0, CP_NORMAL);
	for (int c = 0; c < b->tlen; c++) {
		int on = b->t0 + c >= b->th && b->t0 + c < b->th + b->tsize;
		ui_puts(y, b->t0 + c, on ? "█" : "░", 1, 0, CP_NORMAL);
	}
}

/* Scroll the dragged window so that the thumb's top (or left end, for a
 * horizontal bar) is at screen row (column) `row`. */
void scroll_thumb_to(int row)
{
	sbar_t *b = &sbs[sb_drag];
	int free_rows = b->tlen - b->tsize, range = b->n - b->rows;
	int p = row - b->t0;
	if (p < 0) p = 0;
	if (p > free_rows) p = free_rows;
	*b->top = free_rows > 0 ? (p * range + free_rows / 2) / free_rows : 0;
	sb_moved(sb_drag);
}

/* A press on a scroll bar: arrows scroll a line, the track a page, the thumb
 * starts a drag. Returns 0 when (x, y) is not on a scroll bar. */
int scrollbar_press(int x, int y)
{
	for (int col = 0; col < NSBS; col++) {
		sbar_t *b = &sbs[col];
		/* along: the position on the bar; across: the bar's own row/column */
		int along = b->horiz ? x : y, start = b->horiz ? b->x : b->y;
		if (!b->h || (b->horiz ? y != b->y : x != b->x) || along < start || along >= start + b->h)
			continue;
		int page = b->rows > 1 ? b->rows - 1 : 1, step = b->horiz ? HSTEP : 1;
		if (along == start) *b->top -= step;
		else if (along == start + b->h - 1) *b->top += step;
		else if (along < b->th) *b->top -= page;
		else if (along >= b->th + b->tsize) *b->top += page;
		else {
			b->grab = along - b->th;
			sb_drag = col;
			dragging = DRAG_THUMB;
			ui_mouse_drag(1);
		}
		sb_moved(col);
		return 1;
	}
	return 0;
}

static void draw_column(int col, int x, int y, int w, int h)
{
	int focused = A.focus == col;
	if (col == COL_FOLDERS) draw_folders(x, y, w, h, focused);
	else if (col == COL_ACCTS) draw_accounts(x, y, w, h, focused);
	else if (col == COL_LIST) draw_list(x, y, w, h, focused);
	else draw_message(x, y, w, h, focused);
}

static void draw_statusbar(void)
{
	int y = LINES - 1;
	ui_fill(y, 0, 1, COLS, CP_MENU);
	char left[600];
	char where[520];
	if (ac->type == STORE_LOCAL) snprintf(where, sizeof where, "%.500s", ac->path);
	else snprintf(where, sizeof where, "%.250s@%.250s", ac->user, ac->host);
	if (naccts > 1 && strcmp(ac->name, where))     /* the name, unless it says the same */
		snprintf(left, sizeof left, " %.60s: %s ", ac->name, where);
	else snprintf(left, sizeof left, " %s ", where);
	int lw = (int)strlen(left);
	if (lw > COLS / 2) lw = COLS / 2;
	ui_puts(y, 0, left, lw, A_BOLD, CP_MENU);

	/* the message window has the focus: the active link's address, or its
	 * vim-like ':' commands */
	char lk[600];
	int lkv = A.focus == COL_MSG && link_active_visible();
	if (lkv && A.lk_att) snprintf(lk, sizeof lk, "Attachment: %s   Enter=Open/Save  Esc=Leave", A.lk_url);
	else if (lkv) snprintf(lk, sizeof lk, "Link: %s   Enter=Open  Esc=Leave", A.lk_url);
	const char *msg = *A.status ? A.status
	                  : lkv ? lk
	                  : A.focus == COL_MSG ? ":e Vim  :/text Search  :N Line  :w Save  :q Quit"
	                  : A.focus == COL_LIST ? "(Shift)Enter=Select message(s)  F5 Refresh  Q/^Q Quit"
	                  : "Enter Open  F5 Refresh  Q/^Q Quit";
	ui_puts(y, lw + 1, msg, COLS - lw - 1, 0, CP_MENU);
}

/* More columns than this: Folders | List | Message side by side (no Tab). */
#define WIDE_COLS 150

int wide(void) { return COLS > WIDE_COLS; }

/* More columns than this: Accounts | Folders | List | Message (no Tab). */
#define FULL_COLS 170

int full(void) { return COLS > FULL_COLS; }

/* The visible columns, left to right: what each shows, where and how wide.
 * Two columns: the email list, folders or accounts (Tab), then the message.
 * Wide: Folders (or Accounts) | List | Message. Full: Accounts | Folders |
 * List | Message. col_pct[0] is the folder column's width in % of the
 * screen; col_pct[1] the list's: of the screen in two columns, of what the
 * windows left of it leave otherwise; col_pct[2] the accounts' (full). */

void get_cols(cols_t *c)
{
	memset(c, 0, sizeof *c);
	if (full()) {
		int aw = clampi(COLS * col_pct[2] / 100, MIN_LEFT, COLS - 3 * MIN_LEFT - 20);
		int fw = clampi(COLS * col_pct[0] / 100, MIN_LEFT, COLS - aw - 2 * MIN_LEFT - 20);
		int lw = clampi((COLS - aw - fw) * col_pct[1] / 100, MIN_LEFT, COLS - aw - fw - 20);
		c->n = 4;
		c->col[0] = COL_ACCTS;   c->x[0] = 0;            c->w[0] = aw;
		c->col[1] = COL_FOLDERS; c->x[1] = aw;           c->w[1] = fw;
		c->col[2] = COL_LIST;    c->x[2] = aw + fw;      c->w[2] = lw;
		c->col[3] = COL_MSG;     c->x[3] = aw + fw + lw; c->w[3] = COLS - aw - fw - lw;
	} else if (wide()) {
		int fw = clampi(COLS * col_pct[0] / 100, MIN_LEFT, COLS - 2 * MIN_LEFT - 20);
		int lw = clampi((COLS - fw) * col_pct[1] / 100, MIN_LEFT, COLS - fw - 20);
		c->n = 3;
		c->col[0] = A.side;      c->x[0] = 0;       c->w[0] = fw;
		c->col[1] = COL_LIST;    c->x[1] = fw;      c->w[1] = lw;
		c->col[2] = COL_MSG;     c->x[2] = fw + lw; c->w[2] = COLS - fw - lw;
	} else {
		int hi = COLS - 20 > MIN_LEFT ? COLS - 20 : MIN_LEFT;
		int lw = clampi(COLS * col_pct[A.left == COL_LIST] / 100, MIN_LEFT, hi);
		c->n = 2;
		c->col[0] = A.left;  c->x[0] = 0;  c->w[0] = lw;
		c->col[1] = COL_MSG; c->x[1] = lw; c->w[1] = COLS - lw;
	}
}

/* The width setting that border b (the left edge of visible column b) uses. */
int *border_pct(int b)
{
	if (full()) return &col_pct[b == 1 ? 2 : b == 2 ? 0 : 1];
	if (wide()) return &col_pct[b == 2];
	return &col_pct[A.left == COL_LIST];
}

/* Put border b at screen column x (the first column right of it). */
void set_border(int b, int x)
{
	cols_t c;
	get_cols(&c);
	if (b == c.n - 1 && c.n > 2) {             /* list | message: % of what is left */
		int l = c.x[b - 1], rest = COLS - l;
		col_pct[1] = clampi(((x - l) * 100 + rest / 2) / rest, 5, 95);
	} else if (full() && b == 2) {              /* accounts | folders | : folders' width */
		col_pct[0] = clampi(((x - c.x[1]) * 100 + COLS / 2) / COLS, 5, 95);
	} else {
		*border_pct(b) = clampi((x * 100 + COLS / 2) / COLS, 5, 95);
	}
}

/* The border at screen column x (either side of the line), or 0. */
int border_at(int x)
{
	cols_t c;
	get_cols(&c);
	for (int b = 1; b < c.n; b++)
		if (x == c.x[b] - 1 || x == c.x[b]) return b;
	return 0;
}

/* Paint the main screen into stdscr only; the caller refreshes, so a menu or
 * dialog on top goes out together with it and never flickers. */
static void paint(void)
{
	A.need_list = A.need_more = A.need_hex = 0;
	for (int i = 0; i < NSBS; i++) sbs[i].h = 0;   /* set again by windows that need one */
	A.hwx_x = 0;
	bkgdset(' ' | COLOR_PAIR(CP_NORMAL));
	erase();
	ui_menubar(folder_menu, nfolder_menu, 0);

	int top = 1, h = LINES - 2;
	if (h >= 3 && COLS >= 20) {
		cols_t c;
		get_cols(&c);
		for (int i = 0; i < c.n; i++) draw_column(c.col[i], c.x[i], top, c.w[i], h);
		for (int i = 1; i < c.n; i++)
			if (c.col[i - 1] == COL_LIST && c.col[i] == COL_MSG) {
				/* "<>" in the middle of the border between list and message */
				if (!sbs[COL_LIST].h)          /* the list's scroll bar is there */
					ui_puts(top + h / 2, c.x[i] - 1, "<", 1, A_BOLD, CP_NORMAL);
				ui_puts(top + h / 2, c.x[i], ">", 1, A_BOLD, CP_NORMAL);
			}
	}
	draw_statusbar();
}

void draw(void)
{
	paint();
	refresh();
}

void draw_bg(void) { paint(); }

void login_bg(void)
{
	bkgdset(' ' | COLOR_PAIR(CP_NORMAL));
	erase();
	ui_menubar(NULL, 0, 0);
	ui_puts(0, 1, "mcm " MCM_VERSION, 12, A_BOLD, CP_MENU);
	ui_fill(LINES - 1, 0, 1, COLS, CP_MENU);
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
