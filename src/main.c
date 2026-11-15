/* emsg - console email client in the spirit of PC Tools and Midnight Commander
 *
 * Three columns: Folders | Message list | Message.  Two are visible at a time:
 *   focus on Folders           -> Folders + Message list
 *   focus on List or Message   -> Message list + Message
 */
#include "compose.h"
#include "editor.h"
#include "helptext.h"   /* the help texts, from the files in help/ */
#include "store.h"
#include "mime.h"
#include "msgview.h"
#include "smtp.h"
#include "theme.h"
#include "ui.h"
#include "util.h"
#include "vault.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <locale.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define EMSG_VERSION   "0.94"
#define POLL_SECONDS   60       /* check for new mail when idle */
#define LOAD_DELAY_MS  200      /* load message after selection rests this long */
#define CHUNK          100      /* message headers fetched per block */
#define MAX_CHUNKS     64       /* header blocks kept in memory (least recently used dropped) */

enum { COL_FOLDERS, COL_LIST, COL_MSG };

typedef struct { uint32_t off, len; } line_t;   /* one display line of the message text */

struct chunk {
	uint32_t      first;    /* sequence number of m[0]; 0 = slot unused */
	int           n;
	msg_t        *m;
	unsigned long used;
};

/* One mail account; all of them are kept in emsg.conf. */
typedef struct {
	char  name[64];            /* [account NAME] in the config file */
	int   type;                /* STORE_IMAP, STORE_POP3 or STORE_LOCAL (store.h) */
	char  path[512];           /* LOCAL: the mailboxes; POP3: where Sent/Drafts are kept */
	char  fullname[128];       /* display name in From: */
	char  host[256], port[16], user[256];
	char  smtp_host[256], smtp_port[16], smtp_user[256];
	char  smtp_login[600];     /* "user@host" of the IMAP login the SMTP settings belong to */
	char  smtp_security[16];   /* "tls", "starttls" or "" = by port (465 = tls) */
	int   store;               /* STORE_ASK / STORE_YES / STORE_NO: keep the password on disk */
	char *pass_enc;            /* encrypted IMAP password (vault.h), or NULL */
	char *smtp_pass_enc;       /* encrypted SMTP password, or NULL = use the IMAP one */
} account_t;

enum { STORE_ASK, STORE_YES, STORE_NO };
#define MAX_ACCOUNTS 64

static account_t  accts[MAX_ACCOUNTS];
static int        naccts;
static account_t *ac;              /* the account in use */
static char       last_account[64];
static char      *master_check;    /* encrypted known text: verifies the master password */
static char      *master;          /* master password, in memory only once typed */

/* Settings of the program (emsg.conf, before the accounts) */
static int wrap_width = 72;        /* new mail is wrapped at this column */
static int col_pct[2] = {25, 45};  /* left column width in % of the screen:
                                    * [0] Folders | List, [1] List | Message */
static int dragging;               /* mouse drag: DRAG_BORDER or DRAG_THUMB */
enum { DRAG_BORDER = 1, DRAG_THUMB };
static int drag_border;            /* which border (1 or 2) is being dragged */
/* Scroll bar of the message window, in its right border (as last drawn). */
static struct {
	int x, y, h;     /* column, first row, rows (0: no scroll bar) */
	int t0, tlen;    /* track: first row and length (between the arrows) */
	int th, tsize;   /* thumb: first row and length */
	int grab;        /* while dragging the thumb: row offset of the grab point */
} sb;

static struct {
	store_t   *im;
	char      theme[128];
	char      start_folder[512];   /* from the -f URL; empty = INBOX */
	int       type_given;          /* the command line said IMAP, POP3 or local */
	/* sending */
	char     *smtp_pass;           /* memory only; NULL = use the IMAP password */
	net_trust smtp_trust;

	folder_t *folders;
	int       nfolders, fsel, ftop, frows;
	int       cur_folder;            /* index into folders, -1 if none */
	/* the mouse wheel scrolled a list away from its selection: keep the
	 * view until a key or click moves the selection */
	int       ffree, mfree;

	/* Filter (L): the email list shows only the messages whose subject or
	 * sender contains the filter text; row i is then fseq[nf - 1 - i]. */
	char      filter[256];           /* the text, as applied or being typed */
	int       fedit;                 /* typing in the filter bar */
	int       fon;                   /* a filter is applied */
	uint32_t *fseq;                  /* matching sequence numbers, ascending */
	int       nf;
	int       fbar_y, fbar_x, fbar_w;     /* the list's bottom border, as last drawn */
	int       fesc_x0, fesc_x1;           /* where "Esc=Leave" is on it (x1 = 0: not shown) */
	int       fcur_x;                     /* cursor while typing */

	/* Message list, newest first: row i is sequence number nmsgs - i
	 * (with a filter: see below).
	 * Only blocks of headers around the visible rows are kept in memory. */
	int           nmsgs, msel, mtop, mrows;
	long          unseen;            /* -1 = unknown */
	struct chunk  chunks[MAX_CHUNKS];
	unsigned long clock;

	int       focus;
	int       left;                  /* two columns: COL_LIST or COL_FOLDERS on the left */

	/* message pane */
	msgview_t mv;
	line_t   *lines;
	int       nlines, lines_cap, lines_width, more_line, text_dirty;
	int       vtop, vrows, show_headers, hex;
	int       pending_load;

	/* background work requested by the last draw */
	int       need_list, need_more, need_hex, work_failed;
	size_t    need_hex_off, need_hex_len;

	char      status[256];
	time_t    last_poll;
} A;

enum { MENU_HEX = 4, MENU_HEADER = 5 };   /* indexes in folder_menu */

static const ui_menuitem folder_menu[] = {
	{"Quit", 0, 'q'}, {"Mail", 0, 'm'}, {"Reply", 0, 'r'}, {"Forward", 0, 'f'},
	{"heX", 2, 'x'}, {"Header", 0, 'h'}, {"Account", 0, 'a'}, {"Config", 0, 'c'},
	{"F1=Help", UI_HOT_KEYNAME, KEY_F(1)},
};

static void compose_start(int kind);
static int  wide(void);
static void account_menu(void);
static void config_menu(void);
static int  smtp_settings(const char *err, void (*bg)(void));
static void set_status(const char *fmt, ...) __attribute__((format(EMSG_PRINTF, 1, 2)));
static void set_status(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(A.status, sizeof A.status, fmt, ap);
	va_end(ap);
}

/* ------------------------------------------------------------------ */
/* Config: ~/.config/emsg/emsg.conf                                    */
/*                                                                     */
/*     theme = default                                                 */
/*     last_account = work                                             */
/*     [account work]                                                  */
/*     full_name = ...   server = ...   user = ...   ...               */
/*                                                                     */
/* A file without [account] sections (emsg 0.3) is read as one account. */

static void config_path(char *p, size_t n, const char *file)
{
	const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
	if (xdg && *xdg) snprintf(p, n, "%s/emsg%s%s", xdg, file ? "/" : "", file ? file : "");
	else snprintf(p, n, "%s/.config/emsg%s%s", home ? home : ".", file ? "/" : "", file ? file : "");
}

static void set_secret(char **slot, char *v)
{
	free(*slot);
	*slot = v;
}

static account_t *account_new(const char *name)
{
	if (naccts >= MAX_ACCOUNTS) return NULL;
	account_t *a = &accts[naccts++];
	memset(a, 0, sizeof *a);
	snprintf(a->name, sizeof a->name, "%s", name ? name : "");
	snprintf(a->port, sizeof a->port, "993");
	return a;
}

static account_t *account_find(const char *name)
{
	for (int i = 0; i < naccts; i++)
		if (!strcmp(accts[i].name, name)) return &accts[i];
	return NULL;
}

/* Give an unnamed account a unique name: user@host (or host), then -2, -3 ... */
static void account_name(account_t *a)
{
	if (*a->name) return;
	char base[48];
	if (*a->user && !strchr(a->user, '@')) snprintf(base, sizeof base, "%.20s@%.26s", a->user, a->host);
	else if (*a->user) snprintf(base, sizeof base, "%.47s", a->user);
	else snprintf(base, sizeof base, "%.47s", *a->host ? a->host : a->type == STORE_LOCAL ? "local" : "account");
	snprintf(a->name, sizeof a->name, "%s", base);
	for (int k = 2; ; k++) {
		int dup = 0;
		for (int i = 0; i < naccts; i++)
			if (&accts[i] != a && !strcmp(accts[i].name, a->name)) dup = 1;
		if (!dup) break;
		snprintf(a->name, sizeof a->name, "%s-%d", base, k);
	}
}

static void config_load(void)
{
	char p[1024], line[4096];
	config_path(p, sizeof p, "emsg.conf");
	FILE *f = fopen(p, "r");
	if (!f) return;
	account_t *a = NULL;
	while (fgets(line, sizeof line, f)) {
		char *s = str_trim(line);
		if (*s == '#' || !*s) continue;              /* '#' inside a value is kept */
		if (*s == '[') {
			char *e = strchr(s, ']');
			if (!e) continue;
			*e = '\0';
			s = str_trim(s + 1);
			if (strncasecmp(s, "account", 7)) { a = NULL; continue; }
			s = str_trim(s + 7);
			a = account_find(s);
			if (!a) a = account_new(s);
			continue;
		}
		char *eq = strchr(s, '=');
		if (!eq) continue;
		*eq = '\0';
		char *k = str_trim(s), *v = str_trim(eq + 1);
		if (!strcasecmp(k, "theme")) { snprintf(A.theme, sizeof A.theme, "%s", v); continue; }
		if (!strcasecmp(k, "last_account")) { snprintf(last_account, sizeof last_account, "%s", v); continue; }
		if (!strcasecmp(k, "wrap_width")) {
			int w = atoi(v);
			if (w >= 20 && w <= 998) wrap_width = w;
			continue;
		}
		if (!strcasecmp(k, "folders_width") || !strcasecmp(k, "list_width")) {
			int w = atoi(v);
			if (w >= 5 && w <= 95) col_pct[tolower((unsigned char)*k) == 'l'] = w;
			continue;
		}
		if (!strcasecmp(k, "master_check")) { set_secret(&master_check, xstrdup(v)); continue; }
		if (!a && !(a = account_new(NULL))) break;   /* old file: settings without a section */
		if (!strcasecmp(k, "type"))
			a->type = !strcasecmp(v, "pop3") ? STORE_POP3 : !strcasecmp(v, "local") ? STORE_LOCAL : STORE_IMAP;
		else if (!strcasecmp(k, "path")) snprintf(a->path, sizeof a->path, "%s", v);
		else if (!strcasecmp(k, "full_name")) snprintf(a->fullname, sizeof a->fullname, "%s", v);
		else if (!strcasecmp(k, "server")) snprintf(a->host, sizeof a->host, "%s", v);
		else if (!strcasecmp(k, "port")) snprintf(a->port, sizeof a->port, "%s", v);
		else if (!strcasecmp(k, "user") || !strcasecmp(k, "address"))
			snprintf(a->user, sizeof a->user, "%s", v);
		else if (!strcasecmp(k, "smtp_server")) snprintf(a->smtp_host, sizeof a->smtp_host, "%s", v);
		else if (!strcasecmp(k, "smtp_port")) snprintf(a->smtp_port, sizeof a->smtp_port, "%s", v);
		else if (!strcasecmp(k, "smtp_user")) snprintf(a->smtp_user, sizeof a->smtp_user, "%s", v);
		else if (!strcasecmp(k, "smtp_login")) snprintf(a->smtp_login, sizeof a->smtp_login, "%s", v);
		else if (!strcasecmp(k, "smtp_security")) snprintf(a->smtp_security, sizeof a->smtp_security, "%s", v);
		else if (!strcasecmp(k, "store_password"))
			a->store = !strcasecmp(v, "yes") ? STORE_YES : !strcasecmp(v, "no") ? STORE_NO : STORE_ASK;
		else if (!strcasecmp(k, "password")) set_secret(&a->pass_enc, *v ? xstrdup(v) : NULL);
		else if (!strcasecmp(k, "smtp_password")) set_secret(&a->smtp_pass_enc, *v ? xstrdup(v) : NULL);
	}
	fclose(f);
	for (int i = 0; i < naccts; i++) account_name(&accts[i]);
}

static void config_mkdir(void)
{
	char dir[1024];
	config_path(dir, sizeof dir, NULL);
	char parent[1024];
	snprintf(parent, sizeof parent, "%s", dir);
	char *sl = strrchr(parent, '/');
	if (sl) { *sl = '\0'; mkdir(parent, 0700); }
	mkdir(dir, 0700);
}

/* Write all accounts (to a new file, then rename: never a half file). */
static void config_save(void)
{
	char p[1100], tmp[1110];
	config_mkdir();
	config_path(p, sizeof p, "emsg.conf");
	snprintf(tmp, sizeof tmp, "%s.new", p);
	if (ac) {
		account_name(ac);
		snprintf(last_account, sizeof last_account, "%s", ac->name);
	}
	int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	FILE *f = fd >= 0 ? fdopen(fd, "w") : NULL;
	if (!f) {
		if (fd >= 0) close(fd);
		return;
	}
	fprintf(f, "# emsg settings, one [account NAME] section per mail account.\n"
	           "# Passwords are only stored when you choose so, encrypted with your\n"
	           "# master password (scrypt + AES-256-GCM, see the README).\n\n");
	fprintf(f, "theme = %s\n", A.theme);
	fprintf(f, "wrap_width = %d       # new mail is wrapped at this column (format=flowed)\n",
	        wrap_width);
	fprintf(f, "folders_width = %d    # %% of the screen for Folders (next to the list)\n", col_pct[0]);
	fprintf(f, "list_width = %d       # %% for the list (3 columns: of what the folders leave)\n",
	        col_pct[1]);
	if (*last_account) fprintf(f, "last_account = %s\n", last_account);
	if (master_check) fprintf(f, "master_check = %s\n", master_check);
	for (int i = 0; i < naccts; i++) {
		const account_t *a = &accts[i];
		if (a->type == STORE_LOCAL ? !*a->path : !*a->host) continue;   /* never set up */
		fprintf(f, "\n[account %s]\n", a->name);
		fprintf(f, "type = %s\n", a->type == STORE_POP3 ? "pop3" : a->type == STORE_LOCAL ? "local" : "imap");
		if (*a->fullname) fprintf(f, "full_name = %s\n", a->fullname);
		if (a->type == STORE_LOCAL) fprintf(f, "address = %s\n", a->user);
		else fprintf(f, "server = %s\nport = %s\nuser = %s\n", a->host, a->port, a->user);
		if (*a->path) fprintf(f, "path = %s\n", a->path);
		if (*a->smtp_host)
			fprintf(f, "smtp_server = %s\nsmtp_port = %s\nsmtp_user = %s\nsmtp_login = %s\n",
			        a->smtp_host, a->smtp_port, a->smtp_user, a->smtp_login);
		if (*a->smtp_security) fprintf(f, "smtp_security = %s\n", a->smtp_security);
		if (a->store != STORE_ASK) fprintf(f, "store_password = %s\n", a->store == STORE_YES ? "yes" : "no");
		if (a->pass_enc) fprintf(f, "password = %s\n", a->pass_enc);
		if (a->smtp_pass_enc) fprintf(f, "smtp_password = %s\n", a->smtp_pass_enc);
	}
	int bad = ferror(f);
	if (fclose(f) != 0 || bad) { unlink(tmp); return; }
	rename(tmp, p);
}

/* ------------------------------------------------------------------ */
/* Message list cache                                                  */

static void cache_clear(void)
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

/* Rows of the email list. The cache holds them by position p = rows - i
 * (1 = the oldest): without a filter that is the sequence number, with a
 * filter the index into fseq plus one. */
static int nrows(void) { return A.fon ? A.nf : A.nmsgs; }

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
static msg_t *msg_at(int i)
{
	if (i < 0 || i >= nrows()) return NULL;
	uint32_t seq = (uint32_t)(nrows() - i);   /* position */
	struct chunk *c = chunk_find((seq - 1) / CHUNK);
	if (!c) return NULL;
	uint32_t k = seq - c->first;
	return k < (uint32_t)c->n ? &c->m[k] : NULL;
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

	A.more_line = -1;
	if (A.mv.more_off != MV_NO_MORE)
		for (int i = 0; i < A.nlines && A.lines[i].off <= A.mv.more_off; i++) A.more_line = i;
}

static void close_message(void)
{
	mv_close(&A.mv);
	A.nlines = 0;
	A.vtop = 0;
	A.text_dirty = 1;
	A.lines_width = -1;
}

static int hex_per_line(int width)
{
	int per = 16;
	while (per > 4 && 11 + per * 4 > width) per /= 2;
	return per;
}

/* ------------------------------------------------------------------ */
/* Drawing                                                             */

/* Keep a list's first visible row in range (rows rows, n entries). */
static void clamp_top(int *top, int rows, int n)
{
	if (*top > n - rows) *top = n - rows;
	if (*top < 0) *top = 0;
}

static void ensure_visible(int *sel, int *top, int rows, int n)
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

static void draw_folders(int x, int y, int w, int h, int focused)
{
	ui_box(y, x, h, w, CP_NORMAL, " Folders ", A_BOLD, focused ? CP_HIGHLIGHT : CP_NORMAL);
	/* a narrow window shows Enter=Select only (Tab is also on the bottom line) */
	ui_box_hint(y, x, h, w, wide() || w < 30 ? " Enter=Select " : " Enter=Select  TAB=Emails ", CP_NORMAL);
	int rows = h - 2;
	A.frows = rows;
	if (A.ffree) clamp_top(&A.ftop, rows, A.nfolders);
	else ensure_visible(&A.fsel, &A.ftop, rows, A.nfolders);

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
	int avail = w - 4;                         /* between the corners, one line each side */
	if (avail < 10) return;
	if (avail - 9 - kw < 8) { keys = ""; kw = 0; }   /* narrow: the text only */
	int fw = avail - 9 - kw - (kw ? 1 : 0);    /* the text field */
	int x0 = x + 2;
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
	if (A.fon)
		snprintf(title, sizeof title, " %s filtered  %d of %d msgs ", fname, A.nf, A.nmsgs);
	else if (A.unseen >= 0)
		snprintf(title, sizeof title, " %s  %d msgs, %ld new ", fname, A.nmsgs, A.unseen);
	else
		snprintf(title, sizeof title, " %s  %d msgs ", fname, A.nmsgs);
	ui_box(y, x, h, w, CP_NORMAL, title, A_BOLD, focused ? CP_HIGHLIGHT : CP_NORMAL);
	A.fbar_y = y + h - 1;
	A.fbar_x = x;
	A.fbar_w = w;
	if (A.fedit || A.fon) {
		draw_filter_bar(y + h - 1, x, w);
	} else {
		A.fesc_x1 = 0;
		const char *hint = wide() ? " fiLter  PgUp/PgDn " : " fiLter  TAB=Folders  PgUp/PgDn ";
		int tw = (int)strlen(hint);
		if (tw + 4 > w) {
			hint = wide() ? " PgUp/PgDn " : " TAB=Folders  PgUp/PgDn ";
			tw = (int)strlen(hint);
		}
		ui_box_hint(y, x, h, w, hint, CP_NORMAL);
		if (hint[1] == 'f' && tw + 4 <= w)       /* the L of fiLter in the hotkey color */
			ui_puts(y + h - 1, x + w - 2 - tw + 3, "L", 1, A_BOLD, CP_HOTKEY);
	}

	int rows = h - 2, inner = w - 2;
	A.mrows = rows;
	if (A.mfree) clamp_top(&A.mtop, rows, nrows());
	else ensure_visible(&A.msel, &A.mtop, rows, nrows());

	/* columns: flag | subject | from | date (YYMMDD.HH:MM) */
	int datew = 12;
	int fromw = inner >= 70 ? 22 : inner >= 48 ? 16 : 0;
	int subjw = inner - 2 - 1 - datew - (fromw ? fromw + 1 : 0);
	if (subjw < 4) { fromw = 0; subjw = inner - 3 - datew; }

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
		char flag[3] = {m->deleted ? 'D' : !m->seen ? 'N' : m->answered ? 'A' : m->flagged ? '*' : ' ', ' ', 0};
		char date[32] = "";
		if (m->date) {
			struct tm tm;
			localtime_r(&m->date, &tm);
			strftime(date, sizeof date, "%y%m%d.%H:%M", &tm);
		}
		ui_puts(yy, xx, flag, 2, a, pair);
		xx += 2;
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
	ui_putw(y, x, e, w, 0, CP_NORMAL);
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

/* Scroll bar in the right border: ▲, a track with the thumb, ▼. */
static void draw_scrollbar(int x, int y, int rows)
{
	sb.h = 0;
	if (!A.mv.open || A.nlines <= rows || rows < 4) return;
	sb.x = x;
	sb.y = y;
	sb.h = rows;
	sb.t0 = y + 1;
	sb.tlen = rows - 2;
	sb.tsize = sb.tlen * rows / A.nlines;
	if (sb.tsize < 1) sb.tsize = 1;
	int range = A.nlines - rows, free_rows = sb.tlen - sb.tsize;
	sb.th = sb.t0 + (range > 0 ? (A.vtop * free_rows + range / 2) / range : 0);
	ui_puts(y, x, "▲", 1, 0, CP_NORMAL);
	ui_puts(y + rows - 1, x, "▼", 1, 0, CP_NORMAL);
	for (int r = 0; r < sb.tlen; r++) {
		int on = sb.t0 + r >= sb.th && sb.t0 + r < sb.th + sb.tsize;
		ui_puts(sb.t0 + r, x, on ? "█" : "░", 1, 0, CP_NORMAL);
	}
}

/* Scroll so that the thumb's top is at screen row `row`. */
static void scroll_thumb_to(int row)
{
	int free_rows = sb.tlen - sb.tsize, range = A.nlines - A.vrows;
	int p = row - sb.t0;
	if (p < 0) p = 0;
	if (p > free_rows) p = free_rows;
	A.vtop = free_rows > 0 ? (p * range + free_rows / 2) / free_rows : 0;
}

/* A press on the scroll bar: arrows scroll a line, the track a page, the thumb
 * starts a drag. Returns 0 when (x, y) is not on the scroll bar. */
static int scrollbar_press(int x, int y)
{
	if (!sb.h || x != sb.x || y < sb.y || y >= sb.y + sb.h) return 0;
	int page = A.vrows > 1 ? A.vrows - 1 : 1;
	if (y == sb.y) A.vtop--;
	else if (y == sb.y + sb.h - 1) A.vtop++;
	else if (y < sb.th) A.vtop -= page;
	else if (y >= sb.th + sb.tsize) A.vtop += page;
	else {
		sb.grab = y - sb.th;
		dragging = DRAG_THUMB;
		ui_mouse_drag(1);
	}
	return 1;
}

static void draw_message(int x, int y, int w, int h, int focused)
{
	int rows = h - 2, inner = w - 2;
	A.vrows = rows;

	if (A.mv.open) {
		if (A.hex) {
			int per = hex_per_line(inner);
			A.nlines = (int)((A.mv.size + (uint32_t)per - 1) / (uint32_t)per);
			A.lines_width = -1;   /* text index must be rebuilt when leaving hex */
		} else {
			build_text(inner);
		}
	}
	if (A.vtop > A.nlines - rows) A.vtop = A.nlines - rows;
	if (A.vtop < 0) A.vtop = 0;

	char title[128];
	const char *what = A.hex ? "Hex" : A.show_headers ? "Message, all headers" : "Message";
	if (A.mv.open && A.nlines > rows)
		snprintf(title, sizeof title, " %s  %d%% ", what, (A.vtop + rows) * 100 / A.nlines);
	else
		snprintf(title, sizeof title, " %s ", what);
	ui_box(y, x, h, w, CP_NORMAL, title, A_BOLD, focused ? CP_HIGHLIGHT : CP_NORMAL);
	ui_box_hint(y, x, h, w, " Spc=PgDn Shift+Spc/Bksp=PgUp ", CP_NORMAL);
	draw_scrollbar(x + w - 1, y + 1, rows);

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

static void draw_column(int col, int x, int y, int w, int h)
{
	int focused = A.focus == col;
	if (col == COL_FOLDERS) draw_folders(x, y, w, h, focused);
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

	const char *msg = *A.status ? A.status
	                  : wide() ? "F1 Help  ←→ Column  Enter Open  L Filter  F5 Refresh  Q/^Q Quit"
	                           : "F1 Help  Tab Folders/Emails  ←→ Column  Enter Open  L Filter  F5 Refresh  Q/^Q Quit";
	ui_puts(y, lw + 1, msg, COLS - lw - 1, 0, CP_MENU);
}

/* More columns than this: Folders | List | Message side by side (no Tab). */
#define WIDE_COLS 150

static int wide(void) { return COLS > WIDE_COLS; }

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/* The visible columns, left to right: what each shows, where and how wide.
 * Two columns: the email list or the folders (Tab), then the message.
 * Wide: Folders | List | Message. col_pct[0] is the folder column's width in
 * % of the screen; col_pct[1] the list's: of the screen in two columns, of
 * what the folders leave in three. */
typedef struct { int n, col[3], x[3], w[3]; } cols_t;

static void get_cols(cols_t *c)
{
	memset(c, 0, sizeof *c);
	if (wide()) {
		int fw = clampi(COLS * col_pct[0] / 100, 12, COLS - 40);
		int lw = clampi((COLS - fw) * col_pct[1] / 100, 12, COLS - fw - 20);
		c->n = 3;
		c->col[0] = COL_FOLDERS; c->x[0] = 0;       c->w[0] = fw;
		c->col[1] = COL_LIST;    c->x[1] = fw;      c->w[1] = lw;
		c->col[2] = COL_MSG;     c->x[2] = fw + lw; c->w[2] = COLS - fw - lw;
	} else {
		int lw = clampi(COLS * col_pct[A.left == COL_LIST] / 100, 12, COLS - 20);
		c->n = 2;
		c->col[0] = A.left;  c->x[0] = 0;  c->w[0] = lw;
		c->col[1] = COL_MSG; c->x[1] = lw; c->w[1] = COLS - lw;
	}
}

/* The width setting that border b (the left edge of visible column b) uses. */
static int *border_pct(int b)
{
	if (wide()) return &col_pct[b == 2];
	return &col_pct[A.left == COL_LIST];
}

/* Put border b at screen column x (the first column right of it). */
static void set_border(int b, int x)
{
	cols_t c;
	get_cols(&c);
	if (wide() && b == 2) {
		int rest = COLS - c.x[1];
		col_pct[1] = clampi(((x - c.x[1]) * 100 + rest / 2) / rest, 5, 95);
	} else {
		*border_pct(b) = clampi((x * 100 + COLS / 2) / COLS, 5, 95);
	}
}

/* The border at screen column x (either side of the line), or 0. */
static int border_at(int x)
{
	cols_t c;
	get_cols(&c);
	for (int b = 1; b < c.n; b++)
		if (x == c.x[b] - 1 || x == c.x[b]) return b;
	return 0;
}

static void draw(void)
{
	A.need_list = A.need_more = A.need_hex = 0;
	sb.h = 0;                          /* set again if the message window has one */
	bkgdset(' ' | COLOR_PAIR(CP_NORMAL));
	erase();
	ui_menubar(folder_menu, (int)(sizeof folder_menu / sizeof *folder_menu),
	           (A.hex ? 1u << MENU_HEX : 0) | (A.show_headers ? 1u << MENU_HEADER : 0));

	int top = 1, h = LINES - 2;
	if (h >= 3 && COLS >= 20) {
		cols_t c;
		get_cols(&c);
		for (int i = 0; i < c.n; i++) draw_column(c.col[i], c.x[i], top, c.w[i], h);
		for (int i = 1; i < c.n; i++)
			if (c.col[i - 1] == COL_LIST && c.col[i] == COL_MSG) {
				/* "<>" in the middle of the border between list and message */
				ui_puts(top + h / 2, c.x[i] - 1, "<", 1, A_BOLD, CP_NORMAL);
				ui_puts(top + h / 2, c.x[i], ">", 1, A_BOLD, CP_NORMAL);
			}
	}
	draw_statusbar();
	refresh();
}

static void draw_bg(void) { draw(); }

static void login_bg(void)
{
	bkgdset(' ' | COLOR_PAIR(CP_NORMAL));
	erase();
	ui_menubar(NULL, 0, 0);
	ui_puts(0, 1, "emsg " EMSG_VERSION, 12, A_BOLD, CP_MENU);
	ui_fill(LINES - 1, 0, 1, COLS, CP_MENU);
}

/* ------------------------------------------------------------------ */
/* Actions                                                             */

static void now_status(const char *fmt, ...) __attribute__((format(EMSG_PRINTF, 1, 2)));
static void now_status(const char *fmt, ...)
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

static void load_selected(void)
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

static void select_msg(int i)
{
	if (nrows() == 0) return;
	if (i < 0) i = 0;
	if (i >= nrows()) i = nrows() - 1;
	if (i != A.msel || !A.mv.open) A.pending_load = 1;
	A.msel = i;
	A.mfree = 0;
}

static void filter_off(void)
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
static void sync_changes(void)
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
static void filter_leave(void)
{
	A.fedit = 0;
	A.filter[0] = '\0';
	if (A.fon) filter_apply();
}

/* Type the filter text in the bar; Enter applies it, Esc leaves the filter. */
static void filter_edit(void);

static void open_folder(int idx)
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

static void refresh_folder(int quiet)
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

static int work_pending(void)
{
	return !A.work_failed && (A.need_list || A.need_hex || A.need_more);
}

/* One piece of loading the last draw asked for. */
static void do_work(void)
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

static void show_help(void)
{
	int n = 0;
	while (HELP_KEYS[n]) n++;
	char **l = xcalloc((size_t)n + 1, sizeof *l);
	for (int i = 0; i < n; i++) {
		buf_t b = {0};
		const char *s = HELP_KEYS[i], *v;
		while ((v = strstr(s, "@VERSION@"))) {
			buf_add(&b, s, (size_t)(v - s));
			buf_adds(&b, EMSG_VERSION);
			s = v + 9;
		}
		buf_adds(&b, s);
		l[i] = buf_steal(&b);
	}
	ui_text_dialog("Help", (const char *const *)l, n, draw_bg);
	for (int i = 0; i < n; i++) free(l[i]);
	free(l);
}

static void focus_to(int col)
{
	if (col < COL_FOLDERS) col = COL_FOLDERS;
	if (col > COL_MSG) col = COL_MSG;
	A.focus = col;
	if (col != COL_MSG) A.left = col;       /* two columns: show it on the left */
	if (col == COL_MSG && A.pending_load) load_selected();
}

/* Mouse click at x, y inside the windows (below the menu bar, above the
 * bottom borders): select the email or folder under it and give that window
 * the focus; a second click on the same row within 400 ms opens it (as Enter). */
static void click_window(const cols_t *cl, int x, int y)
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
		} else if (col == COL_MSG) {
			focus_to(COL_MSG);
		}
		if (idx >= 0 && col == last_col && idx == last_idx && ms >= 0 && ms < 400) {
			if (col == COL_LIST) focus_to(COL_MSG);              /* double click: open */
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

static void filter_edit(void)
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

/* Returns 0 to quit. */
static int handle_key(int ch)
{
	set_status("%s", "");
	/* a key brings lists the wheel scrolled back to their selection */
	if (ch != KEY_RESIZE) A.mfree = A.ffree = 0;

	switch (ch) {
	case 'q': case 'Q':
		return 0;
	case KEY_F(1): case '?':
		show_help();
		return 1;
	case '\t': case KEY_BTAB:
		/* two columns: the left one switches between emails and folders;
		 * wide screens show both, so Tab does nothing there */
		if (!wide()) focus_to(A.left == COL_LIST ? COL_FOLDERS : COL_LIST);
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
		int b = wide() && A.focus == COL_FOLDERS ? 1 : wide() ? 2 : 1;
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
	 * Alt-Space and Backspace page it up. (Many terminals send Shift-Space as
	 * a plain space; Ctrl-Space arrives as NUL, Alt-Space as Esc Space.) */
	if (ch == ' ' || ch == ui_key_sspace || ch == 0 || ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
		int page = A.vrows > 1 ? A.vrows - 1 : 1;
		A.vtop += ch == ' ' ? page : -page;
		return 1;
	}

	if (A.focus == COL_FOLDERS) {
		switch (ch) {
		case KEY_UP: A.fsel--; break;
		case KEY_DOWN: A.fsel++; break;
		case KEY_HOME: A.fsel = 0; break;
		case KEY_END: A.fsel = A.nfolders - 1; break;
		case '\n': case '\r': case KEY_ENTER: case KEY_RIGHT:
			if (A.fsel != A.cur_folder) open_folder(A.fsel);
			if (A.cur_folder == A.fsel) focus_to(COL_LIST);
			break;
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
		case KEY_LEFT: focus_to(COL_FOLDERS); break;
		}
	} else {
		switch (ch) {
		case KEY_UP: A.vtop--; break;
		case KEY_DOWN: A.vtop++; break;
		case KEY_HOME: A.vtop = 0; break;
		case KEY_END: A.vtop = A.nlines; break;
		case KEY_LEFT: focus_to(wide() ? COL_LIST : A.left); break;
		case 27: focus_to(COL_LIST); break;
		}
	}
	return 1;
}

/* ------------------------------------------------------------------ */
/* Untrusted certificates                                              */
/* ~/.config/emsg/certs holds "host port sha256" for certificates the   */
/* user chose to always accept.                                        */

/* 1 = this exact certificate is stored, -1 = a different one is stored
 * for this server (it changed), 0 = nothing stored. */
static int cert_lookup(const net_certinfo *ci)
{
	char p[1100], line[512], h[256], po[16], fp[128];
	config_path(p, sizeof p, "certs");
	FILE *f = fopen(p, "r");
	if (!f) return 0;
	int r = 0;
	while (fgets(line, sizeof line, f))
		if (sscanf(line, "%255s %15s %127s", h, po, fp) == 3 &&
		    !strcasecmp(h, ci->host) && !strcmp(po, ci->port)) {
			if (!strcasecmp(fp, ci->sha256)) { r = 1; break; }
			r = -1;
		}
	fclose(f);
	return r;
}

/* Store the certificate, replacing any earlier one for the same server. */
static int cert_store(const net_certinfo *ci)
{
	char p[1100], tmp[1110], line[512], h[256], po[16];
	config_mkdir();
	config_path(p, sizeof p, "certs");
	snprintf(tmp, sizeof tmp, "%s.new", p);
	FILE *out = fopen(tmp, "w");
	if (!out) return -1;
	chmod(tmp, 0600);
	FILE *in = fopen(p, "r");
	if (in) {
		while (fgets(line, sizeof line, in))
			if (!(sscanf(line, "%255s %15s", h, po) == 2 && !strcasecmp(h, ci->host) &&
			      !strcmp(po, ci->port)))
				fputs(line, out);
		fclose(in);
	} else {
		fputs("# certificates accepted in emsg: host port sha256-fingerprint\n", out);
	}
	fprintf(out, "%s %s %s\n", ci->host, ci->port, ci->sha256);
	int bad = ferror(out);
	if (fclose(out) != 0 || bad) { unlink(tmp); return -1; }
	return rename(tmp, p);
}

static int cert_check(const net_certinfo *ci, void *ctx)
{
	ui_bg_fn bg = ctx ? *(ui_bg_fn *)ctx : login_bg;
	int known = cert_lookup(ci);
	if (known == 1) return 1;

	char l[12][160];
	int n = 0;
	if (known < 0) {
		snprintf(l[n++], sizeof l[0], "THE CERTIFICATE OF %s:%s HAS CHANGED", ci->host, ci->port);
		snprintf(l[n++], sizeof l[0], "since you accepted it. Someone may be intercepting the connection.");
	} else {
		snprintf(l[n++], sizeof l[0], "The certificate of %s:%s is not trusted:", ci->host, ci->port);
	}
	snprintf(l[n++], sizeof l[0], "  %.150s", ci->problem);
	l[n++][0] = '\0';
	snprintf(l[n++], sizeof l[0], "Subject   %.140s", ci->subject);
	snprintf(l[n++], sizeof l[0], "Issuer    %.140s", ci->issuer);
	if (*ci->names) snprintf(l[n++], sizeof l[0], "Names     %.140s", ci->names);
	snprintf(l[n++], sizeof l[0], "Valid     %s to %s", ci->not_before, ci->not_after);
	snprintf(l[n++], sizeof l[0], "SHA-256   %.47s", ci->sha256);
	snprintf(l[n++], sizeof l[0], "          %s", strlen(ci->sha256) > 48 ? ci->sha256 + 48 : "");
	l[n++][0] = '\0';
	snprintf(l[n++], sizeof l[0], "Accept only if you know this is the server's real certificate.");
	const char *lines[12];
	for (int i = 0; i < n; i++) lines[i] = l[i];

	static const ui_menuitem btn[] = {
		{"Reject", 0, 'r'}, {"Accept once", 7, 'o'}, {"Always accept", 0, 'a'},
	};
	int c = ui_choice_dialog(known < 0 ? "WARNING: certificate changed" : "Certificate warning",
	                         lines, n, btn, 3, 0, 1, bg);
	if (c == 2 && cert_store(ci) != 0)
		ui_message("Certificate", "Could not save the certificate; accepted for this session only.",
		           bg);
	return c == 1 || c == 2;
}

/* ------------------------------------------------------------------ */
/* Composing and sending                                               */

#define USER_AGENT "emsg/" EMSG_VERSION

static ui_bg_fn smtp_bg = NULL;      /* dialog background while sending */

/* The mail domain of the IMAP server: its name without a leading
 * "imap." / "imaps." / "mail." label, lower case. */
static void mail_domain(char *out, size_t n)
{
	const char *d = ac->host, *at = strrchr(ac->user, '@');
	if (at && (ac->type == STORE_LOCAL || !*d)) d = at + 1;
	static const char *const pre[] = {"imap.", "imaps.", "pop.", "pop3.", "mail.", NULL};
	for (int i = 0; pre[i]; i++)
		if (!strncasecmp(d, pre[i], strlen(pre[i])) && strchr(d + strlen(pre[i]), '.')) {
			d += strlen(pre[i]);
			break;
		}
	snprintf(out, n, "%s", d);
	str_lower(out);
}

/* Our address, derived from the login: the login name if it is an
 * address, else login@mail-domain. */
static void login_address(char *out, size_t n)
{
	if (strchr(ac->user, '@') || ac->type == STORE_LOCAL) {   /* local: as set, maybe empty */
		snprintf(out, n, "%s", ac->user);
		return;
	}
	char dom[256];
	mail_domain(dom, sizeof dom);
	snprintf(out, n, "%.200s@%.250s", ac->user, dom);
}

/* SMTP settings follow the login: server smtp.<domain> (or the IMAP host),
 * the same user and password. Settings changed with C are kept only while
 * the same account is used. */
static void smtp_derive(void)
{
	char login[600];
	snprintf(login, sizeof login, "%.250s@%.300s", ac->user, ac->host);
	if (*ac->smtp_host && !strcmp(ac->smtp_login, login)) return;
	const char *at = strrchr(ac->user, '@');
	if (ac->type == STORE_LOCAL)
		snprintf(ac->smtp_host, sizeof ac->smtp_host, "%s%.240s", at ? "smtp." : "", at ? at + 1 : "");
	else if (!strncasecmp(ac->host, "imap.", 5) || !strncasecmp(ac->host, "imaps.", 6) ||
	         !strncasecmp(ac->host, "pop.", 4) || !strncasecmp(ac->host, "pop3.", 5))
		snprintf(ac->smtp_host, sizeof ac->smtp_host, "smtp.%.240s", strchr(ac->host, '.') + 1);
	else
		snprintf(ac->smtp_host, sizeof ac->smtp_host, "%s", ac->host);
	snprintf(ac->smtp_port, sizeof ac->smtp_port, "587");
	snprintf(ac->smtp_user, sizeof ac->smtp_user, "%s", ac->user);
	ac->smtp_security[0] = '\0';
	snprintf(ac->smtp_login, sizeof ac->smtp_login, "%s", login);
}

/* ------------------------------------------------------------------ */
/* Accounts and stored passwords                                       */

#define MASTER_AAD "emsg master password check"

/* What a stored password belongs to; it is authenticated with it, so it
 * only decrypts for the same user on the same server (vault.h). */
static void aad_imap(const account_t *a, char *o, size_t n)
{
	snprintf(o, n, "%s %.250s@%.300s", a->type == STORE_POP3 ? "pop3" : "imap", a->user, a->host);
}

static void aad_smtp(const account_t *a, char *o, size_t n)
{
	snprintf(o, n, "smtp %.250s@%.300s", a->smtp_user, a->smtp_host);
}

/* Make sure the master password is known: asked once per session, checked
 * against master_check; the first time it is set (typed twice). 0 = known. */
static int get_master(const char *why, ui_bg_fn bg)
{
	if (master) return 0;
	int fresh = !master_check;
	char pw[256] = "", again[256] = "";
	const char *err = NULL;
	int rc = -1;
	for (;;) {
		ui_field f[] = {
			{"Password", pw, sizeof pw, 1, 0},
			{"Repeat", again, sizeof again, 1, 0},
		};
		if (ui_input_dialog(fresh ? "New master password" : "Master password", f, fresh ? 2 : 1,
		                    err ? err : why, bg) < 0)
			break;
		if (fresh) {
			if (strcmp(pw, again)) err = "The two passwords are not the same";
			else if (strlen(pw) < 8) err = "Use at least 8 characters";
			else {
				ui_progress("Encrypting ...", bg);
				char *chk = vault_encrypt(pw, MASTER_AAD, "emsg");
				if (!chk) err = "Encryption failed";
				else {
					set_secret(&master_check, chk);
					rc = 0;
				}
			}
		} else {
			ui_progress("Checking ...", bg);
			char *ok = vault_decrypt(pw, MASTER_AAD, master_check);
			if (ok) rc = 0;
			else err = "Wrong master password";
			vault_free(ok);
		}
		if (rc == 0) {
			master = xstrdup(pw);
			break;
		}
		memset(pw, 0, sizeof pw);
		memset(again, 0, sizeof again);
	}
	memset(pw, 0, sizeof pw);
	memset(again, 0, sizeof again);
	return rc;
}

/* Store the IMAP password (and a separate SMTP password, if one was
 * given) of the current account encrypted in the config file. */
static int store_password(const char *pass, ui_bg_fn bg)
{
	if (get_master("It encrypts the passwords stored on disk", bg) != 0) return -1;
	ui_progress("Encrypting ...", bg);
	char aad[600];
	aad_imap(ac, aad, sizeof aad);
	char *e = vault_encrypt(master, aad, pass);
	if (!e) return -1;
	set_secret(&ac->pass_enc, e);
	if (A.smtp_pass) {
		aad_smtp(ac, aad, sizeof aad);
		set_secret(&ac->smtp_pass_enc, vault_encrypt(master, aad, A.smtp_pass));
	}
	ac->store = STORE_YES;
	config_save();
	return 0;
}

static void forget_passwords(void)
{
	set_secret(&ac->pass_enc, NULL);
	set_secret(&ac->smtp_pass_enc, NULL);
	ac->store = STORE_NO;
	config_save();
}

/* After a login with a typed password: store it if wanted. */
static void offer_store(const char *pass)
{
	if (ac->store == STORE_NO) return;
	if (ac->store == STORE_YES) {             /* new or changed password */
		store_password(pass, login_bg);
		return;
	}
	char addr[512], l0[640];
	login_address(addr, sizeof addr);
	snprintf(l0, sizeof l0, "Store password for %s encrypted (scrypt AES-256-GCM) on disk?", addr);
	const char *lines[] = {l0};
	static const ui_menuitem b[] = {
		{"Store encrypted", 0, 's'}, {"Not now", 0, 'n'}, {"Don't ask again", 0, 'd'},
	};
	int c = ui_choice_dialog("Password", lines, 1, b, 3, 1, 0, login_bg);
	if (c == 0) {
		if (store_password(pass, login_bg) != 0)
			ui_message("Password", "The password was not stored.", login_bg);
	} else if (c == 2) {
		ac->store = STORE_NO;
		config_save();
	}
}

/* Decrypt the stored IMAP password. 0 = pass filled in. */
static int unlock_password(char *pass, size_t n, const char **err)
{
	if (!ac->pass_enc) return -1;
	char why[120];
	snprintf(why, sizeof why, "For the stored password of %.40s", ac->name);
	if (get_master(why, login_bg) != 0) return -1;
	ui_progress("Decrypting ...", login_bg);
	char aad[600];
	aad_imap(ac, aad, sizeof aad);
	char *pw = vault_decrypt(master, aad, ac->pass_enc);
	if (!pw) {
		*err = "Stored password is for another server/user; type it";
		return -1;
	}
	snprintf(pass, n, "%s", pw);
	vault_free(pw);
	return 0;
}

/* The account list. Returns an index, naccts for "New account", -1 for Esc. */
static int pick_account(const char *title, int def, int mark, ui_bg_fn bg)
{
	char (*txt)[200] = xmalloc(sizeof *txt * (size_t)(naccts + 1));
	const char **items = xmalloc(sizeof *items * (size_t)(naccts + 1));
	int nw = 4;
	for (int i = 0; i < naccts; i++)
		if ((int)strlen(accts[i].name) > nw) nw = (int)strlen(accts[i].name);
	if (nw > 30) nw = 30;
	for (int i = 0; i < naccts; i++) {
		const account_t *a = &accts[i];
		if (a->type == STORE_LOCAL)
			snprintf(txt[i], sizeof txt[0], "%c %-*.*s  %.120s", mark && a == ac ? '*' : ' ', nw, nw,
			         a->name, a->path);
		else
			snprintf(txt[i], sizeof txt[0], "%c %-*.*s  %.60s@%.60s%s", mark && a == ac ? '*' : ' ', nw,
			         nw, a->name, a->user, a->host, a->type == STORE_POP3 ? " (POP3)" : "");
		items[i] = txt[i];
	}
	snprintf(txt[naccts], sizeof txt[0], "  New account ...");
	items[naccts] = txt[naccts];
	int r = ui_list_dialog(title, items, naccts + 1, def, "Enter choose   Esc cancel", bg);
	free(items);
	free(txt);
	return r;
}

/* Name, full name and the stored password of the current account. */
static void account_settings(void)
{
	char name[64], full[128], addr[256];
	snprintf(name, sizeof name, "%s", ac->name);
	snprintf(full, sizeof full, "%s", ac->fullname);
	snprintf(addr, sizeof addr, "%s", ac->user);
	int local = ac->type == STORE_LOCAL;
	const char *err = local ? "Full name and address are used in From:"
	                        : "Full name is shown in From: (may be empty)";
	for (;;) {
		ui_field f[] = {
			{"Account", name, sizeof name, 0, 0},
			{"Full name", full, sizeof full, 0, 1},
			{"E-mail address", addr, sizeof addr, 0, 1},
		};
		if (ui_input_dialog("Account settings", f, local ? 3 : 2, err, draw_bg) < 0) return;
		const account_t *o = account_find(name);
		if (o && o != ac) { err = "Another account has that name"; continue; }
		if (strchr(name, ']')) { err = "The name cannot contain ']'"; continue; }
		break;
	}
	snprintf(ac->name, sizeof ac->name, "%s", str_trim(name));
	snprintf(ac->fullname, sizeof ac->fullname, "%s", str_trim(full));
	if (local) snprintf(ac->user, sizeof ac->user, "%s", str_trim(addr));
	config_save();
	if (ac->type == STORE_LOCAL) {
		set_status("Account %s saved", ac->name);
		return;
	}

	char l0[120];
	snprintf(l0, sizeof l0, "The password of %.60s is %s.", ac->name,
	         ac->pass_enc ? "stored encrypted on disk" : "not stored");
	const char *lines[] = {l0};
	if (ac->pass_enc) {
		static const ui_menuitem b[] = {{"Keep it", 0, 'k'}, {"Forget it", 0, 'f'}};
		if (ui_choice_dialog("Password", lines, 1, b, 2, 0, 0, draw_bg) == 1) {
			forget_passwords();
			set_status("Stored password removed from %s", ac->name);
			return;
		}
	} else {
		static const ui_menuitem b[] = {{"Leave it", 0, 'l'}, {"Store encrypted", 0, 's'}};
		if (ui_choice_dialog("Password", lines, 1, b, 2, 0, 0, draw_bg) == 1) {
			if (store_password(store_pass(A.im), draw_bg) == 0) {
				set_status("Password stored encrypted");
				return;
			}
			set_status("Password not stored");
			return;
		}
	}
	set_status("Account %s saved", ac->name);
}

/* Restart emsg on another account (name NULL = a new one). */
static void switch_account(const char *name)
{
	char nm[64];
	snprintf(nm, sizeof nm, "%s", name ? name : "");
	config_save();
	ui_end();
	store_logout(A.im);
	vault_free(master);
	master = NULL;
#ifdef _WIN32
	/* no exec: run emsg again in this console and wait for it */
	char cmd[1200];
	snprintf(cmd, sizeof cmd, "\"%s/emsg.exe\" %s", win_exe_dir(), name ? "-a \"" : "-n");
	if (name) {
		size_t k = strlen(cmd);
		for (const char *c = nm; *c && k + 3 < sizeof cmd; c++)
			if (*c != '"') cmd[k++] = *c;
		cmd[k++] = '"';
		cmd[k] = '\0';
	}
	int rc = win_run(cmd);
	if (rc < 0) perror("emsg: restart");
	exit(rc < 0 ? 1 : rc);
#else
	char *argv[] = {"emsg", name ? "-a" : "-n", name ? nm : NULL, NULL};
	execv("/proc/self/exe", argv);
	perror("emsg: restart");
	exit(1);
#endif
}

/* Remove every stored password and the master password check. */
static void delete_all_passwords(void)
{
	int n = 0;
	for (int i = 0; i < naccts; i++) n += (accts[i].pass_enc != NULL) + (accts[i].smtp_pass_enc != NULL);
	char l0[120];
	if (n == 1) snprintf(l0, sizeof l0, "Delete the stored password?");
	else snprintf(l0, sizeof l0, "Delete all %d stored passwords of all accounts?", n);
	const char *lines[] = {
		l0,
		"The master password is forgotten too; you can choose a new one",
		"the next time you store a password.",
	};
	static const ui_menuitem b[] = {{"Cancel", 0, 'c'}, {"Delete all", 0, 'd'}};
	if (ui_choice_dialog("Delete passwords", lines, 3, b, 2, 0, 1, draw_bg) != 1) return;
	for (int i = 0; i < naccts; i++) {
		set_secret(&accts[i].pass_enc, NULL);
		set_secret(&accts[i].smtp_pass_enc, NULL);
		if (accts[i].store == STORE_YES) accts[i].store = STORE_ASK;
	}
	set_secret(&master_check, NULL);
	vault_free(master);
	master = NULL;
	config_save();
	set_status("All stored passwords deleted");
}

/* Config: the column at which new mail is wrapped while typing. */
static void wrap_settings(void)
{
	char v[8];
	snprintf(v, sizeof v, "%d", wrap_width);
	const char *err = "Columns, 20-998 (72 is usual for e-mail)";
	for (;;) {
		ui_field f[] = {{"Wrap at", v, sizeof v, 0, 0}};
		if (ui_input_dialog("Wrap width", f, 1, err, draw_bg) < 0) return;
		int w = atoi(v);
		if (strspn(v, "0123456789") == strlen(v) && w >= 20 && w <= 998) {
			wrap_width = w;
			config_save();
			set_status("New mail is wrapped at %d columns", w);
			return;
		}
		err = "A number from 20 to 998, please";
	}
}

/* The Config menu. */
static void config_menu(void)
{
	char acc[120];
	snprintf(acc, sizeof acc, "Account %.60s: name, full name, password ...", ac->name);
	char ww[80];
	snprintf(ww, sizeof ww, "Wrap width for new mail: %d ...", wrap_width);
	const char *items[] = {
		"Outgoing mail (SMTP) ...",
		acc,
		"Switch or add account ...",
		ww,
		"Delete all stored passwords ...",
	};
	switch (ui_list_dialog("Config", items, 5, 0, "Enter choose   Esc cancel", draw_bg)) {
	case 0: smtp_settings(NULL, draw_bg); break;
	case 1: account_settings(); break;
	case 2: account_menu(); break;
	case 3: wrap_settings(); break;
	case 4: delete_all_passwords(); break;
	}
}

static void account_menu(void)
{
	int cur = (int)(ac - accts);
	int i = pick_account("Accounts", cur, 1, draw_bg);
	if (i < 0) return;
	if (i == cur) account_settings();
	else if (i == naccts) switch_account(NULL);
	else switch_account(accts[i].name);
}

/* From: "Full Name <address>", the name quoted when it needs to be. */
static void from_line(char *out, size_t n)
{
	char addr[512];
	login_address(addr, sizeof addr);
	if (!*addr) {                 /* not known yet: the user fills it in */
		snprintf(out, n, "%s", "");
		return;
	}
	if (!*ac->fullname) {
		snprintf(out, n, "%s", addr);
		return;
	}
	int plain = 1;
	for (const unsigned char *p = (const unsigned char *)ac->fullname; *p; p++)
		if (!(isalnum(*p) || *p >= 0x80 || *p == ' ' || strchr("!#$%&'*+-/=?^_`{|}~", *p)))
			plain = 0;
	buf_t b = {0};
	if (plain) buf_adds(&b, ac->fullname);
	else {
		buf_addc(&b, '"');
		for (const char *p = ac->fullname; *p; p++) {
			if (*p == '"' || *p == '\\') buf_addc(&b, '\\');
			buf_addc(&b, *p);
		}
		buf_addc(&b, '"');
	}
	snprintf(out, n, "%s <%s>", buf_cstr(&b), addr);
	buf_free(&b);
}

/* Ask for the SMTP settings. Returns 0 when filled in. */
static int smtp_settings(const char *err, ui_bg_fn bg)
{
	char host[256], port[16], user[256], pass[256] = "";
	smtp_derive();
	snprintf(host, sizeof host, "%s", ac->smtp_host);
	snprintf(port, sizeof port, "%s", ac->smtp_port);
	snprintf(user, sizeof user, "%s", ac->smtp_user);

	for (;;) {
		ui_field f[] = {
			{"SMTP server", host, sizeof host, 0, 0},
			{"Port", port, sizeof port, 0, 0},
			{"User", user, sizeof user, 0, 0},
			{"Password", pass, sizeof pass, 1, 1},
		};
		const char *msg = err ? err : "Port 587 STARTTLS, 465 TLS. No password: IMAP's";
		if (ui_input_dialog("Outgoing mail (SMTP)", f, 4, msg, bg) < 0) {
			memset(pass, 0, sizeof pass);
			return -1;
		}
		if (*host && *port && strspn(port, "0123456789") == strlen(port)) break;
		err = "Server and a numeric port are needed";
	}
	snprintf(ac->smtp_host, sizeof ac->smtp_host, "%s", host);
	snprintf(ac->smtp_port, sizeof ac->smtp_port, "%s", port);
	snprintf(ac->smtp_user, sizeof ac->smtp_user, "%s", user);
	if (A.smtp_pass) {
		memset(A.smtp_pass, 0, strlen(A.smtp_pass));
		free(A.smtp_pass);
		A.smtp_pass = NULL;
	}
	if (*pass) A.smtp_pass = xstrdup(pass);
	memset(pass, 0, sizeof pass);
	set_secret(&ac->smtp_pass_enc, NULL);
	if (A.smtp_pass && ac->store == STORE_YES &&
	    get_master("It encrypts the stored SMTP password", bg) == 0) {
		char aad[600];
		aad_smtp(ac, aad, sizeof aad);
		ac->smtp_pass_enc = vault_encrypt(master, aad, A.smtp_pass);
	}
	config_save();
	return 0;
}

/* Find the folder with a special-use attribute, or by a usual name. */
static const folder_t *special_folder(int kind)
{
	for (int i = 0; i < A.nfolders; i++)
		if (A.folders[i].special == kind && !A.folders[i].noselect) return &A.folders[i];
	static const char *const sent[] = {"Sent", "Sent Items", "Sent Messages", "Sent Mail",
	                                   "INBOX.Sent", "INBOX/Sent", NULL};
	static const char *const drafts[] = {"Drafts", "Draft", "INBOX.Drafts", "INBOX/Drafts", NULL};
	const char *const *names = kind == FOLDER_SENT ? sent : drafts;
	for (int k = 0; names[k]; k++)
		for (int i = 0; i < A.nfolders; i++) {
			char *u = mutf7_decode(A.folders[i].name);
			int hit = !strcasecmp(u, names[k]) && !A.folders[i].noselect;
			free(u);
			if (hit) return &A.folders[i];
		}
	return NULL;
}

typedef struct {
	char     auto_date[64];
	uint32_t reply_uid;            /* original message to flag \Answered, or 0 */
} compose_ctx;

static int save_to(int kind, const buf_t *msg, const char *flags, char *note, size_t nl)
{
	const folder_t *f = special_folder(kind);
	const char *what = kind == FOLDER_SENT ? "Sent" : "Drafts";
	if (!f) {
		snprintf(note, nl, "no %s folder found, no copy saved", what);
		return -1;
	}
	if (store_append(A.im, f->name, flags, msg->d, msg->len) != 0) {
		snprintf(note, nl, "could not save in %s: %s", f->display, store_error(A.im));
		return -1;
	}
	if (A.cur_folder >= 0 && !strcmp(A.folders[A.cur_folder].name, f->name)) A.last_poll = 0;
	snprintf(note, nl, "%s", f->display);
	return 0;
}

static int keep_cb(void *ctx, const char *text, size_t len, int *line, char *msg, size_t ml)
{
	compose_ctx *c = ctx;
	compose_opts o = {.user_agent = USER_AGENT, .auto_date = c->auto_date,
	                  .smtputf8 = 1, .eightbit = 1, .draft = 1};
	compose_out out;
	if (compose_build(text, len, &o, &out) != 0) {
		snprintf(msg, ml, "%s", out.err);
		*line = out.err_line;
		return 0;
	}
	ui_progress("Saving draft ...", editor_redraw);
	char note[300];
	int rc = save_to(FOLDER_DRAFTS, &out.copy, "\\Draft \\Seen", note, sizeof note);
	compose_out_free(&out);
	if (rc != 0) {
		snprintf(msg, ml, "Draft not saved: %s", note);
		return 0;
	}
	set_status("Draft saved in %s", note);
	return 1;
}

static int send_cb(void *ctx, const char *text, size_t len, int *line, char *msg, size_t ml)
{
	compose_ctx *c = ctx;
	compose_opts o = {.user_agent = USER_AGENT, .auto_date = c->auto_date,
	                  .smtputf8 = 1, .eightbit = 1};
	compose_out out;

	/* 1. syntax check before anything goes over the network */
	if (compose_build(text, len, &o, &out) != 0) {
		snprintf(msg, ml, "Not sent: %s", out.err);
		*line = out.err_line;
		return 0;
	}
	char who[300] = "";
	for (int i = 0; i < out.nrcpt && strlen(who) < 200; i++)
		snprintf(who + strlen(who), sizeof who - strlen(who), "%s%s", i ? ", " : "", out.rcpt[i]);
	char q[4][400];
	const char *ql[4];
	int nq = 0;
	snprintf(q[nq], sizeof q[0], "Send to %s%s?", who, strlen(who) >= 200 ? " ..." : "");
	ql[nq] = q[nq];
	nq++;
	if (out.nattach) {
		char hs[32];
		mime_human_size(hs, sizeof hs, out.attach_bytes);
		snprintf(q[nq], sizeof q[0], "With %d attachment%s (%s).", out.nattach,
		         out.nattach == 1 ? "" : "s", hs);
		ql[nq] = q[nq];
		nq++;
	}
	if (out.no_subject) {
		snprintf(q[nq], sizeof q[0], "Note: the message has no subject.");
		ql[nq] = q[nq];
		nq++;
	}
	char me[512];
	login_address(me, sizeof me);
	if (strcasecmp(out.mail_from, me)) {
		/* the server will usually refuse a sender that is not the login */
		snprintf(q[nq], sizeof q[0], "Note: sender %.150s is not your login address %.150s.",
		         out.mail_from, me);
		ql[nq] = q[nq];
		nq++;
	}
	compose_out_free(&out);
	static const ui_menuitem yn[] = {{"Send", 0, 's'}, {"Cancel", 0, 'c'}};
	if (ui_choice_dialog("Send message", ql, nq, yn, 2, 0, 0, editor_redraw) != 0)
		return 0;

	/* 2. connect */
	smtp_derive();
	smtp_bg = editor_redraw;
	A.smtp_trust.fn = cert_check;
	A.smtp_trust.ctx = &smtp_bg;
	smtp_caps caps;
	smtp_t *s = NULL;
	char err[512];
	for (;;) {
		char pm[400];
		snprintf(pm, sizeof pm, "Connecting to %s:%s ...", ac->smtp_host, ac->smtp_port);
		ui_progress(pm, editor_redraw);
		if (!A.smtp_pass && ac->smtp_pass_enc &&
		    get_master("For the stored SMTP password", editor_redraw) == 0) {
			char aad[600];
			aad_smtp(ac, aad, sizeof aad);
			A.smtp_pass = vault_decrypt(master, aad, ac->smtp_pass_enc);
		}
		const char *pass = A.smtp_pass ? A.smtp_pass : store_pass(A.im);
		int implicit = *ac->smtp_security ? !strcasecmp(ac->smtp_security, "tls")
		                                : !strcmp(ac->smtp_port, "465");
		s = smtp_open(ac->smtp_host, ac->smtp_port, implicit, ac->smtp_user, pass, &A.smtp_trust, 1,
		              &caps, err, sizeof err);
		if (s) break;
		if (smtp_settings(err, editor_redraw) != 0) {
			snprintf(msg, ml, "Not sent: %s", err);
			return 0;
		}
	}

	/* 3. build for this server: SMTPUTF8 / 8BITMIME decide the encodings */
	o.smtputf8 = caps.smtputf8;
	o.eightbit = caps.eightbitmime;
	if (compose_build(text, len, &o, &out) != 0) {
		smtp_close(s);
		snprintf(msg, ml, "Not sent: %s", out.err);
		*line = out.err_line;
		return 0;
	}
	ui_progress("Sending ...", editor_redraw);
	int rc = smtp_send(s, out.mail_from, out.rcpt, out.nrcpt, out.msg.d, out.msg.len, out.smtputf8,
	                   out.eightbit, err, sizeof err);
	smtp_close(s);
	if (rc != 0) {
		compose_out_free(&out);
		snprintf(msg, ml, "Not sent: %s", err);
		return 0;
	}

	/* 4. copy to Sent, flag the original as answered */
	ui_progress("Saving a copy ...", editor_redraw);
	char note[300];
	int saved = save_to(FOLDER_SENT, &out.copy, "\\Seen", note, sizeof note);
	if (c->reply_uid) store_add_flags(A.im, c->reply_uid, "\\Answered");
	set_status("Sent to %s; %s%s", who, saved == 0 ? "copy saved in " : "", note);
	compose_out_free(&out);
	return 1;
}

static void add_hdr(buf_t *b, const char *name, const char *v)
{
	buf_printf(b, "%s: %s\n", name, v ? v : "");
}

/* Quote one unwrapped (paragraph) line of a flowed message: one more '>'
 * and the text wrapped at 72 columns, each soft break ending in a space. */
static void quote_flowed(buf_t *t, const char *l, size_t n)
{
	size_t d = 0;
	while (d < n && l[d] == '>') d++;
	const char *s = l + d;
	size_t sl = n - d;
	if (sl && *s == ' ') { s++; sl--; }
	while (sl && (s[sl - 1] == ' ' || s[sl - 1] == '\r')) sl--;   /* also "-- " */
	char pre[40];
	size_t pl = d + 1 < 30 ? d + 1 : 30;
	memset(pre, '>', pl);
	pre[pl] = '\0';
	if (!sl) {
		buf_printf(t, "%s\n", pre);
		return;
	}
	int width = 72 - (int)pl - 1;
	if (width < 30) width = 30;
	while (sl) {
		/* take characters up to the width, then back up to a space */
		size_t i = 0, last_sp = 0;
		int col = 0;
		while (i < sl && col < width) {
			if (s[i] == ' ') last_sp = i;
			size_t cl;
			utf8_next(s + i, sl - i, &cl);
			i += cl;
			col++;
		}
		size_t cut;
		if (i >= sl) cut = sl;
		else if (s[i] == ' ') cut = i + 1;
		else if (last_sp) cut = last_sp + 1;
		else {                                     /* one long word */
			while (i < sl && s[i] != ' ') i++;
			cut = i < sl ? i + 1 : sl;
		}
		buf_printf(t, "%s ", pre);
		buf_add(t, s, cut);                        /* ends in a space if soft */
		buf_addc(t, '\n');
		s += cut;
		sl -= cut;
	}
}

/* Start the editor: 'm' new mail, 'r' reply, 'f' forward. */
static void compose_start(int kind)
{
	compose_ctx c = {0};
	snprintf(c.auto_date, sizeof c.auto_date, "%s", compose_date(time(NULL)));
	char from[800];
	from_line(from, sizeof from);

	char *ohdr = NULL, *obody = NULL;
	size_t ohlen = 0;
	if (kind != 'm') {
		const msg_t *m = msg_at(A.msel);
		if (!m || !m->uid) {
			set_status("Select a message to %s first", kind == 'r' ? "reply to" : "forward");
			return;
		}
		if (!A.mv.open || A.mv.uid != m->uid) load_selected();
		if (!A.mv.open) return;
		ohdr = A.mv.hdr;
		ohlen = A.mv.hlen;
		obody = mv_body(&A.mv);
		if (kind == 'r') c.reply_uid = m->uid;
	}

	buf_t t = {0};
	int start = 1;                                   /* To: line */
	add_hdr(&t, "From", from);
	if (kind == 'r') {
		char *rt = mime_header_raw(ohdr, ohlen, "Reply-To");
		char *fr = mime_header_raw(ohdr, ohlen, "From");
		char *to = compose_addrs(rt ? rt : fr, NULL);
		add_hdr(&t, "To", to);
		free(to);
		free(rt);
		free(fr);
	} else {
		add_hdr(&t, "To", "");
	}
	add_hdr(&t, "Cc", "");
	add_hdr(&t, "Bcc", "");

	char *osub = kind != 'm' ? mime_header(ohdr, ohlen, "Subject") : NULL;
	if (kind == 'r') {
		buf_t sub = {0};
		if (!osub || strncasecmp(osub, "Re:", 3)) buf_adds(&sub, "Re: ");
		buf_adds(&sub, osub ? osub : "");
		add_hdr(&t, "Subject", sub.d);
		buf_free(&sub);
	} else if (kind == 'f') {
		buf_t sub = {0};
		buf_printf(&sub, "Fwd: %s", osub ? osub : "");
		add_hdr(&t, "Subject", sub.d);
		buf_free(&sub);
	} else {
		add_hdr(&t, "Subject", "");
	}
	add_hdr(&t, "Date", c.auto_date);

	if (kind == 'r') {
		char *mid = mime_header_raw(ohdr, ohlen, "Message-ID");
		char *refs = mime_header_raw(ohdr, ohlen, "References");
		if (mid && *mid) {
			add_hdr(&t, "In-Reply-To", mid);
			buf_t r = {0};
			if (refs && *refs) buf_printf(&r, "%s ", refs);
			buf_adds(&r, mid);
			add_hdr(&t, "References", r.d);
			buf_free(&r);
		}
		free(mid);
		free(refs);
	}
	buf_addc(&t, '\n');

	if (kind == 'r') {
		char *fr = mime_header(ohdr, ohlen, "From");
		char *dt = mime_header(ohdr, ohlen, "Date");
		buf_printf(&t, "On %s, %s wrote:\n", dt ? dt : "an earlier date", fr ? fr : "someone");
		free(fr);
		free(dt);
		/* A flowed original was unwrapped for display; quote it as flowed
		 * text again (wrapped at 72 with soft breaks). Other text is quoted
		 * line by line, without trailing spaces (they would mean "flowed"). */
		const char *p = obody;
		while (p && *p) {
			const char *e = strchr(p, '\n');
			size_t n = e ? (size_t)(e - p) : strlen(p);
			if (A.mv.flowed) {
				quote_flowed(&t, p, n);
			} else {
				size_t m = n;
				while (m && (p[m - 1] == ' ' || p[m - 1] == '\t' || p[m - 1] == '\r')) m--;
				buf_adds(&t, m && p[0] == '>' ? ">" : m ? "> " : ">");
				buf_add(&t, p, m);
				buf_addc(&t, '\n');
			}
			p += n + (e ? 1 : 0);
		}
		buf_adds(&t, "\n\n");                         /* blank line, then the reply */
		start = -1;                                  /* last line */
	} else if (kind == 'f') {
		buf_adds(&t, "\n\n---------- Forwarded message ----------\n");
		static const char *const fh[] = {"From", "Date", "Subject", "To", "Cc", NULL};
		for (int i = 0; fh[i]; i++) {
			char *v = mime_header(ohdr, ohlen, fh[i]);
			if (v) buf_printf(&t, "%s: %s\n", fh[i], v);
			free(v);
		}
		buf_printf(&t, "\n%s\n", obody ? obody : "");
	}
	free(osub);
	free(obody);

	/* The original may hold text that is not valid UTF-8 (a wrong or missing
	 * charset, Windows-1252 bytes, control characters): clean it here, or it
	 * could not be sent. */
	{
		size_t cl;
		char *ct = utf8_clean(t.d ? t.d : "", t.len, &cl);
		buf_free(&t);
		buf_add(&t, ct, cl);
		free(ct);
	}
	int nlines = 0;
	for (size_t i = 0; i < t.len; i++) nlines += t.d[i] == '\n';
	if (start < 0) start = nlines > 0 ? nlines - 1 : 0;
	if (t.len && t.d[t.len - 1] == '\n') t.len--;

	editor_cfg ec = {
		.title = kind == 'r' ? "Reply" : kind == 'f' ? "Forward" : "New message",
		.send = send_cb,
		.keep = keep_cb,
		.ctx = &c,
		.wrap = kind == 'm',                         /* replies/forwards: W turns it on */
		.wrap_width = wrap_width,
	};
	editor_run(&ec, buf_cstr(&t), start);
	buf_free(&t);
	focus_to(COL_LIST);            /* back to the email list and the message */
}

/* ------------------------------------------------------------------ */
/* Startup                                                             */

/* "~/x" -> "$HOME/x" */
static void expand_home(char *out, size_t n, const char *in)
{
	const char *home = getenv("HOME");
	if (in[0] == '~' && (in[1] == '/' || !in[1]) && home)
		snprintf(out, n, "%s%s", home, in + 1);
	else
		snprintf(out, n, "%s", in);
}

/* Where a POP3 account keeps Sent, Drafts and its other folders. */
static void pop3_default_path(char *out, size_t n)
{
	const char *xdg = getenv("XDG_DATA_HOME"), *home = getenv("HOME");
	char nm[64];
	snprintf(nm, sizeof nm, "%s", ac->name);
	for (char *c = nm; *c; c++)
		if (*c == '/' || *c == '\\' || (*c == '.' && c == nm)) *c = '_';
	if (xdg && *xdg) snprintf(out, n, "%.300s/emsg/%.60s", xdg, nm);
	else snprintf(out, n, "%.300s/.local/share/emsg/%.60s", home ? home : ".", nm);
}

/* Usual places of local mail: ~/Maildir, ~/Mail, $MAIL. */
static void local_default_path(char *out, size_t n)
{
	const char *home = getenv("HOME"), *mail = getenv("MAIL");
	static const char *const sub[] = {"Maildir", "Mail", "mail"};
	struct stat st;
	for (int i = 0; home && i < 3; i++) {
		snprintf(out, n, "%s/%s", home, sub[i]);
		if (stat(out, &st) == 0 && S_ISDIR(st.st_mode)) {
			snprintf(out, n, "~/%s", sub[i]);
			return;
		}
	}
	if (mail && *mail && stat(mail, &st) == 0) {
		snprintf(out, n, "%s", mail);
		return;
	}
	snprintf(out, n, "~/Maildir");
}

/* A new account: IMAP, POP3 or local folders. -1 = cancelled. */
static int choose_type(void)
{
	static const char *const items[] = {
		"IMAP   mail and folders stay on the server (IMAPS)",
		"POP3   INBOX from the server (POP3S); Sent and Drafts here",
		"Local  Maildir, mbox or MH folders on this computer",
	};
	return ui_list_dialog("New account: where is the mail?", items, 3, 0, "Enter choose   Esc cancel",
	                      login_bg);
}

/* Ask for (or check) the account name; 0 = fine. */
static int check_name(const char *name, const char **err)
{
	const account_t *o = account_find(name);
	if (o && o != ac) { *err = "Another account has that name"; return -1; }
	if (strchr(name, ']')) { *err = "No ']' in the account name"; return -1; }
	return 0;
}

static int open_local_account(void)
{
	char name[64], path[512], full[1024];
	const char *err = NULL;
	char errbuf[600];
	int ask = !*ac->path;
	if (!*ac->path) local_default_path(ac->path, sizeof ac->path);
	for (;;) {
		if (ask) {
			snprintf(name, sizeof name, "%s", ac->name);
			snprintf(path, sizeof path, "%s", ac->path);
			ui_field f[] = {
				{"Account", name, sizeof name, 0, 1},
				{"Full name", ac->fullname, sizeof ac->fullname, 0, 1},
				{"E-mail address", ac->user, sizeof ac->user, 0, 1},
				{"Mail folders", path, sizeof path, 0, 0},
			};
			if (!err) err = "Maildir, mbox, MH folder, or a directory of them (~ = home)";
			if (ui_input_dialog("Local mailboxes", f, 4, err, login_bg) < 0) return -1;
			if (check_name(str_trim(name), &err) != 0) continue;
			snprintf(ac->name, sizeof ac->name, "%s", name);
			snprintf(ac->path, sizeof ac->path, "%s", str_trim(path));
			str_trim(ac->fullname);
			str_trim(ac->user);
		}
		expand_home(full, sizeof full, ac->path);
		if (store_open_local(A.im, full, NULL) == 0) {
			account_name(ac);
			return 0;
		}
		snprintf(errbuf, sizeof errbuf, "%s", store_error(A.im));
		err = errbuf;
		ask = 1;
	}
}

static int do_login(void)
{
	char pass[256] = "", name[64];
	const char *err = NULL;
	char errbuf[600];

	if (ac->type == STORE_LOCAL ? !*ac->path : !*ac->host) {
		if (!A.type_given) {
			int t = choose_type();
			if (t < 0) return -1;
			ac->type = t;
		}
		if (ac->type == STORE_POP3 && !strcmp(ac->port, "993")) snprintf(ac->port, sizeof ac->port, "995");
	}
	A.im = store_new(ac->type);
	store_set_cert_check(A.im, cert_check, NULL);
	if (ac->type == STORE_LOCAL) return open_local_account();

	int typed = !(*ac->host && *ac->user && unlock_password(pass, sizeof pass, &err) == 0);
	for (;;) {
		if (typed) {
			snprintf(name, sizeof name, "%s", ac->name);
			ui_field f[] = {
				{"Account", name, sizeof name, 0, 1},
				{"Full name", ac->fullname, sizeof ac->fullname, 0, 1},
				{"Server", ac->host, sizeof ac->host, 0, 0},
				{"Port", ac->port, sizeof ac->port, 0, 0},
				{"User", ac->user, sizeof ac->user, 0, 0},
				{"Password", pass, sizeof pass, 1, 0},
			};
			const char *title = ac->type == STORE_POP3 ? "POP3S Login" : "IMAPS Login";
			if (!err && ac->type == STORE_POP3) err = "Port 995: TLS. Port 110: STLS (required)";
			if (ui_input_dialog(title, f, 6, err, login_bg) < 0) return -1;
			if (check_name(str_trim(name), &err) != 0) continue;
			snprintf(ac->name, sizeof ac->name, "%s", name);
			str_trim(ac->fullname);
		}

		char msg[400];
		snprintf(msg, sizeof msg, "Connecting to %s:%s ...", ac->host, ac->port);
		ui_progress(msg, login_bg);
		if (store_connect(A.im, ac->host, ac->port) == 0) {
			ui_progress("Logging in ...", login_bg);
			if (store_login(A.im, ac->user, pass) == 0) {
				account_name(ac);
				if (typed) offer_store(pass);
				memset(pass, 0, sizeof pass);
				if (ac->type != STORE_POP3) return 0;
				/* the local folders of a POP3 account */
				if (!*ac->path) pop3_default_path(ac->path, sizeof ac->path);
				char full[1024], state[1100];
				expand_home(full, sizeof full, ac->path);
				snprintf(state, sizeof state, "%s/.emsg-pop3-state", full);
				if (store_open_local(A.im, full, state) == 0) return 0;
				char m[700];
				snprintf(m, sizeof m, "Local folders: %s", store_error(A.im));
				ui_message("POP3", m, login_bg);
				return -1;
			}
		}
		snprintf(errbuf, sizeof errbuf, "%s", store_error(A.im));
		store_logout(A.im);
		err = errbuf;
		memset(pass, 0, sizeof pass);
		typed = 1;
	}
}

/* Decode %XX escapes from s[0..n) into out. Returns -1 if it does not fit. */
static int pct_decode(char *out, size_t size, const char *s, size_t n)
{
	size_t o = 0;
	for (size_t i = 0; i < n; i++) {
		char c = s[i];
		if (c == '%' && i + 2 < n && isxdigit((unsigned char)s[i + 1]) &&
		    isxdigit((unsigned char)s[i + 2])) {
			char hex[3] = {s[i + 1], s[i + 2], 0};
			c = (char)strtol(hex, NULL, 16);
			i += 2;
		}
		if (o + 1 >= size) return -1;
		out[o++] = c;
	}
	out[o] = '\0';
	return 0;
}

/* Parse imaps://[user@]host[:port][/folder] (RFC 5092 style),
 * pop3s://[user@]host[:port], or local folders (a path, file://, maildir:,
 * mbox:) into the command line account.
 * The user may contain '@' (an e-mail address); the last '@' ends it. */
static int parse_url(const char *url)
{
	const char *p = url;
	static const char *const local_pfx[] = {"file://", "maildir:", "mbox:", NULL};
	for (int i = 0; local_pfx[i]; i++)
		if (!strncasecmp(p, local_pfx[i], strlen(local_pfx[i]))) {
			p += strlen(local_pfx[i]);
			if (!*p) goto bad;
			break;
		}
#ifdef _WIN32
	/* C:\Mail, C:/Mail, \\server\share\Mail */
	int win_path = (isalpha((unsigned char)p[0]) && p[1] == ':' && (!p[2] || p[2] == '/' || p[2] == '\\')) ||
	               *p == '\\';
#else
	int win_path = 0;
#endif
	if (p != url || *p == '/' || *p == '.' || *p == '~' || win_path) {
		char full[1024], real[4096];
		expand_home(full, sizeof full, p);
#ifdef _WIN32
		for (char *c = full; *c; c++)
			if (*c == '\\') *c = '/';
#endif
		const char *v = realpath(full, real) ? real : full;
		if (strlen(v) >= sizeof ac->path) goto toolong;
		ac->type = STORE_LOCAL;
		snprintf(ac->path, sizeof ac->path, "%s", v);
		A.type_given = 1;
		return 0;
	}
	const char *def_port = "993";
	if (!strncasecmp(p, "imaps://", 8)) {
		p += 8;
	} else if (!strncasecmp(p, "pop3s://", 8)) {
		p += 8;
		ac->type = STORE_POP3;
		def_port = "995";
	} else if (!strncasecmp(p, "imap://", 7) || !strncasecmp(p, "pop3://", 7)) {
		fprintf(stderr, "emsg: only imaps:// and pop3s:// (over TLS) are supported\n");
		return -1;
	} else if (strstr(p, "://")) {
		fprintf(stderr, "emsg: not an imaps:// or pop3s:// URL: %s\n", url);
		return -1;
	}
	A.type_given = 1;

	const char *end = p + strcspn(p, "/?#");
	const char *at = NULL;
	for (const char *q = p; q < end; q++)
		if (*q == '@') at = q;

	if (at) {
		size_t ulen = (size_t)(at - p);
		const char *semi = memchr(p, ';', ulen);       /* ;AUTH=... */
		if (semi) ulen = (size_t)(semi - p);
		if (memchr(p, ':', ulen)) {
			fprintf(stderr, "emsg: leave the password out of the URL; "
			                "it is asked for at login\n");
			return -1;
		}
		if (pct_decode(ac->user, sizeof ac->user, p, ulen) < 0) goto toolong;
		p = at + 1;
	}

	const char *hend = end, *colon = NULL;
	if (*p == '[') {                                     /* [IPv6]:port */
		const char *rb = memchr(p, ']', (size_t)(end - p));
		if (!rb) goto bad;
		if (rb + 1 < end && rb[1] == ':') colon = rb + 1;
		else if (rb + 1 != end) goto bad;
		p++;
		hend = rb;
	} else {
		colon = memchr(p, ':', (size_t)(end - p));
		if (colon) hend = colon;
	}
	if (hend == p) goto bad;
	if (pct_decode(ac->host, sizeof ac->host, p, (size_t)(hend - p)) < 0) goto toolong;

	if (colon) {
		const char *ps = colon + 1;
		size_t plen = (size_t)(end - ps);
		if (!plen || plen >= sizeof ac->port || strspn(ps, "0123456789") < plen) goto bad;
		memcpy(ac->port, ps, plen);
		ac->port[plen] = '\0';
	} else {
		snprintf(ac->port, sizeof ac->port, "%s", def_port);
	}

	A.start_folder[0] = '\0';
	if (*end == '/') {
		const char *f = end + 1;
		size_t flen = strcspn(f, ";?#");
		if (pct_decode(A.start_folder, sizeof A.start_folder, f, flen) < 0) goto toolong;
	}
	return 0;
bad:
	fprintf(stderr, "emsg: bad URL: %s\n"
	                "      expected imaps://[user@]host[:port][/folder], pop3s://[user@]host[:port]\n"
	                "      or a path to local mail\n", url);
	return -1;
toolong:
	fprintf(stderr, "emsg: URL part too long: %s\n", url);
	return -1;
}

/* Print text lines, with @VERSION@ replaced by the version. */
static void print_lines(FILE *f, const char *const *lines)
{
	for (int i = 0; lines[i]; i++) {
		const char *s = lines[i], *v;
		while ((v = strstr(s, "@VERSION@"))) {
			fprintf(f, "%.*s%s", (int)(v - s), s, EMSG_VERSION);
			s = v + 9;
		}
		fprintf(f, "%s\n", s);
	}
}

/* Ctrl-Q or Ctrl-C, anywhere: leave at once. Connections are just closed (no LOGOUT that
 * could wait for the network); a message being written is saved as a file. */
static void quit_now(void)
{
	char saved[1200];
	editor_save_unsent(saved, sizeof saved);
	ui_end();
	if (*saved) fprintf(stderr, "emsg: the message you were writing is saved in %s\n", saved);
	store_free(A.im);
	vault_free(master);
	vault_free(A.smtp_pass);
	exit(0);
}

static void usage(void) { print_lines(stderr, HELP_USAGE); }
static void help(void) { print_lines(stdout, HELP_CMDLINE); }

/* Which account to use: -n, -a NAME, the server/user given on the command
 * line, the only one, or the user's choice. NULL = quit. */
static account_t *choose_account(const char *opt_a, int opt_n, const account_t *cli)
{
	account_t *a = NULL;
	if (opt_n) {
		a = account_new(NULL);
	} else if (opt_a) {
		a = account_find(opt_a);
		if (!a) a = account_new(opt_a);
	} else if (*cli->host || *cli->user || *cli->path) {
		for (int i = 0; !a && i < naccts; i++)
			if ((!A.type_given || accts[i].type == cli->type) &&
			    (!*cli->host || !strcasecmp(accts[i].host, cli->host)) &&
			    (!*cli->user || !strcmp(accts[i].user, cli->user)) &&
			    (!*cli->path || !strcmp(accts[i].path, cli->path)))
				a = &accts[i];
		if (!a) a = account_new(NULL);
	} else if (naccts == 1) {
		a = &accts[0];
	} else if (naccts > 1) {
		const account_t *last = account_find(last_account);
		int i = pick_account("Choose an account", last ? (int)(last - accts) : 0, 0, login_bg);
		if (i < 0) return NULL;
		a = i < naccts ? &accts[i] : account_new(NULL);
	} else {
		a = account_new(NULL);
	}
	if (!a) {
		ui_message("Accounts", "Too many accounts in the config file.", login_bg);
		return NULL;
	}
	if (A.type_given) a->type = cli->type;
	if (*cli->path) snprintf(a->path, sizeof a->path, "%s", cli->path);
	if (*cli->host) snprintf(a->host, sizeof a->host, "%s", cli->host);
	if (*cli->user) snprintf(a->user, sizeof a->user, "%s", cli->user);
	if (*cli->port) snprintf(a->port, sizeof a->port, "%s", cli->port);
	if (!*a->port) snprintf(a->port, sizeof a->port, a->type == STORE_POP3 ? "995" : "993");
	return a;
}

int main(int argc, char **argv)
{
	setlocale(LC_ALL, "");
#ifdef SIGPIPE
	signal(SIGPIPE, SIG_IGN);
#endif

	snprintf(A.theme, sizeof A.theme, "default");
	config_load();

	static account_t cli;              /* -s -p -u -f; parse_url fills ac */
	ac = &cli;
	const char *opt_a = NULL;
	int opt_n = 0;
	for (int i = 1; i < argc && strcmp(argv[i], "--"); i++) {
		if (!strcmp(argv[i], "--help")) { help(); return 0; }
		if (!strcmp(argv[i], "--version")) { puts("emsg " EMSG_VERSION); return 0; }
	}
	int c;
	while ((c = getopt(argc, argv, "a:ns:p:u:t:f:hV")) != -1) {
		switch (c) {
		case 'a': opt_a = optarg; break;
		case 'n': opt_n = 1; break;
		case 'f': if (parse_url(optarg) < 0) return 2; break;
		case 's': snprintf(ac->host, sizeof ac->host, "%s", optarg); break;
		case 'p': snprintf(ac->port, sizeof ac->port, "%s", optarg); break;
		case 'u': snprintf(ac->user, sizeof ac->user, "%s", optarg); break;
		case 't': snprintf(A.theme, sizeof A.theme, "%s", optarg); break;
		case 'V': puts("emsg " EMSG_VERSION); return 0;
		case 'h': help(); return 0;
		default: usage(); return 2;
		}
	}
	if (optind < argc - 1) { usage(); return 2; }
	if (optind == argc - 1 && parse_url(argv[optind]) < 0) return 2;
	ac = NULL;

	/* colors: the text file ~/.config/emsg/NAME.theme, read at every start */
	theme_t theme;
	theme_defaults(&theme);
	char theme_err[1200] = "";
	int created = 0;
	char *tp = theme_user_file(A.theme, &created);
	if (!tp) tp = theme_find(A.theme);
	if (!tp)
		snprintf(theme_err, sizeof theme_err, "theme '%s' not found, using built-in colors", A.theme);
	else if (theme_load(&theme, tp, theme_err, sizeof theme_err) == 0 && created)
		snprintf(theme_err, sizeof theme_err, "Colors are read from %s", tp);
	free(tp);

	A.cur_folder = -1;
	A.lines_width = -1;
	A.unseen = -1;
	A.mv.more_off = MV_NO_MORE;
	A.focus = A.left = COL_LIST;

	ui_init(&theme);
	ui_set_quit(quit_now);

	ac = choose_account(opt_a, opt_n, &cli);
	if (!ac || do_login() < 0) {
		ui_end();
		store_free(A.im);
		return 0;
	}
	smtp_derive();
	config_save();

	ui_progress("Reading folder list ...", login_bg);
	if (store_list(A.im, &A.folders, &A.nfolders) != 0)
		set_status("Cannot list folders: %s", store_error(A.im));

	int inbox = -1;
	if (*A.start_folder) {
		/* the URL may give the raw (modified UTF-7) name or plain UTF-8 */
		for (int i = 0; inbox < 0 && i < A.nfolders; i++) {
			if (A.folders[i].noselect) continue;
			char *u = mutf7_decode(A.folders[i].name);
			if (!strcmp(A.folders[i].name, A.start_folder) || !strcmp(u, A.start_folder) ||
			    (!strcasecmp(A.start_folder, "INBOX") && !strcasecmp(A.folders[i].name, "INBOX")))
				inbox = i;
			free(u);
		}
	}
	int start_missing = *A.start_folder && inbox < 0;
	for (int i = 0; inbox < 0 && i < A.nfolders; i++)
		if (!strcasecmp(A.folders[i].name, "INBOX")) inbox = i;
	if (inbox >= 0) {
		A.fsel = inbox;
		open_folder(inbox);
	} else {
		A.focus = A.left = COL_FOLDERS;
	}
	if (start_missing) set_status("Folder '%s' not found", A.start_folder);
	if (*theme_err && !*A.status) set_status("%s", theme_err);

	for (;;) {
		sync_changes();
		draw();
		int ch;
		if (work_pending()) {
			/* load what the screen needs, but never while keys are waiting:
			 * fast scrolling stays smooth and fills in when it stops */
			timeout(0);
			ch = ui_getch();
			if (ch == ERR) { do_work(); continue; }
		} else {
			timeout(LOAD_DELAY_MS);
			ch = ui_getch();
			if (ch == ERR) {              /* idle */
				if (A.pending_load) load_selected();
				else if (time(NULL) - A.last_poll >= POLL_SECONDS) refresh_folder(1);
				continue;
			}
		}
		A.work_failed = 0;
		if (ch == KEY_MOUSE) {
			MEVENT ev;
			if (getmouse(&ev) != OK) continue;
			cols_t cl;
			get_cols(&cl);
			if (dragging == DRAG_THUMB) {
				scroll_thumb_to(ev.y - sb.grab);
				if (ev.bstate & BUTTON1_RELEASED) {
					dragging = 0;
					ui_mouse_drag(0);
				}
				continue;
			}
			if (dragging) {
				/* the border follows the mouse (movement is reported as
				 * position or as the button still pressed); release ends it */
				int bx = cl.x[drag_border];
				if (ev.x != bx - 1 && ev.x != bx) {
					set_border(drag_border, ev.x + 1);
					A.text_dirty = 1;
				}
				if (ev.bstate & BUTTON1_RELEASED) {
					dragging = 0;
					ui_mouse_drag(0);
					config_save();
				}
				continue;
			}
			if (ev.bstate & (BUTTON4_PRESSED | BUTTON5_PRESSED)) {
				/* wheel: the message scrolls 3 lines, the email and folder
				 * lists move the selection by 3 */
				int d = ev.bstate & BUTTON4_PRESSED ? -3 : 3;
				if (ev.y > 0 && ev.y < LINES - 1)
					for (int k = cl.n - 1; k >= 0; k--) {
						if (ev.x < cl.x[k]) continue;
						if (cl.col[k] == COL_MSG) {
							A.vtop += d;
						} else if (cl.col[k] == COL_LIST) {
							A.mtop += d;              /* the selection stays */
							A.mfree = 1;
						} else {
							A.ftop += d;
							A.ffree = 1;
						}
						break;
					}
				continue;
			}
			if (!(ev.bstate & BUTTON1_PRESSED)) continue;
			if (ev.y > 0 && ev.y < LINES - 1 && (drag_border = border_at(ev.x)) > 0) {
				dragging = DRAG_BORDER;               /* pressed on a column border */
				ui_mouse_drag(1);
				continue;
			}
			if (scrollbar_press(ev.x, ev.y)) continue;
			if (ev.y == A.fbar_y && ev.x > A.fbar_x && ev.x < A.fbar_x + A.fbar_w - 1 &&
			    A.cur_folder >= 0) {
				/* the list's bottom border: the filter (Esc=Leave ends it) */
				if (A.fon && A.fesc_x1 && ev.x >= A.fesc_x0 && ev.x < A.fesc_x1) filter_leave();
				else filter_edit();
				continue;
			}
			if (ev.y >= 2 && ev.y < LINES - 2) {
				click_window(&cl, ev.x, ev.y);
				continue;
			}
			if (ev.y != 0) continue;
			/* click on the menu bar = its hotkey */
			ch = ui_menu_hit(folder_menu, (int)(sizeof folder_menu / sizeof *folder_menu), ev.x);
			if (!ch) continue;
		}
		if (!handle_key(ch)) break;
	}

	ui_end();
	store_logout(A.im);
	store_free(A.im);
	close_message();
	cache_clear();
	free(A.lines);
	folders_free(A.folders, A.nfolders);
	vault_free(master);
	vault_free(A.smtp_pass);
	return 0;
}
