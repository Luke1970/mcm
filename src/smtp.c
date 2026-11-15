/* smtp.c - message submission over SMTP (see smtp.h) */
#include "smtp.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

/* RFC 5321bis 4.5.3.2 timeouts */
#define T_CMD    300
#define T_DATA   600

struct smtp {
	net_t    *net;
	smtp_caps caps;
};

typedef struct {
	int  code;
	char text[512];   /* last line's text, for error messages */
	buf_t all;        /* every line's text (for EHLO keywords) */
} reply_t;

static void setf(char *err, size_t n, const char *fmt, ...) __attribute__((format(EMSG_PRINTF, 3, 4)));
static void setf(char *err, size_t n, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(err, n, fmt, ap);
	va_end(ap);
}

/* Read a (possibly multi-line) reply: "250-..." lines then "250 ...". */
static int get_reply(smtp_t *s, reply_t *r, char *err, size_t errlen)
{
	buf_free(&r->all);
	r->code = 0;
	r->text[0] = '\0';
	for (int lines = 0; lines < 1000; lines++) {
		buf_t b = {0};
		if (net_line(s->net, &b) < 0) {
			buf_free(&b);
			setf(err, errlen, "%s", net_error(s->net));
			return -1;
		}
		while (b.len && (b.d[b.len - 1] == '\n' || b.d[b.len - 1] == '\r')) b.d[--b.len] = '\0';
		if (b.len < 3 || !isdigit((unsigned char)b.d[0]) || !isdigit((unsigned char)b.d[1]) ||
		    !isdigit((unsigned char)b.d[2]) || (b.len > 3 && b.d[3] != ' ' && b.d[3] != '-')) {
			setf(err, errlen, "bad reply from SMTP server: %.100s", b.d);
			buf_free(&b);
			return -1;
		}
		int code = atoi(b.d);
		const char *t = b.len > 4 ? b.d + 4 : "";
		buf_adds(&r->all, t);
		buf_addc(&r->all, '\n');
		snprintf(r->text, sizeof r->text, "%s", t);
		int last = b.len == 3 || b.d[3] == ' ';
		buf_free(&b);
		if (last) {
			r->code = code;
			return 0;
		}
	}
	setf(err, errlen, "SMTP reply too long");
	return -1;
}

static int send_line(smtp_t *s, const char *line, char *err, size_t errlen)
{
	buf_t b = {0};
	buf_printf(&b, "%s\r\n", line);
	int rc = net_write(s->net, b.d, b.len);
	memset(b.d, 0, b.len);   /* may hold credentials */
	buf_free(&b);
	if (rc < 0) setf(err, errlen, "%s", net_error(s->net));
	return rc;
}

/* Send a command and read the reply; 0 if the code is `want` class (2xx/3xx). */
static int command(smtp_t *s, reply_t *r, int want, char *err, size_t errlen, const char *what,
                   const char *line)
{
	if (send_line(s, line, err, errlen) < 0) return -1;
	if (get_reply(s, r, err, errlen) < 0) return -1;
	if (r->code / 100 != want) {
		setf(err, errlen, "%s: %d %s", what, r->code, r->text);
		return -1;
	}
	return 0;
}

static void parse_ehlo(smtp_t *s, const char *all)
{
	memset(&s->caps, 0, sizeof s->caps);
	const char *p = strchr(all, '\n');        /* first line is the greeting */
	while (p && *++p) {
		const char *e = strchr(p, '\n');
		size_t n = e ? (size_t)(e - p) : strlen(p);
		char kw[64];
		size_t k = 0;
		while (k < n && k < sizeof kw - 1 && p[k] != ' ' && p[k] != '=') {
			kw[k] = (char)toupper((unsigned char)p[k]);
			k++;
		}
		kw[k] = '\0';
		const char *arg = p + k;
		if (!strcmp(kw, "SMTPUTF8")) s->caps.smtputf8 = 1;
		else if (!strcmp(kw, "8BITMIME")) s->caps.eightbitmime = 1;
		else if (!strcmp(kw, "STARTTLS")) s->caps.starttls = 1;
		else if (!strcmp(kw, "SIZE")) s->caps.size_max = strtoul(arg, NULL, 10);
		else if (!strcmp(kw, "AUTH")) {
			char line[512];
			snprintf(line, sizeof line, " %.*s ", (int)(n - k), arg);
			for (char *c = line; *c; c++) *c = (char)toupper((unsigned char)*c);
			if (strstr(line, " PLAIN ")) s->caps.auth_plain = 1;
			if (strstr(line, " LOGIN ")) s->caps.auth_login = 1;
		}
		p = e;
	}
}

/* EHLO, falling back to HELO for very old servers (RFC 5321bis 4.1.1.1). */
static int ehlo(smtp_t *s, const char *name, char *err, size_t errlen)
{
	reply_t r = {0};
	char line[600];
	snprintf(line, sizeof line, "EHLO %s", name);
	if (send_line(s, line, err, errlen) < 0 || get_reply(s, &r, err, errlen) < 0) goto fail;
	if (r.code == 250) {
		parse_ehlo(s, r.all.d ? r.all.d : "");
		buf_free(&r.all);
		return 0;
	}
	snprintf(line, sizeof line, "HELO %s", name);
	if (command(s, &r, 2, err, errlen, "HELO", line) < 0) goto fail;
	memset(&s->caps, 0, sizeof s->caps);
	buf_free(&r.all);
	return 0;
fail:
	buf_free(&r.all);
	return -1;
}

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static char *b64(const unsigned char *s, size_t n)
{
	buf_t b = {0};
	for (size_t i = 0; i < n; i += 3) {
		unsigned v = (unsigned)s[i] << 16;
		if (i + 1 < n) v |= (unsigned)s[i + 1] << 8;
		if (i + 2 < n) v |= s[i + 2];
		buf_addc(&b, B64[v >> 18 & 63]);
		buf_addc(&b, B64[v >> 12 & 63]);
		buf_addc(&b, i + 1 < n ? B64[v >> 6 & 63] : '=');
		buf_addc(&b, i + 2 < n ? B64[v & 63] : '=');
	}
	return buf_steal(&b);
}

static void wipe_free(char *p)
{
	if (p) {
		memset(p, 0, strlen(p));
		free(p);
	}
}

static int auth(smtp_t *s, const char *user, const char *pass, char *err, size_t errlen)
{
	reply_t r = {0};
	int rc = -1;
	if (s->caps.auth_plain || !s->caps.auth_login) {
		/* RFC 4616: authzid NUL authcid NUL passwd */
		size_t ul = strlen(user), pl = strlen(pass);
		unsigned char *raw = xmalloc(ul + pl + 2);
		raw[0] = 0;
		memcpy(raw + 1, user, ul);
		raw[ul + 1] = 0;
		memcpy(raw + ul + 2, pass, pl);
		char *enc = b64(raw, ul + pl + 2);
		memset(raw, 0, ul + pl + 2);
		free(raw);
		buf_t line = {0};
		buf_printf(&line, "AUTH PLAIN %s", enc);
		wipe_free(enc);
		if (send_line(s, line.d, err, errlen) == 0 && get_reply(s, &r, err, errlen) == 0) {
			if (r.code == 235) rc = 0;
			else setf(err, errlen, "SMTP login failed: %d %s", r.code, r.text);
		}
		memset(line.d, 0, line.len);
		buf_free(&line);
	} else {
		if (command(s, &r, 3, err, errlen, "AUTH LOGIN", "AUTH LOGIN") == 0) {
			char *u = b64((const unsigned char *)user, strlen(user));
			char *p = b64((const unsigned char *)pass, strlen(pass));
			if (command(s, &r, 3, err, errlen, "AUTH LOGIN", u) == 0 &&
			    send_line(s, p, err, errlen) == 0 && get_reply(s, &r, err, errlen) == 0) {
				if (r.code == 235) rc = 0;
				else setf(err, errlen, "SMTP login failed: %d %s", r.code, r.text);
			}
			wipe_free(u);
			wipe_free(p);
		}
	}
	buf_free(&r.all);
	return rc;
}

smtp_t *smtp_open(const char *host, const char *port, int implicit, const char *user,
                  const char *pass, net_trust *trust, int interactive, smtp_caps *caps,
                  char *err, size_t errlen)
{
	smtp_t *s = xcalloc(1, sizeof *s);
	reply_t r = {0};
	s->net = net_open(host, port, T_CMD, err, errlen);
	if (!s->net) goto fail;

	if (implicit && net_tls(s->net, trust, interactive) < 0) {
		setf(err, errlen, "%s", net_error(s->net));
		goto fail;
	}

	/* 220 greeting (a 554 greeting means: go away) */
	if (get_reply(s, &r, err, errlen) < 0) goto fail;
	if (r.code != 220) {
		setf(err, errlen, "SMTP server refused the connection: %d %s", r.code, r.text);
		goto fail;
	}

	/* EHLO argument: our FQDN, or else an address literal (RFC 5321bis 4.1.4) */
	char name[300];
#ifdef _WIN32
	int hn = win_gethostname(name, sizeof name);
#else
	int hn = gethostname(name, sizeof name);
#endif
	if (hn != 0 || !strchr(name, '.') ||
	    strspn(name, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-.") !=
	        strlen(name))
		net_local_literal(s->net, name, sizeof name);
	if (ehlo(s, name, err, errlen) < 0) goto fail;

	if (!implicit) {
		if (!s->caps.starttls) {
			setf(err, errlen, "%s:%s does not offer STARTTLS; refusing to send without "
			                  "encryption (use port 465 or 587)", host, port);
			goto fail;
		}
		if (command(s, &r, 2, err, errlen, "STARTTLS", "STARTTLS") < 0) goto fail;
		if (net_tls(s->net, trust, interactive) < 0) {
			setf(err, errlen, "%s", net_error(s->net));
			goto fail;
		}
		/* forget everything learned before TLS (RFC 3207 4.2) */
		if (ehlo(s, name, err, errlen) < 0) goto fail;
	}

	if (user && *user) {
		if (!s->caps.auth_plain && !s->caps.auth_login) {
			setf(err, errlen, "SMTP server offers no supported login method (PLAIN or LOGIN)");
			goto fail;
		}
		if (auth(s, user, pass ? pass : "", err, errlen) < 0) goto fail;
	}
	buf_free(&r.all);
	if (caps) *caps = s->caps;
	return s;

fail:
	buf_free(&r.all);
	if (s->net) net_close(s->net);
	free(s);
	return NULL;
}

int smtp_send(smtp_t *s, const char *from, char *const *rcpt, int nrcpt, const char *data,
              size_t len, int smtputf8, int eightbit, char *err, size_t errlen)
{
	reply_t r = {0};
	buf_t line = {0};
	int rc = -1;

	if (smtputf8 && !s->caps.smtputf8) {
		setf(err, errlen, "the SMTP server does not support SMTPUTF8 (needed for "
		                  "non-ASCII addresses)");
		return -1;
	}
	if (eightbit && !s->caps.eightbitmime && !smtputf8) {
		setf(err, errlen, "internal error: 8-bit body without 8BITMIME");
		return -1;
	}
	if (s->caps.size_max && len > s->caps.size_max) {
		setf(err, errlen, "message is %zu bytes; the server accepts at most %lu", len,
		     s->caps.size_max);
		return -1;
	}

	buf_printf(&line, "MAIL FROM:<%s>", from);
	if (eightbit) buf_adds(&line, " BODY=8BITMIME");
	if (smtputf8) buf_adds(&line, " SMTPUTF8");
	if (s->caps.size_max) buf_printf(&line, " SIZE=%zu", len);
	if (command(s, &r, 2, err, errlen, "sender refused", line.d) < 0) goto rset;

	buf_t bad = {0};
	for (int i = 0; i < nrcpt; i++) {
		line.len = 0;
		buf_printf(&line, "RCPT TO:<%s>", rcpt[i]);
		if (send_line(s, line.d, err, errlen) < 0 || get_reply(s, &r, err, errlen) < 0) {
			buf_free(&bad);
			goto out;
		}
		if (r.code != 250 && r.code != 251)
			buf_printf(&bad, "%s%s (%d %s)", bad.len ? ", " : "", rcpt[i], r.code, r.text);
	}
	if (bad.len) {
		setf(err, errlen, "recipient refused: %s", bad.d);
		buf_free(&bad);
		goto rset;
	}

	if (command(s, &r, 3, err, errlen, "DATA refused", "DATA") < 0) goto rset;

	/* dot-stuffing (RFC 5321bis 4.5.2); the data already ends in CRLF */
	buf_t out = {0};
	int bol = 1;
	for (size_t i = 0; i < len; i++) {
		if (bol && data[i] == '.') buf_addc(&out, '.');
		buf_addc(&out, data[i]);
		bol = data[i] == '\n';
	}
	if (!bol) buf_adds(&out, "\r\n");
	buf_adds(&out, ".\r\n");
	net_set_timeout(s->net, T_DATA);
	int w = net_write(s->net, out.d, out.len);
	buf_free(&out);
	if (w < 0) {
		setf(err, errlen, "%s", net_error(s->net));
		goto out;
	}
	if (get_reply(s, &r, err, errlen) < 0) goto out;
	net_set_timeout(s->net, T_CMD);
	if (r.code / 100 != 2) {
		setf(err, errlen, "message refused: %d %s", r.code, r.text);
		goto out;
	}
	rc = 0;
	goto out;

rset:
	{
		reply_t rr = {0};
		char e2[64];
		send_line(s, "RSET", e2, sizeof e2);
		get_reply(s, &rr, e2, sizeof e2);
		buf_free(&rr.all);
	}
out:
	buf_free(&line);
	buf_free(&r.all);
	return rc;
}

void smtp_close(smtp_t *s)
{
	if (!s) return;
	reply_t r = {0};
	char e[64];
	if (send_line(s, "QUIT", e, sizeof e) == 0) get_reply(s, &r, e, sizeof e);
	buf_free(&r.all);
	net_close(s->net);
	free(s);
}
