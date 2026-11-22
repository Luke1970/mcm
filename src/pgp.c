/* pgp.c - OpenPGP (RFC 4880, PGP/MIME RFC 3156) through the gpg program
 * of GnuPG (Gpg4win on Windows); turned on per account (pgp = in mcm.conf).
 *
 * Reading: a multipart/signed message's signature is checked, a
 * multipart/encrypted one is decrypted, and so is text with an inline
 * "-----BEGIN PGP MESSAGE-----" or "-----BEGIN PGP SIGNED MESSAGE-----"
 * block (msgview.c calls the hooks below). Writing: the PGP: line of the
 * editor asks to sign, to encrypt or both (compose.c calls pgp_wrap).
 *
 * gpg runs without a terminal (--batch) with its own key ring and gpg-agent;
 * mcm asks for a passphrase itself when gpg-agent does not have it cached
 * (--pinentry-mode loopback, given on gpg's standard input). Data goes
 * through files in the private attachment folder, which are removed again
 * right away; status (--status-file) tells mcm what happened.
 */
#include "mcm.h"

#include <openssl/rand.h>

#ifndef O_BINARY
#define O_BINARY 0
#endif

static int seq;                    /* numbers the temporary files */

/* ------------------------------------------------------------------ */
/* Running gpg                                                         */

static const char *gpg_prog(void)
{
#ifdef _WIN32
	static char path[MAX_PATH + 1];
	if (*path) return path;
	char *fp;
	if (SearchPathA(NULL, "gpg.exe", NULL, sizeof path, path, &fp)) return path;
	static const char *const known[] = {"C:\\Program Files (x86)\\GnuPG\\bin\\gpg.exe",
	                                    "C:\\Program Files\\GnuPG\\bin\\gpg.exe", NULL};
	for (int i = 0; known[i]; i++)
		if (GetFileAttributesA(known[i]) != INVALID_FILE_ATTRIBUTES) {
			snprintf(path, sizeof path, "%s", known[i]);
			return path;
		}
	snprintf(path, sizeof path, "gpg.exe");
	return path;
#else
	return "gpg";
#endif
}

#ifdef _WIN32
/* One argument for a Windows command line (the rules of CommandLineToArgvW). */
static void win_arg(buf_t *cl, const char *a)
{
	buf_addc(cl, '"');
	for (const char *p = a;; p++) {
		size_t bs = 0;
		while (*p == '\\') { bs++; p++; }
		if (!*p) {
			for (size_t i = 0; i < bs * 2; i++) buf_addc(cl, '\\');
			break;
		}
		if (*p == '"') {
			for (size_t i = 0; i < bs * 2 + 1; i++) buf_addc(cl, '\\');
		} else {
			for (size_t i = 0; i < bs; i++) buf_addc(cl, '\\');
		}
		buf_addc(cl, *p);
	}
	buf_addc(cl, '"');
}
#endif

/* Run gpg with argv (argv[0] is replaced by the program); pass, if not
 * NULL, is written to its standard input (--passphrase-fd 0). The exit
 * code, or -1 when gpg could not be started. */
static int exec_gpg(const char **argv, const char *pass)
{
	argv[0] = gpg_prog();
#ifdef _WIN32
	buf_t cl = {0};
	for (int i = 0; argv[i]; i++) {
		if (i) buf_addc(&cl, ' ');
		win_arg(&cl, argv[i]);
	}
	SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
	HANDLE rd, wr;
	if (!CreatePipe(&rd, &wr, &sa, 0)) {
		buf_free(&cl);
		return -1;
	}
	SetHandleInformation(wr, HANDLE_FLAG_INHERIT, 0);
	HANDLE nul = CreateFileA("NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
	                         OPEN_EXISTING, 0, NULL);
	STARTUPINFOA si = {.cb = sizeof si, .dwFlags = STARTF_USESTDHANDLES,
	                   .hStdInput = rd, .hStdOutput = nul, .hStdError = nul};
	PROCESS_INFORMATION pi;
	BOOL ok = CreateProcessA(NULL, buf_cstr(&cl), NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL,
	                         &si, &pi);
	buf_free(&cl);
	CloseHandle(rd);
	if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
	if (!ok) {
		CloseHandle(wr);
		return -1;
	}
	DWORD w;
	if (pass) {
		WriteFile(wr, pass, (DWORD)strlen(pass), &w, NULL);
		WriteFile(wr, "\n", 1, &w, NULL);
	}
	CloseHandle(wr);
	WaitForSingleObject(pi.hProcess, INFINITE);
	DWORD code = 2;
	GetExitCodeProcess(pi.hProcess, &code);
	CloseHandle(pi.hProcess);
	CloseHandle(pi.hThread);
	return (int)code;
#else
	int pfd[2];
	if (pipe(pfd) != 0) return -1;
	pid_t pid = fork();
	if (pid < 0) {
		close(pfd[0]);
		close(pfd[1]);
		return -1;
	}
	if (pid == 0) {
		dup2(pfd[0], 0);
		close(pfd[0]);
		close(pfd[1]);
		int fd = open("/dev/null", O_RDWR);
		if (fd >= 0) {
			dup2(fd, 1);
			dup2(fd, 2);
			if (fd > 2) close(fd);
		}
		execvp(argv[0], (char *const *)argv);
		_exit(127);
	}
	close(pfd[0]);
	if (pass) {
		ssize_t r = write(pfd[1], pass, strlen(pass));
		r = write(pfd[1], "\n", 1);
		(void)r;
	}
	close(pfd[1]);
	int st;
	while (waitpid(pid, &st, 0) < 0)
		if (errno != EINTR) return -1;
	if (!WIFEXITED(st)) return -1;
	return WEXITSTATUS(st) == 127 ? -1 : WEXITSTATUS(st);
#endif
}

/* A new temporary file name in the private folder. */
static int tmp_name(char *out, size_t n, const char *what)
{
	char dir[1024];
	if (att_private_dir(dir, sizeof dir) != 0) return -1;
	snprintf(out, n, "%s/pgp-%ld-%d-%s", dir, (long)getpid(), ++seq, what);
	return 0;
}

static int write_file(const char *path, const char *d, size_t n)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, 0600);
	if (fd < 0) return -1;
	size_t off = 0;
	while (off < n) {
		ssize_t w = write(fd, d + off, n - off);
		if (w <= 0) {
			close(fd);
			return -1;
		}
		off += (size_t)w;
	}
	return close(fd);
}

static int read_file(const char *path, buf_t *out)
{
	FILE *f = fopen(path, "rb");
	if (!f) return -1;
	char b[16384];
	size_t r;
	while ((r = fread(b, 1, sizeof b, f)) > 0) buf_add(out, b, r);
	fclose(f);
	return 0;
}

/* The rest of the status line "[GNUPG:] KW ..." (the first one), copied
 * into out with %XX decoded; NULL if there is none. */
static const char *st_get(const char *st, const char *kw, char *out, size_t n)
{
	size_t kl = strlen(kw);
	for (const char *p = st; p && *p;) {
		if (!strncmp(p, "[GNUPG:] ", 9) && !strncmp(p + 9, kw, kl) &&
		    (p[9 + kl] == ' ' || p[9 + kl] == '\n' || p[9 + kl] == '\r' || !p[9 + kl])) {
			const char *v = p + 9 + kl;
			if (*v == ' ') v++;
			size_t k = 0;
			while (*v && *v != '\n' && *v != '\r' && k + 1 < n) {
				if (*v == '%' && isxdigit((unsigned char)v[1]) && isxdigit((unsigned char)v[2])) {
					char h[3] = {v[1], v[2], 0};
					out[k++] = (char)strtol(h, NULL, 16);
					v += 3;
				} else out[k++] = *v++;
			}
			out[k] = '\0';
			return out;
		}
		p = strchr(p, '\n');
		if (p) p++;
	}
	return NULL;
}

static int st_has(const char *st, const char *kw)
{
	char v[8];
	return st_get(st, kw, v, sizeof v) != NULL;
}

/* gpg said the passphrase was wrong: BAD_PASSPHRASE, or (gpg 2.4 with
 * loopback) an ERROR or FAILURE line with error code 11 (GPG_ERR_BAD_PASSPHRASE). */
static int bad_pass(const char *st)
{
	if (st_has(st, "BAD_PASSPHRASE")) return 1;
	for (const char *p = st; p && *p;) {
		if (!strncmp(p, "[GNUPG:] ERROR ", 15) || !strncmp(p, "[GNUPG:] FAILURE ", 17)) {
			const char *e = strchr(p, '\n'), *sp = NULL;
			for (const char *q = p; q < (e ? e : p + strlen(p)); q++)
				if (*q == ' ') sp = q;
			if (sp && (strtoul(sp + 1, NULL, 10) & 0xFFFF) == 11) return 1;
		}
		p = strchr(p, '\n');
		if (p) p++;
	}
	return 0;
}

/* Run gpg with the options args (NULL-terminated); *status gets what it reported (malloc'd). When a secret key needs a
 * passphrase that gpg-agent does not have, it is asked here (on top of bg)
 * and gpg runs again. The exit code; -1 gpg not found, -2 no passphrase
 * given. */
static int gpg_call(const char *const *args, char **status, ui_bg_fn bg)
{
	char sf[1200];
	*status = xstrdup("");
	if (tmp_name(sf, sizeof sf, "status") != 0) return -1;
	char pass[256] = "";
	int have = 0, rc;
	for (int tries = 0;; tries++) {
		int na = 0;
		while (args[na]) na++;
		const char **argv = xcalloc((size_t)na + 16, sizeof *argv);
		int n = 0;
		argv[n++] = "gpg";
		static const char *const common[] = {"--batch", "--no-tty", "--yes", "--pinentry-mode",
		                                     "loopback", "--status-file", NULL};
		for (int i = 0; common[i]; i++) argv[n++] = common[i];
		argv[n++] = sf;
		if (have) {
			argv[n++] = "--passphrase-fd";
			argv[n++] = "0";
		}
		for (int i = 0; args[i]; i++) argv[n++] = args[i];
		argv[n] = NULL;
		remove(sf);
		rc = exec_gpg(argv, have ? pass : NULL);
		free(argv);
		buf_t st = {0};
		read_file(sf, &st);
		remove(sf);
		free(*status);
		*status = st.d ? buf_steal(&st) : xstrdup("");
		int need = have ? bad_pass(*status) : st_has(*status, "NEED_PASSPHRASE");
		if (rc <= 0 || !need || tries >= 4 || st_has(*status, "SIG_CREATED") ||
		    st_has(*status, "DECRYPTION_OKAY"))
			break;
		/* asked for a passphrase: none given yet, or a wrong one */
		static char hint[300];                  /* (a wrong passphrase: the key named before) */
		char msg[400];
		if (!have) {
			const char *h = st_get(*status, "USERID_HINT", hint, sizeof hint);
			if (h && strchr(h, ' ')) memmove(hint, strchr(h, ' ') + 1, strlen(strchr(h, ' ') + 1) + 1);
			else snprintf(hint, sizeof hint, "your secret key");
		}
		const char *who = hint;
		snprintf(msg, sizeof msg, "%s %.300s", have ? "Wrong passphrase, again for" : "For", who);
		memset(pass, 0, sizeof pass);
		ui_field f[] = {{"Passphrase", pass, sizeof pass, 1, 0}};
		if (ui_input_dialog("PGP passphrase", f, 1, msg, bg) < 0) {
			rc = -2;
			break;
		}
		ui_progress("PGP ...", bg);
		have = 1;
	}
	memset(pass, 0, sizeof pass);
	return rc;
}

/* ------------------------------------------------------------------ */
/* Reading                                                             */

/* The user ID of a signature line ("KEYID User ID"). */
static const char *uid_of(const char *v)
{
	const char *s = strchr(v, ' ');
	return s ? s + 1 : v;
}

/* What gpg found out about the signature(s), as a phrase. from: the
 * sender's address, compared with the key's user ID. */
static void sig_note(const char *st, const char *from, buf_t *b)
{
	char v[600], t[64], fpr[120] = "";
	if (st_get(st, "VALIDSIG", v, sizeof v)) {
		sscanf(v, "%119s", fpr);
		size_t fl = strlen(fpr);
		if (fl > 16) memmove(fpr, fpr + fl - 16, 17);   /* the long key ID */
	}
	const char *uid = NULL;
	if (st_get(st, "GOODSIG", v, sizeof v)) {
		uid = uid_of(v);
		buf_printf(b, "good signature from %s", uid);
		if (st_get(st, "TRUST_UNDEFINED", t, sizeof t) || st_get(st, "TRUST_NEVER", t, sizeof t))
			buf_adds(b, " (key not certified)");
		else if (st_get(st, "TRUST_MARGINAL", t, sizeof t))
			buf_adds(b, " (key partly trusted)");
	} else if (st_get(st, "EXPKEYSIG", v, sizeof v)) {
		uid = uid_of(v);
		buf_printf(b, "good signature from %s, but the key has expired", uid);
	} else if (st_get(st, "REVKEYSIG", v, sizeof v)) {
		uid = uid_of(v);
		buf_printf(b, "signature from %s, but the KEY HAS BEEN REVOKED", uid);
	} else if (st_get(st, "EXPSIG", v, sizeof v)) {
		uid = uid_of(v);
		buf_printf(b, "signature from %s has expired", uid);
	} else if (st_get(st, "BADSIG", v, sizeof v)) {
		buf_printf(b, "BAD SIGNATURE from %s: the message was changed after it was signed",
		           uid_of(v));
		return;
	} else if (st_get(st, "NO_PUBKEY", v, sizeof v)) {
		buf_printf(b, "signed with key %s, which is not in your key ring", v);
		if (from && *from) buf_printf(b, " (gpg --locate-keys %s)", from);
		return;
	} else if (st_has(st, "ERRSIG") || st_has(st, "NEWSIG")) {
		buf_adds(b, "the signature could not be checked");
		return;
	} else {
		return;
	}
	if (*fpr) buf_printf(b, ", key %s", fpr);
	if (from && *from && uid && !strcasestr(uid, from))
		buf_printf(b, "; NOTE: the key is not for the sender's address %s", from);
}

/* Upper-case the first letter of a note. */
static char *note_done(buf_t *b)
{
	if (!b->len) buf_adds(b, "nothing to check");
	b->d[0] = (char)toupper((unsigned char)b->d[0]);
	return buf_steal(b);
}

char *pgp_verify(const char *data, size_t n, const char *sig, size_t sn, const char *from)
{
	char df[1200], sf[1200];
	buf_t b = {0};
	if (tmp_name(df, sizeof df, "data") != 0 || tmp_name(sf, sizeof sf, "sig") != 0 ||
	    write_file(df, data, n) != 0 || write_file(sf, sig, sn) != 0) {
		remove(df);
		buf_adds(&b, "the signature could not be checked (no temporary file)");
		return note_done(&b);
	}
	now_status("Checking the PGP signature ...");
	const char *args[] = {"--verify", sf, df, NULL};
	char *st;
	int rc = gpg_call(args, &st, draw_bg);
	remove(df);
	remove(sf);
	if (rc == -1) buf_adds(&b, "signed, but gpg (GnuPG) was not found to check it");
	else sig_note(st, from, &b);
	free(st);
	set_status("%s", "");
	return note_done(&b);
}

char *pgp_decrypt(const char *in, size_t n, buf_t *out, int *encrypted, const char *from)
{
	char inf[1200], of[1200];
	buf_t b = {0};
	*encrypted = 0;
	if (tmp_name(inf, sizeof inf, "in") != 0 || tmp_name(of, sizeof of, "out") != 0 ||
	    write_file(inf, in, n) != 0) {
		buf_adds(&b, "not decrypted (no temporary file)");
		return note_done(&b);
	}
	now_status("PGP ...");
	const char *args[] = {"--decrypt", "--output", of, inf, NULL};
	char *st;
	int rc = gpg_call(args, &st, draw_bg);
	remove(inf);
	*encrypted = st_has(st, "ENC_TO") || st_has(st, "BEGIN_DECRYPTION") ||
	             st_has(st, "DECRYPTION_OKAY");
	if (rc == -1) {
		buf_adds(&b, "gpg (GnuPG) was not found");
	} else if (*encrypted) {
		char v[300];
		if (st_has(st, "DECRYPTION_OKAY")) {
			read_file(of, out);
			buf_adds(&b, "decrypted");
		} else if (rc == -2) {
			buf_adds(&b, "encrypted; not decrypted (no passphrase given)");
		} else if (bad_pass(st)) {
			buf_adds(&b, "encrypted; not decrypted (wrong passphrase)");
		} else if (st_get(st, "NO_SECKEY", v, sizeof v) && !st_has(st, "BEGIN_DECRYPTION")) {
			buf_printf(&b, "encrypted for key %s, whose secret key you do not have", v);
		} else {
			buf_adds(&b, "encrypted; it could not be decrypted");
		}
		size_t l = b.len;
		buf_adds(&b, "; ");
		sig_note(st, from, &b);
		if (b.len == l + 2) b.len = l;
	} else {
		read_file(of, out);       /* signed text: as gpg gives it back */
		sig_note(st, from, &b);
	}
	remove(of);
	free(st);
	set_status("%s", "");
	return note_done(&b);
}

/* ------------------------------------------------------------------ */
/* Writing                                                             */

/* Append text with every line ending in CRLF. */
static void add_crlf(buf_t *b, const char *s, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		if (s[i] == '\r') continue;
		if (s[i] == '\n') buf_addc(b, '\r');
		buf_addc(b, s[i]);
	}
	if (n && s[n - 1] != '\n') buf_adds(b, "\r\n");
}

static void boundary(char *out, size_t n)
{
	unsigned char r[12];
	if (RAND_bytes(r, (int)sizeof r) != 1)
		for (size_t i = 0; i < sizeof r; i++) r[i] = (unsigned char)(time(NULL) >> (i % 4) ^ seq * 97 ^ i * 151);
	int k = snprintf(out, n, "=_mcm_pgp_");
	for (size_t i = 0; i < sizeof r && (size_t)k + 3 < n; i++) k += snprintf(out + k, n - (size_t)k, "%02x", r[i]);
}

/* Why encrypting or signing failed, from gpg's status. */
static void wrap_error(const char *st, int rc, const char *key, char *err, size_t errlen)
{
	if (rc == -1) {
		snprintf(err, errlen, "PGP: gpg (GnuPG) was not found; install it, or empty the PGP: line");
		return;
	}
	if (rc == -2) {
		snprintf(err, errlen, "PGP: no passphrase given");
		return;
	}
	buf_t b = {0};
	for (const char *p = st; p && *p;) {          /* every recipient without a usable key */
		if (!strncmp(p, "[GNUPG:] INV_RECP ", 18)) {
			const char *a = strchr(p + 18, ' ');
			const char *e = strchr(p, '\n');
			if (a) buf_printf(&b, "%s%.*s", b.len ? ", " : "", (int)((e ? e : a + strlen(a)) - a - 1), a + 1);
		}
		p = strchr(p, '\n');
		if (p) p++;
	}
	if (b.len) {
		while (b.len && b.d[b.len - 1] == '\r') b.len--;
		snprintf(err, errlen, "PGP: no usable key for %.300s (gpg --locate-keys ADDRESS imports one)",
		         buf_cstr(&b));
	} else if (bad_pass(st)) {
		snprintf(err, errlen, "PGP: wrong passphrase");
	} else if (st_has(st, "INV_SGNR") || st_has(st, "NO_SECKEY")) {
		snprintf(err, errlen, "PGP: no secret key for %.200s to sign with (pgp_key in mcm.conf)", key);
	} else {
		snprintf(err, errlen, "PGP: gpg failed (exit code %d)", rc);
	}
	buf_free(&b);
}

/* The hash for micalg= (RFC 3156 5, RFC 4880 9.4). */
static const char *micalg(const char *st)
{
	char v[200];
	int type = 0, pk = 0, hash = 0;
	if (st_get(st, "SIG_CREATED", v, sizeof v)) sscanf(v, "%*s %d %d %d", &pk, &hash, &type);
	switch (hash) {
	case 1: return "pgp-md5";
	case 2: return "pgp-sha1";
	case 3: return "pgp-ripemd160";
	case 8: return "pgp-sha256";
	case 9: return "pgp-sha384";
	case 10: return "pgp-sha512";
	case 11: return "pgp-sha224";
	default: return "pgp-sha256";
	}
}

int pgp_wrap(int how, const char *key, const buf_t *entity, const char *const *rcpt, int nrcpt,
             const char *const *hidden, int nhidden, buf_t *out, char *err, size_t errlen)
{
	char inf[1200], of[1200];
	if (tmp_name(inf, sizeof inf, "in") != 0 || tmp_name(of, sizeof of, "out") != 0) {
		snprintf(err, errlen, "PGP: no folder for temporary files");
		return -1;
	}
	/* the entity without the line break before the boundary that follows it */
	size_t elen = entity->len;
	if (elen >= 2 && entity->d[elen - 2] == '\r' && entity->d[elen - 1] == '\n') elen -= 2;
	if (write_file(inf, entity->d, elen) != 0) {
		snprintf(err, errlen, "PGP: cannot write %s: %s", inf, strerror(errno));
		return -1;
	}
	int nargs = 0, cap = 12 + 2 * (nrcpt + nhidden);
	const char **args = xcalloc((size_t)cap, sizeof *args);
	args[nargs++] = "--armor";
	if (how & PGP_ENCRYPT) {
		args[nargs++] = "--encrypt";
		args[nargs++] = "--trust-model";      /* keys need not be certified (as most mail programs) */
		args[nargs++] = "always";
		if (how & PGP_SIGN) args[nargs++] = "--sign";
		args[nargs++] = "--recipient";        /* yourself: the copy in Sent can be read */
		args[nargs++] = key;
		for (int i = 0; i < nrcpt; i++) {
			args[nargs++] = "--recipient";
			args[nargs++] = rcpt[i];
		}
		for (int i = 0; i < nhidden; i++) {   /* Bcc: not named in the message */
			args[nargs++] = "--hidden-recipient";
			args[nargs++] = hidden[i];
		}
	} else {
		args[nargs++] = "--detach-sign";
	}
	if (how & PGP_SIGN) {
		args[nargs++] = "--local-user";
		args[nargs++] = key;
	}
	args[nargs++] = "--output";
	args[nargs++] = of;
	args[nargs++] = inf;
	args[nargs] = NULL;
	ui_progress(how & PGP_ENCRYPT ? "Encrypting (PGP) ..." : "Signing (PGP) ...", editor_redraw);
	char *st;
	int rc = gpg_call(args, &st, editor_redraw);
	free(args);
	remove(inf);
	buf_t res = {0};
	if (rc == 0) read_file(of, &res);
	remove(of);
	if (rc != 0 || !res.len) {
		wrap_error(st, rc, key, err, errlen);
		free(st);
		buf_free(&res);
		return -1;
	}
	char bd[64];
	boundary(bd, sizeof bd);
	if (how & PGP_ENCRYPT) {
		buf_printf(out, "Content-Type: multipart/encrypted;\r\n\tprotocol=\"application/pgp-encrypted\";\r\n"
		                "\tboundary=\"%s\"\r\n\r\n"
		                "This is an OpenPGP/MIME encrypted message (RFC 4880 and 3156)\r\n"
		                "--%s\r\nContent-Type: application/pgp-encrypted\r\n"
		                "Content-Description: PGP/MIME version identification\r\n\r\nVersion: 1\r\n\r\n"
		                "--%s\r\nContent-Type: application/octet-stream; name=\"encrypted.asc\"\r\n"
		                "Content-Description: OpenPGP encrypted message\r\n"
		                "Content-Disposition: inline; filename=\"encrypted.asc\"\r\n\r\n",
		           bd, bd, bd);
	} else {
		buf_printf(out, "Content-Type: multipart/signed; micalg=%s;\r\n"
		                "\tprotocol=\"application/pgp-signature\";\r\n\tboundary=\"%s\"\r\n\r\n"
		                "This is an OpenPGP/MIME signed message (RFC 4880 and 3156)\r\n--%s\r\n",
		           micalg(st), bd, bd);
		buf_add(out, entity->d, elen);
		buf_printf(out, "\r\n--%s\r\nContent-Type: application/pgp-signature; name=\"signature.asc\"\r\n"
		                "Content-Description: OpenPGP digital signature\r\n"
		                "Content-Disposition: attachment; filename=\"signature.asc\"\r\n\r\n",
		           bd);
	}
	add_crlf(out, res.d, res.len);
	buf_printf(out, "\r\n--%s--\r\n", bd);
	free(st);
	buf_free(&res);
	return 0;
}

/* ------------------------------------------------------------------ */
/* Settings                                                            */

int pgp_on(const account_t *a)
{
	return a && *a->pgp && strcasecmp(a->pgp, "no") && strcasecmp(a->pgp, "off");
}

void pgp_key(const account_t *a, char *out, size_t n)
{
	if (*a->pgp_key) snprintf(out, n, "%s", a->pgp_key);
	else login_address(out, n);
}

const char *pgp_program(void) { return gpg_prog(); }

int pgp_found(void)
{
	const char *argv[] = {"gpg", "--version", NULL};
	return exec_gpg(argv, NULL) == 0;
}

/* The hooks msgview.c uses while the account has PGP on. */
static char *verify_hook(const char *d, size_t n, const char *s, size_t sn, const char *from)
{
	return pgp_verify(d, n, s, sn, from);
}

static char *decrypt_hook(const char *in, size_t n, buf_t *out, int *enc, const char *from)
{
	return pgp_decrypt(in, n, out, enc, from);
}

const mv_pgp_t pgp_hooks = {verify_hook, decrypt_hook};
