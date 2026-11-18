/* account.c - accounts and their stored passwords, the Config menu and
 * the SMTP settings
 */
#include "mcm.h"

static void mail_domain(char *out, size_t n);
static void aad_imap(const account_t *a, char *o, size_t n);
static void forget_passwords(void);
static void account_settings(void);
static void delete_all_passwords(void);
static void wrap_settings(void);
static void theme_settings(void);

/* Enter (or a double click) on row i of the accounts window: the current
 * account shows its folders; another one, or New account, is switched to. */
void account_open(int i)
{
	if (i == (int)(ac - accts)) focus_to(COL_FOLDERS);
	else if (i == naccts) switch_account(NULL);
	else if (i >= 0 && i < naccts) switch_account(accts[i].name);
}

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
void login_address(char *out, size_t n)
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
void smtp_derive(void)
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

/* Keeps the program's old name (emsg): master_check values written by
 * earlier versions must still decrypt. Do not change. */
#define MASTER_AAD "emsg master password check"

/* What a stored password belongs to; it is authenticated with it, so it
 * only decrypts for the same user on the same server (vault.h). */
static void aad_imap(const account_t *a, char *o, size_t n)
{
	snprintf(o, n, "%s %.250s@%.300s", a->type == STORE_POP3 ? "pop3" : "imap", a->user, a->host);
}

void aad_smtp(const account_t *a, char *o, size_t n)
{
	snprintf(o, n, "smtp %.250s@%.300s", a->smtp_user, a->smtp_host);
}

/* Make sure the master password is known: asked once per session, checked
 * against master_check; the first time it is set (typed twice). 0 = known. */
int get_master(const char *why, ui_bg_fn bg)
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
int store_password(const char *pass, ui_bg_fn bg)
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
void offer_store(const char *pass)
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
int unlock_password(char *pass, size_t n, const char **err)
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
int pick_account(const char *title, int def, int mark, ui_bg_fn bg)
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

/* Colors of theme A.theme: ~/.config/mcm/NAME.theme (copied there from the
 * installed theme the first time). msg: an error, a note about a new file,
 * or "". */
void load_theme(theme_t *t, char *msg, size_t n)
{
	theme_defaults(t);
	*msg = '\0';
	int created = 0;
	char *tp = theme_user_file(A.theme, &created);
	if (!tp) tp = theme_find(A.theme);
	if (!tp)
		snprintf(msg, n, "theme '%s' not found, using built-in colors", A.theme);
	else if (theme_load(t, tp, msg, n) == 0 && created)
		snprintf(msg, n, "Colors are read from %s", tp);
	free(tp);
}

/* Config: choose a theme from the ones found; used at once and saved. */
static void theme_settings(void)
{
	char **names;
	int n = theme_names(&names), cur = 0;
	if (!n) {
		ui_message("Theme", "No themes found.", draw_bg);
		return;
	}
	for (int i = 0; i < n; i++)
		if (!strcasecmp(names[i], A.theme)) cur = i;
	int i = ui_list_dialog("Theme", (const char *const *)names, n, cur, "Enter choose   Esc cancel", draw_bg);
	if (i >= 0) {
		snprintf(A.theme, sizeof A.theme, "%s", names[i]);
		theme_t t;
		char msg[1200];
		load_theme(&t, msg, sizeof msg);
		theme_apply(&t);
		bkgdset(' ' | COLOR_PAIR(CP_NORMAL));
		clear();
		config_save();
		if (*msg) set_status("%s", msg);
		else set_status("Theme %s", A.theme);
	}
	for (int k = 0; k < n; k++) free(names[k]);
	free(names);
}

/* The Config menu. */
void config_menu(void)
{
	char acc[120];
	snprintf(acc, sizeof acc, "Account %.60s: name, full name, password ...", ac->name);
	char ww[80];
	snprintf(ww, sizeof ww, "Wrap width for new mail: %d ...", wrap_width);
	char th[160];
	snprintf(th, sizeof th, "Theme (colors): %.60s ...", A.theme);
	const char *items[] = {
		"Outgoing mail (SMTP) ...",
		acc,
		"Switch or add account ...",
		ww,
		th,
		"Delete all stored passwords ...",
	};
	switch (ui_list_dialog("Config", items, 6, 0, "Enter choose   Esc cancel", draw_bg)) {
	case 0: smtp_settings(NULL, draw_bg); break;
	case 1: account_settings(); break;
	case 2: account_menu(); break;
	case 3: wrap_settings(); break;
	case 4: theme_settings(); break;
	case 5: delete_all_passwords(); break;
	}
}

void account_menu(void)
{
	int cur = (int)(ac - accts);
	int i = pick_account("Accounts", cur, 1, draw_bg);
	if (i < 0) return;
	if (i == cur) account_settings();
	else if (i == naccts) switch_account(NULL);
	else switch_account(accts[i].name);
}

/* Account > Edit: the current account's name, full name, password. */
void account_edit(void) { account_settings(); }

/* Account > Remove: forget an account (not the one in use): its settings
 * and stored passwords. Its mail stays where it is. */
void account_remove(void)
{
	int cur = (int)(ac - accts);
	if (naccts < 2) {
		set_status("%s is the only account", ac->name);
		return;
	}
	const char **items = xmalloc(sizeof *items * (size_t)naccts);
	char (*txt)[160] = xmalloc(sizeof *txt * (size_t)naccts);
	int *idx = xmalloc(sizeof *idx * (size_t)naccts), n = 0;
	for (int i = 0; i < naccts; i++) {
		if (i == cur) continue;
		const account_t *a = &accts[i];
		if (a->type == STORE_LOCAL)
			snprintf(txt[n], sizeof txt[0], "%.40s  %.100s", a->name, a->path);
		else
			snprintf(txt[n], sizeof txt[0], "%.40s  %.50s@%.50s", a->name, a->user, a->host);
		items[n] = txt[n];
		idx[n++] = i;
	}
	int r = ui_list_dialog("Remove account", items, n, 0, "Enter remove   Esc cancel   (not the open one)",
	                       draw_bg);
	int i = r >= 0 ? idx[r] : -1;
	free(items);
	free(txt);
	free(idx);
	if (i < 0) return;
	char name[64], l0[120];
	snprintf(name, sizeof name, "%s", accts[i].name);
	snprintf(l0, sizeof l0, "Remove account %.60s and its stored passwords?", name);
	const char *lines[] = {l0, "Its mail stays on the server or on disk."};
	static const ui_menuitem b[] = {{"Cancel", 0, 'c'}, {"Remove", 0, 'r'}};
	if (ui_choice_dialog("Remove account", lines, 2, b, 2, 0, 1, draw_bg) != 1) return;
	set_secret(&accts[i].pass_enc, NULL);
	set_secret(&accts[i].smtp_pass_enc, NULL);
	memmove(&accts[i], &accts[i + 1], sizeof *accts * (size_t)(naccts - i - 1));
	naccts--;
	memset(&accts[naccts], 0, sizeof *accts);
	if (ac > &accts[i]) ac--;
	A.asel = (int)(ac - accts);
	config_save();
	set_status("Account %s removed", name);
}

/* Ask for the SMTP settings. Returns 0 when filled in. */
int smtp_settings(const char *err, ui_bg_fn bg)
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
