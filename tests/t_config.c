/* t_config.c - config.c: reading mcm.conf, writing it, reading it back
 *
 * Works in a temporary directory ($XDG_CONFIG_HOME), never in your own
 * ~/.config/mcm.
 */
#include "check.h"
#include "../src/mcm.h"
#include <unistd.h>

static char dir[256];

static void reset(void)
{
	for (int i = 0; i < naccts; i++) {
		free(accts[i].pass_enc);
		free(accts[i].smtp_pass_enc);
	}
	memset(accts, 0, sizeof accts);
	naccts = 0;
	ac = NULL;
	*last_account = 0;
	free(master_check);
	master_check = NULL;
	wrap_width = 72;
	check_mail = 60;
	memset(col_pct, 0, sizeof col_pct);
	memset(&A, 0, sizeof A);
	*cfg_editor = *cfg_pdf_viewer = *cfg_html_viewer = 0;
}

static void write_conf(const char *text)
{
	char p[512];
	config_path(p, sizeof p, "mcm.conf");
	FILE *f = fopen(p, "w");
	CHECK(f != NULL);
	if (!f) return;
	fputs(text, f);
	fclose(f);
}

static void t_load(void)
{
	reset();
	write_conf("# comment\n"
	           "theme = dark   \n"
	           "wrap_width = 80\n"
	           "check_mail = 30\n"
	           "message_wrap = 0\n"
	           "list_width = 50\n"
	           "editor = vim -n\n"
	           "last_account = work\n"
	           "master_check = enc1$abc\n"
	           "\n[account work]\n"
	           "type = imap\nserver = imap.example.org\nport = 993\nuser = me@example.org\n"
	           "full_name = Me Myself\nsmtp_server = smtp.example.org\nsmtp_port = 587\n"
	           "store_password = yes\npassword = enc1$secret#notacomment\npgp = sign\n"
	           "\n[ACCOUNT home]\n"
	           "type = local\npath = /tmp/Maildir\naddress = me@home.example\n"
	           "\n[other section]\nignored = 1\n"
	           "\n[account pop]\n"
	           "type = POP3\nserver = pop.example.org\nport = 995\nstore_password = no\n");
	config_load();
	CHECK_STR(A.theme, "dark");
	CHECK_INT(wrap_width, 80);
	CHECK_INT(check_mail, 30);
	CHECK_INT(A.nowrap, 1);
	CHECK_INT(col_pct[1], 50);
	CHECK_STR(cfg_editor, "vim -n");
	CHECK_STR(last_account, "work");
	CHECK_STR(master_check, "enc1$abc");
	CHECK_INT(naccts, 3);
	account_t *w = account_find("work"), *h = account_find("home"), *p = account_find("pop");
	CHECK(w && h && p);
	if (!w || !h || !p) return;
	CHECK_INT(w->type, STORE_IMAP);
	CHECK_STR(w->host, "imap.example.org");
	CHECK_STR(w->user, "me@example.org");
	CHECK_STR(w->fullname, "Me Myself");
	CHECK_STR(w->smtp_port, "587");
	CHECK_INT(w->store, STORE_YES);
	CHECK_STR(w->pass_enc, "enc1$secret#notacomment");
	CHECK_STR(w->pgp, "sign");
	CHECK_INT(h->type, STORE_LOCAL);
	CHECK_STR(h->path, "/tmp/Maildir");
	CHECK_STR(h->user, "me@home.example");
	CHECK_INT(p->type, STORE_POP3);
	CHECK_INT(p->store, STORE_NO);
	CHECK_STR(p->port, "995");
}

static void t_limits(void)
{
	reset();
	write_conf("check_mail = 5\nwrap_width = 5\nlist_width = 99\n");
	config_load();
	CHECK_INT(check_mail, 60);            /* out of range: the default stays */
	CHECK_INT(wrap_width, 72);
	CHECK_INT(col_pct[1], 0);
	reset();
	write_conf("check_mail = 0\nwrap_width = 998\n");
	config_load();
	CHECK_INT(check_mail, 0);             /* 0: only F5 */
	CHECK_INT(wrap_width, 998);
	reset();
	write_conf("check_mail = 86401\n");
	config_load();
	CHECK_INT(check_mail, 60);
}

static void t_old_file(void)
{
	/* emsg 0.3: one account, no [account] section, gets a name */
	reset();
	write_conf("server = mail.example.org\nuser = joe\nport = 143\n");
	config_load();
	CHECK_INT(naccts, 1);
	CHECK_STR(accts[0].host, "mail.example.org");
	CHECK_STR(accts[0].name, "joe@mail.example.org");
	/* two unnamed accounts with the same user get unique names */
	account_t *b = account_new(NULL);
	snprintf(b->user, sizeof b->user, "joe");
	snprintf(b->host, sizeof b->host, "mail.example.org");
	account_name(b);
	CHECK_STR(b->name, "joe@mail.example.org-2");
}

static void t_roundtrip(void)
{
	reset();
	write_conf("theme = light\ncheck_mail = 120\nwrap_width = 66\n"
	           "[account a]\nserver = imap.a.example\nuser = u\nsmtp_server = s.a.example\n"
	           "smtp_port = 465\nsmtp_user = u\nsmtp_login = u\nsmtp_security = tls\n"
	           "store_password = yes\npassword = enc1$p\npgp_key = ABCDEF\n"
	           "[account b]\ntype = local\npath = /m\naddress = b@b.example\n"
	           "[account never]\nuser = set-up-not-finished\n");
	config_load();
	ac = account_find("b");
	config_save();
	reset();
	config_load();
	CHECK_STR(A.theme, "light");
	CHECK_INT(check_mail, 120);
	CHECK_INT(wrap_width, 66);
	CHECK_STR(last_account, "b");                 /* the current account */
	CHECK_INT(naccts, 2);                         /* "never" has no server: not saved */
	account_t *a = account_find("a");
	CHECK(a != NULL);
	if (a) {
		CHECK_STR(a->smtp_host, "s.a.example");
		CHECK_STR(a->smtp_security, "tls");
		CHECK_STR(a->pass_enc, "enc1$p");
		CHECK_STR(a->pgp_key, "ABCDEF");
		CHECK_INT(a->store, STORE_YES);
	}
	account_t *b = account_find("b");
	CHECK(b && b->type == STORE_LOCAL && !strcmp(b->path, "/m"));

	char p[512];
	config_path(p, sizeof p, "mcm.conf");
	struct stat st;
	CHECK(stat(p, &st) == 0 && (st.st_mode & 077) == 0);   /* only for you: 0600 */
	strcat(p, ".new");
	CHECK(access(p, F_OK) != 0);                            /* no half file left */
}

void run_config(void)
{
	snprintf(dir, sizeof dir, "%s/mcm-test-XXXXXX", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
	if (!mkdtemp(dir)) { check_fail(__FILE__, __LINE__, "mkdtemp"); return; }
	setenv("XDG_CONFIG_HOME", dir, 1);
	config_mkdir();
	RUN(t_load);
	RUN(t_limits);
	RUN(t_old_file);
	RUN(t_roundtrip);
	reset();
	char p[600];
	config_path(p, sizeof p, "mcm.conf");
	unlink(p);
	config_path(p, sizeof p, NULL);
	rmdir(p);
	rmdir(dir);
}
