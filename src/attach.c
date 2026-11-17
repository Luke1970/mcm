/* attach.c - attachments of the open message (T): open with the
 * desktop's program, or save
 */
#include "mcm.h"

/* One attachment of the open message. */
typedef struct {
	char         *name, *type;
	size_t        size;           /* decoded (estimated for base64 on IMAP) */
	const mpart_t *part;          /* IMAP: fetched when chosen; NULL: data is here */
	char         *data;
	size_t        len;
} att_t;

static int att_dir(char *dir, size_t n);
static int att_runnable(const char *name);
static void att_filename(buf_t *b, const char *name, char *ext, size_t extn);
static void att_free(att_t *a, int n);
static int att_list(att_t **out);
static int att_write(att_t *a, const char *dir, buf_t *path);
static int open_with_viewer(const char *path);

/* ------------------------------------------------------------------ */
/* Attachments (T)                                                      */

/* Where opened attachments are put for the viewer: a private folder,
 * emptied when mcm starts and ends. */
static int att_dir(char *dir, size_t n)
{
#ifdef _WIN32
	char tmp[MAX_PATH + 1];
	DWORD tn = GetTempPathA(MAX_PATH, tmp);
	if (!tn || tn > MAX_PATH) return -1;
	snprintf(dir, n, "%smcm-attachments", tmp);
	mkdir(dir, 0700);
#else
	const char *rt = getenv("XDG_RUNTIME_DIR"), *home = getenv("HOME");
	if (rt && *rt) snprintf(dir, n, "%s/mcm-attachments", rt);
	else if (home && *home) {
		snprintf(dir, n, "%s/.cache", home);
		mkdir(dir, 0700);
		snprintf(dir, n, "%s/.cache/mcm-attachments", home);
	} else return -1;
	mkdir(dir, 0700);
#endif
	struct stat st;
	return stat(dir, &st) == 0 && S_ISDIR(st.st_mode) ? 0 : -1;
}

void att_cleanup(void)
{
	char dir[1024];
	if (att_dir(dir, sizeof dir) != 0) return;
	DIR *d = opendir(dir);
	if (!d) return;
	struct dirent *de;
	while ((de = readdir(d))) {
		if (de->d_name[0] == '.') continue;
		char p[1400];
		snprintf(p, sizeof p, "%s/%s", dir, de->d_name);
		remove(p);
	}
	closedir(d);
}

/* Program files are never opened from mail (they could run): only Save. */
static int att_runnable(const char *name)
{
	static const char *const ext[] = {
		"exe", "com", "bat", "cmd", "scr", "pif", "msi", "msp", "cpl", "dll", "js", "jse", "vbs",
		"vbe", "wsf", "wsh", "ps1", "psm1", "hta", "lnk", "url", "reg", "jar", "appref-ms",
		"sh", "bash", "csh", "zsh", "run", "bin", "desktop", "appimage", "deb", "rpm", "py",
		"pl", "rb", "command", "app", "pkg", "iso", "img", NULL};
	const char *dot = strrchr(name, '.');
	if (!dot) return 0;
	for (int i = 0; ext[i]; i++)
		if (!strcasecmp(dot + 1, ext[i])) return 1;
	return 0;
}

/* The file name of an attachment, made safe (no folders, no leading dot),
 * split into base (appended to b) and extension (in ext). */
static void att_filename(buf_t *b, const char *name, char *ext, size_t extn)
{
	const char *n = name;
	for (const char *c = name; *c; c++)
		if (*c == '/' || *c == '\\') n = c + 1;           /* never a folder */
	const char *dot = strrchr(n, '.');
	*ext = '\0';
	buf_t base = {0};
	if (dot && dot > n && strlen(dot) < 16) {
		buf_t e = {0};
		buf_addc(&e, '.');
		name_part(&e, dot + 1, 14);
		snprintf(ext, extn, "%s", buf_cstr(&e));
		buf_free(&e);
		buf_add(&base, n, (size_t)(dot - n));
	} else {
		buf_adds(&base, n);
	}
	buf_cstr(&base);
	if (base.d[0] == '.') buf_addc(b, '_');
	name_part(b, base.d, 150);
	buf_free(&base);
}

static void att_free(att_t *a, int n)
{
	for (int i = 0; i < n; i++) {
		free(a[i].name);
		free(a[i].type);
		free(a[i].data);
	}
	free(a);
}

/* The open message's attachments. -1 on error (status set). */
static int att_list(att_t **out)
{
	*out = NULL;
	mpart_t **parts;
	int n = mv_attachments(&A.mv, &parts);
	if (A.mv.root) {                                   /* IMAP: from the structure */
		att_t *a = xcalloc((size_t)n + 1, sizeof *a);
		for (int i = 0; i < n; i++) {
			const mpart_t *p = parts[i];
			a[i].name = xstrdup(p->name ? p->name : "(unnamed)");
			a[i].type = xmalloc(strlen(p->type) + strlen(p->subtype) + 2);
			sprintf(a[i].type, "%s/%s", p->type, p->subtype);
			a[i].size = !strcmp(p->cte, "base64") ? (size_t)p->size * 3 / 4 : p->size;
			a[i].part = p;
		}
		free(parts);
		*out = a;
		return n;
	}
	free(parts);
	/* local mail and POP3: the raw message, parsed here */
	buf_t raw = {0};
	imap_partial_t req = {.section = "", .off = 0, .len = (size_t)-1 / 2, .out = &raw};
	if (store_fetch_partial(A.im, A.mv.uid, &req, 1) != 0) {
		set_status("Error: %s", store_error(A.im));
		buf_free(&raw);
		return -1;
	}
	mime_att *m;
	n = mime_attachments(raw.d ? raw.d : "", raw.len, &m);
	buf_free(&raw);
	att_t *a = xcalloc((size_t)n + 1, sizeof *a);
	for (int i = 0; i < n; i++) {
		a[i].name = m[i].name;
		a[i].type = m[i].type;
		a[i].data = m[i].data;
		a[i].len = a[i].size = m[i].len;
	}
	free(m);                                          /* the strings moved to a */
	*out = a;
	return n;
}

/* Write attachment a (fetched and decoded first, on IMAP) to a new file
 * in dir; path gets its name. 0 = done. */
static int att_write(att_t *a, const char *dir, buf_t *path)
{
	if (a->part && !a->data) {
		now_status("Loading %s ...", a->name);
		buf_t enc = {0};
		if (mv_fetch_part(&A.mv, A.im, a->part, &enc) != 0) {
			set_status("Error: %s", store_error(A.im));
			buf_free(&enc);
			return -1;
		}
		a->data = mime_decode_cte(enc.d ? enc.d : "", enc.len, a->part->cte, &a->len);
		buf_free(&enc);
	}
	char ext[32];
	buf_printf(path, "%s/", dir);
	att_filename(path, a->name, ext, sizeof ext);
	FILE *f = new_file(path, ext);
	if (!f) {
		set_status("Error: cannot write %s: %s", path->d, strerror(errno));
		return -1;
	}
	int bad = fwrite(a->data, 1, a->len, f) != a->len;
	if (fclose(f) != 0) bad = 1;
	if (bad) {
		set_status("Error: writing %s failed", path->d);
		remove(path->d);
		return -1;
	}
	return 0;
}

/* Show a file with the program the desktop uses for it (not waited for). */
static int open_with_viewer(const char *path)
{
#ifdef _WIN32
	HINSTANCE h = ShellExecuteA(NULL, "open", path, NULL, NULL, SW_SHOWNORMAL);
	return (INT_PTR)h > 32 ? 0 : -1;
#else
	pid_t pid = fork();
	if (pid < 0) return -1;
	if (pid == 0) {
		/* a grandchild runs the viewer, so nothing waits for it and its
		 * output does not reach the screen */
		if (fork() == 0) {
			setsid();
			int fd = open("/dev/null", O_RDWR);
			if (fd >= 0) {
				dup2(fd, 0);
				dup2(fd, 1);
				dup2(fd, 2);
			}
			execlp("xdg-open", "xdg-open", path, (char *)NULL);
			_exit(127);
		}
		_exit(0);
	}
	int st;
	waitpid(pid, &st, 0);
	return 0;
#endif
}

/* T: list the open message's attachments; the chosen one is opened (in the
 * desktop's viewer) or saved in ~/Downloads. */
void attachments(void)
{
	if (A.pending_load) load_selected();
	if (!A.mv.open) {
		set_status("%s", "No message is open");
		return;
	}
	att_t *a;
	int n = att_list(&a);
	if (n < 0) return;
	if (n == 0) {
		set_status("%s", "This message has no attachments");
		att_free(a, 0);
		return;
	}
	char **items = xcalloc((size_t)n, sizeof *items);
	for (int i = 0; i < n; i++) {
		char hs[32];
		mime_human_size(hs, sizeof hs, a[i].size);
		buf_t b = {0};
		buf_printf(&b, "%s  (%s, %s)", a[i].name, a[i].type, hs);
		items[i] = buf_steal(&b);
	}
	int sel = 0;
	for (;;) {
		sel = ui_list_dialog("Attachments", (const char *const *)items, n, sel,
		                     "Enter: open or save   Esc: close", draw_bg);
		if (sel < 0) break;
		const char *lines[] = {items[sel]};
		static const ui_menuitem btn[] = {{"Open", 0, 'o'}, {"Save", 0, 's'}, {"Cancel", 0, 'c'}};
		int c = ui_choice_dialog("Attachment", lines, 1, btn, 3, 0, 0, draw_bg);
		if (c != 0 && c != 1) continue;
		char dir[1024];
		buf_t path = {0};
		if (c == 0) {                                    /* Open */
			if (att_runnable(a[sel].name)) {
				set_status("%s is a program file: mcm does not open those, use Save", a[sel].name);
				break;
			}
			if (att_dir(dir, sizeof dir) != 0) {
				set_status("%s", "Error: no folder for opened attachments");
				break;
			}
			if (att_write(&a[sel], dir, &path) == 0) {
				if (open_with_viewer(path.d) != 0) set_status("Error: cannot open %s", path.d);
				else set_status("Opened %s", a[sel].name);
			}
		} else {                                         /* Save */
			if (downloads_dir(dir, sizeof dir) != 0) set_status("%s", "Error: no home folder (HOME)");
			else if (att_write(&a[sel], dir, &path) == 0) set_status("Saved %s", path.d);
		}
		buf_free(&path);
		break;
	}
	for (int i = 0; i < n; i++) free(items[i]);
	free(items);
	att_free(a, n);
}
