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
		"pl", "rb", "command", "app", "pkg", "iso", "img",
		/* opened in the browser as a local file, where scripts run */
		"html", "htm", "xhtml", "shtml", "mht", "mhtml", "svg", "svgz", "xsl", "xslt",
		/* Office files with macros */
		"docm", "dotm", "xlsm", "xltm", "xlam", "xll", "pptm", "potm", "ppam", "ppsm", "sldm",
		/* other Windows types used in attacks */
		"chm", "msc", "one", "onepkg", "vhd", "vhdx", "inf", "sys", "application", "gadget",
		"settingcontent-ms", "library-ms", "search-ms", "searchconnector-ms", "diagcab",
		"appx", "msix", "appxbundle", "msixbundle", "ws", "wsc", "sct", "ade", "adp", "mde",
		"accde", "cer", "crt", "der", "website", "theme", "themepack", "scf", "xbap",
		NULL};
	/* The extension as it will be on disk (after att_filename has made the
	 * name safe), not as written in the mail: "x.exe " would else pass the
	 * check and still be saved as x.exe. */
	buf_t b = {0};
	char e[32];
	att_filename(&b, name, e, sizeof e);
	buf_free(&b);
	if (!*e) return 0;
	for (int i = 0; ext[i]; i++)
		if (!strcasecmp(e + 1, ext[i])) return 1;
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

#ifndef _WIN32
/* Programs that need the terminal: mcm waits while they run. */
static int console_program(const char *cmd)
{
	static const char *const tui[] = {"w3m", "lynx", "links", "links2", "elinks", "browsh",
		"carbonyl", "less", "more", "most", "vi", "vim", "nvim", "view", "nano", "mc",
		"mcview", "fbpdf", "fbgs", "jfbview", "pdftotext", NULL};
	const char *name = cmd, *sp = cmd + strcspn(cmd, " \t");
	for (const char *c = cmd; c < sp; c++)
		if (*c == '/' || *c == '\\') name = c + 1;
	size_t nl = (size_t)(sp - name);
	for (int i = 0; tui[i]; i++)
		if (strlen(tui[i]) == nl && !strncmp(name, tui[i], nl)) return 1;
	return 0;
}

/* name (no arguments) is a program in $PATH. */
static int in_path(const char *name)
{
	const char *p = getenv("PATH");
	if (!p || !*p) p = "/usr/bin:/bin";
	while (*p) {
		size_t n = strcspn(p, ":");
		char f[1024];
		if (n && snprintf(f, sizeof f, "%.*s/%s", (int)n, p, name) < (int)sizeof f &&
		    access(f, X_OK) == 0)
			return 1;
		p += n;
		if (*p) p++;
	}
	return 0;
}
#endif

/* A graphical desktop (X or Wayland) mcm can open windows on: always on
 * Windows. */
int graphical(void)
{
#ifdef _WIN32
	return 1;
#else
	const char *x = getenv("DISPLAY"), *w = getenv("WAYLAND_DISPLAY");
	return (x && *x) || (w && *w);
#endif
}

/* The program used for kind ('p' PDF, 'h' web link) here: the setting
 * from mcm.conf for a graphical desktop or the console, otherwise on the
 * console the first one installed. "" means the desktop's program
 * (xdg-open, Windows: ShellExecute); NULL: none found. */
const char *viewer_for(int kind)
{
	int g = graphical();
	const char *set = kind == 'p' ? (g ? cfg_pdf_viewer : cfg_pdf_viewer_console)
	                              : (g ? cfg_html_viewer : cfg_html_viewer_console);
	if (*set || g) return set;
#ifdef _WIN32
	return "";
#else
	/* framebuffer viewers show the pages (Linux console); pdftotext only
	 * the text */
	static const char *const pdf[] = {"fbpdf", "jfbview", "fbgs", NULL};
	static const char *const web[] = {"w3m", "lynx", "elinks", "links", "links2", NULL};
	const char *const *l = kind == 'p' ? pdf : web;
	for (int i = 0; l[i]; i++)
		if (in_path(l[i])) return l[i];
	if (kind == 'p' && in_path("pdftotext") && in_path("less"))
		return "pdftotext -layout %s - | less";
	return NULL;
#endif
}

/* Open path (a file or a link) with cmd from mcm.conf, which may have
 * arguments: path is added at the end, or put where %s is (then cmd is a
 * shell command line: pdftotext %s - | less). A console program (or any
 * program without a graphical desktop) runs in the terminal and mcm waits;
 * others are started and left running. */
static int run_viewer(const char *cmd, const char *path)
{
#ifdef _WIN32
	char cl[2200];
	const char *ps = strstr(cmd, "%s");
	if (ps) snprintf(cl, sizeof cl, "%.*s\"%s\"%s", (int)(ps - cmd), cmd, path, ps + 2);
	else snprintf(cl, sizeof cl, "%s \"%s\"", cmd, path);
	STARTUPINFOA si = {.cb = sizeof si};
	PROCESS_INFORMATION pi;
	if (!CreateProcessA(NULL, cl, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) return -1;
	CloseHandle(pi.hProcess);
	CloseHandle(pi.hThread);
	return 0;
#else
	if (console_program(cmd) || !graphical()) {
		int rc = ui_run_program(cmd, path, "");
		clearok(curscr, TRUE);
		return rc == 0 ? 0 : -1;
	}
	char script[1100];
	viewer_script(script, sizeof script, cmd, "");
	pid_t pid = fork();
	if (pid < 0) return -1;
	if (pid == 0) {
		if (fork() == 0) {                 /* see open_with_viewer */
			setsid();
			int fd = open("/dev/null", O_RDWR);
			if (fd >= 0) {
				dup2(fd, 0);
				dup2(fd, 1);
				dup2(fd, 2);
			}
			execl("/bin/sh", "sh", "-c", script, cmd, path, "", (char *)NULL);
			_exit(127);
		}
		_exit(0);
	}
	int st;
	waitpid(pid, &st, 0);
	return 0;
#endif
}

/* A web link (http, https, ftp) in the html viewer (viewer_for), otherwise
 * (and mailto:) as open_with_viewer. */
int open_url(const char *url)
{
	if (strncasecmp(url, "mailto:", 7)) {
		const char *v = viewer_for('h');
		if (!v) {
			set_status("No console web browser: set html_viewer_console in mcm.conf");
			return 0;
		}
		if (*v) return run_viewer(v, url);
	}
	return open_with_viewer(url);
}

/* Show a file (or a URL) with the program the desktop uses for it (not
 * waited for); a PDF file with the pdf viewer (viewer_for). Without a
 * graphical desktop xdg-open runs in the terminal (it may start a console
 * program) and mcm waits. */
int open_with_viewer(const char *path)
{
	size_t pl = strlen(path);
	if (pl > 4 && !strcasecmp(path + pl - 4, ".pdf")) {
		const char *v = viewer_for('p');
		if (!v) {
			set_status("No console PDF viewer: set pdf_viewer_console in mcm.conf");
			return 0;
		}
		if (*v) return run_viewer(v, path);
	}
#ifdef _WIN32
	HINSTANCE h = ShellExecuteA(NULL, "open", path, NULL, NULL, SW_SHOWNORMAL);
	return (INT_PTR)h > 32 ? 0 : -1;
#else
	if (!graphical()) {
		int rc = ui_run_program("xdg-open", path, "");
		clearok(curscr, TRUE);
		return rc == 0 ? 0 : -1;
	}
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

/* Open (in the desktop's viewer) or save (in ~/Downloads) attachment a,
 * after asking which; item describes it. 0 = done, 1 = cancelled. */
static int att_open_or_save(att_t *a, const char *item)
{
	const char *lines[] = {item};
	static const ui_menuitem btn[] = {{"Open", 0, 'o'}, {"Save", 0, 's'}, {"Cancel", 0, 'c'}};
	int c = ui_choice_dialog("Attachment", lines, 1, btn, 3, 0, 0, draw_bg);
	if (c != 0 && c != 1) return 1;
	char dir[1024];
	buf_t path = {0};
	if (c == 0) {                                    /* Open */
		if (att_runnable(a->name))
			set_status("%s can run code: mcm does not open such files, use Save", a->name);
		else if (att_dir(dir, sizeof dir) != 0)
			set_status("%s", "Error: no folder for opened attachments");
		else if (att_write(a, dir, &path) == 0) {
			if (open_with_viewer(path.d) != 0) set_status("Error: cannot open %s", path.d);
			else set_status("Opened %s", a->name);
		}
	} else {                                         /* Save */
		if (downloads_dir(dir, sizeof dir) != 0) set_status("%s", "Error: no home folder (HOME)");
		else if (att_write(a, dir, &path) == 0) set_status("Saved %s", path.d);
	}
	buf_free(&path);
	return 0;
}

/* "name  (type, size)" */
static char *att_item(const att_t *a)
{
	char hs[32];
	mime_human_size(hs, sizeof hs, a->size);
	buf_t b = {0};
	buf_printf(&b, "%s  (%s, %s)", a->name, a->type, hs);
	return buf_steal(&b);
}

/* For the links window (K): the open message's attachments, kept until
 * att_release. */
static att_t *kept;
static int    nkept;

int att_collect(char ***items)
{
	*items = NULL;
	att_release();
	int n = att_list(&kept);
	if (n <= 0) {
		att_free(kept, 0);
		kept = NULL;
		return 0;
	}
	nkept = n;
	*items = xcalloc((size_t)n, sizeof **items);
	for (int i = 0; i < n; i++) (*items)[i] = att_item(&kept[i]);
	return n;
}

void att_choose(int i, const char *item)
{
	if (i >= 0 && i < nkept) att_open_or_save(&kept[i], item);
}

void att_release(void)
{
	att_free(kept, nkept);
	kept = NULL;
	nkept = 0;
}

void att_open_shown(int k, const char *name, const char *item)
{
	char **items;
	int n = att_collect(&items);
	/* the k-th one, if the name agrees; else the first of that name */
	int i = k < n && !strcmp(kept[k].name, name) ? k : -1;
	for (int j = 0; i < 0 && j < n; j++)
		if (!strcmp(kept[j].name, name)) i = j;
	if (i < 0 && k < n) i = k;
	if (i >= 0) att_choose(i, item);
	else if (!*A.status) set_status("%s", "Error: the attachment was not found");
	for (int j = 0; j < n; j++) free(items[j]);
	free(items);
	att_release();
}
