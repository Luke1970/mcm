/* menu.c - the menu window (F1 or F10): every command of mcm in six columns
 *
 *   MCM    Account  Folders  Emaillist  Email   Help
 *
 * Left/Right (or Tab) choose the column, Up/Down the item, Enter or the
 * item's highlighted letter (from any column) runs it, Esc (or F1, F10) closes the window. A
 * click on an item runs it, a click elsewhere closes the window. Under the
 * columns: keys that work everywhere.
 */
#include "mcm.h"

/* What an item does: a key of the main screen, or one of these. */
enum { DO_ABOUT = 0x10000, DO_ACC_SELECT, DO_ACC_ADD, DO_ACC_EDIT, DO_ACC_REMOVE, DO_FLD_SELECT,
       DO_FLD_CREATE, DO_FLD_DELETE, DO_FLD_RENAME, DO_FLD_MOVE, DO_ACC_EXPORT, DO_ACC_IMPORT, DO_FIND, DO_KEYS, DO_CMDLINE };

typedef struct {
	const char *label;
	int         hot;    /* index of the highlighted letter, -1 none */
	int         act;
} mitem;

/* alphabetical in each column (case aside). The highlighted letter (a
 * capital, or '/') runs the item from any column, so each one is used only
 * once; items in lower case have none. */
static const mitem M_MCM[] = {
	{"Config", 0, 'c'}, {"Quit", 0, 'q'},
};
static const mitem M_ACC[] = {
	{"Add", 0, DO_ACC_ADD}, {"edIt", 2, DO_ACC_EDIT}, {"export", -1, DO_ACC_EXPORT},
	{"import", -1, DO_ACC_IMPORT}, {"rEmove", 1, DO_ACC_REMOVE}, {"select", -1, DO_ACC_SELECT},
};
static const mitem M_FLD[] = {
	{"creaTe", 4, DO_FLD_CREATE}, {"Delete", 0, DO_FLD_DELETE}, {"moVe", 2, DO_FLD_MOVE},
	{"reName", 2, DO_FLD_RENAME}, {"select", -1, DO_FLD_SELECT},
};
static const mitem M_LIST[] = {
	{"fiLter", 2, 'l'}, {"Other", 0, 'o'},
};
static const mitem M_MSG[] = {
	{"/ find", 0, DO_FIND}, {"edit draft", -1, 'e'}, {"Fwd", 0, 'f'}, {"Header", 0, 'h'}, {"heX", 2, 'x'}, {"Mail", 0, 'm'},
	{"Print", 0, 'p'}, {"Reply", 0, 'r'}, {"Save", 0, 's'}, {"Wrap", 0, 'w'},
};
static const mitem M_HELP[] = {
	{"about", -1, DO_ABOUT}, {"command line", -1, DO_CMDLINE}, {"key list", -1, DO_KEYS},
};

#define N(a) (int)(sizeof a / sizeof *a)
static const struct {
	const char  *title;
	const mitem *it;
	int          n;
} COLS_[] = {
	{"MCM", M_MCM, N(M_MCM)},       {"Account", M_ACC, N(M_ACC)},  {"Folders", M_FLD, N(M_FLD)},
	{"Emaillist", M_LIST, N(M_LIST)}, {"Email", M_MSG, N(M_MSG)}, {"Help", M_HELP, N(M_HELP)},
};
#define NCOL N(COLS_)

static const char *const NOTES[] = {
	":e         opens email with default editor",
	"^Q         global quit",
	"^C         abort program",
	"<- ->      change active window",
	"Up Dn      scroll through active window",
};
#define NNOTES N(NOTES)

static int maxrows(void)
{
	int m = 0;
	for (int c = 0; c < NCOL; c++)
		if (COLS_[c].n > m) m = COLS_[c].n;
	return m;
}

static int colw(int c)
{
	int w = (int)strlen(COLS_[c].title);
	for (int i = 0; i < COLS_[c].n; i++)
		if ((int)strlen(COLS_[c].it[i].label) > w) w = (int)strlen(COLS_[c].it[i].label);
	return w;
}

/* Where things are, for drawing and for clicks. */
typedef struct {
	int y, x, h, w;
	int first;           /* the first column shown (narrow screens) */
	int cx[NCOL];        /* screen column of each column, -1 not shown */
	int notes;           /* the notes are shown */
} layout_t;

static void place(layout_t *L, int cur)
{
	int rows = maxrows(), need = 2;
	for (int c = 0; c < NCOL; c++) need += colw(c) + 3;
	for (int i = 0; i < NNOTES; i++)
		if ((int)strlen(NOTES[i]) + 4 > need) need = (int)strlen(NOTES[i]) + 4;
	L->y = 1;
	L->x = 0;
	L->w = need + 1 < COLS ? need + 1 : COLS;
	L->notes = LINES - 2 >= rows + 5 + NNOTES;   /* the status line stays */
	L->h = rows + 4 + (L->notes ? NNOTES + 1 : 0);
	if (L->h > LINES - 2) L->h = LINES - 2;
	/* too narrow for all columns: start at a column that keeps cur in view */
	for (L->first = 0; L->first < cur; L->first++) {
		int x = 2;
		for (int c = L->first; c <= cur; c++) x += colw(c) + 3;
		if (x <= L->w - 1) break;
	}
	int x = L->x + 2;
	for (int c = 0; c < NCOL; c++) {
		L->cx[c] = -1;
		if (c < L->first || x + colw(c) > L->x + L->w - 1) continue;
		L->cx[c] = x;
		x += colw(c) + 3;
	}
}

static void draw_menu(const layout_t *L, int cur, int sel)
{
	draw_bg();                              /* no refresh: no flicker */
	ui_fill(L->y, L->x, L->h, L->w, CP_MENU);
	ui_box(L->y, L->x, L->h, L->w, CP_MENU, " Menu ", A_BOLD, CP_MENU);
	int rows = maxrows();
	for (int c = 0; c < NCOL; c++) {
		int x = L->cx[c], w = colw(c);
		if (x < 0) continue;
		ui_puts(L->y + 1, x, COLS_[c].title, w, A_BOLD, c == cur ? CP_HIGHLIGHT : CP_MENU);
		attron(COLOR_PAIR(CP_MENU));
		mvhline_set(L->y + 2, x, WACS_HLINE, (int)strlen(COLS_[c].title));
		attroff(COLOR_PAIR(CP_MENU));
		for (int i = 0; i < COLS_[c].n && 3 + i < L->h - 1; i++) {
			const mitem *m = &COLS_[c].it[i];
			int on = c == cur && i == sel, y = L->y + 3 + i;
			/* the bar one column wider on both sides */
			ui_puts(y, x - 1, " ", 1, 0, on ? CP_HIGHLIGHT : CP_MENU);
			ui_puts(y, x, m->label, w + 1, 0, on ? CP_HIGHLIGHT : CP_MENU);
			if (m->hot >= 0) {
				char h[2] = {m->label[m->hot], 0};
				ui_puts(y, x + m->hot, h, 1, A_BOLD, on ? CP_HOTKEY_HL : CP_HOTKEY);
			}
		}
	}
	if (L->notes) {
		int y = L->y + 3 + rows;
		attron(COLOR_PAIR(CP_MENU));
		mvadd_wch(y, L->x, WACS_LTEE);
		mvhline_set(y, L->x + 1, WACS_HLINE, L->w - 2);
		mvadd_wch(y, L->x + L->w - 1, WACS_RTEE);
		attroff(COLOR_PAIR(CP_MENU));
		for (int i = 0; i < NNOTES && y + 1 + i < L->y + L->h - 1; i++)
			ui_puts(y + 1 + i, L->x + 2, NOTES[i], L->w - 4, A_DIM, CP_MENU);
	}
	ui_box_hint(L->y, L->x, L->h, L->w, " Enter=Run  Esc=Close ", CP_MENU);
	refresh();
}

/* Run what an item does. 0 = quit. */
static int run(int act)
{
	switch (act) {
	case DO_ABOUT: show_about(); return 1;
	case DO_ACC_SELECT: focus_to(COL_ACCTS); return 1;
	case DO_ACC_ADD: switch_account(NULL); return 1;
	case DO_ACC_EDIT: account_edit(); return 1;
	case DO_ACC_REMOVE: account_remove(); return 1;
	case DO_FLD_SELECT: focus_to(COL_FOLDERS); return 1;
	case DO_FLD_CREATE: folder_create(); return 1;
	case DO_FLD_DELETE: folder_delete(); return 1;
	case DO_FLD_RENAME: folder_rename(); return 1;
	case DO_FLD_MOVE: folder_move(); return 1;
	case DO_ACC_EXPORT: folder_export(); return 1;
	case DO_ACC_IMPORT: folder_import(); return 1;
	case DO_KEYS: show_help(); return 1;
	case DO_CMDLINE: show_cmdline_help(); return 1;
	case DO_FIND:
		if (!A.mv.open) {
			set_status("Open an email first");
			return 1;
		}
		focus_to(COL_MSG);
		return msg_colon_with("/");
	}
	return handle_key(act);                /* the same as its key */
}

int menu_run(void)
{
	static int cur, sel;                   /* where it was the last time */
	layout_t L;
	timeout(1000);                         /* keep the clock ticking */
	for (;;) {
		if (sel >= COLS_[cur].n) sel = COLS_[cur].n - 1;
		place(&L, cur);
		draw_menu(&L, cur, sel);
		int ch = ui_getch();
		if (ch == ERR || ch == KEY_RESIZE) continue;
		int act = 0;
		switch (ch) {
		case 27: case KEY_F(1): case KEY_F(10):
			return 1;
		case KEY_LEFT: case KEY_BTAB: cur = (cur + NCOL - 1) % NCOL; break;
		case KEY_RIGHT: case '\t': cur = (cur + 1) % NCOL; break;
		case KEY_UP: sel = (sel + COLS_[cur].n - 1) % COLS_[cur].n; break;
		case KEY_DOWN: sel = (sel + 1) % COLS_[cur].n; break;
		case KEY_HOME: case KEY_PPAGE: sel = 0; break;
		case KEY_END: case KEY_NPAGE: sel = COLS_[cur].n - 1; break;
		case '\n': case '\r': case KEY_ENTER: act = COLS_[cur].it[sel].act; break;
		case KEY_MOUSE: {
			MEVENT ev;
			if (getmouse(&ev) != OK || !(ev.bstate & (BUTTON1_PRESSED | BUTTON1_CLICKED))) break;
			if (ev.y < L.y || ev.y >= L.y + L.h || ev.x < L.x || ev.x >= L.x + L.w) return 1;
			for (int c = 0; c < NCOL; c++) {
				if (L.cx[c] < 0 || ev.x < L.cx[c] - 1 || ev.x > L.cx[c] + colw(c)) continue;
				int i = ev.y - L.y - 3;
				cur = c;
				if (i >= 0 && i < COLS_[c].n) act = COLS_[c].it[sel = i].act;
			}
			break;
		}
		default:
			/* an item's letter, in whichever column */
			for (int c = 0; c < NCOL && ch > 0 && ch < 256; c++)
				for (int i = 0; i < COLS_[c].n; i++) {
					const mitem *m = &COLS_[c].it[i];
					if (m->hot >= 0 && tolower(ch) == tolower((unsigned char)m->label[m->hot])) {
						act = m->act;
						cur = c;
						sel = i;
					}
				}
			break;
		}
		if (act) {
			draw();
			return run(act);
		}
	}
}
