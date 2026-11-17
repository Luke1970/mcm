/* config.c - mcm.conf: reading and writing it, and the list of accounts
 */
#include "mcm.h"

static void base_dir(char *p, size_t n, const char *xdg_var, const char *home_sub, const char *name);
static int is_dir(const char *p);

/* ------------------------------------------------------------------ */
/* Config: ~/.config/mcm/mcm.conf                                    */
/*                                                                     */
/*     theme = default                                                 */
/*     last_account = work                                             */
/*     [account work]                                                  */
/*     full_name = ...   server = ...   user = ...   ...               */
/*                                                                     */
/* A file without [account] sections (emsg 0.3) is read as one account. */

/* Before 0.95 the program was called emsg: its folders (~/.config/emsg,
 * ~/.local/share/emsg, or %APPDATA%\emsg and %LOCALAPPDATA%\emsg) are
 * renamed once, and emsg.conf becomes mcm.conf. Account paths that pointed
 * into the old data folder are rewritten by config_load. */
static char old_data[1024], new_data[1024];
static int path_moved;            /* an account path was rewritten: save */

static void base_dir(char *p, size_t n, const char *xdg_var, const char *home_sub, const char *name)
{
	const char *xdg = getenv(xdg_var), *home = getenv("HOME");
	if (xdg && *xdg) snprintf(p, n, "%s/%s", xdg, name);
	else snprintf(p, n, "%s/%s/%s", home ? home : ".", home_sub, name);
}

static int is_dir(const char *p)
{
	struct stat st;
	return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

void migrate_from_emsg(void)
{
	char o[1024], nw[1024];
	base_dir(o, sizeof o, "XDG_CONFIG_HOME", ".config", "emsg");
	base_dir(nw, sizeof nw, "XDG_CONFIG_HOME", ".config", "mcm");
	if (!is_dir(nw) && is_dir(o) && rename(o, nw) == 0) {
		char a[1100], b[1100];
		snprintf(a, sizeof a, "%s/emsg.conf", nw);
		snprintf(b, sizeof b, "%s/mcm.conf", nw);
		rename(a, b);
	}
	base_dir(o, sizeof o, "XDG_DATA_HOME", ".local/share", "emsg");
	base_dir(nw, sizeof nw, "XDG_DATA_HOME", ".local/share", "mcm");
	if (!is_dir(nw) && is_dir(o) && rename(o, nw) == 0) {
		snprintf(old_data, sizeof old_data, "%s", o);
		snprintf(new_data, sizeof new_data, "%s", nw);
	}
}

void config_path(char *p, size_t n, const char *file)
{
	const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
	if (xdg && *xdg) snprintf(p, n, "%s/mcm%s%s", xdg, file ? "/" : "", file ? file : "");
	else snprintf(p, n, "%s/.config/mcm%s%s", home ? home : ".", file ? "/" : "", file ? file : "");
}

void set_secret(char **slot, char *v)
{
	free(*slot);
	*slot = v;
}

account_t *account_new(const char *name)
{
	if (naccts >= MAX_ACCOUNTS) return NULL;
	account_t *a = &accts[naccts++];
	memset(a, 0, sizeof *a);
	snprintf(a->name, sizeof a->name, "%s", name ? name : "");
	snprintf(a->port, sizeof a->port, "993");
	return a;
}

account_t *account_find(const char *name)
{
	for (int i = 0; i < naccts; i++)
		if (!strcmp(accts[i].name, name)) return &accts[i];
	return NULL;
}

/* Give an unnamed account a unique name: user@host (or host), then -2, -3 ... */
void account_name(account_t *a)
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

void config_load(void)
{
	char p[1024], line[4096];
	config_path(p, sizeof p, "mcm.conf");
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
		if (!strcasecmp(k, "message_wrap")) { A.nowrap = !atoi(v); continue; }
		if (!strcasecmp(k, "wrap_width")) {
			int w = atoi(v);
			if (w >= 20 && w <= 998) wrap_width = w;
			continue;
		}
		if (!strcasecmp(k, "folders_width") || !strcasecmp(k, "list_width") ||
		    !strcasecmp(k, "accounts_width")) {
			int w = atoi(v);
			if (w >= 5 && w <= 95) col_pct[tolower((unsigned char)*k) == 'l' ? 1 : tolower((unsigned char)*k) == 'a' ? 2 : 0] = w;
			continue;
		}
		if (!strcasecmp(k, "master_check")) { set_secret(&master_check, xstrdup(v)); continue; }
		if (!a && !(a = account_new(NULL))) break;   /* old file: settings without a section */
		if (!strcasecmp(k, "type"))
			a->type = !strcasecmp(v, "pop3") ? STORE_POP3 : !strcasecmp(v, "local") ? STORE_LOCAL : STORE_IMAP;
		else if (!strcasecmp(k, "path")) {
			size_t ol = strlen(old_data);
			if (ol && !strncmp(v, old_data, ol) && (v[ol] == '/' || !v[ol])) {
				buf_t np = {0};                          /* moved (emsg) */
				buf_printf(&np, "%s%s", new_data, v + ol);
				if (np.len < sizeof a->path) {
					memcpy(a->path, np.d, np.len + 1);
					path_moved = 1;
				} else {
					snprintf(a->path, sizeof a->path, "%s", v);
				}
				buf_free(&np);
			} else {
				snprintf(a->path, sizeof a->path, "%s", v);
			}
		}
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
	if (path_moved) config_save();
}

void config_mkdir(void)
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
void config_save(void)
{
	char p[1100], tmp[1110];
	config_mkdir();
	config_path(p, sizeof p, "mcm.conf");
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
	fprintf(f, "# mcm settings, one [account NAME] section per mail account.\n"
	           "# Passwords are only stored when you choose so, encrypted with your\n"
	           "# master password (scrypt + AES-256-GCM, see the README).\n\n");
	fprintf(f, "theme = %s\n", A.theme);
	fprintf(f, "wrap_width = %d       # new mail is wrapped at this column (format=flowed)\n",
	        wrap_width);
	fprintf(f, "message_wrap = %d     # 1: long lines in the message are wrapped (W)\n", !A.nowrap);
	fprintf(f, "folders_width = %d    # %% of the screen for Folders (next to the list)\n", col_pct[0]);
	fprintf(f, "list_width = %d       # %% for the list (3 columns: of what the folders leave)\n",
	        col_pct[1]);
	fprintf(f, "accounts_width = %d   # %% of the screen for Accounts (4 columns, over 170)\n",
	        col_pct[2]);
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
