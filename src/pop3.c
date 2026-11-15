/* pop3.c - minimal POP3 client over TLS (see pop3.h) */
#include "pop3.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define IO_TIMEOUT 60
#define MAX_MSGS   1000000

struct pop3 {
	char     *host, *port, *user, *pass;
	net_t    *net;
	int       broken;
	unsigned  gen;
	char      err[512];
	net_trust trust;
};

static void seterr(pop3_t *p, const char *fmt, ...) __attribute__((format(EMSG_PRINTF, 2, 3)));
static void seterr(pop3_t *p, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(p->err, sizeof p->err, fmt, ap);
	va_end(ap);
	for (char *s = p->err; *s; s++)
		if (*s == '\r' || *s == '\n') *s = ' ';
}

static void conn_close(pop3_t *p)
{
	net_close(p->net);
	p->net = NULL;
	p->broken = 1;
}

static int io_fail(pop3_t *p)
{
	seterr(p, "%s", net_error(p->net));
	conn_close(p);
	return -1;
}

static void free_secret(char **s)
{
	if (*s) {
		memset(*s, 0, strlen(*s));
		free(*s);
		*s = NULL;
	}
}

/* One status line: 0 for +OK (the rest in *rest if given), -1 otherwise. */
static int status(pop3_t *p, buf_t *line)
{
	line->len = 0;
	if (p->broken || net_line(p->net, line) < 0) return io_fail(p);
	buf_cstr(line);
	if (!strncmp(line->d, "+OK", 3)) return 0;
	seterr(p, "server: %.300s", line->d);
	return -1;
}

static int send_cmd(pop3_t *p, const char *c)
{
	if (p->broken) {
		seterr(p, "not connected");
		return -1;
	}
	buf_t b = {0};
	buf_adds(&b, c);
	buf_adds(&b, "\r\n");
	int rc = net_write(p->net, b.d, b.len) < 0 ? io_fail(p) : 0;
	memset(b.d, 0, b.len);   /* may hold the password */
	buf_free(&b);
	return rc;
}

/* Command with a one-line answer. */
static int cmd(pop3_t *p, const char *c, buf_t *line)
{
	buf_t tmp = {0};
	if (!line) line = &tmp;
	int rc = send_cmd(p, c) == 0 ? status(p, line) : -1;
	buf_free(&tmp);
	return rc;
}

/* Multi-line data after +OK: lines up to ".", dot-stuffing removed, CRLF kept. */
static int read_multi(pop3_t *p, buf_t *out)
{
	buf_t l = {0};
	for (;;) {
		l.len = 0;
		if (net_line(p->net, &l) < 0) {
			buf_free(&l);
			return io_fail(p);
		}
		const char *s = l.d;
		size_t n = l.len;
		if (n && s[n - 1] == '\n') n--;
		if (n && s[n - 1] == '\r') n--;
		if (n == 1 && s[0] == '.') break;
		if (n && s[0] == '.') { s++; n--; }
		buf_add(out, s, n);
		buf_adds(out, "\r\n");
	}
	buf_free(&l);
	return 0;
}

/* TLS from the start, or (implicit = 0) a plain greeting followed by STLS. */
static int open_mode(pop3_t *p, const char *host, const char *port, int interactive, int implicit)
{
	conn_close(p);
	p->net = net_open(host, port, IO_TIMEOUT, p->err, sizeof p->err);
	if (!p->net) return -1;
	p->broken = 0;
	if (implicit && net_tls(p->net, &p->trust, interactive) < 0) return io_fail(p);
	if (!implicit) net_set_timeout(p->net, 10);   /* a TLS-only server never greets */

	buf_t l = {0};
	int rc = status(p, &l);
	if (rc == 0 && !implicit) {
		/* STLS is required: no password over a plain connection */
		if (cmd(p, "STLS", &l) != 0) {
			seterr(p, "the server on port %s offers no STLS (TLS); use port 995", port);
			rc = -1;
		} else if (net_tls(p->net, &p->trust, interactive) < 0) {
			rc = io_fail(p);
		}
	}
	if (rc == 0) net_set_timeout(p->net, IO_TIMEOUT);
	buf_free(&l);
	if (rc != 0) conn_close(p);
	return rc;
}

/* Port 110 uses STLS; any other port TLS from the start (995 is the usual
 * one), and STLS when the server turns out to speak plain text first. */
static int do_open(pop3_t *p, const char *host, const char *port, int interactive)
{
	int implicit = strcmp(port, "110") != 0;
	if (open_mode(p, host, port, interactive, implicit) == 0) return 0;
	/* only when the TLS handshake itself failed (not a refused certificate) */
	if (!implicit || !strcmp(port, "995") || !strstr(p->err, "handshake")) return -1;
	char first[512];
	snprintf(first, sizeof first, "%s", p->err);
	if (open_mode(p, host, port, interactive, 0) == 0) return 0;
	if (strstr(p->err, "STLS") == NULL) seterr(p, "%s", first);
	return -1;
}

static int do_login(pop3_t *p, const char *user, const char *pass)
{
	buf_t c = {0};
	buf_printf(&c, "USER %s", user);
	int rc = cmd(p, buf_cstr(&c), NULL);
	buf_free(&c);
	if (rc != 0) return -1;
	buf_printf(&c, "PASS %s", pass);
	rc = cmd(p, buf_cstr(&c), NULL);
	memset(c.d, 0, c.len);
	buf_free(&c);
	if (rc == 0) p->gen++;
	return rc;
}

pop3_t *pop3_new(void)
{
	pop3_t *p = xcalloc(1, sizeof *p);
	p->broken = 1;
	return p;
}

void pop3_free(pop3_t *p)
{
	if (!p) return;
	conn_close(p);
	free(p->host);
	free(p->port);
	free(p->user);
	free_secret(&p->pass);
	net_trust_free(&p->trust);
	free(p);
}

const char *pop3_error(pop3_t *p) { return p->err; }
unsigned pop3_generation(pop3_t *p) { return p->gen; }
const char *pop3_password(pop3_t *p) { return p->pass ? p->pass : ""; }

void pop3_set_cert_check(pop3_t *p, net_cert_fn fn, void *ctx)
{
	p->trust.fn = fn;
	p->trust.ctx = ctx;
}

int pop3_connect(pop3_t *p, const char *host, const char *port)
{
	free(p->user);
	p->user = NULL;
	free_secret(&p->pass);
	if (do_open(p, host, port, 1) != 0) return -1;
	free(p->host);
	free(p->port);
	p->host = xstrdup(host);
	p->port = xstrdup(port);
	return 0;
}

int pop3_login(pop3_t *p, const char *user, const char *pass)
{
	if (do_login(p, user, pass) != 0) {
		if (!*p->err) seterr(p, "login failed");
		return -1;
	}
	p->user = xstrdup(user);
	p->pass = xstrdup(pass);   /* memory only: for new sessions */
	return 0;
}

void pop3_quit(pop3_t *p)
{
	if (!p->broken) cmd(p, "QUIT", NULL);
	conn_close(p);
}

int pop3_reconnect(pop3_t *p)
{
	if (!p->host || !p->user) {
		seterr(p, "not logged in");
		return -1;
	}
	pop3_quit(p);
	if (do_open(p, p->host, p->port, 0) != 0) return -1;
	return do_login(p, p->user, p->pass);
}

int pop3_ensure(pop3_t *p)
{
	if (!p->broken) return 0;
	return pop3_reconnect(p);
}

int pop3_noop(pop3_t *p)
{
	if (pop3_ensure(p) != 0) return -1;
	if (cmd(p, "NOOP", NULL) == 0) return 0;
	return p->broken ? pop3_reconnect(p) : -1;
}

static int find_num(const pop3_msg *m, int n, int num)
{
	if (num >= 1 && num <= n && m[num - 1].num == num) return num - 1;
	for (int i = 0; i < n; i++)
		if (m[i].num == num) return i;
	return -1;
}

int pop3_scan(pop3_t *p, pop3_msg **out, int *nout)
{
	*out = NULL;
	*nout = 0;
	if (pop3_ensure(p) != 0) return -1;
	buf_t d = {0};
	if (cmd(p, "LIST", NULL) != 0 || read_multi(p, &d) != 0) {
		buf_free(&d);
		return -1;
	}
	pop3_msg *m = NULL;
	int n = 0;
	for (char *s = buf_cstr(&d), *e; *s; s = e) {
		e = strchr(s, '\n');
		e = e ? e + 1 : s + strlen(s);
		int num;
		unsigned long size;
		if (sscanf(s, "%d %lu", &num, &size) == 2 && num > 0 && n < MAX_MSGS) {
			m = xrealloc(m, sizeof *m * (size_t)(n + 1));
			m[n] = (pop3_msg){.num = num, .size = (uint32_t)size};
			n++;
		}
	}
	d.len = 0;
	if (cmd(p, "UIDL", NULL) != 0 || read_multi(p, &d) != 0) {
		seterr(p, "the server does not support UIDL (unique message ids)");
		buf_free(&d);
		free(m);
		return -1;
	}
	for (char *s = buf_cstr(&d), *e; *s; s = e) {
		e = strchr(s, '\n');
		e = e ? e + 1 : s + strlen(s);
		int num;
		char id[72];
		if (sscanf(s, "%d %71s", &num, id) == 2) {
			int i = find_num(m, n, num);
			if (i >= 0) snprintf(m[i].uidl, sizeof m[i].uidl, "%s", id);
		}
	}
	buf_free(&d);
	/* a message without a unique id cannot be followed: leave it out */
	int k = 0;
	for (int i = 0; i < n; i++)
		if (m[i].uidl[0]) m[k++] = m[i];
	*out = m;
	*nout = k;
	return 0;
}

static int fetch(pop3_t *p, const char *c, buf_t *out)
{
	if (p->broken) {
		seterr(p, "connection lost");
		return -1;
	}
	size_t start = out->len;
	if (cmd(p, c, NULL) != 0 || read_multi(p, out) != 0) {
		out->len = start;
		return -1;
	}
	return 0;
}

int pop3_top(pop3_t *p, int num, buf_t *out)
{
	char c[48];
	snprintf(c, sizeof c, "TOP %d 0", num);
	return fetch(p, c, out);
}

int pop3_retr(pop3_t *p, int num, buf_t *out)
{
	char c[48];
	snprintf(c, sizeof c, "RETR %d", num);
	return fetch(p, c, out);
}
