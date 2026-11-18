/* imap.c - minimal IMAP4rev1 client over TLS (IMAPS, port 993) */
#include "imap.h"
#include "mime.h"
#include "net.h"
#include "util.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>


#define IO_TIMEOUT 60   /* seconds */
#define RC_IOERR   -2

struct imap {
	char    *host, *port, *user, *pass, *selected;
	net_t   *net;
	int      broken;
	unsigned tag;
	uint32_t exists;
	char     err[512];
	/* unsolicited mailbox changes, drained by imap_changes() */
	uint32_t *exp, *exp_given;
	int       nexp, capexp, changed;
	net_trust trust;           /* certificate accepted for this server */
	char     *caps;            /* CAPABILITY reply (upper case), asked once */
	uint32_t  validity;        /* UIDVALIDITY of the selected mailbox (0: not given) */
	/* the copies the last imap_copy/imap_move made (for undo): their uids
	 * from COPYUID (RFC 4315), or the UIDNEXT the mailbox had before */
	uint32_t *cu;
	int       ncu, capcu;
	uint32_t  cu_next, cu_valid;
};

typedef struct {
	buf_t *items;
	int    n;
} resp_t;

/* ------------------------------------------------------------------ */
/* Response parser: turns IMAP data into a tree of atoms/strings/lists */

typedef enum { N_ATOM, N_STR, N_NIL, N_LIST } ntype_t;

typedef struct node {
	ntype_t      t;
	char        *s;
	size_t       len;
	struct node *kid, *next;
} node_t;

static node_t *nnew(ntype_t t)
{
	node_t *n = xcalloc(1, sizeof *n);
	n->t = t;
	return n;
}

static void nfree(node_t *n)
{
	while (n) {
		node_t *nx = n->next;
		nfree(n->kid);
		free(n->s);
		free(n);
		n = nx;
	}
}

static const char *nstr(const node_t *n)
{
	return (n && (n->t == N_STR || n->t == N_ATOM) && n->s) ? n->s : "";
}

static int nis(const node_t *n, const char *atom)
{
	return n && n->t == N_ATOM && !strcasecmp(n->s, atom);
}

static node_t *parse_seq(const char **pp, const char *end, int nested)
{
	node_t *head = NULL, **tail = &head;
	const char *p = *pp;

	while (p < end) {
		char c = *p;
		node_t *n;

		if (c == ' ' || c == '\r' || c == '\n' || c == '\t') { p++; continue; }
		if (c == ')') {
			p++;
			if (nested) break;
			continue;
		}
		if (c == '(') {
			p++;
			n = nnew(N_LIST);
			*pp = p;
			n->kid = parse_seq(pp, end, 1);
			p = *pp;
		} else if (c == '"') {
			buf_t b = {0};
			p++;
			while (p < end && *p != '"') {
				if (*p == '\\' && p + 1 < end) p++;
				buf_addc(&b, *p++);
			}
			if (p < end) p++;
			n = nnew(N_STR);
			n->len = b.len;
			n->s = buf_steal(&b);
		} else if (c == '{') {
			const char *q = p + 1;
			size_t L = 0;
			while (q < end && isdigit((unsigned char)*q)) L = L * 10 + (size_t)(*q++ - '0');
			if (q < end && *q == '+') q++;
			if (q < end && *q == '}') q++;
			if (q < end && *q == '\r') q++;
			if (q < end && *q == '\n') q++;
			if (L > (size_t)(end - q)) L = (size_t)(end - q);
			n = nnew(N_STR);
			n->s = xstrndup(q, L);
			n->len = L;
			p = q + L;
		} else {
			const char *s = p;
			int br = 0;
			while (p < end) {
				char d = *p;
				if (d == '[') br++;
				else if (d == ']' && br) br--;
				else if (!br && (d == ' ' || d == '(' || d == ')' || d == '\r' || d == '\n')) break;
				p++;
			}
			n = nnew(N_ATOM);
			n->s = xstrndup(s, (size_t)(p - s));
			n->len = (size_t)(p - s);
			if (n->len == 3 && !strcasecmp(n->s, "NIL")) n->t = N_NIL;
		}
		*tail = n;
		tail = &n->next;
	}
	*pp = p;
	return head;
}

static node_t *parse_resp(const buf_t *b)
{
	const char *p = b->d;
	return parse_seq(&p, b->d + b->len, 0);
}

/* ------------------------------------------------------------------ */
/* Network / TLS                                                       */

static void seterr(imap_t *im, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(im->err, sizeof im->err, fmt, ap);
	va_end(ap);
}

static void conn_close(imap_t *im)
{
	net_close(im->net);
	im->net = NULL;
	im->broken = 1;
}

static int io_fail(imap_t *im)
{
	seterr(im, "%s", net_error(im->net));
	im->broken = 1;
	return -1;
}

static int io_write(imap_t *im, const char *p, size_t n)
{
	if (im->broken) return -1;
	return net_write(im->net, p, n) < 0 ? io_fail(im) : 0;
}

/* Append one line (including the trailing LF) to b. */
static int io_line(imap_t *im, buf_t *b)
{
	if (im->broken) return -1;
	return net_line(im->net, b) < 0 ? io_fail(im) : 0;
}

static int io_bytes(imap_t *im, buf_t *b, size_t n)
{
	if (im->broken) return -1;
	return net_bytes(im->net, b, n) < 0 ? io_fail(im) : 0;
}

/* Read a complete response, including any {n} literals it contains. */
static int read_response(imap_t *im, buf_t *b)
{
	b->len = 0;
	for (;;) {
		size_t start = b->len;
		if (io_line(im, b) < 0) return -1;

		/* does the line end in {n}\r\n or {n+}\r\n ? */
		const char *s = b->d + start, *e = b->d + b->len;
		if (e > s && e[-1] == '\n') e--;
		if (e > s && e[-1] == '\r') e--;
		if (e > s && e[-1] == '}') {
			const char *q = e - 1;
			if (q > s && q[-1] == '+') q--;
			const char *d = q;
			while (d > s && isdigit((unsigned char)d[-1])) d--;
			if (d < q && d > s && d[-1] == '{') {
				size_t n = strtoul(d, NULL, 10);
				if (io_bytes(im, b, n) < 0) return -1;
				continue;
			}
		}
		buf_cstr(b);
		return 0;
	}
}

static void strip_crlf(char *s)
{
	size_t n = strlen(s);
	while (n && (s[n - 1] == '\r' || s[n - 1] == '\n')) s[--n] = '\0';
}

/* ------------------------------------------------------------------ */
/* Commands                                                            */

static void resp_free(resp_t *r)
{
	if (!r) return;
	for (int i = 0; i < r->n; i++) buf_free(&r->items[i]);
	free(r->items);
	r->items = NULL;
	r->n = 0;
}

/* Send one command and collect untagged responses. 0 = OK, -1 = NO/BAD, RC_IOERR. */
/* Record mailbox size changes reported by the server at any time. */
static void note_event(imap_t *im, unsigned num, const char *word)
{
	if (!strcasecmp(word, "EXISTS")) {
		if (num != im->exists) im->changed = 1;
		im->exists = num;
	} else if (!strcasecmp(word, "EXPUNGE")) {
		if (im->nexp == im->capexp) {
			im->capexp = im->capexp ? im->capexp * 2 : 16;
			im->exp = xrealloc(im->exp, sizeof *im->exp * (size_t)im->capexp);
		}
		im->exp[im->nexp++] = num;
		if (im->exists) im->exists--;
		im->changed = 1;
	}
}

/* Add the uids of a set ("4:6,9") to im->cu, up to the first space or ']'. */
static void cu_add_set(imap_t *im, const char *p)
{
	while (*p && *p != ' ' && *p != ']') {
		char *e;
		unsigned long a = strtoul(p, &e, 10), b = a;
		if (e == p) return;
		if (*e == ':') {
			p = e + 1;
			b = strtoul(p, &e, 10);
			if (e == p) return;
		}
		if (a > b) { unsigned long t = a; a = b; b = t; }
		for (unsigned long u = a; u <= b && im->ncu < 1000000; u++) {
			if (im->ncu == im->capcu) {
				im->capcu = im->capcu ? im->capcu * 2 : 64;
				im->cu = xrealloc(im->cu, sizeof *im->cu * (size_t)im->capcu);
			}
			im->cu[im->ncu++] = (uint32_t)u;
		}
		p = *e == ',' ? e + 1 : e;
	}
}

/* Response codes mcm keeps: [UIDVALIDITY n] and [COPYUID valid src dst]. */
static void note_code(imap_t *im, const char *s)
{
	const char *p = strstr(s, "[UIDVALIDITY ");
	if (p) im->validity = (uint32_t)strtoul(p + 13, NULL, 10);
	if (!(p = strstr(s, "[COPYUID "))) return;
	char *e;
	im->cu_valid = (uint32_t)strtoul(p + 9, &e, 10);
	while (*e == ' ') e++;
	while (*e && *e != ' ' && *e != ']') e++;     /* the source uids */
	while (*e == ' ') e++;
	cu_add_set(im, e);
}

/* Send a command and read until its tagged reply. The command may contain
 * literals: cmd ends in a {n} marker, and after each "+" of the server the
 * next of the nlit parts is sent (each but the last ending in another {n}). */
static int run_parts(imap_t *im, resp_t *r, const char *cmd, const char *const *lit,
                     const size_t *litlen, int nlit)
{
	char tag[16];
	snprintf(tag, sizeof tag, "E%04u", ++im->tag % 10000);
	size_t tl = strlen(tag);

	buf_t out = {0};
	buf_printf(&out, "%s %s\r\n", tag, cmd);
	int rc = io_write(im, out.d, out.len);
	buf_free(&out);
	if (rc < 0) return RC_IOERR;

	for (;;) {
		buf_t b = {0};
		if (read_response(im, &b) < 0) {
			buf_free(&b);
			return RC_IOERR;
		}
		if (nlit > 0 && b.len && b.d[0] == '+') {
			buf_free(&b);
			if (io_write(im, *lit, *litlen) < 0 || io_write(im, "\r\n", 2) < 0) return RC_IOERR;
			lit++, litlen++, nlit--;
			continue;
		}
		note_code(im, buf_cstr(&b));
		if (b.len > tl && !memcmp(b.d, tag, tl) && b.d[tl] == ' ') {
			const char *st = b.d + tl + 1;
			int ok = !strncasecmp(st, "OK", 2);
			if (!ok) {
				seterr(im, "%s", st);
				strip_crlf(im->err);
			}
			buf_free(&b);
			return ok ? 0 : -1;
		}
		unsigned num;
		char word[16];
		if (sscanf(b.d, "* %u %15[A-Za-z]", &num, word) == 2) note_event(im, num, word);
		if (!strncasecmp(b.d, "* BYE", 5)) {
			seterr(im, "server closed connection: %s", b.d + 5);
			strip_crlf(im->err);
		}
		if (r) {
			r->items = xrealloc(r->items, sizeof *r->items * (size_t)(r->n + 1));
			r->items[r->n++] = b;
		} else {
			buf_free(&b);
		}
	}
}

static int run_lit(imap_t *im, resp_t *r, const char *cmd, const char *lit, size_t litlen)
{
	return run_parts(im, r, cmd, &lit, &litlen, lit ? 1 : 0);
}

static int run(imap_t *im, resp_t *r, const char *cmd)
{
	return run_lit(im, r, cmd, NULL, 0);
}

static char *quote(const char *s)
{
	buf_t b = {0};
	buf_addc(&b, '"');
	for (; *s; s++) {
		if (*s == '"' || *s == '\\') buf_addc(&b, '\\');
		if (*s != '\r' && *s != '\n') buf_addc(&b, *s);
	}
	buf_addc(&b, '"');
	return buf_steal(&b);
}

void imap_set_cert_check(imap_t *im, net_cert_fn fn, void *ctx)
{
	im->trust.fn = fn;
	im->trust.ctx = ctx;
}

static int do_open(imap_t *im, const char *host, const char *port, int interactive)
{
	conn_close(im);
	im->net = net_open(host, port, IO_TIMEOUT, im->err, sizeof im->err);
	if (!im->net) return -1;
	im->broken = 0;
	if (net_tls(im->net, &im->trust, interactive) < 0) {
		io_fail(im);
		conn_close(im);
		return -1;
	}

	buf_t g = {0};
	if (read_response(im, &g) < 0) { buf_free(&g); conn_close(im); return -1; }
	int ok = !strncasecmp(g.d, "* OK", 4) || !strncasecmp(g.d, "* PREAUTH", 9);
	if (!ok) {
		seterr(im, "unexpected greeting: %.200s", g.d);
		strip_crlf(im->err);
		buf_free(&g);
		conn_close(im);
		return -1;
	}
	buf_free(&g);
	return 0;
}

static int do_login(imap_t *im, const char *user, const char *pass)
{
	char *qu = quote(user), *qp = quote(pass);
	buf_t c = {0};
	buf_printf(&c, "LOGIN %s %s", qu, qp);
	int rc = run(im, NULL, c.d);
	/* scrub the password from memory we are about to free */
	memset(c.d, 0, c.len);
	memset(qp, 0, strlen(qp));
	buf_free(&c);
	free(qu);
	free(qp);
	return rc;
}

static int reconnect(imap_t *im)
{
	if (do_open(im, im->host, im->port, 0) < 0) return -1;
	if (do_login(im, im->user, im->pass) != 0) return -1;
	if (im->selected) {
		char *q = quote(im->selected);
		buf_t c = {0};
		buf_printf(&c, "SELECT %s", q);
		int rc = run(im, NULL, c.d);
		buf_free(&c);
		free(q);
		if (rc != 0) return -1;
	}
	return 0;
}

/* Run a command; on a dropped connection, reconnect once and retry. */
static int cmd(imap_t *im, resp_t *r, const char *fmt, ...)
{
	va_list ap;
	char *c = NULL;
	va_start(ap, fmt);
	if (vasprintf(&c, fmt, ap) < 0) c = NULL;
	va_end(ap);
	if (!c) return -1;

	int rc = im->broken ? RC_IOERR : run(im, r, c);
	if (rc == RC_IOERR && im->user) {
		resp_free(r);
		if (reconnect(im) == 0) rc = run(im, r, c);
	}
	free(c);
	return rc == 0 ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */

imap_t *imap_new(void)
{
	imap_t *im = xcalloc(1, sizeof *im);
	im->broken = 1;
	return im;
}

static void free_secret(char **p)
{
	if (*p) {
		memset(*p, 0, strlen(*p));
		free(*p);
		*p = NULL;
	}
}

void imap_free(imap_t *im)
{
	if (!im) return;
	conn_close(im);
	free(im->host);
	free(im->port);
	free(im->user);
	free_secret(&im->pass);
	free(im->selected);
	free(im->exp);
	free(im->exp_given);
	free(im->caps);
	free(im->cu);
	net_trust_free(&im->trust);
	free(im);
}

const char *imap_error(imap_t *im) { return im->err; }

int imap_connect(imap_t *im, const char *host, const char *port)
{
	free(im->caps);
	im->caps = NULL;
	free(im->user);
	im->user = NULL;
	free_secret(&im->pass);
	free(im->selected);
	im->selected = NULL;

	if (do_open(im, host, port, 1) < 0) return -1;
	free(im->host);
	free(im->port);
	im->host = xstrdup(host);
	im->port = xstrdup(port);
	return 0;
}

int imap_login(imap_t *im, const char *user, const char *pass)
{
	int rc = do_login(im, user, pass);
	if (rc != 0) {
		if (rc == RC_IOERR) return -1;
		if (!*im->err) seterr(im, "login failed");
		return -1;
	}
	im->user = xstrdup(user);
	im->pass = xstrdup(pass);   /* kept in memory only, for automatic reconnects */
	return 0;
}

void imap_logout(imap_t *im)
{
	if (!im->broken) run(im, NULL, "LOGOUT");
	conn_close(im);
}

/* Decode an RFC 3501 modified UTF-7 mailbox name to UTF-8. */
char *mutf7_decode(const char *s)
{
	buf_t b = {0};
	while (*s) {
		if (*s != '&') { buf_addc(&b, *s++); continue; }
		s++;
		if (*s == '-') { buf_addc(&b, '&'); s++; continue; }
		uint32_t bits = 0, hi = 0;
		int nbits = 0;
		while (*s && *s != '-') {
			int c = (unsigned char)*s, v;
			if (c >= 'A' && c <= 'Z') v = c - 'A';
			else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
			else if (c >= '0' && c <= '9') v = c - '0' + 52;
			else if (c == '+') v = 62;
			else if (c == ',') v = 63;
			else break;
			bits = (bits << 6) | (uint32_t)v;
			nbits += 6;
			if (nbits >= 16) {
				nbits -= 16;
				uint32_t u = (bits >> nbits) & 0xFFFF;
				bits &= (1u << nbits) - 1;
				if (u >= 0xD800 && u < 0xDC00) {
					hi = u;
				} else {
					uint32_t cp = u;
					if (u >= 0xDC00 && u < 0xE000 && hi)
						cp = 0x10000 + ((hi - 0xD800) << 10) + (u - 0xDC00);
					hi = 0;
					utf8_put(&b, cp);
				}
			}
			s++;
		}
		if (*s == '-') s++;
	}
	return buf_steal(&b);
}

/* UTF-8 -> modified UTF-7 (RFC 3501 5.1.3), for folder names typed here. */
char *mutf7_encode(const char *s)
{
	static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+,";
	buf_t b = {0};
	const unsigned char *p = (const unsigned char *)s;
	while (*p) {
		if (*p >= 0x20 && *p < 0x7F) {
			if (*p == '&') buf_adds(&b, "&-");
			else buf_addc(&b, (char)*p);
			p++;
			continue;
		}
		buf_addc(&b, '&');
		uint32_t bits = 0;
		int nbits = 0;
		while (*p && !(*p >= 0x20 && *p < 0x7F)) {
			/* one UTF-8 character (a bad byte counts as itself) */
			uint32_t cp = *p++;
			int more = cp >= 0xF0 ? 3 : cp >= 0xE0 ? 2 : cp >= 0xC0 ? 1 : 0;
			if (more) cp &= 0x3F >> more;
			for (; more && (*p & 0xC0) == 0x80; more--) cp = cp << 6 | (*p++ & 0x3F);
			uint32_t u[2];
			int nu = 0;
			if (cp >= 0x10000 && cp <= 0x10FFFF) {
				u[nu++] = 0xD800 + ((cp - 0x10000) >> 10);
				u[nu++] = 0xDC00 + ((cp - 0x10000) & 0x3FF);
			} else {
				u[nu++] = cp & 0xFFFF;
			}
			for (int i = 0; i < nu; i++) {
				bits = bits << 16 | u[i];
				nbits += 16;
				while (nbits >= 6) {
					nbits -= 6;
					buf_addc(&b, b64[(bits >> nbits) & 0x3F]);
				}
				bits &= (1u << nbits) - 1;
			}
		}
		if (nbits) buf_addc(&b, b64[(bits << (6 - nbits)) & 0x3F]);
		buf_addc(&b, '-');
	}
	return buf_steal(&b);
}

static int is_inbox(const folder_t *f)
{
	size_t n = strlen(f->name);
	if (n < 5 || strncasecmp(f->name, "INBOX", 5)) return 0;
	return n == 5 || (f->delim && f->name[5] == f->delim);
}

static int folder_cmp(const void *a, const void *b)
{
	const folder_t *x = a, *y = b;
	int ix = is_inbox(x), iy = is_inbox(y);
	if (ix != iy) return iy - ix;
	/* compare with the hierarchy delimiter sorting before everything else */
	const unsigned char *p = (const unsigned char *)x->name, *q = (const unsigned char *)y->name;
	for (;; p++, q++) {
		int c1 = *p == (unsigned char)x->delim ? 1 : tolower(*p);
		int c2 = *q == (unsigned char)y->delim ? 1 : tolower(*q);
		if (c1 != c2 || !c1) return c1 - c2;
	}
}

int imap_list(imap_t *im, folder_t **out, int *nout)
{
	resp_t r = {0};
	*out = NULL;
	*nout = 0;
	if (cmd(im, &r, "LIST \"\" \"*\"") != 0) { resp_free(&r); return -1; }

	folder_t *f = NULL;
	int n = 0;
	for (int i = 0; i < r.n; i++) {
		node_t *t = parse_resp(&r.items[i]);
		node_t *flags = t && t->next ? t->next->next : NULL;
		node_t *delim = flags ? flags->next : NULL;
		node_t *name = delim ? delim->next : NULL;

		if (nis(t, "*") && nis(t->next, "LIST") && flags && flags->t == N_LIST && name) {
			folder_t fo = {0};
			fo.name = xstrdup(nstr(name));
			fo.delim = (delim->t == N_STR && delim->s[0]) ? delim->s[0] : 0;
			for (node_t *k = flags->kid; k; k = k->next)
			{
				if (nis(k, "\\Noselect") || nis(k, "\\NonExistent")) fo.noselect = 1;
				else if (nis(k, "\\Sent")) fo.special = FOLDER_SENT;
				else if (nis(k, "\\Drafts")) fo.special = FOLDER_DRAFTS;
				else if (nis(k, "\\Trash")) fo.special = FOLDER_TRASH;
			}

			const char *last = fo.name;
			if (fo.delim)
				for (const char *c = fo.name; *c; c++)
					if (*c == fo.delim && c[1]) { fo.depth++; last = c + 1; }
			fo.display = mutf7_decode(last);

			f = xrealloc(f, sizeof *f * (size_t)(n + 1));
			f[n++] = fo;
		}
		nfree(t);
	}
	resp_free(&r);
	qsort(f, (size_t)n, sizeof *f, folder_cmp);
	*out = f;
	*nout = n;
	return 0;
}

int imap_select(imap_t *im, const char *mbox, uint32_t *exists)
{
	char *q = quote(mbox);
	free(im->selected);
	im->selected = NULL;
	im->exists = 0;
	im->validity = 0;

	int rc = cmd(im, NULL, "SELECT %s", q);
	free(q);
	if (rc == 0) {
		im->selected = xstrdup(mbox);
		*exists = im->exists;
	}
	return rc;
}

static char *addr_display(const node_t *addrs)
{
	if (!addrs || addrs->t != N_LIST || !addrs->kid || addrs->kid->t != N_LIST)
		return xstrdup("");
	const node_t *a = addrs->kid->kid;   /* (name adl mailbox host) */
	const node_t *name = a, *mbox = a && a->next ? a->next->next : NULL;
	const node_t *host = mbox ? mbox->next : NULL;

	if (*nstr(name)) return mime_decode_words(nstr(name));
	buf_t b = {0};
	buf_adds(&b, nstr(mbox));
	if (*nstr(host)) {
		buf_addc(&b, '@');
		buf_adds(&b, nstr(host));
	}
	return buf_steal(&b);
}

/* The first address of an ENVELOPE address list: mailbox@host, or NULL. */
static char *addr_email(const node_t *addrs)
{
	if (!addrs || addrs->t != N_LIST || !addrs->kid || addrs->kid->t != N_LIST)
		return NULL;
	const node_t *a = addrs->kid->kid;   /* (name adl mailbox host) */
	const node_t *mbox = a && a->next ? a->next->next : NULL;
	const node_t *host = mbox ? mbox->next : NULL;
	if (!*nstr(mbox)) return NULL;
	buf_t b = {0};
	buf_adds(&b, nstr(mbox));
	if (*nstr(host)) {
		buf_addc(&b, '@');
		buf_adds(&b, nstr(host));
	}
	return buf_steal(&b);
}

/* Index of seq in the ascending list (or seq - from when there is none), -1 if absent. */
static long seq_index(const uint32_t *seqs, uint32_t count, uint32_t from, uint32_t seq)
{
	if (!seqs) return seq >= from && seq - from < count ? (long)(seq - from) : -1;
	uint32_t lo = 0, hi = count;
	while (lo < hi) {
		uint32_t mid = lo + (hi - lo) / 2;
		if (seqs[mid] < seq) lo = mid + 1;
		else hi = mid;
	}
	return lo < count && seqs[lo] == seq ? (long)lo : -1;
}

static void bs_parse(const node_t *list, const char *sec, mpart_t *p, int depth);

/* An attachment in the structure: a part marked so, or one that is not
 * shown as text (inside multipart/related: only when marked). */
static int part_has_att(const mpart_t *p, int related)
{
	if (!strcmp(p->type, "multipart")) {
		int rel = related || !strcmp(p->subtype, "related");
		for (int i = 0; i < p->nkids; i++)
			if (part_has_att(&p->kids[i], rel)) return 1;
		return 0;
	}
	if (p->attachment) return 1;
	if (related) return 0;
	if (!strcmp(p->type, "text") && (!strcmp(p->subtype, "plain") || !strcmp(p->subtype, "html") ||
	                                 strstr(p->subtype, "markdown")))
		return 0;
	return !(!strcmp(p->type, "message") && !strcmp(p->subtype, "rfc822"));
}

/* Envelopes for the messages in set ("from:to" or "a,b,c:d"); m[i] belongs
 * to seqs[i], or to from + i when seqs is NULL. */
static int fetch_env(imap_t *im, const char *set, const uint32_t *seqs, uint32_t from,
                     uint32_t count, msg_t **out, int *nout)
{
	resp_t r = {0};
	if (cmd(im, &r, "FETCH %s (UID FLAGS INTERNALDATE RFC822.SIZE ENVELOPE BODYSTRUCTURE)", set) != 0) {
		resp_free(&r);
		return -1;
	}

	msg_t *m = xcalloc(count, sizeof *m);
	for (int i = 0; i < r.n; i++) {
		node_t *t = parse_resp(&r.items[i]);
		node_t *seqn = t ? t->next : NULL;
		node_t *kw = seqn ? seqn->next : NULL;
		node_t *list = kw ? kw->next : NULL;

		if (nis(t, "*") && nis(kw, "FETCH") && list && list->t == N_LIST) {
			uint32_t seq = (uint32_t)strtoul(nstr(seqn), NULL, 10);
			long ix = seq_index(seqs, count, from, seq);
			if (ix >= 0) {
				msg_t *x = &m[ix];
				time_t idate = 0;
				for (node_t *k = list->kid; k && k->next; k = k->next->next) {
					node_t *v = k->next;
					if (nis(k, "UID")) {
						x->uid = (uint32_t)strtoul(nstr(v), NULL, 10);
					} else if (nis(k, "RFC822.SIZE")) {
						x->size = (uint32_t)strtoul(nstr(v), NULL, 10);
					} else if (nis(k, "INTERNALDATE")) {
						idate = mime_parse_date(nstr(v));
					} else if (nis(k, "FLAGS") && v->t == N_LIST) {
						x->seen = x->answered = x->flagged = x->deleted = 0;
						for (node_t *f = v->kid; f; f = f->next) {
							if (nis(f, "\\Seen")) x->seen = 1;
							else if (nis(f, "\\Answered")) x->answered = 1;
							else if (nis(f, "\\Flagged")) x->flagged = 1;
							else if (nis(f, "\\Deleted")) x->deleted = 1;
						}
					} else if (nis(k, "BODYSTRUCTURE") && v->t == N_LIST) {
						mpart_t *bs = xcalloc(1, sizeof *bs);
						bs_parse(v, "", bs, 0);
						x->att = part_has_att(bs, 0);
						mpart_free(bs);
					} else if (nis(k, "ENVELOPE") && v->t == N_LIST) {
						node_t *date = v->kid;
						node_t *subj = date ? date->next : NULL;
						node_t *frm = subj ? subj->next : NULL;
						free(x->subject);
						free(x->from);
						free(x->addr);
						free(x->msgid);
						x->date = mime_parse_date(nstr(date));
						x->subject = mime_decode_words(nstr(subj));
						x->from = addr_display(frm);
						x->addr = addr_email(frm);
						/* (date subject from sender reply-to to cc bcc in-reply-to message-id) */
						const node_t *e = frm;
						for (int f = 2; f < 9 && e; f++) e = e->next;
						x->msgid = e && *nstr(e) ? xstrdup(nstr(e)) : NULL;
					}
				}
				if (!x->date) x->date = idate;
			}
		}
		nfree(t);
	}
	resp_free(&r);

	for (uint32_t i = 0; i < count; i++) {
		m[i].seq = seqs ? seqs[i] : from + i;
		if (!m[i].subject) m[i].subject = xstrdup("");
		if (!m[i].from) m[i].from = xstrdup("");
	}
	*out = m;
	*nout = (int)count;
	return 0;
}

int imap_fetch_headers(imap_t *im, uint32_t from, uint32_t to, msg_t **out, int *nout)
{
	*out = NULL;
	*nout = 0;
	if (from < 1 || to < from) return 0;
	char set[32];
	snprintf(set, sizeof set, "%u:%u", from, to);
	return fetch_env(im, set, NULL, from, to - from + 1, out, nout);
}

int imap_fetch_set(imap_t *im, const uint32_t *seqs, int n, msg_t **out, int *nout)
{
	*out = NULL;
	*nout = 0;
	if (n <= 0) return 0;
	buf_t set = {0};
	for (int i = 0; i < n;) {
		int j = i;
		while (j + 1 < n && seqs[j + 1] == seqs[j] + 1) j++;
		if (set.len) buf_addc(&set, ',');
		if (j > i) buf_printf(&set, "%u:%u", seqs[i], seqs[j]);
		else buf_printf(&set, "%u", seqs[i]);
		i = j + 1;
	}
	int rc = fetch_env(im, buf_cstr(&set), seqs, 0, (uint32_t)n, out, nout);
	buf_free(&set);
	return rc;
}

int imap_status_unseen(imap_t *im, const char *mbox, uint32_t *unseen)
{
	resp_t r = {0};
	char *q = quote(mbox);
	int rc = cmd(im, &r, "STATUS %s (UNSEEN)", q);
	free(q);
	if (rc == 0) {
		rc = -1;
		for (int i = 0; i < r.n; i++) {
			node_t *t = parse_resp(&r.items[i]);
			if (nis(t, "*") && nis(t->next, "STATUS"))
				for (node_t *k = t->next->next; k; k = k->next)
					if (k->t == N_LIST)
						for (node_t *a = k->kid; a && a->next; a = a->next->next)
							if (nis(a, "UNSEEN")) {
								*unseen = (uint32_t)strtoul(nstr(a->next), NULL, 10);
								rc = 0;
							}
			nfree(t);
		}
	}
	resp_free(&r);
	return rc;
}

int imap_mark_seen(imap_t *im, uint32_t uid)
{
	return cmd(im, NULL, "UID STORE %u +FLAGS.SILENT (\\Seen)", uid);
}

int imap_add_flags(imap_t *im, uint32_t uid, const char *flags)
{
	return cmd(im, NULL, "UID STORE %u +FLAGS.SILENT (%s)", uid, flags);
}

/* Does the server announce capability cap (MOVE, UIDPLUS)? Asked once. */
static int has_cap(imap_t *im, const char *cap)
{
	if (!im->caps) {
		resp_t r = {0};
		buf_t c = {0};
		buf_adds(&c, " ");
		if (cmd(im, &r, "CAPABILITY") == 0)
			for (int i = 0; i < r.n; i++)
				if (!strncasecmp(buf_cstr(&r.items[i]), "* CAPABILITY ", 13)) {
					buf_adds(&c, r.items[i].d + 13);
					strip_crlf(c.d);
					c.len = strlen(c.d);
				}
		resp_free(&r);
		buf_adds(&c, " ");
		for (char *k = c.d; *k; k++) *k = (char)toupper((unsigned char)*k);
		im->caps = buf_steal(&c);
	}
	char want[64];
	snprintf(want, sizeof want, " %s ", cap);
	return strstr(im->caps, want) != NULL;
}

/* "1:3,7,9" for uids[i..] (ascending), at most about 900 characters;
 * returns the index after the last one used. */
static int uid_set(buf_t *b, const uint32_t *uids, int n, int i)
{
	b->len = 0;
	while (i < n && b->len < 900) {
		int j = i;
		while (j + 1 < n && uids[j + 1] == uids[j] + 1) j++;
		if (b->len) buf_addc(b, ',');
		if (j > i) buf_printf(b, "%u:%u", uids[i], uids[j]);
		else buf_printf(b, "%u", uids[i]);
		i = j + 1;
	}
	buf_cstr(b);
	return i;
}

static int u32cmp_(const void *a, const void *b)
{
	uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
	return x < y ? -1 : x > y;
}

/* Run "UID <verb> <set> <tail>" for all uids, in pieces. */
static int uid_cmd(imap_t *im, const char *verb, const uint32_t *uids, int n, const char *tail)
{
	uint32_t *u = xmalloc(sizeof *u * (size_t)(n ? n : 1));
	memcpy(u, uids, sizeof *u * (size_t)n);
	qsort(u, (size_t)n, sizeof *u, u32cmp_);
	buf_t set = {0};
	int rc = 0;
	for (int i = 0; i < n && rc == 0;) {
		i = uid_set(&set, u, n, i);
		rc = cmd(im, NULL, "UID %s %s%s%s", verb, set.d, *tail ? " " : "", tail);
	}
	buf_free(&set);
	free(u);
	return rc;
}

/* Before copying to mbox: forget the last copies; without UIDPLUS (no
 * COPYUID) note the UIDNEXT of mbox, to find the copies by later. */
static void copy_start(imap_t *im, const char *mbox)
{
	im->ncu = 0;
	im->cu_next = im->cu_valid = 0;
	if (has_cap(im, "UIDPLUS")) return;
	resp_t r = {0};
	char *q = quote(mbox);
	if (cmd(im, &r, "STATUS %s (UIDNEXT UIDVALIDITY)", q) == 0)
		for (int i = 0; i < r.n; i++) {
			node_t *t = parse_resp(&r.items[i]);
			if (nis(t, "*") && nis(t->next, "STATUS"))
				for (node_t *k = t->next->next; k; k = k->next)
					if (k->t == N_LIST)
						for (node_t *a = k->kid; a && a->next; a = a->next->next) {
							uint32_t v = (uint32_t)strtoul(nstr(a->next), NULL, 10);
							if (nis(a, "UIDNEXT")) im->cu_next = v;
							else if (nis(a, "UIDVALIDITY")) im->cu_valid = v;
						}
			nfree(t);
		}
	resp_free(&r);
	free(q);
}

static int copy_to(imap_t *im, const char *verb, const uint32_t *uids, int n, const char *mbox)
{
	char *q = quote(mbox);
	int rc = uid_cmd(im, verb, uids, n, q);
	free(q);
	return rc;
}

int imap_copy(imap_t *im, const uint32_t *uids, int n, const char *mbox)
{
	copy_start(im, mbox);
	return copy_to(im, "COPY", uids, n, mbox);
}

int imap_expunge(imap_t *im, const uint32_t *uids, int n)
{
	if (uid_cmd(im, "STORE", uids, n, "+FLAGS.SILENT (\\Deleted)") != 0) return -1;
	if (!has_cap(im, "UIDPLUS")) {
		/* a plain EXPUNGE would also remove other messages marked deleted */
		seterr(im, "the server cannot remove single messages (no UIDPLUS): "
		           "they are marked deleted (D)");
		return 1;
	}
	return uid_cmd(im, "EXPUNGE", uids, n, "");
}

int imap_move(imap_t *im, const uint32_t *uids, int n, const char *mbox)
{
	copy_start(im, mbox);
	if (has_cap(im, "MOVE")) return copy_to(im, "MOVE", uids, n, mbox);
	if (copy_to(im, "COPY", uids, n, mbox) != 0) return -1;
	return imap_expunge(im, uids, n);
}

void imap_copied(imap_t *im, uint32_t **uids, int *n, uint32_t *next, uint32_t *validity)
{
	*uids = NULL;
	*n = 0;
	if (im->ncu) {
		*uids = xmalloc(sizeof **uids * (size_t)im->ncu);
		memcpy(*uids, im->cu, sizeof **uids * (size_t)im->ncu);
		*n = im->ncu;
	}
	*next = im->ncu ? 0 : im->cu_next;
	*validity = im->cu_valid;
}

uint32_t imap_uidvalidity(imap_t *im) { return im->validity; }

int imap_uids_from(imap_t *im, uint32_t next, uint32_t **out, int *nout)
{
	*out = NULL;
	*nout = 0;
	resp_t r = {0};
	if (cmd(im, &r, "UID SEARCH UID %u:*", next) != 0) {
		resp_free(&r);
		return -1;
	}
	uint32_t *v = NULL;
	int n = 0;
	for (int i = 0; i < r.n; i++) {
		const char *p = buf_cstr(&r.items[i]);
		if (strncasecmp(p, "* SEARCH", 8)) continue;
		for (p += 8; *p;) {
			char *e;
			unsigned long u = strtoul(p, &e, 10);
			if (e == p) { p++; continue; }
			p = e;
			if (u < next) continue;     /* "n:*" also matches the highest uid below n */
			v = xrealloc(v, sizeof *v * (size_t)(n + 1));
			v[n++] = (uint32_t)u;
		}
	}
	resp_free(&r);
	*out = v;
	*nout = n;
	return 0;
}

int imap_message_ids(imap_t *im, const uint32_t *uids, int n, char ***out, uint32_t **outu, int *nout)
{
	*out = NULL;
	*outu = NULL;
	*nout = 0;
	if (n <= 0) return 0;
	uint32_t *u = xmalloc(sizeof *u * (size_t)(n ? n : 1));
	memcpy(u, uids, sizeof *u * (size_t)n);
	qsort(u, (size_t)n, sizeof *u, u32cmp_);
	buf_t set = {0};
	char **ids = NULL;
	uint32_t *idu = NULL;
	int k = 0, rc = 0;
	for (int i = 0; i < n && rc == 0;) {
		i = uid_set(&set, u, n, i);
		resp_t r = {0};
		if ((rc = cmd(im, &r, "UID FETCH %s (UID ENVELOPE)", set.d)) == 0)
			for (int j = 0; j < r.n; j++) {
				node_t *t = parse_resp(&r.items[j]);
				node_t *kw = t && t->next ? t->next->next : NULL;
				node_t *list = kw ? kw->next : NULL;
				if (nis(t, "*") && nis(kw, "FETCH") && list && list->t == N_LIST) {
					uint32_t uid = 0;
					node_t *envl = NULL;
					for (node_t *a = list->kid; a && a->next; a = a->next->next) {
						if (nis(a, "UID")) uid = (uint32_t)strtoul(nstr(a->next), NULL, 10);
						else if (nis(a, "ENVELOPE") && a->next->t == N_LIST) envl = a->next;
					}
					if (uid && envl) {
						/* (date subject from sender reply-to to cc bcc in-reply-to message-id) */
						node_t *e = envl->kid;
						for (int f = 0; f < 9 && e; f++) e = e->next;
						ids = xrealloc(ids, sizeof *ids * (size_t)(k + 1));
						idu = xrealloc(idu, sizeof *idu * (size_t)(k + 1));
						idu[k] = uid;
						ids[k++] = xstrdup(e ? nstr(e) : "");
					}
				}
				nfree(t);
			}
		resp_free(&r);
	}
	buf_free(&set);
	free(u);
	if (rc != 0) {
		for (int i = 0; i < k; i++) free(ids[i]);
		free(ids);
		free(idu);
		return -1;
	}
	*out = ids;
	*outu = idu;
	*nout = k;
	return 0;
}

int imap_unflag(imap_t *im, const uint32_t *uids, int n, const char *flags)
{
	char tail[96];
	snprintf(tail, sizeof tail, "-FLAGS.SILENT (%s)", flags);
	return uid_cmd(im, "STORE", uids, n, tail);
}

int imap_create(imap_t *im, const char *mbox)
{
	char *q = quote(mbox);
	int rc = cmd(im, NULL, "CREATE %s", q);
	if (rc == 0) cmd(im, NULL, "SUBSCRIBE %s", q);
	free(q);
	return rc;
}

int imap_delete(imap_t *im, const char *mbox)
{
	char *q = quote(mbox);
	int rc = cmd(im, NULL, "DELETE %s", q);
	if (rc == 0) cmd(im, NULL, "UNSUBSCRIBE %s", q);
	free(q);
	return rc;
}

int imap_rename(imap_t *im, const char *from, const char *to)
{
	char *qf = quote(from), *qt = quote(to);
	int rc = cmd(im, NULL, "RENAME %s %s", qf, qt);
	if (rc == 0) {
		cmd(im, NULL, "UNSUBSCRIBE %s", qf);
		cmd(im, NULL, "SUBSCRIBE %s", qt);
	}
	free(qf);
	free(qt);
	return rc;
}

const char *imap_password(imap_t *im) { return im->pass ? im->pass : ""; }

int imap_noop(imap_t *im)
{
	return cmd(im, NULL, "NOOP");
}

int imap_changes(imap_t *im, uint32_t *exists, const uint32_t **expunged, int *n)
{
	int changed = im->changed;
	free(im->exp_given);
	im->exp_given = im->exp;
	*expunged = im->exp_given;
	*n = im->nexp;
	*exists = im->exists;
	im->exp = NULL;
	im->nexp = im->capexp = 0;
	im->changed = 0;
	return changed;
}

/* ------------------------------------------------------------------ */
/* BODYSTRUCTURE                                                       */

static const node_t *nth(const node_t *n, int i)
{
	while (n && i-- > 0) n = n->next;
	return n;
}

/* Parameter `want` of a BODYSTRUCTURE parameter list ("NAME" "x.pdf" ...),
 * decoded (RFC 2047 or RFC 2231). malloc'd, or NULL. */
static char *bs_param(const node_t *list, const char *want)
{
	if (!list || list->t != N_LIST) return NULL;
	const char *attr[64], *vals[64];
	int n = 0;
	for (const node_t *a = list->kid; a && a->next && n < 64; a = a->next->next) {
		attr[n] = nstr(a);
		vals[n++] = nstr(a->next);
	}
	return mime_param_decode(attr, vals, n, want);
}

static void bs_parse(const node_t *list, const char *sec, mpart_t *p, int depth)
{
	const node_t *k = list->kid;

	if (k && k->t == N_LIST) {                 /* multipart */
		const node_t *c;
		int cnt = 0;
		for (c = k; c && c->t == N_LIST; c = c->next) cnt++;
		snprintf(p->section, sizeof p->section, "%s", sec);
		snprintf(p->type, sizeof p->type, "multipart");
		snprintf(p->subtype, sizeof p->subtype, "%s", nstr(c));
		str_lower(p->subtype);
		if (depth > 32) return;
		p->kids = xcalloc((size_t)cnt, sizeof *p->kids);
		p->nkids = cnt;
		int i = 0;
		for (c = k; c && c->t == N_LIST; c = c->next, i++) {
			char s[sizeof p->section];
			if (*sec) snprintf(s, sizeof s, "%.50s.%d", sec, i + 1);
			else snprintf(s, sizeof s, "%d", i + 1);
			bs_parse(c, s, &p->kids[i], depth + 1);
		}
		return;
	}

	snprintf(p->section, sizeof p->section, "%s", *sec ? sec : "1");
	snprintf(p->type, sizeof p->type, "%s", nstr(k));
	snprintf(p->subtype, sizeof p->subtype, "%s", nstr(nth(k, 1)));
	str_lower(p->type);
	str_lower(p->subtype);

	const node_t *params = nth(k, 2);
	if (params && params->t == N_LIST)
		for (const node_t *a = params->kid; a && a->next; a = a->next->next) {
			if (!strcasecmp(nstr(a), "CHARSET"))
				snprintf(p->charset, sizeof p->charset, "%s", nstr(a->next));
			else if (!strcasecmp(nstr(a), "FORMAT"))
				p->flowed = !strcasecmp(nstr(a->next), "flowed");
			else if (!strcasecmp(nstr(a), "DELSP"))
				p->delsp = !strcasecmp(nstr(a->next), "yes");
		}
	snprintf(p->cte, sizeof p->cte, "%s", nstr(nth(k, 5)));
	str_lower(p->cte);
	p->size = (uint32_t)strtoul(nstr(nth(k, 6)), NULL, 10);

	/* extension data: disposition position depends on the body type */
	int di = !strcmp(p->type, "text") ? 9
	       : (!strcmp(p->type, "message") && !strcmp(p->subtype, "rfc822")) ? 11 : 8;
	const node_t *d = nth(k, di);
	if (d && d->t == N_LIST && d->kid) {
		if (!strcasecmp(nstr(d->kid), "attachment")) p->attachment = 1;
		p->name = bs_param(d->kid->next, "filename");    /* also RFC 2231 */
	}
	if (!p->name) p->name = bs_param(params, "name");
}

int imap_fetch_structure(imap_t *im, uint32_t uid, char **hdr, size_t *hlen, mpart_t **root)
{
	resp_t r = {0};
	*hdr = NULL;
	*hlen = 0;
	*root = NULL;
	if (cmd(im, &r, "UID FETCH %u (BODYSTRUCTURE BODY.PEEK[HEADER])", uid) != 0) {
		resp_free(&r);
		return -1;
	}
	for (int i = 0; i < r.n; i++) {
		node_t *t = parse_resp(&r.items[i]);
		node_t *kw = t && t->next ? t->next->next : NULL;
		node_t *list = kw ? kw->next : NULL;
		if (nis(t, "*") && nis(kw, "FETCH") && list && list->t == N_LIST) {
			for (node_t *k = list->kid; k && k->next; k = k->next->next) {
				node_t *v = k->next;
				if (nis(k, "BODYSTRUCTURE") && v->t == N_LIST && !*root) {
					*root = xcalloc(1, sizeof **root);
					bs_parse(v, "", *root, 0);
				} else if (k->t == N_ATOM && !strncasecmp(k->s, "BODY[HEADER]", 12) &&
				           v->t == N_STR && !*hdr) {
					*hdr = v->s;            /* steal */
					*hlen = v->len;
					v->s = NULL;
				}
			}
		}
		nfree(t);
	}
	resp_free(&r);
	if (!*hdr) {
		mpart_free(*root);
		*root = NULL;
		seterr(im, "message %u not returned by server", uid);
		return -1;
	}
	return 0;
}

int imap_fetch_partial(imap_t *im, uint32_t uid, imap_partial_t *req, int n)
{
	buf_t c = {0};
	buf_printf(&c, "UID FETCH %u (", uid);
	for (int i = 0; i < n; i++) {
		buf_printf(&c, "%sBODY.PEEK[%s]<%zu.%zu>", i ? " " : "", req[i].section,
		           req[i].off, req[i].len);
		req[i].got = 0;
	}
	buf_addc(&c, ')');

	resp_t r = {0};
	int rc = cmd(im, &r, "%s", c.d);
	buf_free(&c);
	if (rc != 0) {
		resp_free(&r);
		return -1;
	}

	for (int i = 0; i < r.n; i++) {
		node_t *t = parse_resp(&r.items[i]);
		node_t *kw = t && t->next ? t->next->next : NULL;
		node_t *list = kw ? kw->next : NULL;
		if (nis(t, "*") && nis(kw, "FETCH") && list && list->t == N_LIST) {
			for (node_t *k = list->kid; k && k->next; k = k->next->next) {
				node_t *v = k->next;
				if (k->t != N_ATOM || strncasecmp(k->s, "BODY[", 5) || v->t != N_STR) continue;
				const char *sb = k->s + 5, *se = strrchr(k->s, ']');
				if (!se || se < sb) continue;
				size_t off = se[1] == '<' ? strtoul(se + 2, NULL, 10) : 0;
				for (int j = 0; j < n; j++) {
					if (strlen(req[j].section) == (size_t)(se - sb) &&
					    !strncasecmp(req[j].section, sb, (size_t)(se - sb)) && off == req[j].off) {
						buf_add(req[j].out, v->s, v->len);
						req[j].got = v->len;
						break;
					}
				}
			}
		}
		nfree(t);
	}
	resp_free(&r);
	return 0;
}

static void mpart_clear(mpart_t *p)
{
	for (int i = 0; i < p->nkids; i++) mpart_clear(&p->kids[i]);
	free(p->kids);
	free(p->name);
}

void mpart_free(mpart_t *p)
{
	if (!p) return;
	mpart_clear(p);
	free(p);
}

void folders_free(folder_t *f, int n)
{
	for (int i = 0; i < n; i++) {
		free(f[i].name);
		free(f[i].display);
	}
	free(f);
}

void msgs_free(msg_t *m, int n)
{
	for (int i = 0; i < n; i++) {
		free(m[i].from);
		free(m[i].subject);
		free(m[i].addr);
		free(m[i].msgid);
	}
	free(m);
}

int imap_append(imap_t *im, const char *mbox, const char *flags, const char *data, size_t len)
{
	char *q = quote(mbox);
	buf_t c = {0};
	buf_printf(&c, "APPEND %s (%s) {%zu}", q, flags ? flags : "", len);
	free(q);
	int rc = im->broken ? RC_IOERR : run_lit(im, NULL, c.d, data, len);
	if (rc == RC_IOERR && im->user && reconnect(im) == 0) rc = run_lit(im, NULL, c.d, data, len);
	buf_free(&c);
	return rc == 0 ? 0 : -1;
}

static int u32cmp(const void *a, const void *b)
{
	uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
	return x < y ? -1 : x > y;
}

/* SEARCH OR SUBJECT text FROM text. ASCII text goes as quoted strings;
 * other text as UTF-8 literals with CHARSET UTF-8 (RFC 3501 6.4.4). */
int imap_search(imap_t *im, const char *text, uint32_t **out, int *nout)
{
	*out = NULL;
	*nout = 0;
	int ascii = 1;
	for (const char *p = text; *p; p++)
		if ((unsigned char)*p >= 0x80 || *p == '\r' || *p == '\n') ascii = 0;
	size_t tl = strlen(text);
	buf_t c = {0};
	char *q = NULL;
	const char *parts[2];
	size_t lens[2];
	int nparts = 0;
	if (ascii) {
		q = quote(text);
		buf_printf(&c, "SEARCH OR SUBJECT %s FROM %s", q, q);
	} else {
		char mid[64];
		snprintf(mid, sizeof mid, " FROM {%zu}", tl);
		buf_t p1 = {0};
		buf_add(&p1, text, tl);
		buf_adds(&p1, mid);
		buf_printf(&c, "SEARCH CHARSET UTF-8 OR SUBJECT {%zu}", tl);
		q = buf_steal(&p1);
		parts[0] = q;
		lens[0] = strlen(q);
		parts[1] = text;
		lens[1] = tl;
		nparts = 2;
	}
	resp_t r = {0};
	int rc = im->broken ? RC_IOERR : run_parts(im, &r, c.d, parts, lens, nparts);
	if (rc == RC_IOERR && im->user) {
		resp_free(&r);
		if (reconnect(im) == 0) rc = run_parts(im, &r, c.d, parts, lens, nparts);
	}
	buf_free(&c);
	free(q);
	if (rc != 0) {
		resp_free(&r);
		return -1;
	}
	uint32_t *v = NULL;
	int n = 0, cap = 0;
	for (int i = 0; i < r.n; i++) {
		const char *p = r.items[i].d;
		if (strncasecmp(p, "* SEARCH", 8)) continue;
		p += 8;
		for (;;) {
			while (*p == ' ') p++;
			if (!isdigit((unsigned char)*p)) break;
			unsigned long x = strtoul(p, (char **)&p, 10);
			if (!x || x > UINT32_MAX) continue;
			if (n == cap) {
				cap = cap ? cap * 2 : 256;
				v = xrealloc(v, sizeof *v * (size_t)cap);
			}
			v[n++] = (uint32_t)x;
		}
	}
	resp_free(&r);
	if (n) qsort(v, (size_t)n, sizeof *v, u32cmp);
	*out = v;
	*nout = n;
	return 0;
}
