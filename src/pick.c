/* pick.c - selected messages: Enter (one), Shift-Enter (a range) and
 * Shift-Up/Down in the email list; O shows them in a window of their own,
 * where they can be copied, moved or deleted (moved to Deleted Items).
 */
#include "mcm.h"

static msg_t   *pick;          /* the selected messages (copies of their headers) */
static int      npick;
static uint32_t anchor_uid;    /* the last one Enter selected: Shift-Enter starts there */
static int      anchor_row;

/* The window (O) */
static int psel, ptop, prows;
static int hint_x[5], hint_w[5];   /* C M D Enter Esc on its bottom border (as drawn) */

static void pick_add(const msg_t *m);
static void pick_del(int i);
static int  pick_find(uint32_t uid);
static void fmt_bytes(char *dst, size_t n, uint32_t bytes);
static void pw_draw(void);
static int  folder_choice(const char *title);
static int  trash_folder(void);
static int  create_trash(void);
static int  do_action(int key);

int pick_count(void) { return npick; }

int pick_has(uint32_t uid) { return pick_find(uid) >= 0; }

static int pick_find(uint32_t uid)
{
	for (int i = 0; i < npick; i++)
		if (pick[i].uid == uid) return i;
	return -1;
}

static void pick_add(const msg_t *m)
{
	if (!m || !m->uid || pick_find(m->uid) >= 0) return;
	pick = xrealloc(pick, sizeof *pick * (size_t)(npick + 1));
	msg_t *c = &pick[npick++];
	*c = *m;
	c->from = xstrdup(m->from ? m->from : "");
	c->subject = xstrdup(m->subject ? m->subject : "");
}

static void pick_del(int i)
{
	if (i < 0 || i >= npick) return;
	free(pick[i].from);
	free(pick[i].subject);
	memmove(&pick[i], &pick[i + 1], sizeof *pick * (size_t)(npick - i - 1));
	npick--;
}

void pick_clear(void)
{
	msgs_free(pick, npick);
	pick = NULL;
	npick = 0;
	anchor_uid = 0;
}

/* Enter: select the message under the cursor, or unselect it; then the
 * next one. */
void pick_toggle(void)
{
	msg_t *m = msg_load(A.msel);
	if (!m || !m->uid) return;
	int i = pick_find(m->uid);
	if (i >= 0) pick_del(i);
	else pick_add(m);
	anchor_uid = m->uid;
	anchor_row = A.msel;
	if (A.msel + 1 < nrows()) select_msg(A.msel + 1);
}

/* Shift-Enter: select everything from the last message Enter selected to
 * the one under the cursor. */
void pick_range(void)
{
	int a = A.msel, b = A.msel;
	msg_t *am = anchor_uid ? msg_load(anchor_row) : NULL;
	if (am && am->uid == anchor_uid) {
		if (anchor_row < a) a = anchor_row;
		else b = anchor_row;
	}
	if (b - a > 200) now_status("Selecting %d messages ...", b - a + 1);
	for (int i = a; i <= b; i++) pick_add(msg_load(i));
	set_status("%d selected", npick);
}

/* Shift-Up / Shift-Down: select the message under the cursor and the one
 * the cursor moves to. */
void pick_extend(int dir)
{
	pick_add(msg_load(A.msel));
	if (A.msel + dir < 0 || A.msel + dir >= nrows()) return;
	select_msg(A.msel + dir);
	pick_add(msg_load(A.msel));
}

/* 255 bytes, 12.3 kB, 8.6 MB */
static void fmt_bytes(char *dst, size_t n, uint32_t bytes)
{
	if (bytes < 1000) snprintf(dst, n, "%u bytes", bytes);
	else if (bytes < 1000000) snprintf(dst, n, "%.1f kB", bytes / 1e3);
	else if (bytes < 1000000000) snprintf(dst, n, "%.1f MB", bytes / 1e6);
	else snprintf(dst, n, "%.1f GB", bytes / 1e9);
}

static int by_date_desc(const void *a, const void *b)
{
	const msg_t *x = a, *y = b;
	if (x->date != y->date) return x->date < y->date ? 1 : -1;
	return x->uid < y->uid ? 1 : x->uid > y->uid ? -1 : 0;
}

/* ------------------------------------------------------------------ */
/* The window                                                          */

static void pw_draw(void)
{
	draw();
	int y = 1, x = 0, h = LINES - 2, w = COLS;
	if (h < 5 || w < 30) return;
	uint64_t total = 0;
	for (int i = 0; i < npick; i++) total += pick[i].size;
	char ts[32], title[160];
	fmt_bytes(ts, sizeof ts, total > UINT32_MAX ? UINT32_MAX : (uint32_t)total);
	snprintf(title, sizeof title, " Selected: %d message%s, %s ", npick, npick == 1 ? "" : "s", ts);
	ui_fill(y, x, h, w, CP_NORMAL);
	ui_box(y, x, h, w, CP_NORMAL, title, A_BOLD, CP_TITLE);

	/* columns: flags | subject | from | date | size */
	int inner = w - 2, flagw = 4, datew = 16, sizew = 10;
	int fromw = inner >= 110 ? 30 : inner >= 70 ? 18 : 0;
	int subjw = inner - flagw - 1 - (fromw ? fromw + 1 : 0) - datew - 1 - sizew - 1;
	if (subjw < 8) { datew = 0; subjw += 17; }
	int cx = x + 1;
	ui_puts(y + 1, cx, "Flag", flagw + 1, A_BOLD | A_UNDERLINE, CP_NORMAL);
	ui_puts(y + 1, cx + flagw + 1, "Subject", subjw + 1, A_BOLD | A_UNDERLINE, CP_NORMAL);
	int fx = cx + flagw + 1 + subjw + 1;
	if (fromw) ui_puts(y + 1, fx, "From", fromw + 1, A_BOLD | A_UNDERLINE, CP_NORMAL);
	int dx = fx + (fromw ? fromw + 1 : 0);
	if (datew) ui_puts(y + 1, dx, "Date", datew + 1, A_BOLD | A_UNDERLINE, CP_NORMAL);
	int sx = dx + (datew ? datew + 1 : 0);
	ui_puts(y + 1, sx, "      Size", sizew, A_BOLD | A_UNDERLINE, CP_NORMAL);

	prows = h - 3;
	if (psel >= npick) psel = npick - 1;
	if (psel < 0) psel = 0;
	ensure_visible(&psel, &ptop, prows, npick);
	for (int r = 0; r < prows && ptop + r < npick; r++) {
		const msg_t *m = &pick[ptop + r];
		int yy = y + 2 + r, cur = ptop + r == psel;
		short pair = cur ? CP_HIGHLIGHT : CP_NORMAL;
		attr_t a = !m->seen ? A_BOLD : 0;
		/* IMAP system flags: Seen Answered Flagged Deleted */
		char fl[5] = {m->seen ? 'S' : '-', m->answered ? 'A' : '-', m->flagged ? 'F' : '-',
		              m->deleted ? 'D' : '-', 0};
		char date[32] = "", size[32];
		if (m->date) {
			struct tm tm;
			localtime_r(&m->date, &tm);
			strftime(date, sizeof date, "%Y-%m-%d %H:%M", &tm);
		}
		char sz[32];
		fmt_bytes(sz, sizeof sz, m->size);
		snprintf(size, sizeof size, "%*s", sizew, sz);
		ui_puts(yy, cx, fl, flagw + 1, a, pair);
		ui_puts(yy, cx + flagw + 1, *m->subject ? m->subject : "(no subject)", subjw + 1, a, pair);
		if (fromw) ui_puts(yy, fx, m->from, fromw + 1, a, pair);
		if (datew) ui_puts(yy, dx, date, datew + 1, a, pair);
		ui_puts(yy, sx, size, sizew, a, pair);
	}
	draw_scrollbar(COL_LIST, x + w - 1, y + 2, prows, npick, &ptop);

	/* the keys, on the bottom border */
	static const char *const keys[] = {"C=Copy", "M=Move", "D=Delete", "Enter=Unselect", "Esc=Close"};
	int kx = x + 2;
	for (int i = 0; i < 5; i++) {
		int kw = (int)strlen(keys[i]);
		hint_x[i] = hint_w[i] = 0;
		if (kx + kw + 2 > x + w - 2) break;
		ui_puts(y + h - 1, kx, " ", 1, 0, CP_NORMAL);
		ui_puts(y + h - 1, kx + 1, keys[i], kw, 0, CP_NORMAL);
		int hk = (int)(strchr(keys[i], '=') - keys[i]);
		ui_puts(y + h - 1, kx + 1, keys[i], hk, A_BOLD, CP_PICK);
		ui_puts(y + h - 1, kx + 1 + kw, " ", 1, 0, CP_NORMAL);
		hint_x[i] = kx + 1;
		hint_w[i] = kw;
		kx += kw + 3;
	}
	ui_fill(LINES - 1, 0, 1, COLS, CP_MENU);
	ui_puts(LINES - 1, 1, *A.status ? A.status : "Flags: S=Seen A=Answered F=Flagged D=Deleted",
	        COLS - 2, 0, CP_MENU);
}

/* Choose a folder of this account to copy or move to (not the open one). */
static int folder_choice(const char *title)
{
	int *idx = xmalloc(sizeof *idx * (size_t)(A.nfolders + 1));
	char **items = xmalloc(sizeof *items * (size_t)(A.nfolders + 1));
	int n = 0;
	for (int i = 0; i < A.nfolders; i++) {
		if (A.folders[i].noselect || i == A.cur_folder) continue;
		items[n] = mutf7_decode(A.folders[i].name);   /* the whole path: Archive/2025 */
		idx[n++] = i;
	}
	int r = -1;
	if (!n) set_status("There is no other folder");
	else {
		int c = ui_list_dialog(title, (const char *const *)items, n, 0, "Enter=Choose  Esc=Cancel",
		                       pw_draw);
		if (c >= 0) r = idx[c];
	}
	for (int i = 0; i < n; i++) free(items[i]);
	free(items);
	free(idx);
	return r;
}

/* The folder for deleted messages: the server's \Trash folder, else one
 * with a usual name; -1 when there is none. */
static int trash_folder(void)
{
	for (int i = 0; i < A.nfolders; i++)
		if (A.folders[i].special == FOLDER_TRASH && !A.folders[i].noselect) return i;
	static const char *const names[] = {"Deleted Items", "Trash", "Deleted", "Deleted Messages",
	                                    "INBOX.Deleted Items", "INBOX/Deleted Items", "INBOX.Trash",
	                                    "INBOX/Trash", NULL};
	for (int k = 0; names[k]; k++)
		for (int i = 0; i < A.nfolders; i++) {
			if (A.folders[i].noselect) continue;
			char *u = mutf7_decode(A.folders[i].name);
			int hit = !strcasecmp(u, names[k]);
			free(u);
			if (hit) return i;
		}
	return -1;
}

/* Create "Deleted Items" (next to the other folders: under INBOX when all
 * of them are) and read the folder list again. Its index, or -1. */
static int create_trash(void)
{
	char delim = 0;
	int under_inbox = 0, others = 0;
	for (int i = 0; i < A.nfolders; i++) {
		const folder_t *f = &A.folders[i];
		if (!strcasecmp(f->name, "INBOX")) { delim = f->delim; continue; }
		others++;
		if (f->delim && !strncasecmp(f->name, "INBOX", 5) && f->name[5] == f->delim) under_inbox++;
	}
	char name[64];
	if (delim && others && under_inbox == others) snprintf(name, sizeof name, "INBOX%cDeleted Items", delim);
	else snprintf(name, sizeof name, "Deleted Items");
	if (store_create(A.im, name) != 0) {
		set_status("Cannot create %s: %s", name, store_error(A.im));
		return -1;
	}
	char *open = A.cur_folder >= 0 ? xstrdup(A.folders[A.cur_folder].name) : NULL;
	folder_t *nf;
	int nn;
	if (store_list(A.im, &nf, &nn) != 0) {
		free(open);
		set_status("Cannot list folders: %s", store_error(A.im));
		return -1;
	}
	folders_free(A.folders, A.nfolders);
	A.folders = nf;
	A.nfolders = nn;
	A.cur_folder = -1;
	for (int i = 0; open && i < nn; i++)
		if (!strcmp(nf[i].name, open)) A.cur_folder = A.fsel = i;
	free(open);
	for (int i = 0; i < nn; i++)
		if (!strcmp(nf[i].name, name)) return i;
	return trash_folder();
}

/* C, M or D in the window. 1 = done (the window closes). */
static int do_action(int key)
{
	if (!npick || A.cur_folder < 0) return 0;
	char what[64];
	snprintf(what, sizeof what, "%d message%s", npick, npick == 1 ? "" : "s");
	int dest = -1, forever = 0;
	if (key == 'c' || key == 'm') {
		char t[96];
		snprintf(t, sizeof t, "%s %s to", key == 'c' ? "Copy" : "Move", what);
		dest = folder_choice(t);
		if (dest < 0) return 0;
	} else {
		dest = trash_folder();
		if (dest == A.cur_folder) {
			char q[160];
			snprintf(q, sizeof q, "Delete %s for good? %s in %s already.", what,
			         npick == 1 ? "It is" : "They are", A.folders[dest].display);
			if (!ui_confirm("Delete", q, pw_draw)) return 0;
			forever = 1;
		} else if (dest < 0) {
			if (store_kind(A.im) != STORE_IMAP) {
				set_status("There is no Deleted Items folder");
				return 0;
			}
			if (!ui_confirm("Delete", "There is no Deleted Items folder. Create it?", pw_draw)) return 0;
			if ((dest = create_trash()) < 0) return 0;
		}
	}
	/* oldest first (the window shows the newest first), so they arrive in
	 * the other folder in the same order */
	uint32_t *uids = xmalloc(sizeof *uids * (size_t)npick);
	for (int i = 0; i < npick; i++) uids[i] = pick[npick - 1 - i].uid;
	char msg[160];
	snprintf(msg, sizeof msg, "%s %s ...", key == 'c' ? "Copying" : forever ? "Deleting" : "Moving", what);
	ui_progress(msg, pw_draw);
	int rc = key == 'c' ? store_copy(A.im, uids, npick, A.folders[dest].name)
	       : forever ? store_delete(A.im, uids, npick)
	       : store_move(A.im, uids, npick, A.folders[dest].name);
	free(uids);
	if (rc < 0) {
		char err[400];
		snprintf(err, sizeof err, "%s", store_error(A.im));
		ui_message(key == 'c' ? "Copy" : key == 'm' ? "Move" : "Delete", err, pw_draw);
		sync_changes();      /* some may have gone already */
		cache_clear();
		return 0;
	}
	const char *u = pick_can_undo() ? "  (U=Undo)" : "";
	if (rc > 0) set_status("%s%s", store_error(A.im), u);
	else if (forever) set_status("%s deleted", what);
	else set_status("%s %s %s%s", what, key == 'c' ? "copied to" : "moved to", A.folders[dest].display, u);
	sync_changes();          /* the moved ones leave the list */
	cache_clear();           /* flags may have changed (\Deleted) */
	pick_clear();
	return 1;
}

int pick_can_undo(void)
{
	int n;
	const char *src, *dst;
	return A.im && store_undo_info(A.im, &n, &src, &dst) != 0;
}

/* A folder's name as shown (the whole path). */
static char *folder_label(const char *raw)
{
	return store_kind(A.im) == STORE_IMAP ? mutf7_decode(raw) : xstrdup(raw);
}

/* U: undo the last copy or move (also a delete to Deleted Items), after
 * asking. */
void pick_undo(void)
{
	int n;
	const char *src, *dst;
	int kind = A.im ? store_undo_info(A.im, &n, &src, &dst) : 0;
	if (!kind) {
		set_status("Nothing to undo (deleting for good cannot be undone)");
		return;
	}
	char *s = folder_label(src), *d = folder_label(dst);
	char q[600], what[64];
	snprintf(what, sizeof what, "%d message%s", n, n == 1 ? "" : "s");
	if (kind == 'c')
		snprintf(q, sizeof q, "Undo the copy: remove the %s copied to %s again?", what, d);
	else
		snprintf(q, sizeof q, "Undo the move: put the %s in %s back in %s?", what, d, s);
	if (ui_confirm("Undo", q, draw_bg)) {
		ui_progress("Undoing ...", draw_bg);
		int rc = store_undo(A.im);
		char err[400];
		snprintf(err, sizeof err, "%s", store_error(A.im));
		reopen_folder();                     /* the store has another folder selected */
		if (rc < 0) ui_message("Undo", err, draw_bg);
		else if (rc > 0)        /* IMAP without UIDPLUS */
			set_status("%s undone; the cop%s in %s %s only marked deleted (D): the server cannot "
			           "remove single messages", kind == 'c' ? "Copy" : "Move", n == 1 ? "y" : "ies", d,
			           n == 1 ? "is" : "are");
		else if (kind == 'c') set_status("Copy undone: %s removed from %s", what, d);
		else set_status("Move undone: %s back in %s", what, s);
	}
	free(s);
	free(d);
}

/* O: the window with the selected messages (none selected: the one under
 * the cursor). */
void pick_window(void)
{
	if (A.cur_folder < 0) return;
	if (!npick) pick_add(msg_load(A.msel));
	if (!npick) return;
	qsort(pick, (size_t)npick, sizeof *pick, by_date_desc);
	psel = ptop = 0;
	char keep[sizeof A.status];
	snprintf(keep, sizeof keep, "%s", A.status);
	A.status[0] = '\0';
	for (;;) {
		pw_draw();
		refresh();
		timeout(-1);
		int ch = ui_getch();
		A.status[0] = '\0';
		switch (ch) {
		case KEY_UP: psel--; break;
		case KEY_DOWN: psel++; break;
		case KEY_PPAGE: psel -= prows > 1 ? prows - 1 : 1; break;
		case KEY_NPAGE: psel += prows > 1 ? prows - 1 : 1; break;
		case KEY_HOME: psel = 0; break;
		case KEY_END: psel = npick - 1; break;
		case '\n': case '\r': case KEY_ENTER: case KEY_DC:
			pick_del(psel);
			if (!npick) return;
			break;
		case 'c': case 'C': if (do_action('c')) return; break;
		case 'm': case 'M': if (do_action('m')) return; break;
		case 'd': case 'D': if (do_action('d')) return; break;
		case 27: case 'q': case 'Q': case 'o': case 'O':
			snprintf(A.status, sizeof A.status, "%s", keep);
			return;
		case KEY_MOUSE: {
			MEVENT ev;
			if (getmouse(&ev) != OK) break;
			if (ev.bstate & BUTTON4_PRESSED) { ptop -= 3; if (ptop < 0) ptop = 0; psel = ptop; break; }
			if (ev.bstate & BUTTON5_PRESSED) { ptop += 3; psel = ptop; break; }
			if (!(ev.bstate & BUTTON1_PRESSED)) break;
			if (ev.y == LINES - 2) {             /* the keys on the bottom border */
				static const int k[] = {'c', 'm', 'd', '\n', 27};
				for (int i = 0; i < 5; i++)
					if (hint_w[i] && ev.x >= hint_x[i] && ev.x < hint_x[i] + hint_w[i]) {
						if (k[i] == 27) {
							snprintf(A.status, sizeof A.status, "%s", keep);
							return;
						}
						if (k[i] == '\n') {
							pick_del(psel);
							if (!npick) return;
						} else if (do_action(k[i])) {
							return;
						}
					}
				break;
			}
			int r = ev.y - 3;
			if (r >= 0 && r < prows && ptop + r < npick) psel = ptop + r;
			break;
		}
		}
	}
}
