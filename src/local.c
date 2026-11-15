/* local.c - Maildir and mbox folders, and the POP3 inbox (see local.h) */
#include "local.h"
#include "mime.h"
#include "util.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* Maildir file names end in ":2,FLAGS"; on Windows "!2,FLAGS" (see compat.h) */
#ifdef _WIN32
#define MD_INFO   "!2,"
#define MD_SEPSTR "!"
#else
#define MAILDIR_SEP ':'
#define MD_INFO   ":2,"
#define MD_SEPSTR ":"
#endif

/* modification time as a struct timespec (Windows: whole seconds) */
#ifdef _WIN32
#define ST_MTIM(s) ((struct timespec){(s).st_mtime, 0})
#else
#define ST_MTIM(s) ((s).st_mtim)
#endif

#define HDR_MAX       (64 * 1024)  /* header bytes read for the message list */
#define MAX_DEPTH     6            /* directory levels searched for folders */
#define POP3_REFRESH  300          /* seconds between POP3 sessions when idle */

enum { LF_MAILDIR, LF_MBOX, LF_MH, LF_POP3, LF_DIR, LF_NEW_MAILDIR, LF_NEW_MBOX, LF_NEW_MH };

typedef struct {
	char *name;      /* folder name used by the caller */
	char *path;
	int   kind;
	char  delim;
} lfolder;

typedef struct {
	uint32_t  uid, size;
	char     *key;       /* identity across rescans: file base name, Message-ID or offset, UIDL */
	char     *file;      /* Maildir: "cur/NAME" or "new/NAME" */
	long long off;       /* mbox: message start (after the From_ line) */
	size_t    len;
	int       num;       /* POP3 message number in the current session */
	int       seen, answered, flagged, deleted, have_hdr;
	time_t    date;
	char     *from, *subject;
} lmsg;

typedef struct {
	char id[72];
	char fl[8];          /* "S", "R", "SR" */
} pstate;

struct local {
	char      err[512];
	char     *root;
	lfolder  *f;
	int       nf;

	pop3_t   *pop;
	char     *state_file;
	pstate   *ps;          /* sorted by id */
	int       nps;
	unsigned  pop_gen;
	time_t    pop_last;

	/* the open folder */
	int       sel;
	lmsg     *m;
	int       n;
	uint32_t  uidnext;
	struct timespec mt_new, mt_cur;
	off_t     msize;
	uint32_t *exp, *exp_given;
	int       nexp, changed;

	/* the last message read whole */
	uint32_t  raw_uid;
	buf_t     raw;
	size_t    raw_hlen;
};

static void seterr(local_t *l, const char *fmt, ...) __attribute__((format(EMSG_PRINTF, 2, 3)));
static void seterr(local_t *l, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(l->err, sizeof l->err, fmt, ap);
	va_end(ap);
}

static char *path_join(const char *a, const char *b)
{
	buf_t p = {0};
	buf_adds(&p, a);
	if (p.len && p.d[p.len - 1] != '/') buf_addc(&p, '/');
	buf_adds(&p, b);
	return buf_steal(&p);
}

static int is_dir(const char *p)
{
	struct stat st;
	return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static int is_maildir(const char *p)
{
	char *c = path_join(p, "cur"), *n = path_join(p, "new");
	int r = is_dir(c) && is_dir(n);
	free(c);
	free(n);
	return r;
}

/* An mbox file starts with "From " (or is empty). */
static int is_mbox(const char *p)
{
	struct stat st;
	if (stat(p, &st) != 0 || !S_ISREG(st.st_mode)) return 0;
	if (st.st_size == 0) return 1;
	FILE *f = fopen(p, "r");
	if (!f) return 0;
	char b[5];
	int r = fread(b, 1, 5, f) == 5 && !memcmp(b, "From ", 5);
	fclose(f);
	return r;
}

static int all_digits(const char *s)
{
	if (!*s) return 0;
	for (; *s; s++)
		if (!isdigit((unsigned char)*s)) return 0;
	return 1;
}

/* An MH folder: a directory with .mh_sequences or numbered message files. */
static int is_mh(const char *p)
{
	if (!is_dir(p) || is_maildir(p)) return 0;
	char *seq = path_join(p, ".mh_sequences");
	struct stat st;
	int r = stat(seq, &st) == 0;
	free(seq);
	if (r) return 1;
	DIR *d = opendir(p);
	if (!d) return 0;
	struct dirent *e;
	while (!r && (e = readdir(d))) {
		if (!all_digits(e->d_name)) continue;
		char *f = path_join(p, e->d_name);
		r = stat(f, &st) == 0 && S_ISREG(st.st_mode);
		free(f);
	}
	closedir(d);
	return r;
}

static void msg_clear(lmsg *m)
{
	free(m->key);
	free(m->file);
	free(m->from);
	free(m->subject);
	memset(m, 0, sizeof *m);
}

static void msgs_clear(lmsg *m, int n)
{
	for (int i = 0; i < n; i++) msg_clear(&m[i]);
	free(m);
}

static void folders_clear(local_t *l)
{
	for (int i = 0; i < l->nf; i++) {
		free(l->f[i].name);
		free(l->f[i].path);
	}
	free(l->f);
	l->f = NULL;
	l->nf = 0;
}

static void close_folder(local_t *l)
{
	msgs_clear(l->m, l->n);
	l->m = NULL;
	l->n = 0;
	l->sel = -1;
	free(l->exp);
	l->exp = NULL;
	l->nexp = 0;
	l->changed = 0;
	buf_free(&l->raw);
	l->raw_uid = 0;
}

local_t *local_new(void)
{
	local_t *l = xcalloc(1, sizeof *l);
	l->sel = -1;
	l->uidnext = 1;
	return l;
}

void local_free(local_t *l)
{
	if (!l) return;
	close_folder(l);
	folders_clear(l);
	free(l->exp_given);
	free(l->root);
	free(l->state_file);
	free(l->ps);
	free(l);
}

const char *local_error(local_t *l) { return l->err; }

static int mkdirs(const char *p)
{
	char *d = xstrdup(p);
	for (char *s = d + 1; *s; s++)
		if (*s == '/') {
			*s = '\0';
			mkdir(d, 0700);
			*s = '/';
		}
	int r = mkdir(d, 0700) == 0 || errno == EEXIST ? 0 : -1;
	free(d);
	return r;
}

int local_open(local_t *l, const char *root, int create)
{
	free(l->root);
	l->root = xstrdup(root);
	struct stat st;
	if (stat(root, &st) != 0) {
		if (!create) {
			seterr(l, "%s: %s", root, strerror(errno));
			return -1;
		}
		if (mkdirs(root) != 0) {
			seterr(l, "cannot create %s: %s", root, strerror(errno));
			return -1;
		}
	} else if (!S_ISDIR(st.st_mode) && !is_mbox(root)) {
		seterr(l, "%s is not a mailbox (an mbox file, a Maildir or a directory)", root);
		return -1;
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* POP3 state file: "UIDL FLAGS" per line                              */

static int ps_cmp(const void *a, const void *b)
{
	return strcmp(((const pstate *)a)->id, ((const pstate *)b)->id);
}

static pstate *ps_find(local_t *l, const char *id)
{
	pstate k;
	snprintf(k.id, sizeof k.id, "%s", id);
	return l->nps ? bsearch(&k, l->ps, (size_t)l->nps, sizeof *l->ps, ps_cmp) : NULL;
}

static void ps_load(local_t *l)
{
	free(l->ps);
	l->ps = NULL;
	l->nps = 0;
	FILE *f = fopen(l->state_file, "r");
	if (!f) return;
	char line[256];
	while (fgets(line, sizeof line, f)) {
		char id[72], fl[8] = "";
		if (sscanf(line, "%71s %7s", id, fl) < 1 || id[0] == '#') continue;
		l->ps = xrealloc(l->ps, sizeof *l->ps * (size_t)(l->nps + 1));
		snprintf(l->ps[l->nps].id, sizeof l->ps[0].id, "%s", id);
		snprintf(l->ps[l->nps].fl, sizeof l->ps[0].fl, "%s", fl);
		l->nps++;
	}
	fclose(f);
	if (l->nps) qsort(l->ps, (size_t)l->nps, sizeof *l->ps, ps_cmp);
}

static void ps_save(local_t *l)
{
	buf_t tmp = {0};
	buf_printf(&tmp, "%s.new", l->state_file);
	int fd = open(buf_cstr(&tmp), O_WRONLY | O_CREAT | O_TRUNC, 0600);
	FILE *f = fd >= 0 ? fdopen(fd, "w") : NULL;
	if (!f) {
		if (fd >= 0) close(fd);
		buf_free(&tmp);
		return;
	}
	fprintf(f, "# emsg: POP3 messages already seen (S) or answered (R), by unique id\n");
	for (int i = 0; i < l->nps; i++)
		if (l->ps[i].fl[0]) fprintf(f, "%s %s\n", l->ps[i].id, l->ps[i].fl);
	int bad = ferror(f);
	if (fclose(f) != 0 || bad) unlink(tmp.d);
	else rename(tmp.d, l->state_file);
	buf_free(&tmp);
}

static void ps_set(local_t *l, const char *id, char flag)
{
	pstate *p = ps_find(l, id);
	if (!p) {
		l->ps = xrealloc(l->ps, sizeof *l->ps * (size_t)(l->nps + 1));
		p = &l->ps[l->nps++];
		memset(p, 0, sizeof *p);
		snprintf(p->id, sizeof p->id, "%s", id);
		qsort(l->ps, (size_t)l->nps, sizeof *l->ps, ps_cmp);
		p = ps_find(l, id);
	}
	if (!strchr(p->fl, flag) && strlen(p->fl) + 1 < sizeof p->fl) {
		size_t k = strlen(p->fl);
		p->fl[k] = flag;
		p->fl[k + 1] = '\0';
	}
	ps_save(l);
}

/* Forget messages that are no longer on the server. */
static void ps_prune(local_t *l, const pop3_msg *m, int n)
{
	int k = 0, dropped = 0;
	for (int i = 0; i < l->nps; i++) {
		int keep = 0;
		for (int j = 0; j < n && !keep; j++) keep = !strcmp(m[j].uidl, l->ps[i].id);
		if (keep) l->ps[k++] = l->ps[i];
		else dropped = 1;
	}
	l->nps = k;
	if (dropped) ps_save(l);
}

int local_set_pop3(local_t *l, pop3_t *p, const char *state_file)
{
	l->pop = p;
	free(l->state_file);
	l->state_file = xstrdup(state_file);
	ps_load(l);
	l->pop_gen = pop3_generation(p);
	l->pop_last = time(NULL);
	return 0;
}

/* ------------------------------------------------------------------ */
/* Folder list                                                         */

static void add_folder(local_t *l, const char *name, const char *path, int kind, char delim)
{
	l->f = xrealloc(l->f, sizeof *l->f * (size_t)(l->nf + 1));
	l->f[l->nf++] = (lfolder){xstrdup(name), xstrdup(path), kind, delim};
}

static lfolder *find_folder(local_t *l, const char *name)
{
	for (int i = 0; i < l->nf; i++)
		if (!strcmp(l->f[i].name, name)) return &l->f[i];
	return NULL;
}

static int name_used(local_t *l, const char *name)
{
	for (int i = 0; i < l->nf; i++)
		if (!strcasecmp(l->f[i].name, name)) return 1;
	return 0;
}

/* A directory of folders: mbox files, Maildirs and MH folders; directories
 * nest. cnt counts the Maildirs, mbox files and MH folders found. */
static void walk_dir(local_t *l, const char *dir, const char *prefix, int depth, int cnt[3])
{
	if (depth > MAX_DEPTH) return;
	DIR *d = opendir(dir);
	if (!d) return;
	struct dirent *e;
	while ((e = readdir(d))) {
		if (e->d_name[0] == '.' || all_digits(e->d_name)) continue;   /* MH messages are numbers */
		char *p = path_join(dir, e->d_name);
		buf_t name = {0};
		if (*prefix) {
			buf_adds(&name, prefix);
			buf_addc(&name, '/');
		}
		buf_adds(&name, e->d_name);
		if (is_maildir(p)) {
			add_folder(l, buf_cstr(&name), p, LF_MAILDIR, '/');
			cnt[0]++;
		} else if (is_mh(p)) {
			add_folder(l, buf_cstr(&name), p, LF_MH, '/');   /* may hold subfolders too */
			cnt[2]++;
			walk_dir(l, p, buf_cstr(&name), depth + 1, cnt);
		} else if (is_dir(p)) {
			int before = l->nf;
			add_folder(l, buf_cstr(&name), p, LF_DIR, '/');
			walk_dir(l, p, buf_cstr(&name), depth + 1, cnt);
			if (l->nf == before + 1) {           /* no folders inside: leave it out */
				l->nf--;
				free(l->f[l->nf].name);
				free(l->f[l->nf].path);
			}
		} else if (is_mbox(p)) {
			add_folder(l, buf_cstr(&name), p, LF_MBOX, '/');
			cnt[1]++;
		}
		buf_free(&name);
		free(p);
	}
	closedir(d);
}

/* Maildir++: subfolders are directories ".A.B" next to cur/ new/ tmp/. */
static void maildirpp(local_t *l, const char *root)
{
	DIR *d = opendir(root);
	if (!d) return;
	struct dirent *e;
	while ((e = readdir(d))) {
		if (e->d_name[0] != '.' || !e->d_name[1] || !strcmp(e->d_name, "..")) continue;
		char *p = path_join(root, e->d_name);
		if (is_maildir(p)) add_folder(l, e->d_name + 1, p, LF_MAILDIR, '.');
		free(p);
	}
	closedir(d);
}

static int lf_cmp(const void *a, const void *b)
{
	const lfolder *x = a, *y = b;
	int ix = !strcasecmp(x->name, "INBOX"), iy = !strcasecmp(y->name, "INBOX");
	if (ix != iy) return iy - ix;
	const unsigned char *p = (const unsigned char *)x->name, *q = (const unsigned char *)y->name;
	for (;; p++, q++) {
		int c1 = *p == (unsigned char)x->delim ? 1 : tolower(*p);
		int c2 = *q == (unsigned char)y->delim ? 1 : tolower(*q);
		if (c1 != c2 || !c1) return c1 - c2;
	}
}

static void scan_folders(local_t *l)
{
	/* keep the open folder's entry valid: remember its name */
	char *open_name = l->sel >= 0 ? xstrdup(l->f[l->sel].name) : NULL;
	folders_clear(l);

	if (l->pop) add_folder(l, "INBOX", "", LF_POP3, '/');
	int cnt[3] = {0, 0, 0}, new_kind = -1;
	char sub_delim = '/';
	const char *root = l->root;
	if (root && is_maildir(root)) {
		if (!l->pop) add_folder(l, "INBOX", root, LF_MAILDIR, '.');
		maildirpp(l, root);
		new_kind = LF_NEW_MAILDIR;
		sub_delim = '.';
	} else if (root && is_mh(root)) {
		if (!l->pop) add_folder(l, "INBOX", root, LF_MH, '/');
		walk_dir(l, root, "", 0, cnt);
		new_kind = LF_NEW_MH;
	} else if (root && is_dir(root)) {
		walk_dir(l, root, "", 0, cnt);
		new_kind = cnt[2] > cnt[0] && cnt[2] > cnt[1] ? LF_NEW_MH
		         : cnt[1] > cnt[0] ? LF_NEW_MBOX : LF_NEW_MAILDIR;
		const char *mail = getenv("MAIL");
		if (!l->pop && !name_used(l, "INBOX") && mail && *mail && (is_mbox(mail) || is_maildir(mail)))
			add_folder(l, "INBOX", mail, is_maildir(mail) ? LF_MAILDIR : LF_MBOX, '/');
	} else if (root && is_mbox(root)) {
		add_folder(l, l->pop ? "Local" : "INBOX", root, LF_MBOX, '/');
	}
	if (new_kind >= 0) {
		static const char *const want[] = {"Sent", "Drafts"};
		for (int i = 0; i < 2; i++) {
			if (name_used(l, want[i])) continue;
			char *p;
			if (sub_delim == '.') {
				char dot[16];
				snprintf(dot, sizeof dot, ".%s", want[i]);
				p = path_join(root, dot);
			} else {
				p = path_join(root, want[i]);
			}
			add_folder(l, want[i], p, new_kind, sub_delim);
			free(p);
		}
	}
	qsort(l->f, (size_t)l->nf, sizeof *l->f, lf_cmp);

	l->sel = -1;
	if (open_name) {
		for (int i = 0; i < l->nf; i++)
			if (!strcmp(l->f[i].name, open_name)) l->sel = i;
		free(open_name);
	}
}

int local_list(local_t *l, folder_t **out, int *nout)
{
	scan_folders(l);
	folder_t *f = xcalloc((size_t)l->nf + 1, sizeof *f);
	for (int i = 0; i < l->nf; i++) {
		const lfolder *x = &l->f[i];
		f[i].name = xstrdup(x->name);
		f[i].delim = x->delim;
		const char *last = strrchr(x->name, x->delim);
		last = last ? last + 1 : x->name;
		f[i].display = x->delim == '.' ? mutf7_decode(last) : xstrdup(last);
		for (const char *s = x->name; *s; s++) f[i].depth += *s == x->delim;
		f[i].noselect = x->kind == LF_DIR;
		if (!strcasecmp(x->name, "Sent")) f[i].special = FOLDER_SENT;
		if (!strcasecmp(x->name, "Drafts")) f[i].special = FOLDER_DRAFTS;
	}
	*out = f;
	*nout = l->nf;
	return 0;
}

/* ------------------------------------------------------------------ */
/* Reading folders                                                     */

static void parse_hdr(lmsg *m, const char *h, size_t n)
{
	char *v = mime_header_raw(h, n, "From");
	free(m->from);
	m->from = mime_addr_display(v);
	free(v);
	free(m->subject);
	m->subject = mime_header(h, n, "Subject");
	if (!m->subject) m->subject = xstrdup("");
	v = mime_header_raw(h, n, "Date");
	m->date = v ? mime_parse_date(v) : 0;
	free(v);
	m->have_hdr = 1;
}

/* Length of the header block including the blank line after it. */
static size_t header_len(const char *s, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		if (s[i] != '\n') continue;
		if (i + 1 < n && s[i + 1] == '\n') return i + 2;
		if (i + 2 < n && s[i + 1] == '\r' && s[i + 2] == '\n') return i + 3;
	}
	return n;
}

static void maildir_flags(lmsg *m, const char *name, int in_new)
{
	const char *info = strstr(name, MD_INFO);
	m->seen = m->answered = m->flagged = m->deleted = 0;
	if (in_new || !info) return;
	for (const char *p = info + 3; *p; p++) {
		if (*p == 'S') m->seen = 1;
		else if (*p == 'R') m->answered = 1;
		else if (*p == 'F') m->flagged = 1;
		else if (*p == 'T') m->deleted = 1;
	}
}

static int lmsg_order(const void *a, const void *b)
{
	const lmsg *x = a, *y = b;
	unsigned long long tx = strtoull(x->key, NULL, 10), ty = strtoull(y->key, NULL, 10);
	if (tx != ty) return tx < ty ? -1 : 1;
	return strcmp(x->key, y->key);
}

static int scan_maildir(local_t *l, const char *path, lmsg **out, int *nout)
{
	lmsg *m = NULL;
	int n = 0;
	static const char *const sub[] = {"new", "cur"};
	for (int s = 0; s < 2; s++) {
		char *dp = path_join(path, sub[s]);
		DIR *d = opendir(dp);
		free(dp);
		if (!d) {
			seterr(l, "%s/%s: %s", path, sub[s], strerror(errno));
			msgs_clear(m, n);
			return -1;
		}
		struct dirent *e;
		while ((e = readdir(d))) {
			if (e->d_name[0] == '.') continue;
			m = xrealloc(m, sizeof *m * (size_t)(n + 1));
			lmsg *x = &m[n++];
			memset(x, 0, sizeof *x);
			size_t kl = strcspn(e->d_name, MD_SEPSTR);
			x->key = xstrndup(e->d_name, kl);
			buf_t f = {0};
			buf_printf(&f, "%s/%s", sub[s], e->d_name);
			x->file = buf_steal(&f);
			maildir_flags(x, e->d_name, s == 0);
		}
		closedir(d);
	}
	if (n) qsort(m, (size_t)n, sizeof *m, lmsg_order);
	*out = m;
	*nout = n;
	return 0;
}

/* MH sequences (.mh_sequences): "unseen: 1-3 7", "replied: 2", "flagged: 5" */
typedef struct {
	char *name;
	int  *v;
	int   n;
} mhseq;

static void seqs_free(mhseq *q, int n)
{
	for (int i = 0; i < n; i++) {
		free(q[i].name);
		free(q[i].v);
	}
	free(q);
}

static int int_cmp(const void *a, const void *b)
{
	int x = *(const int *)a, y = *(const int *)b;
	return (x > y) - (x < y);
}

static void seq_add_num(mhseq *q, int v)
{
	q->v = xrealloc(q->v, sizeof *q->v * (size_t)(q->n + 1));
	q->v[q->n++] = v;
}

static int seqs_read(const char *dir, mhseq **out)
{
	*out = NULL;
	char *p = path_join(dir, ".mh_sequences");
	FILE *f = fopen(p, "r");
	free(p);
	if (!f) return 0;
	mhseq *q = NULL;
	int n = 0;
	char *line = NULL;
	size_t cap = 0;
	while (getline(&line, &cap, f) > 0) {
		char *s = line;
		mhseq *cur;
		if ((*s == ' ' || *s == '\t') && n) {
			cur = &q[n - 1];                       /* continuation line */
		} else {
			char *c = strchr(s, ':');
			if (!c) continue;
			*c = '\0';
			q = xrealloc(q, sizeof *q * (size_t)(n + 1));
			cur = &q[n++];
			memset(cur, 0, sizeof *cur);
			cur->name = xstrdup(str_trim(s));
			s = c + 1;
		}
		for (char *t = strtok(s, " \t\r\n"); t; t = strtok(NULL, " \t\r\n")) {
			int a = atoi(t), b = a;
			char *dash = strchr(t, '-');
			if (dash) b = atoi(dash + 1);
			if (a < 1 || b < a || b - a > 10000000) continue;
			for (int v = a; v <= b; v++) seq_add_num(cur, v);
		}
	}
	free(line);
	fclose(f);
	for (int i = 0; i < n; i++)
		if (q[i].n) qsort(q[i].v, (size_t)q[i].n, sizeof *q[i].v, int_cmp);
	*out = q;
	return n;
}

static mhseq *seq_find(mhseq *q, int n, const char *name)
{
	for (int i = 0; i < n; i++)
		if (!strcmp(q[i].name, name)) return &q[i];
	return NULL;
}

static int seq_has(mhseq *q, int n, const char *name, int v)
{
	const mhseq *s = seq_find(q, n, name);
	return s && s->n && bsearch(&v, s->v, (size_t)s->n, sizeof v, int_cmp);
}

/* Put v in sequence `name` (on) or take it out; the file is rewritten
 * (as a new file, then renamed). */
static int seq_update(local_t *l, const char *dir, const char *name, int v, int on)
{
	mhseq *q;
	int n = seqs_read(dir, &q);
	mhseq *s = seq_find(q, n, name);
	if (on && !s) {
		q = xrealloc(q, sizeof *q * (size_t)(n + 1));
		s = &q[n++];
		memset(s, 0, sizeof *s);
		s->name = xstrdup(name);
	}
	if (s) {
		int k = 0;
		for (int i = 0; i < s->n; i++)
			if (s->v[i] != v) s->v[k++] = s->v[i];
		s->n = k;
		if (on) seq_add_num(s, v);
		if (s->n) qsort(s->v, (size_t)s->n, sizeof *s->v, int_cmp);
	}
	buf_t o = {0};
	for (int i = 0; i < n; i++) {
		if (!q[i].n) continue;
		buf_printf(&o, "%s:", q[i].name);
		for (int j = 0; j < q[i].n;) {
			int k = j;
			while (k + 1 < q[i].n && q[i].v[k + 1] <= q[i].v[k] + 1) k++;
			if (q[i].v[k] > q[i].v[j]) buf_printf(&o, " %d-%d", q[i].v[j], q[i].v[k]);
			else buf_printf(&o, " %d", q[i].v[j]);
			j = k + 1;
		}
		buf_addc(&o, '\n');
	}
	seqs_free(q, n);
	char *p = path_join(dir, ".mh_sequences");
	buf_t tmp = {0};
	buf_printf(&tmp, "%s.emsg%d", p, (int)getpid());
	int rc = -1;
	int fd = open(tmp.d, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd >= 0) {
		ssize_t w = o.len ? write(fd, o.d, o.len) : 0;
		int ok = w == (ssize_t)o.len;
		if (close(fd) != 0) ok = 0;
		if (ok && rename(tmp.d, p) == 0) rc = 0;
		else unlink(tmp.d);
	}
	if (rc != 0) seterr(l, "cannot update %s: %s", p, strerror(errno));
	free(p);
	buf_free(&tmp);
	buf_free(&o);
	return rc;
}

static int scan_mh(local_t *l, const char *path, lmsg **out, int *nout)
{
	DIR *d = opendir(path);
	if (!d) {
		seterr(l, "%s: %s", path, strerror(errno));
		return -1;
	}
	mhseq *q;
	int nq = seqs_read(path, &q);
	lmsg *m = NULL;
	int n = 0;
	struct dirent *e;
	while ((e = readdir(d))) {
		if (!all_digits(e->d_name)) continue;
		int v = atoi(e->d_name);
		m = xrealloc(m, sizeof *m * (size_t)(n + 1));
		lmsg *x = &m[n++];
		memset(x, 0, sizeof *x);
		x->key = xstrdup(e->d_name);
		x->file = xstrdup(e->d_name);
		x->seen = !seq_has(q, nq, "unseen", v);
		x->answered = seq_has(q, nq, "replied", v);
		x->flagged = seq_has(q, nq, "flagged", v);
	}
	closedir(d);
	seqs_free(q, nq);
	if (n) qsort(m, (size_t)n, sizeof *m, lmsg_order);
	*out = m;
	*nout = n;
	return 0;
}

static int from_line(const char *s, size_t n)
{
	return n >= 5 && !memcmp(s, "From ", 5);
}

static int scan_mbox(local_t *l, const char *path, lmsg **out, int *nout)
{
	FILE *f = fopen(path, "r");
	if (!f) {
		seterr(l, "%s: %s", path, strerror(errno));
		return -1;
	}
	lmsg *m = NULL;
	int n = 0;
	char *line = NULL;
	size_t cap = 0;
	ssize_t got;
	long long pos = 0, blank_at = -1;
	int prev_blank = 1, in_hdr = 0;
	buf_t hdr = {0};

	/* end the message before `at` (dropping the blank line that separates messages) */
	#define FINISH(at) do { \
		if (n) { \
			lmsg *x_ = &m[n - 1]; \
			long long e_ = (prev_blank && blank_at >= x_->off) ? blank_at : (at); \
			x_->len = (size_t)(e_ > x_->off ? e_ - x_->off : 0); \
			x_->size = (uint32_t)x_->len; \
			if (in_hdr) parse_hdr(x_, hdr.d ? hdr.d : "", hdr.len); \
		} \
	} while (0)

	while ((got = getline(&line, &cap, f)) > 0) {
		long long start = pos;
		pos += got;
		if (prev_blank && from_line(line, (size_t)got)) {
			FINISH(start);
			m = xrealloc(m, sizeof *m * (size_t)(n + 1));
			lmsg *x = &m[n++];
			memset(x, 0, sizeof *x);
			x->off = pos;
			in_hdr = 1;
			hdr.len = 0;
			prev_blank = 0;
			continue;
		}
		int blank = (got == 1 && line[0] == '\n') || (got == 2 && line[0] == '\r' && line[1] == '\n');
		if (in_hdr && n) {
			if (blank) {
				parse_hdr(&m[n - 1], hdr.d ? hdr.d : "", hdr.len);
				lmsg *x = &m[n - 1];
				const char *hd = hdr.d ? hdr.d : "";
				char *st = mime_header_raw(hd, hdr.len, "Status");
				char *xs = mime_header_raw(hd, hdr.len, "X-Status");
				char *id = mime_header_raw(hd, hdr.len, "Message-ID");
				x->seen = st && strchr(st, 'R');
				x->answered = xs && strchr(xs, 'A');
				x->flagged = xs && strchr(xs, 'F');
				x->deleted = xs && strchr(xs, 'D');
				if (id && *id) x->key = xstrdup(str_trim(id));
				free(st);
				free(xs);
				free(id);
				in_hdr = 0;
			} else if (hdr.len < HDR_MAX) {
				buf_add(&hdr, line, (size_t)got);
			}
		}
		if (blank) blank_at = start;
		prev_blank = blank;
	}
	FINISH(pos);
	#undef FINISH
	free(line);
	buf_free(&hdr);
	fclose(f);
	for (int i = 0; i < n; i++) {
		if (!m[i].key) {
			char k[40];
			snprintf(k, sizeof k, "off:%lld", m[i].off);
			m[i].key = xstrdup(k);
		}
		if (!m[i].have_hdr) parse_hdr(&m[i], "", 0);
	}
	*out = m;
	*nout = n;
	return 0;
}

static int scan_pop3(local_t *l, lmsg **out, int *nout)
{
	pop3_msg *pm;
	int pn;
	if (pop3_scan(l->pop, &pm, &pn) != 0) {
		seterr(l, "%s", pop3_error(l->pop));
		return -1;
	}
	l->pop_gen = pop3_generation(l->pop);
	l->pop_last = time(NULL);
	ps_prune(l, pm, pn);
	lmsg *m = xcalloc((size_t)pn + 1, sizeof *m);
	for (int i = 0; i < pn; i++) {
		m[i].num = pm[i].num;
		m[i].size = pm[i].size;
		m[i].key = xstrdup(pm[i].uidl);
		const pstate *s = ps_find(l, pm[i].uidl);
		m[i].seen = s && strchr(s->fl, 'S');
		m[i].answered = s && strchr(s->fl, 'R');
	}
	free(pm);
	*out = m;
	*nout = pn;
	return 0;
}

static void stamp(local_t *l, const lfolder *f)
{
	struct stat st;
	memset(&l->mt_new, 0, sizeof l->mt_new);
	memset(&l->mt_cur, 0, sizeof l->mt_cur);
	l->msize = 0;
	if (f->kind == LF_MAILDIR) {
		char *a = path_join(f->path, "new"), *b = path_join(f->path, "cur");
		if (stat(a, &st) == 0) l->mt_new = ST_MTIM(st);
		if (stat(b, &st) == 0) l->mt_cur = ST_MTIM(st);
		free(a);
		free(b);
	} else if (f->kind == LF_MH) {
		char *q = path_join(f->path, ".mh_sequences");
		if (stat(f->path, &st) == 0) l->mt_new = ST_MTIM(st);    /* messages added/removed */
		if (stat(q, &st) == 0) l->mt_cur = ST_MTIM(st);          /* flags */
		free(q);
	} else if (f->kind == LF_MBOX && stat(f->path, &st) == 0) {
		l->mt_new = ST_MTIM(st);
		l->msize = st.st_size;
	}
}

static int scan(local_t *l, const lfolder *f, lmsg **m, int *n)
{
	*m = NULL;
	*n = 0;
	switch (f->kind) {
	case LF_MAILDIR: return scan_maildir(l, f->path, m, n);
	case LF_MH:      return scan_mh(l, f->path, m, n);
	case LF_MBOX:    return scan_mbox(l, f->path, m, n);
	case LF_POP3:    return scan_pop3(l, m, n);
	case LF_NEW_MAILDIR: case LF_NEW_MBOX: case LF_NEW_MH: return 0;   /* not created yet: empty */
	}
	seterr(l, "%s is not a mailbox", f->name);
	return -1;
}

int local_select(local_t *l, const char *name, uint32_t *exists)
{
	lfolder *f = find_folder(l, name);
	if (!f) {
		seterr(l, "no folder %s", name);
		return -1;
	}
	close_folder(l);
	lmsg *m;
	int n;
	stamp(l, f);
	if (scan(l, f, &m, &n) != 0) return -1;
	for (int i = 0; i < n; i++) m[i].uid = l->uidnext++;
	l->m = m;
	l->n = n;
	l->sel = (int)(f - l->f);
	*exists = (uint32_t)n;
	return 0;
}

/* ------------------------------------------------------------------ */
/* Messages                                                            */

static lmsg *by_uid(local_t *l, uint32_t uid)
{
	for (int i = 0; i < l->n; i++)
		if (l->m[i].uid == uid) return &l->m[i];
	seterr(l, "message not found (it may have been moved or deleted)");
	return NULL;
}

static const lfolder *cur(local_t *l) { return l->sel >= 0 ? &l->f[l->sel] : NULL; }

/* POP3: after a new session the message numbers may differ; look them up. */
static int pop_sync(local_t *l)
{
	if (pop3_ensure(l->pop) != 0) {
		seterr(l, "%s", pop3_error(l->pop));
		return -1;
	}
	if (pop3_generation(l->pop) == l->pop_gen) return 0;
	pop3_msg *pm;
	int pn;
	if (pop3_scan(l->pop, &pm, &pn) != 0) {
		seterr(l, "%s", pop3_error(l->pop));
		return -1;
	}
	for (int i = 0; i < l->n; i++) {
		l->m[i].num = 0;
		for (int j = 0; j < pn; j++)
			if (!strcmp(pm[j].uidl, l->m[i].key)) l->m[i].num = pm[j].num;
	}
	free(pm);
	l->pop_gen = pop3_generation(l->pop);
	return 0;
}

/* Run TOP or RETR; when the connection had broken, once more in a new session. */
static int pop_fetch(local_t *l, lmsg *m, int whole, buf_t *out)
{
	for (int attempt = 0; attempt < 2; attempt++) {
		if (pop_sync(l) != 0) return -1;
		if (!m->num) {
			seterr(l, "the message is no longer on the server");
			return -1;
		}
		int rc = whole ? pop3_retr(l->pop, m->num, out) : pop3_top(l->pop, m->num, out);
		if (rc == 0) return 0;
		unsigned gen = pop3_generation(l->pop);
		if (pop3_ensure(l->pop) != 0 || pop3_generation(l->pop) == gen) break;   /* a real error */
	}
	seterr(l, "%s", pop3_error(l->pop));
	return -1;
}

static int load_header(local_t *l, lmsg *m)
{
	const lfolder *f = cur(l);
	if (f->kind == LF_POP3) {
		buf_t h = {0};
		if (pop_fetch(l, m, 0, &h) != 0) {
			buf_free(&h);
			return -1;
		}
		parse_hdr(m, h.d ? h.d : "", h.len);
		buf_free(&h);
		return 0;
	}
	/* Maildir */
	char *p = path_join(f->path, m->file);
	FILE *fp = fopen(p, "r");
	free(p);
	if (!fp) {
		parse_hdr(m, "", 0);          /* moved away meanwhile: show it empty */
		return 0;
	}
	struct stat st;
	if (fstat(fileno(fp), &st) == 0) m->size = (uint32_t)st.st_size;
	char *b = xmalloc(HDR_MAX + 1);
	size_t got = fread(b, 1, HDR_MAX, fp);
	fclose(fp);
	parse_hdr(m, b, header_len(b, got));
	free(b);
	return 0;
}

int local_fetch_headers(local_t *l, uint32_t from, uint32_t to, msg_t **out, int *nout)
{
	*out = NULL;
	*nout = 0;
	if (!cur(l)) {
		seterr(l, "no folder open");
		return -1;
	}
	if (from < 1 || to < from) return 0;
	if (to > (uint32_t)l->n) to = (uint32_t)l->n;
	if (from > to) return 0;
	uint32_t count = to - from + 1;
	msg_t *r = xcalloc(count, sizeof *r);
	for (uint32_t i = 0; i < count; i++) {
		lmsg *m = &l->m[from - 1 + i];
		if (!m->have_hdr && load_header(l, m) != 0) {
			msgs_free(r, (int)i);
			return -1;
		}
		r[i] = (msg_t){.seq = from + i, .uid = m->uid, .size = m->size, .seen = m->seen,
		               .answered = m->answered, .flagged = m->flagged, .deleted = m->deleted,
		               .date = m->date, .from = xstrdup(m->from ? m->from : ""),
		               .subject = xstrdup(m->subject ? m->subject : "")};
	}
	*out = r;
	*nout = (int)count;
	return 0;
}

int local_status_unseen(local_t *l, const char *name, uint32_t *unseen)
{
	if (!cur(l) || strcmp(cur(l)->name, name)) {
		seterr(l, "folder not open");
		return -1;
	}
	uint32_t u = 0;
	for (int i = 0; i < l->n; i++) u += !l->m[i].seen;
	*unseen = u;
	return 0;
}

/* Undo mboxrd quoting: ">From " -> "From ", ">>From " -> ">From ". */
static void unquote_from(buf_t *b)
{
	size_t o = 0;
	for (size_t i = 0; i < b->len;) {
		size_t e = i;
		while (e < b->len && b->d[e] != '\n') e++;
		if (e < b->len) e++;
		size_t j = i;
		while (j < e && b->d[j] == '>') j++;
		int skip = j > i && e - j >= 5 && !memcmp(b->d + j, "From ", 5);
		size_t from = i + (size_t)skip;
		memmove(b->d + o, b->d + from, e - from);
		o += e - from;
		i = e;
	}
	b->len = o;
}

static int load_raw(local_t *l, uint32_t uid)
{
	if (l->raw_uid == uid && l->raw.d) return 0;
	lmsg *m = by_uid(l, uid);
	if (!m) return -1;
	const lfolder *f = cur(l);
	buf_free(&l->raw);
	l->raw_uid = 0;
	if (f->kind == LF_POP3) {
		if (pop_fetch(l, m, 1, &l->raw) != 0) return -1;
	} else if (f->kind == LF_MAILDIR || f->kind == LF_MH) {
		char *p = path_join(f->path, m->file);
		FILE *fp = fopen(p, "r");
		free(p);
		if (!fp) {
			seterr(l, "cannot read the message: %s (it may have been moved)", strerror(errno));
			return -1;
		}
		char tmp[16384];
		size_t got;
		while ((got = fread(tmp, 1, sizeof tmp, fp)) > 0) buf_add(&l->raw, tmp, got);
		fclose(fp);
	} else {
		FILE *fp = fopen(f->path, "r");
		if (!fp || fseeko(fp, (off_t)m->off, SEEK_SET) != 0) {
			if (fp) fclose(fp);
			seterr(l, "cannot read %s: %s", f->path, strerror(errno));
			return -1;
		}
		char *d = xmalloc(m->len + 1);
		size_t got = fread(d, 1, m->len, fp);
		fclose(fp);
		buf_add(&l->raw, d, got);
		free(d);
		unquote_from(&l->raw);
	}
	buf_cstr(&l->raw);
	l->raw_hlen = header_len(l->raw.d, l->raw.len);
	l->raw_uid = uid;
	if (f->kind != LF_POP3) m->size = (uint32_t)l->raw.len;
	return 0;
}

int local_fetch_structure(local_t *l, uint32_t uid, char **hdr, size_t *hlen, mpart_t **root)
{
	*hdr = NULL;
	*hlen = 0;
	*root = NULL;          /* no structure: the viewer decodes the raw message itself */
	if (load_raw(l, uid) != 0) return -1;
	*hdr = xstrndup(l->raw.d, l->raw_hlen);
	*hlen = l->raw_hlen;
	return 0;
}

int local_fetch_partial(local_t *l, uint32_t uid, imap_partial_t *req, int n)
{
	if (load_raw(l, uid) != 0) return -1;
	for (int i = 0; i < n; i++) {
		const char *s = l->raw.d;
		size_t len = l->raw.len;
		if (!strcmp(req[i].section, "TEXT")) {
			s += l->raw_hlen;
			len -= l->raw_hlen;
		} else if (!strcmp(req[i].section, "HEADER")) {
			len = l->raw_hlen;
		} else if (*req[i].section) {
			seterr(l, "no part %s", req[i].section);
			return -1;
		}
		size_t off = req[i].off < len ? req[i].off : len;
		size_t k = len - off < req[i].len ? len - off : req[i].len;
		buf_add(req[i].out, s + off, k);
		req[i].got = k;
	}
	return 0;
}

/* Maildir: rewrite the flags in the file name (and move new/ to cur/). */
static int maildir_set(local_t *l, lmsg *m, char flag)
{
	const lfolder *f = cur(l);
	const char *base = strchr(m->file, '/') + 1;
	const char *info = strstr(base, MD_INFO);
	char fl[32] = "";
	if (info) snprintf(fl, sizeof fl, "%s", info + 3);
	if (!strchr(fl, flag) && strlen(fl) + 1 < sizeof fl) {
		size_t k = strlen(fl);
		fl[k] = flag;
		fl[k + 1] = '\0';
	}
	/* flags in ASCII order */
	for (size_t i = 0; fl[i]; i++)
		for (size_t j = i + 1; fl[j]; j++)
			if (fl[j] < fl[i]) { char t = fl[i]; fl[i] = fl[j]; fl[j] = t; }
	buf_t nf = {0};
	buf_printf(&nf, "cur/%.*s" MD_INFO "%s", (int)(info ? (size_t)(info - base) : strlen(base)), base, fl);
	char *from = path_join(f->path, m->file), *to = path_join(f->path, nf.d);
	int rc = strcmp(from, to) ? rename(from, to) : 0;
	if (rc != 0) seterr(l, "cannot set the flag: %s", strerror(errno));
	free(from);
	free(to);
	if (rc == 0) {
		free(m->file);
		m->file = buf_steal(&nf);
		stamp(l, f);      /* our own change is not news */
	} else {
		buf_free(&nf);
	}
	return rc;
}

static int set_flag(local_t *l, uint32_t uid, char flag)
{
	lmsg *m = by_uid(l, uid);
	if (!m) return -1;
	if (flag == 'S') m->seen = 1;
	if (flag == 'R') m->answered = 1;
	if (flag == 'F') m->flagged = 1;
	const lfolder *f = cur(l);
	if (f->kind == LF_MAILDIR) return maildir_set(l, m, flag);
	if (f->kind == LF_MH) {
		int v = atoi(m->key);
		int rc = flag == 'S' ? seq_update(l, f->path, "unseen", v, 0)
		       : seq_update(l, f->path, flag == 'R' ? "replied" : "flagged", v, 1);
		stamp(l, f);      /* our own change is not news */
		return rc;
	}
	if (f->kind == LF_POP3) ps_set(l, m->key, flag);
	return 0;   /* mbox: kept in memory only */
}

int local_mark_seen(local_t *l, uint32_t uid) { return set_flag(l, uid, 'S'); }

int local_add_flags(local_t *l, uint32_t uid, const char *flags)
{
	int rc = 0;
	if (strstr(flags, "\\Answered")) rc |= set_flag(l, uid, 'R');
	if (strstr(flags, "\\Seen")) rc |= set_flag(l, uid, 'S');
	if (strstr(flags, "\\Flagged")) rc |= set_flag(l, uid, 'F');
	return rc;
}

/* ------------------------------------------------------------------ */
/* Changes                                                             */

typedef struct { const char *key; int idx; } keyidx;

static int ki_cmp(const void *a, const void *b)
{
	const keyidx *x = a, *y = b;
	int c = strcmp(x->key, y->key);
	return c ? c : x->idx - y->idx;
}

/* Merge a new scan into the open folder: kept messages keep their uid and
 * place, removed ones are reported as expunged, new ones go at the end. */
static void merge(local_t *l, lmsg *nm, int nn)
{
	keyidx *ki = xmalloc(sizeof *ki * ((size_t)nn + 1));
	char *used = xcalloc((size_t)nn + 1, 1);
	for (int j = 0; j < nn; j++) ki[j] = (keyidx){nm[j].key, j};
	if (nn) qsort(ki, (size_t)nn, sizeof *ki, ki_cmp);

	lmsg *res = xmalloc(sizeof *res * ((size_t)l->n + (size_t)nn + 1));
	int nr = 0;
	int *gone = xmalloc(sizeof *gone * ((size_t)l->n + 1));
	int ngone = 0, flags_changed = 0;
	for (int i = 0; i < l->n; i++) {
		lmsg *o = &l->m[i];
		int j = -1;
		int lo = 0, hi = nn;
		while (lo < hi) {                       /* first entry with this key */
			int mid = (lo + hi) / 2;
			if (strcmp(ki[mid].key, o->key) < 0) lo = mid + 1;
			else hi = mid;
		}
		for (int k = lo; k < nn && !strcmp(ki[k].key, o->key); k++)
			if (!used[ki[k].idx]) { j = ki[k].idx; break; }
		if (j < 0) {
			gone[ngone++] = i + 1;
			msg_clear(o);
			continue;
		}
		used[j] = 1;
		lmsg *x = &nm[j];
		x->uid = o->uid;
		if (!x->have_hdr && o->have_hdr) {   /* keep what was read already */
			x->have_hdr = 1;
			x->date = o->date;
			x->from = o->from;
			x->subject = o->subject;
			o->from = o->subject = NULL;
			if (!x->size) x->size = o->size;
		}
		if (x->seen != o->seen || x->answered != o->answered || x->flagged != o->flagged)
			flags_changed = 1;
		if (cur(l)->kind == LF_MBOX) {       /* "seen" set here lives in memory */
			x->seen |= o->seen;
			x->answered |= o->answered;
		}
		msg_clear(o);
		res[nr++] = *x;
		memset(x, 0, sizeof *x);
	}
	for (int j = 0; j < nn; j++)
		if (!used[j]) {
			nm[j].uid = l->uidnext++;
			res[nr++] = nm[j];
			memset(&nm[j], 0, sizeof nm[j]);
		}
	int added = nr - (l->n - ngone);
	free(l->m);
	l->m = res;
	l->n = nr;
	/* expunges in descending order: each number stays valid on its own */
	for (int i = ngone - 1; i >= 0; i--) {
		l->exp = xrealloc(l->exp, sizeof *l->exp * (size_t)(l->nexp + 1));
		l->exp[l->nexp++] = (uint32_t)gone[i];
	}
	if (ngone || added || flags_changed) l->changed = 1;
	if (ngone) {
		buf_free(&l->raw);
		l->raw_uid = 0;
	}
	free(gone);
	free(used);
	free(ki);
	free(nm);
}

int local_noop(local_t *l, int force)
{
	const lfolder *f = cur(l);
	if (!f) return 0;
	if (f->kind == LF_POP3) {
		if (!force && time(NULL) - l->pop_last < POP3_REFRESH) {
			if (pop3_noop(l->pop) != 0) {
				seterr(l, "%s", pop3_error(l->pop));
				return -1;
			}
			return pop_sync(l);
		}
		if (pop3_reconnect(l->pop) != 0) {
			l->pop_last = time(NULL);
			seterr(l, "%s", pop3_error(l->pop));
			return -1;
		}
	} else {
		struct timespec a = l->mt_new, b = l->mt_cur;
		off_t sz = l->msize;
		stamp(l, f);
		if (!force && !memcmp(&a, &l->mt_new, sizeof a) && !memcmp(&b, &l->mt_cur, sizeof b) &&
		    sz == l->msize)
			return 0;
	}
	lmsg *nm;
	int nn;
	if (scan(l, f, &nm, &nn) != 0) return -1;
	merge(l, nm, nn);
	return 0;
}

int local_changes(local_t *l, uint32_t *exists, const uint32_t **expunged, int *n)
{
	int changed = l->changed;
	free(l->exp_given);
	l->exp_given = l->exp;
	*expunged = l->exp_given;
	*n = l->nexp;
	*exists = (uint32_t)l->n;
	l->exp = NULL;
	l->nexp = 0;
	l->changed = 0;
	return changed;
}

/* ------------------------------------------------------------------ */
/* Saving messages (Sent, Drafts)                                      */

/* CRLF -> LF */
static void to_lf(buf_t *out, const char *d, size_t n)
{
	for (size_t i = 0; i < n; i++)
		if (!(d[i] == '\r' && i + 1 < n && d[i + 1] == '\n')) buf_addc(out, d[i]);
	if (out->len && out->d[out->len - 1] != '\n') buf_addc(out, '\n');
}

static int write_all(int fd, const char *p, size_t n)
{
	while (n) {
		ssize_t w = write(fd, p, n);
		if (w < 0) {
			if (errno == EINTR) continue;
			return -1;
		}
		p += w;
		n -= (size_t)w;
	}
	return 0;
}

static int append_maildir(local_t *l, const char *path, const char *flags, const buf_t *msg)
{
	static const char *const sub[] = {"", "/cur", "/new", "/tmp"};
	for (int i = 0; i < 4; i++) {
		buf_t d = {0};
		buf_printf(&d, "%s%s", path, sub[i]);
		if (mkdir(d.d, 0700) != 0 && errno != EEXIST) {
			seterr(l, "cannot create %s: %s", d.d, strerror(errno));
			buf_free(&d);
			return -1;
		}
		buf_free(&d);
	}
	static unsigned counter;
	char host[128] = "localhost";
#ifdef _WIN32
	win_gethostname(host, sizeof host - 1);
#else
	gethostname(host, sizeof host - 1);
#endif
	for (char *s = host; *s; s++)
		if (*s == '/' || *s == ':' || *s == MAILDIR_SEP) *s = '_';
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	char fl[8] = "";
	size_t k = 0;
	if (flags && strstr(flags, "\\Draft")) fl[k++] = 'D';
	if (flags && strstr(flags, "\\Flagged")) fl[k++] = 'F';
	if (flags && strstr(flags, "\\Answered")) fl[k++] = 'R';
	if (flags && strstr(flags, "\\Seen")) fl[k++] = 'S';
	buf_t base = {0}, tmp = {0}, dst = {0};
	buf_printf(&base, "%lld.M%ldP%dQ%u.%s", (long long)ts.tv_sec, ts.tv_nsec / 1000, (int)getpid(),
	           ++counter, host);
	buf_printf(&tmp, "%s/tmp/%s", path, base.d);
	buf_printf(&dst, "%s/cur/%s" MD_INFO "%s", path, base.d, fl);
	int rc = -1;
	int fd = open(tmp.d, O_WRONLY | O_CREAT | O_EXCL, 0600);
	if (fd < 0) {
		seterr(l, "cannot write %s: %s", tmp.d, strerror(errno));
	} else {
		int ok = write_all(fd, msg->d, msg->len) == 0 && fsync(fd) == 0;
		if (close(fd) != 0) ok = 0;
		if (ok && rename(tmp.d, dst.d) == 0) rc = 0;
		else {
			seterr(l, "cannot write %s: %s", tmp.d, strerror(errno));
			unlink(tmp.d);
		}
	}
	buf_free(&base);
	buf_free(&tmp);
	buf_free(&dst);
	return rc;
}

static int append_mbox(local_t *l, const char *path, const char *flags, const buf_t *msg)
{
	int fd = open(path, O_RDWR | O_APPEND | O_CREAT, 0600);
	if (fd < 0) {
		seterr(l, "cannot open %s: %s", path, strerror(errno));
		return -1;
	}
#ifdef _WIN32
	if (win_lock(fd) != 0) {
#else
	struct flock lk = {.l_type = F_WRLCK, .l_whence = SEEK_SET};
	if (fcntl(fd, F_SETLKW, &lk) != 0) {
#endif
		seterr(l, "cannot lock %s: %s", path, strerror(errno));
		close(fd);
		return -1;
	}
	buf_t o = {0};
	struct stat st;
	if (fstat(fd, &st) == 0 && st.st_size > 0) {
		/* the previous message must end in a blank line */
		char end[2] = {0, 0};
		off_t at = st.st_size >= 2 ? st.st_size - 2 : 0;
		ssize_t r = pread(fd, end, (size_t)(st.st_size - at), at);
		if (r == 2 && !(end[0] == '\n' && end[1] == '\n')) buf_adds(&o, end[1] == '\n' ? "\n" : "\n\n");
		else if (r == 1 && end[0] != '\n') buf_adds(&o, "\n\n");
	}
	time_t now = time(NULL);
	char date[64];
	struct tm tm;
	gmtime_r(&now, &tm);
	char wd_mon[16];
	strftime(wd_mon, sizeof wd_mon, "%a %b", &tm);
	snprintf(date, sizeof date, "%s %2d %02d:%02d:%02d %d", wd_mon, tm.tm_mday, tm.tm_hour, tm.tm_min,
	         tm.tm_sec, tm.tm_year + 1900);
	buf_printf(&o, "From emsg@localhost %s\n", date);
	const char *st_flags = flags && strstr(flags, "\\Seen") ? "RO" : "O";
	buf_printf(&o, "Status: %s\n", st_flags);
	if (flags && strstr(flags, "\\Draft")) buf_adds(&o, "X-Status: T\n");
	/* the message, with mboxrd quoting of ">*From " lines */
	for (size_t i = 0; i < msg->len;) {
		size_t e = i;
		while (e < msg->len && msg->d[e] != '\n') e++;
		if (e < msg->len) e++;
		size_t j = i;
		while (j < e && msg->d[j] == '>') j++;
		if (e - j >= 5 && !memcmp(msg->d + j, "From ", 5)) buf_addc(&o, '>');
		buf_add(&o, msg->d + i, e - i);
		i = e;
	}
	buf_addc(&o, '\n');
	int rc = write_all(fd, o.d, o.len) == 0 && fsync(fd) == 0 ? 0 : -1;
	if (rc != 0) seterr(l, "cannot write %s: %s", path, strerror(errno));
	buf_free(&o);
	close(fd);   /* also releases the lock */
	return rc;
}

/* MH: the next free number; unread messages go into the unseen sequence. */
static int append_mh(local_t *l, const char *path, const char *flags, const buf_t *msg)
{
	if (mkdir(path, 0700) != 0 && errno != EEXIST) {
		seterr(l, "cannot create %s: %s", path, strerror(errno));
		return -1;
	}
	int max = 0;
	DIR *d = opendir(path);
	if (d) {
		struct dirent *e;
		while ((e = readdir(d)))
			if (all_digits(e->d_name) && atoi(e->d_name) > max) max = atoi(e->d_name);
		closedir(d);
	}
	for (int v = max + 1; v < max + 1000; v++) {
		char num[16];
		snprintf(num, sizeof num, "%d", v);
		char *p = path_join(path, num);
		int fd = open(p, O_WRONLY | O_CREAT | O_EXCL, 0600);
		if (fd < 0) {
			free(p);
			if (errno == EEXIST) continue;      /* taken meanwhile: the next one */
			seterr(l, "cannot write in %s: %s", path, strerror(errno));
			return -1;
		}
		int ok = write_all(fd, msg->d, msg->len) == 0 && fsync(fd) == 0;
		if (close(fd) != 0) ok = 0;
		if (!ok) {
			seterr(l, "cannot write %s: %s", p, strerror(errno));
			unlink(p);
			free(p);
			return -1;
		}
		free(p);
		if (!(flags && strstr(flags, "\\Seen"))) return seq_update(l, path, "unseen", v, 1);
		return 0;
	}
	seterr(l, "no free message number in %s", path);
	return -1;
}

int local_append(local_t *l, const char *name, const char *flags, const char *data, size_t len)
{
	lfolder *f = find_folder(l, name);
	if (!f) {
		seterr(l, "no folder %s", name);
		return -1;
	}
	if (f->kind == LF_POP3 || f->kind == LF_DIR) {
		seterr(l, "messages cannot be stored in %s", name);
		return -1;
	}
	buf_t msg = {0};
	to_lf(&msg, data, len);
	int rc;
	if (f->kind == LF_MH || f->kind == LF_NEW_MH) {
		rc = append_mh(l, f->path, flags, &msg);
		if (rc == 0) f->kind = LF_MH;
	} else if (f->kind == LF_MAILDIR || f->kind == LF_NEW_MAILDIR) {
		rc = append_maildir(l, f->path, flags, &msg);
		if (rc == 0) f->kind = LF_MAILDIR;
	} else {
		rc = append_mbox(l, f->path, flags, &msg);
		if (rc == 0) f->kind = LF_MBOX;
	}
	buf_free(&msg);
	return rc;
}
