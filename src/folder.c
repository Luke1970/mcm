/* folder.c - the message list (a cache of header blocks), opening and
 * refreshing folders, the filter (L) and the background work after a draw
 */
#include "mcm.h"

static struct chunk *chunk_find(uint32_t c);
static void chunk_store(uint32_t first, int n, msg_t *m);
static uint32_t row_seq(int i);
static int seq_row(uint32_t seq);
static int fetch_positions(uint32_t from, uint32_t to, msg_t **m, int *n);
static void load_window(void);
static void update_unseen(void);
static int filter_search(void);
static void filter_apply(void);

/* ------------------------------------------------------------------ */
/* Message list cache                                                  */

void cache_clear(void)
{
	for (int i = 0; i < MAX_CHUNKS; i++) {
		msgs_free(A.chunks[i].m, A.chunks[i].n);
		memset(&A.chunks[i], 0, sizeof A.chunks[i]);
	}
}

/* Block number c holds sequence numbers c*CHUNK+1 .. (c+1)*CHUNK. */
static struct chunk *chunk_find(uint32_t c)
{
	uint32_t first = c * CHUNK + 1;
	for (int i = 0; i < MAX_CHUNKS; i++)
		if (A.chunks[i].first == first) {
			A.chunks[i].used = ++A.clock;
			return &A.chunks[i];
		}
	return NULL;
}

static void chunk_store(uint32_t first, int n, msg_t *m)
{
	struct chunk *slot = &A.chunks[0];
	for (int i = 0; i < MAX_CHUNKS; i++) {
		if (!A.chunks[i].first) { slot = &A.chunks[i]; break; }
		if (A.chunks[i].used < slot->used) slot = &A.chunks[i];
	}
	msgs_free(slot->m, slot->n);
	slot->first = first;
	slot->n = n;
	slot->m = m;
	slot->used = ++A.clock;
}

/* Sequence number of list row i (0 if there is none). */
static uint32_t row_seq(int i)
{
	int n = nrows();
	if (i < 0 || i >= n) return 0;
	return A.fon ? A.fseq[n - 1 - i] : (uint32_t)(n - i);
}

/* The row showing sequence number seq, or (with a filter) the row of the
 * nearest older match; 0 when there is none. */
static int seq_row(uint32_t seq)
{
	if (!A.fon) return seq >= 1 && (int)seq <= A.nmsgs ? A.nmsgs - (int)seq : 0;
	int lo = 0, hi = A.nf;               /* first index with fseq > seq */
	while (lo < hi) {
		int mid = lo + (hi - lo) / 2;
		if (A.fseq[mid] <= seq) lo = mid + 1;
		else hi = mid;
	}
	return lo > 0 ? A.nf - lo : (A.nf ? A.nf - 1 : 0);
}

/* Headers for positions from..to (see nrows). */
static int fetch_positions(uint32_t from, uint32_t to, msg_t **m, int *n)
{
	if (A.fon) return store_fetch_set(A.im, A.fseq + from - 1, (int)(to - from + 1), m, n);
	return store_fetch_headers(A.im, from, to, m, n);
}

/* Header of list row i, or NULL if that block is not loaded. */
msg_t *msg_at(int i)
{
	if (i < 0 || i >= nrows()) return NULL;
	uint32_t seq = (uint32_t)(nrows() - i);   /* position */
	struct chunk *c = chunk_find((seq - 1) / CHUNK);
	if (!c) return NULL;
	uint32_t k = seq - c->first;
	return k < (uint32_t)c->n ? &c->m[k] : NULL;
}

/* Header of list row i, read from the server when its block is not loaded
 * (NULL on an error). */
msg_t *msg_load(int i)
{
	msg_t *m = msg_at(i);
	if (m || i < 0 || i >= nrows()) return m;
	uint32_t pos = (uint32_t)(nrows() - i);
	uint32_t from = (pos - 1) / CHUNK * CHUNK + 1, to = from + CHUNK - 1;
	if (to > (uint32_t)nrows()) to = (uint32_t)nrows();
	int n;
	if (fetch_positions(from, to, &m, &n) != 0) {
		set_status("Error: %s", store_error(A.im));
		return NULL;
	}
	chunk_store(from, n, m);
	return msg_at(i);
}

/* Load the header blocks for the visible rows plus one page either side. */
static void load_window(void)
{
	A.need_list = 0;
	if (nrows() <= 0 || A.cur_folder < 0) return;

	int rows = A.mrows > 0 ? A.mrows : 25;
	long lo = (long)A.mtop - rows, hi = (long)A.mtop + 2L * rows;
	if (lo < 0) lo = 0;
	if (hi > nrows() - 1) hi = nrows() - 1;
	uint32_t N = (uint32_t)nrows();
	uint32_t c0 = (N - (uint32_t)hi - 1) / CHUNK, c1 = (N - (uint32_t)lo - 1) / CHUNK;

	/* newest blocks first; contiguous missing blocks go in one request */
	for (uint32_t c = c1 + 1; c-- > c0;) {
		if (chunk_find(c)) continue;
		uint32_t ce = c;
		while (ce > c0 && !chunk_find(ce - 1)) ce--;

		uint32_t from = ce * CHUNK + 1, to = (c + 1) * CHUNK;
		if (to > N) to = N;
		msg_t *m;
		int n;
		if (fetch_positions(from, to, &m, &n) != 0) {
			set_status("Error: %s", store_error(A.im));
			A.work_failed = 1;
			return;
		}
		for (uint32_t k = ce; k <= c; k++) {
			uint32_t first = k * CHUNK + 1;
			uint32_t cnt = to - first + 1 < CHUNK ? to - first + 1 : CHUNK;
			msg_t *part = xmalloc(sizeof *part * cnt);
			memcpy(part, m + (first - from), sizeof *part * cnt);
			chunk_store(first, (int)cnt, part);
		}
		free(m);   /* strings now owned by the chunks */
		c = ce;
	}
}

/* ------------------------------------------------------------------ */
/* Actions                                                             */

void now_status(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(A.status, sizeof A.status, fmt, ap);
	va_end(ap);
	draw();
}

static void update_unseen(void)
{
	uint32_t u;
	A.unseen = (A.cur_folder >= 0 &&
	            store_status_unseen(A.im, A.folders[A.cur_folder].name, &u) == 0) ? (long)u : -1;
}

void load_selected(void)
{
	if (nrows() == 0) {
		A.pending_load = 0;
		close_message();
		return;
	}
	msg_t *m = msg_at(A.msel);
	if (!m) return;                        /* header not loaded yet: stay pending */
	A.pending_load = 0;
	if (A.mv.open && A.mv.uid == m->uid) return;

	close_message();
	if (!m->uid) return;
	char before[sizeof A.status];
	memcpy(before, A.status, sizeof before);
	A.pending_load = 1;
	now_status("Loading message...");
	A.pending_load = 0;
	if (mv_open(&A.mv, A.im, m->uid, m->size) != 0) {
		set_status("Error: %s", store_error(A.im));
		return;
	}
	A.text_dirty = 1;
	if (!m->seen && store_mark_seen(A.im, m->uid) == 0) {
		m->seen = 1;
		if (A.unseen > 0) A.unseen--;
	}
	set_status("%s", before);   /* keep a message that was showing */
}

void select_msg(int i)
{
	if (nrows() == 0) return;
	if (i < 0) i = 0;
	if (i >= nrows()) i = nrows() - 1;
	if (i != A.msel || !A.mv.open) A.pending_load = 1;
	A.msel = i;
	A.mfree = 0;
}

void filter_off(void)
{
	free(A.fseq);
	A.fseq = NULL;
	A.nf = 0;
	A.fon = 0;
}

/* Search the folder for A.filter; 0 = done (A.fseq set). */
static int filter_search(void)
{
	uint32_t *v;
	int n;
	if (store_search(A.im, A.filter, (uint32_t)A.nmsgs, &v, &n) != 0) {
		set_status("Filter: %s", store_error(A.im));
		return -1;
	}
	free(A.fseq);
	A.fseq = v;
	A.nf = n;
	A.fon = 1;
	return 0;
}

/* Apply new mail and expunges reported by the server, keeping the selection. */
void sync_changes(void)
{
	uint32_t ex;
	const uint32_t *exp;
	int ne;
	if (!store_changes(A.im, &ex, &exp, &ne) || A.cur_folder < 0) return;

	long sel = row_seq(A.msel), top = row_seq(A.mtop);    /* sequence numbers, 0 = none */
	for (int i = 0; i < ne; i++) {
		if ((long)exp[i] < sel) sel--;
		if ((long)exp[i] < top) top--;
	}
	int added = (int)ex - (A.nmsgs - ne);
	A.nmsgs = (int)ex;
	if (A.fon && filter_search() != 0) filter_off();   /* the matches moved too */
	if (!A.fon && sel <= 0 && A.nmsgs) sel = A.nmsgs;    /* (as before: the newest) */
	A.msel = sel > 0 ? seq_row((uint32_t)sel) : 0;
	A.mtop = top > 0 ? seq_row((uint32_t)top) : 0;
	if (A.mfree) clamp_top(&A.mtop, A.mrows, nrows());
	else ensure_visible(&A.msel, &A.mtop, A.mrows, nrows());

	cache_clear();          /* sequence numbers moved: reload what is visible */
	update_unseen();
	if (added > 0) set_status("%d new message(s)", added);
	A.pending_load = 1;     /* reloads only if the selected message changed */
}

/* Show only the messages matching A.filter (empty: all of them again); the
 * selection stays on the same message, or the nearest one. */
static void filter_apply(void)
{
	uint32_t sel = row_seq(A.msel);
	if (!*A.filter) {
		filter_off();
	} else {
		now_status("Filtering %d messages...", A.nmsgs);
		if (filter_search() != 0) return;
		set_status("%d of %d messages match \"%s\"", A.nf, A.nmsgs, A.filter);
	}
	cache_clear();
	A.msel = sel ? seq_row(sel) : 0;
	A.mtop = 0;
	A.mfree = 0;
	A.pending_load = 1;
}

/* Esc: leave the filter, show the whole folder. */
void filter_leave(void)
{
	A.fedit = 0;
	A.filter[0] = '\0';
	if (A.fon) filter_apply();
}

/* Type the filter text in the bar; Enter applies it, Esc leaves the filter. */

void open_folder(int idx)
{
	if (idx < 0 || idx >= A.nfolders) return;
	folder_t *f = &A.folders[idx];
	if (f->noselect) {
		set_status("%s is not a mailbox (it only contains other folders)", f->display);
		return;
	}
	now_status("Opening %s...", f->display);
	close_message();
	cache_clear();
	pick_clear();                          /* so does a selection */
	filter_off();                          /* a filter belongs to one folder */
	A.filter[0] = '\0';
	A.nmsgs = A.msel = A.mtop = 0;
	A.cur_folder = -1;
	A.unseen = -1;

	uint32_t ex;
	if (store_select(A.im, f->name, &ex) != 0) {
		set_status("Cannot open %s: %s", f->display, store_error(A.im));
		return;
	}
	uint32_t e;
	const uint32_t *x;
	int n;
	store_changes(A.im, &e, &x, &n);       /* SELECT itself is not a change */

	A.cur_folder = idx;
	A.nmsgs = (int)ex;
	update_unseen();
	set_status("%s", "");
	A.pending_load = A.nmsgs > 0;          /* show the latest message */
	A.last_poll = time(NULL);
}

/* Read the folder list again; the open folder keeps its place in it, and
 * the folders window goes to folder sel (raw name; NULL: the open one). */
int folders_reload(const char *sel)
{
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
	A.fsel = 0;
	for (int i = 0; open && i < nn; i++)
		if (!strcmp(nf[i].name, open)) A.cur_folder = A.fsel = i;
	for (int i = 0; sel && i < nn; i++)
		if (!strcmp(nf[i].name, sel)) A.fsel = i;
	free(open);
	A.ffree = 0;
	ensure_visible(&A.fsel, &A.ftop, A.frows, A.nfolders);
	return 0;
}

/* Open the current folder again (after the store had another one
 * selected), keeping the filter and, as far as it goes, the cursor. */
void reopen_folder(void)
{
	int idx = A.cur_folder, sel = A.msel, top = A.mtop, fon = A.fon;
	char filt[sizeof A.filter];
	snprintf(filt, sizeof filt, "%s", A.filter);
	if (idx < 0) return;
	open_folder(idx);
	if (A.cur_folder != idx) return;
	if (fon) {
		snprintf(A.filter, sizeof A.filter, "%s", filt);
		filter_apply();
	}
	int n = nrows();
	A.msel = sel < n ? sel : n - 1;
	if (A.msel < 0) A.msel = 0;
	A.mtop = top;
	ensure_visible(&A.msel, &A.mtop, A.mrows, n);
	A.pending_load = n > 0;
}

void refresh_folder(int quiet)
{
	if (A.cur_folder < 0) return;
	A.last_poll = time(NULL);
	if (store_noop(A.im, !quiet) != 0) {
		set_status("Error: %s", store_error(A.im));
		return;
	}
	if (!quiet) {
		cache_clear();     /* re-read flags of the visible messages */
		update_unseen();
		set_status("Folder refreshed");
	}
	sync_changes();
}

int work_pending(void)
{
	return !A.work_failed && (A.need_list || A.need_hex || A.need_more);
}

/* One piece of loading the last draw asked for. */
void do_work(void)
{
	if (A.need_list) {
		load_window();
	} else if (A.need_hex) {
		A.need_hex = 0;
		if (mv_hex_load(&A.mv, A.im, A.need_hex_off, A.need_hex_len) != 0) {
			set_status("Error: %s", store_error(A.im));
			A.work_failed = 1;
		}
	} else if (A.need_more) {
		A.need_more = 0;
		if (mv_more(&A.mv, A.im) != 0) {
			set_status("Error: %s", store_error(A.im));
			A.work_failed = 1;
		}
		A.text_dirty = 1;
	}
}

void filter_edit(void)
{
	focus_to(COL_LIST);                 /* the bar is in the list's border */
	A.fedit = 1;
	for (;;) {
		draw();
		move(A.fbar_y, A.fcur_x);
		curs_set(1);
		refresh();
		timeout(1000);                  /* keeps the clock running */
		wint_t wc;
		int r = ui_get_wch(&wc);
		if (r == ERR) continue;
		size_t n = strlen(A.filter);
		if (r == KEY_CODE_YES) {
			if (wc == KEY_ENTER) break;
			if (wc == KEY_BACKSPACE || wc == KEY_DC) wc = 127;
			else if (wc == KEY_MOUSE) {
				MEVENT ev;
				if (getmouse(&ev) != OK || !(ev.bstate & BUTTON1_PRESSED)) continue;
				if (ev.y == A.fbar_y && A.fesc_x1 && ev.x >= A.fesc_x0 && ev.x < A.fesc_x1) {
					filter_leave();
					break;
				}
				if (ev.y == A.fbar_y && ev.x > A.fbar_x && ev.x < A.fbar_x + A.fbar_w - 1) continue;
				break;                      /* a click elsewhere: apply */
			} else continue;
		}
		if (wc == 27) {                     /* Esc: leave the filter */
			filter_leave();
			break;
		}
		if (wc == '\n' || wc == '\r') break;
		if (wc == 127 || wc == 8) {         /* Backspace: the last character */
			while (n > 0 && (A.filter[n - 1] & 0xC0) == 0x80) n--;
			if (n > 0) n--;
			A.filter[n] = '\0';
			continue;
		}
		if (wc == 21) { A.filter[0] = '\0'; continue; }   /* Ctrl-U: empty it */
		if (wc < 0x20 || wc == 0x7f) continue;
		uint32_t cp = (uint32_t)wc;
		if (!utf16_join(&cp)) continue;              /* first half of a Windows pair */
		buf_t b = {0};
		utf8_put(&b, cp);
		if (n + b.len < sizeof A.filter) {
			memcpy(A.filter + n, b.d, b.len);
			A.filter[n + b.len] = '\0';
		}
		buf_free(&b);
	}
	curs_set(0);
	if (A.fedit) {                          /* Enter or a click: apply */
		A.fedit = 0;
		filter_apply();
	}
}

/* Rows of the email list. The cache holds them by position p = rows - i
 * (1 = the oldest): without a filter that is the sequence number, with a
 * filter the index into fseq plus one. */
int nrows(void) { return A.fon ? A.nf : A.nmsgs; }
