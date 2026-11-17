/* mcm - console email client in the spirit of PC Tools and Midnight Commander
 *
 * main.c: startup, the command line, login, switching accounts and the main
 * loop (keys, mouse, background work). The other parts: see mcm.h.
 */
#include "mcm.h"
#include "helptext.h"   /* the help texts, from the files in help/ */

/* the shared state (mcm.h) */
account_t  accts[MAX_ACCOUNTS];
int        naccts;
account_t *ac;
char       last_account[64];
char      *master_check;
char      *master;
int wrap_width = 72;
int col_pct[3] = {25, 45, 15};
int dragging;
int drag_border;
sbar_t sbs[NSBS];
int sb_drag;
app_t A;
const ui_menuitem folder_menu[] = {
	{"Quit", 0, 'q'}, {"Mail", 0, 'm'}, {"Reply", 0, 'r'}, {"Forward", 0, 'f'},
	{"Account", 0, 'a'}, {"Config", 0, 'c'},
	{"F1=Help", UI_HOT_KEYNAME, KEY_F(1)},
};
const int nfolder_menu = (int)(sizeof folder_menu / sizeof *folder_menu);

static void pop3_default_path(char *out, size_t n);
static void local_default_path(char *out, size_t n);
static int choose_type(void);
static int check_name(const char *name, const char **err);
static int open_local_account(void);
static int do_login(void);
static int pct_decode(char *out, size_t size, const char *s, size_t n);
static int parse_url(const char *url);
static void print_lines(FILE *f, const char *const *lines);
static void quit_now(void);
static void usage(void);
static void help(void);
static account_t *choose_account(const char *opt_a, int opt_n, const account_t *cli);
static void open_start_folder(void);
static void leave_account(void);

void set_status(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(A.status, sizeof A.status, fmt, ap);
	va_end(ap);
}

/* ------------------------------------------------------------------ */
/* Startup                                                             */

/* "~/x" -> "$HOME/x" */
void expand_home(char *out, size_t n, const char *in)
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
	if (xdg && *xdg) snprintf(out, n, "%.300s/mcm/%.60s", xdg, nm);
	else snprintf(out, n, "%.300s/.local/share/mcm/%.60s", home ? home : ".", nm);
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
				snprintf(state, sizeof state, "%s/.mcm-pop3-state", full);
				struct stat sst;
				if (stat(state, &sst) != 0) {          /* from emsg (before 0.95) */
					char old[1100];
					snprintf(old, sizeof old, "%s/.emsg-pop3-state", full);
					rename(old, state);
				}
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
		fprintf(stderr, "mcm: only imaps:// and pop3s:// (over TLS) are supported\n");
		return -1;
	} else if (strstr(p, "://")) {
		fprintf(stderr, "mcm: not an imaps:// or pop3s:// URL: %s\n", url);
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
			fprintf(stderr, "mcm: leave the password out of the URL; "
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
	fprintf(stderr, "mcm: bad URL: %s\n"
	                "      expected imaps://[user@]host[:port][/folder], pop3s://[user@]host[:port]\n"
	                "      or a path to local mail\n", url);
	return -1;
toolong:
	fprintf(stderr, "mcm: URL part too long: %s\n", url);
	return -1;
}

/* Print text lines, with @VERSION@ replaced by the version. */
static void print_lines(FILE *f, const char *const *lines)
{
	for (int i = 0; lines[i]; i++) {
		const char *s = lines[i], *v;
		while ((v = strstr(s, "@VERSION@"))) {
			fprintf(f, "%.*s%s", (int)(v - s), s, MCM_VERSION);
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
#ifndef _WIN32
	att_cleanup();                  /* (Windows: a viewer may still hold them; next start) */
#endif
	if (*saved) fprintf(stderr, "mcm: the message you were writing is saved in %s\n", saved);
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

/* After a login: read the folder list and open INBOX, or the folder the
 * command line named. */
static void open_start_folder(void)
{
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
		A.focus = A.left = A.side = COL_FOLDERS;
	}
	if (start_missing) set_status("Folder '%s' not found", A.start_folder);
}

/* Close everything of the current account (folders, messages, the
 * connection, its SMTP password), but keep the master password. */
static void leave_account(void)
{
	close_message();
	cache_clear();
	filter_off();
	A.filter[0] = '\0';
	folders_free(A.folders, A.nfolders);
	A.folders = NULL;
	A.nfolders = A.fsel = A.ftop = 0;
	A.cur_folder = -1;
	A.nmsgs = A.msel = A.mtop = 0;
	A.unseen = -1;
	A.pending_load = 0;
	A.vtop = 0;
	store_logout(A.im);
	store_free(A.im);
	A.im = NULL;
	vault_free(A.smtp_pass);
	A.smtp_pass = NULL;
}

/* Go to another account (name NULL = set up a new one) without restarting:
 * the master password stays in memory, so it is asked only once per run.
 * If the login is cancelled or fails, the previous account is opened again. */
void switch_account(const char *name)
{
	account_t *old = ac;
	config_save();
	leave_account();
	A.type_given = 0;
	A.start_folder[0] = '\0';
	account_t *a = name ? account_find(name) : account_new(NULL);
	if (!a) {
		set_status(name ? "No account %s" : "Too many accounts", name ? name : "");
		a = old;
	}
	if (!*a->port) snprintf(a->port, sizeof a->port, a->type == STORE_POP3 ? "995" : "993");
	ac = a;
	if (do_login() < 0) {
		store_free(A.im);
		A.im = NULL;
		if (!name && ac == &accts[naccts - 1]) naccts--;   /* a new account that was not made */
		ac = old;
		if (ac == a || do_login() < 0) {
			ui_end();
			store_free(A.im);
			vault_free(master);
			exit(0);
		}
	}
	smtp_derive();
	config_save();
	open_start_folder();
	focus_to(A.cur_folder >= 0 ? COL_LIST : COL_FOLDERS);
	if (!*A.status) set_status("Account %s", ac->name);
}

int main(int argc, char **argv)
{
	setlocale(LC_ALL, "");
	migrate_from_emsg();
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
		if (!strcmp(argv[i], "--version")) { puts("Mail Commander (mcm) " MCM_VERSION); return 0; }
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
		case 'V': puts("Mail Commander (mcm) " MCM_VERSION); return 0;
		case 'h': help(); return 0;
		default: usage(); return 2;
		}
	}
	if (optind < argc - 1) { usage(); return 2; }
	if (optind == argc - 1 && parse_url(argv[optind]) < 0) return 2;
	ac = NULL;

	/* colors: the text file ~/.config/mcm/NAME.theme, read at every start */
	theme_t theme;
	char theme_err[1200];
	load_theme(&theme, theme_err, sizeof theme_err);

	A.cur_folder = -1;
	A.lines_width = -1;
	A.unseen = -1;
	A.mv.more_off = MV_NO_MORE;
	A.focus = A.left = COL_LIST;
	A.side = COL_FOLDERS;

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

	att_cleanup();                  /* attachments opened last time */
	open_start_folder();
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
				scroll_thumb_to((sbs[sb_drag].horiz ? ev.x : ev.y) - sbs[sb_drag].grab);
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
						} else if (cl.col[k] == COL_ACCTS) {
							A.atop += d;              /* the selection stays */
							A.afree = 1;
						} else {
							A.ftop += d;
							A.ffree = 1;
						}
						break;
					}
				continue;
			}
			if (!(ev.bstate & BUTTON1_PRESSED)) continue;
			/* a scroll bar in a list's right border goes before dragging that
			 * border (the window right of it still has its own edge) */
			if (scrollbar_press(ev.x, ev.y)) continue;
			if (A.hwx_x && ev.y == A.hwx_y && ev.x >= A.hwx_x && ev.x <= A.hwx_x + 10 &&
			    (ev.x - A.hwx_x) % 2 == 0) {
				/* H W X S P T on the message's border: like the keys */
				handle_key(tolower((unsigned char)HWXSP[(ev.x - A.hwx_x) / 2]));
				continue;
			}
			if (ev.y > 0 && ev.y < LINES - 1 && (drag_border = border_at(ev.x)) > 0) {
				dragging = DRAG_BORDER;               /* pressed on a column border */
				ui_mouse_drag(1);
				continue;
			}
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
			ch = ui_menu_hit(folder_menu, nfolder_menu, ev.x);
			if (!ch) continue;
		}
		if (!handle_key(ch)) break;
	}

	ui_end();
#ifndef _WIN32
	att_cleanup();
#endif
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
