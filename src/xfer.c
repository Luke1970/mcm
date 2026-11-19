/* xfer.c - Account > export and import (the menu, F1), mcm --export / --import
 *
 * Export writes the folders of the account (or one folder) into a new
 * Maildir: INBOX in the Maildir itself, the other folders as Maildir++
 * subfolders (.Name.Sub, names in modified UTF-7, as Dovecot keeps them).
 * mcm only writes into a directory it makes itself (or one that is empty),
 * never into or below an existing Maildir or an account's own mail, so
 * mail that is already there is not touched.
 *
 * Import reads a Maildir, an mbox file, an MH folder or a directory of
 * them (as a local account does) and copies everything into one new folder
 * of the account, Imported-YYYY-MM-DD, with the folders below it as they
 * were. When the account cannot keep that structure (a folder that cannot
 * hold subfolders), mcm asks first and then uses flat names at the top:
 * "Imported-YYYY-MM-DD - Archive - 2025".
 */
#include "mcm.h"
#include "local.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>

#define XFER_CHUNK 200                /* message headers fetched at a time */

/* ------------------------------------------------------------------ */
/* Shared                                                              */

static int is_dir(const char *p)
{
	struct stat st;
	return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

/* An empty directory (nothing but . and ..). */
static int empty_dir(const char *p)
{
	DIR *d = opendir(p);
	if (!d) return 0;
	struct dirent *e;
	int empty = 1;
	while (empty && (e = readdir(d)))
		if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) empty = 0;
	closedir(d);
	return empty;
}

/* IMAP flags of message m (\Deleted only when keep_deleted). */
static void msg_flags(char *out, size_t n, const msg_t *m, int keep_deleted)
{
	snprintf(out, n, "%s%s%s%s", m->seen ? "\\Seen " : "", m->answered ? "\\Answered " : "",
	         m->flagged ? "\\Flagged " : "", keep_deleted && m->deleted ? "\\Deleted " : "");
	size_t k = strlen(out);
	if (k) out[k - 1] = '\0';
}

/* Esc pressed while working: ask whether to stop. */
static int stop_asked(const char *what)
{
	timeout(0);
	int ch = getch();
	timeout(-1);
	if (ch != 27) return 0;
	char q[80];
	snprintf(q, sizeof q, "Stop the %s?", what);
	q[9] = (char)tolower((unsigned char)q[9]);   /* "Stop the export?" */
	return ui_confirm(what, q, draw_bg);
}

/* The progress window, at most every 1/10 s (or when force). */
static void progress(int force, const char *fmt, ...) __attribute__((format(MCM_PRINTF, 2, 3)));
static void progress(int force, const char *fmt, ...)
{
	static struct timespec last;
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	long ms = (now.tv_sec - last.tv_sec) * 1000 + (now.tv_nsec - last.tv_nsec) / 1000000;
	if (!force && ms < 100) return;
	last = now;
	char msg[300];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(msg, sizeof msg, fmt, ap);
	va_end(ap);
	char full[340];
	snprintf(full, sizeof full, "%s   (Esc: stop)", msg);
	ui_progress(full, draw_bg);
}

/* The parts of folder name raw (split at delim), as UTF-8; free with
 * parts_free. dec: the names are modified UTF-7. */
static char **parts_of(const char *raw, char delim, int dec, int *n)
{
	char **p = NULL;
	*n = 0;
	const char *s = raw;
	for (;;) {
		const char *e = delim ? strchr(s, delim) : NULL;
		char *one = e ? xstrndup(s, (size_t)(e - s)) : xstrdup(s);
		if (dec) {
			char *d = mutf7_decode(one);
			free(one);
			one = d;
		}
		p = xrealloc(p, sizeof *p * (size_t)(*n + 1));
		p[(*n)++] = one;
		if (!e) break;
		s = e + 1;
	}
	return p;
}

static void parts_free(char **p, int n)
{
	for (int i = 0; i < n; i++) free(p[i]);
	free(p);
}

/* Replace the characters in bad (and control characters) with '_'; 1 when
 * something was replaced. */
static int clean(char *s, const char *bad)
{
	int changed = 0;
	for (unsigned char *p = (unsigned char *)s; *p; p++)
		if (*p < 0x20 || *p == 0x7F || strchr(bad, *p)) {
			*p = '_';
			changed = 1;
		}
	return changed;
}

/* The messages of the folder the store has open (exists of them), one by
 * one: fn(m, raw message, ctx); a non-zero return stops. stop: Esc. */
typedef int (*msg_fn)(const msg_t *m, const buf_t *raw, void *ctx);

static int each_message(store_t *st, local_t *lo, uint32_t exists, msg_fn fn, void *ctx, int *stop,
                        const char *what)
{
	for (uint32_t from = 1; from <= exists; from += XFER_CHUNK) {
		uint32_t to = from + XFER_CHUNK - 1 < exists ? from + XFER_CHUNK - 1 : exists;
		msg_t *ms;
		int n;
		int rc = st ? store_fetch_headers(st, from, to, &ms, &n) : local_fetch_headers(lo, from, to, &ms, &n);
		if (rc != 0) return -1;
		for (int i = 0; i < n && rc == 0; i++) {
			if (!ms[i].uid) continue;
			if (stop_asked(what)) {
				*stop = 1;
				rc = 1;
				break;
			}
			buf_t raw = {0};
			imap_partial_t req = {.section = "", .off = 0, .len = 0x7fffffff, .out = &raw};
			if ((st ? store_fetch_partial(st, ms[i].uid, &req, 1) : local_fetch_partial(lo, ms[i].uid, &req, 1)) != 0)
				rc = -1;
			else
				rc = fn(&ms[i], &raw, ctx);
			buf_free(&raw);
		}
		msgs_free(ms, n);
		if (rc != 0) return rc < 0 ? -1 : 0;
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* Export                                                              */

typedef struct {
	const char *dir;       /* the Maildir (folder) being written */
	const char *name;      /* the folder, shown */
	int         done, total;
	long        all;       /* messages exported in all folders */
	char        err[600];
} exp_t;

static int export_one(const msg_t *m, const buf_t *raw, void *ctx)
{
	exp_t *x = ctx;
	char fl[64];
	msg_flags(fl, sizeof fl, m, 1);
	if (local_maildir_write(x->dir, fl, raw->d ? raw->d : "", raw->len, m->date, x->err, sizeof x->err) != 0)
		return -1;
	x->done++;
	x->all++;
	progress(0, "Exporting %s  %d/%d", x->name, x->done, x->total);
	return 0;
}

/* The last path separator in p ('/', on Windows also '\\'), or NULL. */
static char *last_sep(char *p)
{
	char *e = strrchr(p, '/');
#ifdef _WIN32
	char *b = strrchr(p, '\\');
	if (b && (!e || b > e)) e = b;
#endif
	return e;
}

/* Why path cannot be exported to, or NULL. It must be new (its parent
 * there) or an empty directory, and not inside a Maildir or the mail of a
 * local account. */
static const char *export_target_bad(const char *path, char *why, size_t n)
{
	if (!*path) return "Type a directory";
	struct stat st;
	if (stat(path, &st) == 0 && (!S_ISDIR(st.st_mode) || !empty_dir(path))) {
		snprintf(why, n, "Exists and is not empty: give a new directory (%.900s)", path);
		return why;
	}
	/* the directory above it, and every one above that */
	char parent[1024];
	snprintf(parent, sizeof parent, "%s", path);
	size_t k = strlen(parent);
	while (k > 1 && (parent[k - 1] == '/' || parent[k - 1] == '\\')) parent[--k] = '\0';
	char *sl = last_sep(parent);
	if (!sl) snprintf(parent, sizeof parent, ".");
	else if (sl == parent) parent[1] = '\0';
	else *sl = '\0';
	char real[4096];
	if (!realpath(parent, real) || !is_dir(real)) {
		snprintf(why, n, "The directory above it does not exist (%.900s)", parent);
		return why;
	}
	for (char up[4096];;) {
		if (local_is_maildir(real)) {
			snprintf(why, n, "Inside an existing Maildir: export somewhere else (%.900s)", real);
			return why;
		}
		for (int i = 0; i < naccts; i++) {
			if (accts[i].type == STORE_IMAP || !*accts[i].path) continue;
			char p[1024], r[4096];
			expand_home(p, sizeof p, accts[i].path);
			if (realpath(p, r) && !strcmp(r, real)) {
				snprintf(why, n, "Inside the mail of account %s: export somewhere else", accts[i].name);
				return why;
			}
		}
		char *e = last_sep(real);
		if (!e || e == real || e[-1] == ':') break;
		snprintf(up, sizeof up, "%.*s", (int)(e - real), real);
		snprintf(real, sizeof real, "%s", up);
	}
	return NULL;
}

/* An empty Maildir at dir (cur new tmp); sub: also the Maildir++ marker. */
static int make_maildir(const char *dir, int sub, char *err, size_t n)
{
	static const char *const d[] = {"", "/cur", "/new", "/tmp"};
	for (int i = 0; i < 4; i++) {
		char p[1100];
		snprintf(p, sizeof p, "%s%s", dir, d[i]);
		if (mkdir(p, 0700) != 0 && errno != EEXIST) {
			snprintf(err, n, "cannot create %.400s: %s", p, strerror(errno));
			return -1;
		}
	}
	if (sub) {
		char p[1100];
		snprintf(p, sizeof p, "%s/maildirfolder", dir);
		FILE *f = fopen(p, "w");
		if (f) fclose(f);
	}
	return 0;
}

void folder_export(void)
{
	if (!A.im || !A.nfolders) {
		set_status("No folders to export");
		return;
	}
	int one = A.fsel >= 0 && A.fsel < A.nfolders ? A.fsel : A.cur_folder;
	char l1[300];
	snprintf(l1, sizeof l1, "Export to a new Maildir: every folder of %s,", ac ? ac->name : "this account");
	char l2[300];
	snprintf(l2, sizeof l2, "or only %s (the folder under the cursor).",
	         one >= 0 ? A.folders[one].display : "one folder");
	const char *lines[] = {l1, l2, "No mail is changed, here or in an existing Maildir."};
	ui_menuitem btn[] = {{"All folders", 0, 'a'}, {"This folder", 0, 't'}, {"Cancel", 0, 'c'}};
	int c = ui_choice_dialog("Export", lines, 3, btn, one >= 0 ? 3 : 2, 0, 0, draw_bg);
	if (c < 0 || (one >= 0 ? c == 2 : c == 1)) return;
	int only = c == 1 ? one : -1;

	/* where: a new directory */
	char day[16], path[1024], full[1024], why[1200];
	time_t now = time(NULL);
	struct tm tm;
	localtime_r(&now, &tm);
	strftime(day, sizeof day, "%Y-%m-%d", &tm);
	for (int k = 1; k < 100; k++) {
		if (k == 1) snprintf(path, sizeof path, "~/mcm-export-%s", day);
		else snprintf(path, sizeof path, "~/mcm-export-%s-%d", day, k);
		expand_home(full, sizeof full, path);
		struct stat st;
		if (stat(full, &st) != 0) break;
	}
	const char *err = NULL;
	for (;;) {
		ui_field f[] = {{"Directory", path, sizeof path, 0, 0}};
		if (ui_input_dialog("Export to Maildir", f, 1, err ? err : "A new directory: it becomes the Maildir",
		                    draw_bg) < 0)
			return;
		expand_home(full, sizeof full, str_trim(path));
		err = export_target_bad(full, why, sizeof why);
		if (!err) break;
	}
	buf_t m = {0};
	xfer_export(full, only, &m);
	ui_message("Export", buf_cstr(&m), draw_bg);
	set_status("%s", m.d);
	buf_free(&m);
}

/* Export folder only (-1: all) into the new Maildir full (checked with
 * export_target_bad); what happened goes in msg. 0 = all done. */
int xfer_export(const char *full, int only, buf_t *msg)
{
	char e[600];
	if (make_maildir(full, 0, e, sizeof e) != 0) {
		buf_adds(msg, e);
		return -1;
	}

	/* the folders: INBOX the Maildir itself, others .A.B */
	char pre[64];
	fld_prefix(pre, sizeof pre);
	int dec = store_kind(A.im) == STORE_IMAP || fld_delim() == '.';
	exp_t x = {0};
	int nf = 0, renamed = 0, failed = 0, stop = 0;
	char **used = NULL;
	int nused = 0;
	for (int i = 0; i < A.nfolders && !stop && !failed; i++) {
		if (only >= 0 && i != only) continue;
		const folder_t *f = &A.folders[i];
		if (!store_folder_real(A.im, f->name)) continue;   /* listed, not made yet */
		const char *raw = f->name;
		if (*pre && !strncmp(raw, pre, strlen(pre))) raw += strlen(pre);
		buf_t sub = {0};
		if (strcasecmp(f->name, "INBOX")) {
			int np;
			char **p = parts_of(raw, f->delim, dec, &np);
			for (int j = 0; j < np; j++) {
#ifdef _WIN32
				renamed += clean(p[j], "./\\:*?\"<>|");
#else
				renamed += clean(p[j], "./");
#endif
				char *enc = mutf7_encode(p[j]);
				buf_printf(&sub, ".%s", enc);
				free(enc);
			}
			parts_free(p, np);
			/* two folders that became the same name: number the second */
			buf_t base = {0};
			buf_adds(&base, buf_cstr(&sub));
			for (int k = 2;; k++) {
				int dup = 0;
				for (int u = 0; u < nused; u++)
					if (!strcmp(used[u], buf_cstr(&sub))) dup = 1;
				if (!dup) break;
				sub.len = 0;
				buf_printf(&sub, "%s_%d", base.d, k);
			}
			buf_free(&base);
			used = xrealloc(used, sizeof *used * (size_t)(nused + 1));
			used[nused++] = xstrdup(buf_cstr(&sub));
		}
		char dir[2200];
		snprintf(dir, sizeof dir, "%s%s%s", full, sub.len ? "/" : "", sub.len ? sub.d : "");
		buf_free(&sub);
		if (f->noselect) continue;           /* holds only other folders */
		if (make_maildir(dir, dir[strlen(full)] != '\0', e, sizeof e) != 0) {
			snprintf(x.err, sizeof x.err, "%s", e);
			failed = 1;
			break;
		}
		uint32_t exists;
		progress(1, "Opening %s ...", f->display);
		if (store_select(A.im, f->name, &exists) != 0) {
			snprintf(x.err, sizeof x.err, "cannot open %s: %s", f->display, store_error(A.im));
			failed = 1;
			break;
		}
		x.dir = dir;
		x.name = f->display;
		x.done = 0;
		x.total = (int)exists;
		nf++;
		progress(1, "Exporting %s  0/%d", f->display, x.total);
		if (each_message(A.im, NULL, exists, export_one, &x, &stop, "Export") != 0) {
			if (!*x.err) snprintf(x.err, sizeof x.err, "%s: %s", f->display, store_error(A.im));
			failed = 1;
		}
	}
	for (int u = 0; u < nused; u++) free(used[u]);
	free(used);
	reopen_folder();                         /* the store had other folders open */

	buf_t *m = msg;
	if (failed) buf_printf(m, "Error: %s. ", x.err);
	else if (stop) buf_adds(m, "Stopped. ");
	buf_printf(m, "%ld message%s from %d folder%s written to %s", x.all, x.all == 1 ? "" : "s", nf,
	           nf == 1 ? "" : "s", full);
	if (renamed) buf_printf(m, " (%d name%s with . or / got _ instead)", renamed, renamed == 1 ? "" : "s");
	return failed || stop ? -1 : 0;
}

/* --export DIR: why DIR cannot be used, or NULL (checked before login). */
const char *xfer_export_check(const char *dir, char *why, size_t n)
{
	return export_target_bad(dir, why, n);
}

/* ------------------------------------------------------------------ */
/* Import                                                              */

typedef struct {
	const char *dest;      /* raw name of the folder written to */
	const char *name;      /* the source folder, shown */
	int         crlf;      /* IMAP wants CRLF line ends */
	int         done, total;
	long        all;
	char        err[600];
} imp_t;

static int import_one(const msg_t *m, const buf_t *raw, void *ctx)
{
	imp_t *x = ctx;
	char fl[64];
	msg_flags(fl, sizeof fl, m, 0);          /* never marked deleted here */
	buf_t d = {0};
	const char *s = raw->d ? raw->d : "";
	if (x->crlf) {
		for (size_t i = 0; i < raw->len; i++) {
			if (s[i] == '\n' && (i == 0 || s[i - 1] != '\r')) buf_addc(&d, '\r');
			buf_addc(&d, s[i]);
		}
	} else {
		buf_add(&d, s, raw->len);
	}
	int rc = store_append(A.im, x->dest, fl, d.d ? d.d : "", d.len);
	buf_free(&d);
	if (rc != 0) {
		snprintf(x->err, sizeof x->err, "%s", store_error(A.im));
		return -1;
	}
	x->done++;
	x->all++;
	progress(0, "Importing %s  %d/%d", x->name, x->done, x->total);
	return 0;
}

/* Does folder raw exist in the account (read again)? */
static int folder_exists(const char *raw)
{
	folder_t *f;
	int n, found = 0;
	if (store_list(A.im, &f, &n) != 0) return 0;
	for (int i = 0; i < n; i++)
		if (!strcmp(f[i].name, raw)) found = 1;
	folders_free(f, n);
	return found;
}

/* Create the folder shown as name; 0 when it is there now. *raw_out: its
 * raw name. */
static int make_folder(const char *name, char **raw_out)
{
	char *raw = fld_stored(name);
	int rc = store_create(A.im, raw) == 0 || folder_exists(raw) ? 0 : -1;
	if (rc == 0 && raw_out) *raw_out = raw;
	else free(raw);
	return rc;
}

/* dest (top/A/B) with flat names: below: top/A - B, else "top - A - B". */
static char *flat_name(const char *dest, const char *top, char d, int below)
{
	const char *s = dest + strlen(top);      /* "/A/B" */
	buf_t b = {0};
	buf_adds(&b, top);
	if (below && *s) {
		buf_addc(&b, d);
		s++;
	}
	for (; *s; s++)
		if (*s == d) buf_adds(&b, " - ");
		else buf_addc(&b, *s);
	return buf_steal(&b);
}

/* Is full this account's own mail (its local path)? */
static int own_mail(const char *full)
{
	char r1[4096], r2[4096], p[1024];
	if (!ac || ac->type == STORE_IMAP || !*ac->path) return 0;
	expand_home(p, sizeof p, ac->path);
	return realpath(full, r1) && realpath(p, r2) && !strcmp(r1, r2);
}

void folder_import(void)
{
	if (!A.im) return;
	if (store_kind(A.im) == STORE_POP3) {
		ui_message("Import", "A POP3 account has no folders to import into: use an IMAP or local account",
		           draw_bg);
		return;
	}
	char path[1024] = "~/", full[1024];
	buf_t why = {0};
	local_t *src = NULL;
	for (;;) {
		ui_field f[] = {{"Import from", path, sizeof path, 0, 0}};
		int rc = ui_input_dialog("Import", f, 1,
		                         why.len ? buf_cstr(&why) : "A Maildir, an mbox file, an MH folder or a directory of them",
		                         draw_bg);
		if (rc < 0) {
			buf_free(&why);
			return;
		}
		expand_home(full, sizeof full, str_trim(path));
		why.len = 0;
		if ((src = xfer_import_open(full, &why))) break;
	}
	buf_free(&why);
	buf_t m = {0};
	int rc = xfer_import(src, &m);
	if (m.len) {
		ui_message(rc > 0 ? "Import: warning" : "Import", buf_cstr(&m), draw_bg);
		set_status("%s", m.d);
	}
	buf_free(&m);
}

/* The source of an import: opened, or NULL with the reason in msg. */
local_t *xfer_import_open(const char *full, buf_t *msg)
{
	struct stat st;
	if (stat(full, &st) != 0) {
		buf_printf(msg, "There is nothing there: %s", full);
		return NULL;
	}
	if (own_mail(full)) {
		buf_adds(msg, "That is this account's own mail");
		return NULL;
	}
	local_t *src = local_new();
	if (local_open(src, full, 0) != 0) {
		buf_printf(msg, "%s", local_error(src));
		local_free(src);
		return NULL;
	}
	return src;
}

/* Import everything in src (freed here) into a new folder of the account;
 * what happened goes in msg. 0 done, 1 done with a warning, -1 error,
 * stopped or cancelled. */
int xfer_import(local_t *src, buf_t *msg)
{
	if (store_kind(A.im) == STORE_POP3) {
		buf_adds(msg, "A POP3 account has no folders to import into: use an IMAP or local account");
		local_free(src);
		return -1;
	}
	folder_t *sf;
	int sn;
	if (local_list(src, &sf, &sn) != 0) {
		buf_adds(msg, local_error(src));
		local_free(src);
		return -1;
	}
	/* only the folders that are there (not Sent and the like before they are made) */
	int keep = 0;
	for (int i = 0; i < sn; i++) {
		if (local_folder_real(src, sf[i].name) || sf[i].noselect) {
			sf[keep++] = sf[i];
			continue;
		}
		free(sf[i].name);
		free(sf[i].display);
	}
	sn = keep;
	if (!sn) {
		buf_adds(msg, "There are no folders there");
		folders_free(sf, sn);
		local_free(src);
		return -1;
	}

	/* the new folder: Imported-DATE (-2, -3 when there is one) */
	char d = fld_delim(), pre[64], day[16], top[200];
	fld_prefix(pre, sizeof pre);
	time_t now = time(NULL);
	struct tm tm;
	localtime_r(&now, &tm);
	strftime(day, sizeof day, "%Y-%m-%d", &tm);
	for (int k = 1; k < 100; k++) {
		if (k == 1) snprintf(top, sizeof top, "%sImported-%s", pre, day);
		else snprintf(top, sizeof top, "%sImported-%s-%d", pre, day, k);
		char *raw = fld_stored(top);
		int there = 0;
		for (int i = 0; i < A.nfolders; i++)
			if (!strcmp(A.folders[i].name, raw) || (!strncmp(A.folders[i].name, raw, strlen(raw)) &&
			                                        A.folders[i].name[strlen(raw)] == d))
				there = 1;
		free(raw);
		if (!there) break;
	}

	/* each source folder's place: top/A/B, or (flat) "top - A - B" */
	char **dest = xcalloc((size_t)sn + 1, sizeof *dest);
	int renamed = 0;
	for (int i = 0; i < sn; i++) {
		int np;
		char **p = parts_of(sf[i].name, sf[i].delim, sf[i].delim == '.', &np);
		buf_t b = {0};
		buf_adds(&b, top);
		for (int j = 0; j < np; j++) {
			char bad[4] = {d, '*', '%', 0};
			renamed += clean(p[j], bad);
			buf_printf(&b, "%c%s", d, p[j]);
		}
		parts_free(p, np);
		dest[i] = buf_steal(&b);
	}

	/* can folders here hold subfolders? Try with the first one. */
	int flat = 0;
	char why[400];
	progress(1, "Creating %s ...", top);
	if (make_folder(dest[0], NULL) != 0) {
		snprintf(why, sizeof why, "%s", store_error(A.im));
		/* some servers want the folder above first */
		if (make_folder(top, NULL) != 0) {
			buf_printf(msg, "No folder can be made in this account: %s", why);
			goto out;
		}
		if (make_folder(dest[0], NULL) != 0) {
			snprintf(why, sizeof why, "%s", store_error(A.im));
			char l1[500], l2[400];
			snprintf(l1, sizeof l1, "This account cannot keep the folder structure (%s).", why);
			snprintf(l2, sizeof l2, "Import with flat names instead, like \"%s - INBOX\"?", top + strlen(pre));
			const char *lines[] = {l1, l2};
			ui_menuitem btn[] = {{"Import flat", 0, 'i'}, {"Cancel", 0, 'c'}};
			int c = ui_choice_dialog("Import: folder structure", lines, 2, btn, 2, 0, 1, draw_bg);
			char *raw = fld_stored(top);          /* empty, and cannot hold the others */
			store_delete_folder(A.im, raw);
			free(raw);
			if (c != 0) goto out;
			flat = 1;
		}
	}
	if (flat)
		for (int i = 0; i < sn; i++) {
			char *f = flat_name(dest[i], top, d, 0);
			free(dest[i]);
			dest[i] = f;
		}

	imp_t x = {.crlf = store_kind(A.im) == STORE_IMAP};
	int nf = 0, stop = 0, failed = 0, nflat = 0;
	buf_t flatnames = {0};
	char *first_raw = NULL;
	for (int i = 0; i < sn && !stop && !failed; i++) {
		char *raw = NULL;
		progress(1, "Creating %s ...", dest[i]);
		if (make_folder(dest[i], &raw) != 0 && !flat) {
			/* this one cannot be below its own parent: "A - B" in the new
			 * folder, else "Imported-... - A - B" at the top */
			for (int below = 1; below >= 0 && !raw; below--) {
				char *f = flat_name(dest[i], top, d, below);
				if (make_folder(f, &raw) == 0) {
					nflat++;
					char *sh = f + strlen(pre);
					if (flatnames.len < 300) buf_printf(&flatnames, "%s%s", flatnames.len ? ", " : "", sh);
				}
				free(f);
			}
		}
		if (!raw) {
			snprintf(x.err, sizeof x.err, "cannot create %s: %s", dest[i], store_error(A.im));
			failed = 1;
			break;
		}
		if (!first_raw) first_raw = xstrdup(raw);
		uint32_t exists = 0;
		if (!sf[i].noselect && local_select(src, sf[i].name, &exists) != 0) {
			snprintf(x.err, sizeof x.err, "cannot read %s: %s", sf[i].display, local_error(src));
			free(raw);
			failed = 1;
			break;
		}
		x.dest = raw;
		x.name = sf[i].display;
		x.done = 0;
		x.total = (int)exists;
		nf++;
		if (exists && each_message(NULL, src, exists, import_one, &x, &stop, "Import") != 0) {
			if (!*x.err) snprintf(x.err, sizeof x.err, "%s: %s", sf[i].display, local_error(src));
			failed = 1;
		}
		free(raw);
	}
	/* the folder above them, when the account did not make it (Maildir++) */
	if (!flat && first_raw) {
		char *raw = fld_stored(top);
		if (!folder_exists(raw)) make_folder(top, NULL);
		free(raw);
	}
	folders_reload(first_raw);
	free(first_raw);
	for (int i = 0; i < sn; i++) free(dest[i]);
	free(dest);
	folders_free(sf, sn);
	local_free(src);

	buf_t *m = msg;
	if (failed) buf_printf(m, "Error: %s. ", x.err);
	else if (stop) buf_adds(m, "Stopped. ");
	buf_printf(m, "%ld message%s from %d folder%s imported into %s", x.all, x.all == 1 ? "" : "s", nf,
	           nf == 1 ? "" : "s", top + strlen(pre));
	if (flat) buf_adds(m, " (flat names: this account cannot keep the folder structure)");
	if (nflat)
		buf_printf(m, ". Warning: the folder structure could not be kept for %d folder%s (a folder "
		           "here cannot hold both messages and subfolders), imported as: %s",
		           nflat, nflat == 1 ? "" : "s", flatnames.d);
	if (renamed) buf_printf(m, ". %d name%s with %c got _ instead", renamed, renamed == 1 ? "" : "s", d);
	buf_free(&flatnames);
	return failed || stop ? -1 : nflat || flat ? 1 : 0;
out:
	for (int i = 0; i < sn; i++) free(dest[i]);
	free(dest);
	folders_free(sf, sn);
	local_free(src);
	folders_reload(NULL);
	return -1;
}
