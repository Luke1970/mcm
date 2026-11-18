/* mime.c - RFC 2047 header decoding and MIME message rendering */
#include "mime.h"
#include "textconv.h"
#include "util.h"

#include <ctype.h>
#include <errno.h>
#include <iconv.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define MAX_DEPTH 16

/* ------------------------------------------------------------------ */
/* Transfer encodings                                                  */

static int b64val(int c)
{
	if (c >= 'A' && c <= 'Z') return c - 'A';
	if (c >= 'a' && c <= 'z') return c - 'a' + 26;
	if (c >= '0' && c <= '9') return c - '0' + 52;
	if (c == '+') return 62;
	if (c == '/') return 63;
	return -1;
}

static void b64_decode(const char *in, size_t len, buf_t *out)
{
	unsigned acc = 0;
	int bits = 0;
	for (size_t i = 0; i < len; i++) {
		int v = b64val((unsigned char)in[i]);
		if (v < 0) continue;              /* skips '=', whitespace, garbage */
		acc = (acc << 6) | (unsigned)v;
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			buf_addc(out, (char)((acc >> bits) & 0xFF));
			acc &= (1u << bits) - 1;
		}
	}
}

static int hexval(int c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	return -1;
}

static void qp_decode(const char *in, size_t len, buf_t *out, int header)
{
	for (size_t i = 0; i < len; i++) {
		char c = in[i];
		if (header && c == '_') {
			buf_addc(out, ' ');
		} else if (c == '=') {
			if (i + 2 < len && hexval((unsigned char)in[i + 1]) >= 0 &&
			    hexval((unsigned char)in[i + 2]) >= 0) {
				buf_addc(out, (char)(hexval((unsigned char)in[i + 1]) * 16 +
				                     hexval((unsigned char)in[i + 2])));
				i += 2;
			} else if (!header) {
				/* soft line break: '=' [whitespace] CRLF */
				size_t j = i + 1;
				while (j < len && (in[j] == ' ' || in[j] == '\t')) j++;
				if (j < len && in[j] == '\r') j++;
				if (j < len && in[j] == '\n') i = j;
				else if (j >= len) i = j;
				else buf_addc(out, '=');
			} else {
				buf_addc(out, '=');
			}
		} else {
			buf_addc(out, c);
		}
	}
}

/* ------------------------------------------------------------------ */
/* Charsets                                                            */

char *conv_to_utf8(const char *charset, const char *in, size_t len, size_t *outlen)
{
	char cs[64];
	size_t n = 0;

	if (charset)
		while (charset[n] && charset[n] != '*' && n < sizeof cs - 1) {
			cs[n] = charset[n];
			n++;
		}
	cs[n] = '\0';
	str_trim(cs);

	if (!*cs || !strcasecmp(cs, "utf-8") || !strcasecmp(cs, "utf8") ||
	    !strcasecmp(cs, "us-ascii") || !strcasecmp(cs, "ascii")) {
		if (outlen) *outlen = len;
		return xstrndup(in, len);
	}
	/* Latin-1 is very often mislabelled Windows-1252, which is a superset. */
	if (!strcasecmp(cs, "iso-8859-1") || !strcasecmp(cs, "latin1"))
		strcpy(cs, "WINDOWS-1252");

	iconv_t cd = iconv_open("UTF-8", cs);
	if (cd == (iconv_t)-1) {
		if (outlen) *outlen = len;
		return xstrndup(in, len);
	}

	buf_t out = {0};
	char tmp[4096];
#ifdef _WIN32
	const char *ip = in;     /* win-iconv: const input */
#else
	char *ip = (char *)in;
#endif
	size_t il = len;

	while (il > 0) {
		char *op = tmp;
		size_t ol = sizeof tmp;
		size_t r = iconv(cd, &ip, &il, &op, &ol);
		buf_add(&out, tmp, (size_t)(op - tmp));
		if (r == (size_t)-1) {
			if (errno == E2BIG) continue;
			/* invalid or incomplete sequence: replace one byte */
			buf_adds(&out, "\xEF\xBF\xBD");
			ip++;
			il--;
			iconv(cd, NULL, NULL, NULL, NULL);
		}
	}
	char *op = tmp;
	size_t ol = sizeof tmp;
	iconv(cd, NULL, NULL, &op, &ol);
	buf_add(&out, tmp, (size_t)(op - tmp));
	iconv_close(cd);

	if (outlen) *outlen = out.len;
	return buf_steal(&out);
}

/* ------------------------------------------------------------------ */
/* RFC 2047                                                            */

char *mime_decode_words(const char *s)
{
	buf_t out = {0};
	const char *p = s;
	int pending_ws = 0, last_word = 0;

	while (*p) {
		if (p[0] == '=' && p[1] == '?') {
			const char *cs = p + 2;
			const char *q1 = strchr(cs, '?');
			if (q1 && q1[1] && q1[2] == '?') {
				char enc = (char)toupper((unsigned char)q1[1]);
				const char *txt = q1 + 3;
				const char *endw = strstr(txt, "?=");
				if (endw && (enc == 'B' || enc == 'Q') && q1 - cs < 60) {
					char csn[64];
					snprintf(csn, sizeof csn, "%.*s", (int)(q1 - cs), cs);
					buf_t raw = {0};
					if (enc == 'B') b64_decode(txt, (size_t)(endw - txt), &raw);
					else qp_decode(txt, (size_t)(endw - txt), &raw, 1);
					size_t ol;
					char *u = conv_to_utf8(csn, raw.d ? raw.d : "", raw.len, &ol);
					/* whitespace between two encoded words is dropped */
					if (pending_ws && !last_word && out.len) buf_addc(&out, ' ');
					pending_ws = 0;
					buf_add(&out, u, ol);
					free(u);
					buf_free(&raw);
					p = endw + 2;
					last_word = 1;
					continue;
				}
			}
		}
		if (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
			while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
			pending_ws = 1;
			continue;
		}
		if (pending_ws && out.len) buf_addc(&out, ' ');
		pending_ws = 0;
		buf_addc(&out, *p++);
		last_word = 0;
	}
	return buf_steal(&out);
}

/* ------------------------------------------------------------------ */
/* Header helpers                                                      */

/* Split an entity into header block and body at the first empty line. */
static void split_entity(const char *p, size_t len, const char **h, size_t *hl,
                         const char **b, size_t *bl)
{
	const char *end = p + len;

	*h = p;
	if (len && p[0] == '\n') { *hl = 0; *b = p + 1; *bl = (size_t)(end - *b); return; }
	if (len > 1 && p[0] == '\r' && p[1] == '\n') { *hl = 0; *b = p + 2; *bl = (size_t)(end - *b); return; }

	const char *q = p;
	while (q < end) {
		const char *e = memchr(q, '\n', (size_t)(end - q));
		if (!e) break;
		const char *n = e + 1;
		if (n < end && *n == '\n') {
			*hl = (size_t)(n - p); *b = n + 1; *bl = (size_t)(end - *b); return;
		}
		if (n + 1 < end && n[0] == '\r' && n[1] == '\n') {
			*hl = (size_t)(n - p); *b = n + 2; *bl = (size_t)(end - *b); return;
		}
		q = n;
	}
	*hl = len;
	*b = end;
	*bl = 0;
}

/* Unfolded raw value of header `name` in a header block, or NULL. */
static char *hdr_get(const char *h, size_t hlen, const char *name)
{
	size_t nl = strlen(name);
	const char *p = h, *end = h + hlen;

	while (p < end) {
		const char *eol = memchr(p, '\n', (size_t)(end - p));
		const char *next = eol ? eol + 1 : end;

		if ((size_t)(end - p) > nl && !strncasecmp(p, name, nl) && p[nl] == ':') {
			buf_t b = {0};
			const char *q = p + nl + 1;
			for (;;) {
				const char *e = memchr(q, '\n', (size_t)(end - q));
				const char *ce = e ? e : end;
				if (ce > q && ce[-1] == '\r') ce--;
				buf_add(&b, q, (size_t)(ce - q));
				if (!e) break;
				q = e + 1;
				if (q < end && (*q == ' ' || *q == '\t')) continue;
				break;
			}
			char *v = buf_steal(&b);
			str_trim(v);
			return v;
		}
		p = next;
	}
	return NULL;
}

/* The parameters of a structured header value ("type/sub; a=1; b=\"x y\""):
 * attribute names and values (quotes removed) in malloc'd arrays. */
static int params_list(const char *val, char ***attr, char ***vals)
{
	int n = 0;
	*attr = *vals = NULL;
	const char *p = val ? strchr(val, ';') : NULL;
	while (p) {
		p++;
		while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
		const char *an = p;
		while (*p && *p != '=' && *p != ';' && *p != ' ' && *p != '\t') p++;
		size_t al = (size_t)(p - an);
		while (*p == ' ' || *p == '\t') p++;
		if (*p == '=' && al) {
			p++;
			while (*p == ' ' || *p == '\t') p++;
			buf_t b = {0};
			if (*p == '"') {
				p++;
				while (*p && *p != '"') {
					if (*p == '\\' && p[1]) p++;
					buf_addc(&b, *p++);
				}
				if (*p == '"') p++;
			} else {
				while (*p && *p != ';' && *p != ' ' && *p != '\t')
					buf_addc(&b, *p++);
			}
			*attr = xrealloc(*attr, sizeof **attr * (size_t)(n + 1));
			*vals = xrealloc(*vals, sizeof **vals * (size_t)(n + 1));
			(*attr)[n] = xstrndup(an, al);
			(*vals)[n] = b.d ? buf_steal(&b) : xstrdup("");
			n++;
		}
		/* skip to next ';' outside quotes */
		int inq = 0;
		while (*p && (inq || *p != ';')) {
			if (*p == '"') inq = !inq;
			else if (*p == '\\' && inq && p[1]) p++;
			p++;
		}
		p = *p ? p : NULL;
	}
	return n;
}

static void params_free(char **attr, char **vals, int n)
{
	for (int i = 0; i < n; i++) {
		free(attr[i]);
		free(vals[i]);
	}
	free(attr);
	free(vals);
}

/* Get a parameter (e.g. charset, boundary) from a structured header value. */
static char *param_get(const char *val, const char *name)
{
	char **attr, **vals, *r = NULL;
	int n = params_list(val, &attr, &vals);
	for (int i = 0; i < n && !r; i++)
		if (!strcasecmp(attr[i], name)) r = xstrdup(vals[i]);
	params_free(attr, vals, n);
	return r;
}

/* Undo %XX (RFC 2231 extended values) into b. */
static void pct_decode(buf_t *b, const char *s)
{
	for (; *s; s++) {
		int h, l;
		if (*s == '%' && (h = hexval((unsigned char)s[1])) >= 0 &&
		    (l = hexval((unsigned char)s[2])) >= 0) {
			buf_addc(b, (char)(h << 4 | l));
			s += 2;
		} else {
			buf_addc(b, *s);
		}
	}
}

/* The first part of an extended value: "charset'language'" goes to cs. */
static const char *ext_start(const char *v, char *cs, size_t csn)
{
	const char *q1 = strchr(v, '\'');
	const char *q2 = q1 ? strchr(q1 + 1, '\'') : NULL;
	if (!q2) return v;
	snprintf(cs, csn, "%.*s", (int)(q1 - v), v);
	return q2 + 1;
}

char *mime_param_decode(const char *const *attr, const char *const *vals, int n, const char *want)
{
	size_t wl = strlen(want);
	const char *plain = NULL;
	char cs[64] = "";
	buf_t raw = {0};
	int found = 0;

	for (int i = 0; i < n; i++) {
		if (strncasecmp(attr[i], want, wl)) continue;
		if (!attr[i][wl]) plain = vals[i];
		else if (!strcmp(attr[i] + wl, "*")) {             /* name*=utf-8''%C3%A9.pdf */
			pct_decode(&raw, ext_start(vals[i], cs, sizeof cs));
			found = 1;
			break;
		}
	}
	/* name*0=...; name*1*=... : continuations, in order (at most 100) */
	for (int k = 0; !found && k < 100; k++) {
		int hit = -1, enc = 0;
		for (int i = 0; i < n && hit < 0; i++) {
			if (strncasecmp(attr[i], want, wl) || attr[i][wl] != '*') continue;
			const char *d = attr[i] + wl + 1;
			if (!isdigit((unsigned char)*d) || (*d == '0' && isdigit((unsigned char)d[1]))) continue;
			char *e;
			if (strtol(d, &e, 10) != k) continue;
			if (!*e) hit = i;
			else if (e[0] == '*' && !e[1]) { hit = i; enc = 1; }
		}
		if (hit < 0) {
			if (k > 0) found = 1;          /* name*0 .. name*(k-1) */
			break;
		}
		const char *v = vals[hit];
		if (enc && k == 0) v = ext_start(v, cs, sizeof cs);
		if (enc) pct_decode(&raw, v);
		else buf_adds(&raw, v);
		if (k == 99) found = 1;
	}
	if (found && raw.len) {
		char *u = conv_to_utf8(cs, raw.d, raw.len, NULL);
		buf_free(&raw);
		char *r = mime_decode_words(u);      /* (non-standard =?..?= in a continuation) */
		free(u);
		return r;
	}
	buf_free(&raw);
	return plain && *plain ? mime_decode_words(plain) : NULL;
}

/* The decoded file name of a part: filename (Content-Disposition), else
 * name (Content-Type), each also in RFC 2231 form. malloc'd, or NULL. */
static char *part_name(const char *disp, const char *ct)
{
	const char *src[2] = {disp, ct}, *want[2] = {"filename", "name"};
	for (int j = 0; j < 2; j++) {
		char **attr, **vals;
		int n = params_list(src[j], &attr, &vals);
		char *r = mime_param_decode((const char *const *)attr, (const char *const *)vals, n, want[j]);
		params_free(attr, vals, n);
		if (r) return r;
	}
	return NULL;
}

/* "text/plain; charset=..." -> "text/plain" (lowercase, malloc'd) */
static char *mime_type(const char *ct)
{
	if (!ct) return xstrdup("text/plain");
	size_t n = strcspn(ct, ";");
	char *t = xstrndup(ct, n);
	str_trim(t);
	str_lower(t);
	if (!*t) { free(t); return xstrdup("text/plain"); }
	return t;
}

char *mime_header_raw(const char *raw, size_t len, const char *name)
{
	const char *h, *b;
	size_t hl, bl;
	split_entity(raw, len, &h, &hl, &b, &bl);
	return hdr_get(h, hl, name);
}

char *mime_header(const char *raw, size_t len, const char *name)
{
	const char *h, *b;
	size_t hl, bl;
	split_entity(raw, len, &h, &hl, &b, &bl);
	char *v = hdr_get(h, hl, name);
	if (!v) return NULL;
	char *d = mime_decode_words(v);
	free(v);
	return d;
}

/* ------------------------------------------------------------------ */
/* Line helpers (HTML and Markdown: textconv.c)                        */

/* Make sure the output is at the start of a line. */
static void ensure_nl(buf_t *out)
{
	if (out->len && out->d[out->len - 1] != '\n') buf_addc(out, '\n');
}

/* Make sure there is an empty line (paragraph break) before what follows. */
static void ensure_para(buf_t *out)
{
	ensure_nl(out);
	if (out->len >= 2 && out->d[out->len - 2] != '\n') buf_addc(out, '\n');
}

/* ------------------------------------------------------------------ */
/* MIME rendering                                                      */

typedef struct { const char *p; size_t len; } span_t;

static int split_multipart(const char *body, size_t blen, const char *boundary,
                           span_t **parts_out)
{
	const char *pos = body, *end = body + blen, *part_start = NULL;
	size_t bl = strlen(boundary);
	span_t *parts = NULL;
	int n = 0;

	while (pos < end) {
		const char *eol = memchr(pos, '\n', (size_t)(end - pos));
		const char *next = eol ? eol + 1 : end;

		if ((size_t)(end - pos) >= 2 + bl && pos[0] == '-' && pos[1] == '-' &&
		    !memcmp(pos + 2, boundary, bl)) {
			if (part_start) {
				const char *pe = pos;
				if (pe > part_start && pe[-1] == '\n') pe--;
				if (pe > part_start && pe[-1] == '\r') pe--;
				parts = xrealloc(parts, sizeof *parts * (size_t)(n + 1));
				parts[n].p = part_start;
				parts[n].len = (size_t)(pe - part_start);
				n++;
			}
			if ((size_t)(end - pos) >= 4 + bl && pos[2 + bl] == '-' && pos[3 + bl] == '-') {
				part_start = NULL;
				break;
			}
			part_start = next;
		}
		pos = next;
	}
	if (part_start && part_start < end) {   /* unterminated last part */
		parts = xrealloc(parts, sizeof *parts * (size_t)(n + 1));
		parts[n].p = part_start;
		parts[n].len = (size_t)(end - part_start);
		n++;
	}
	*parts_out = parts;
	return n;
}

static char *part_type(span_t s)
{
	const char *h, *b;
	size_t hl, bl;
	split_entity(s.p, s.len, &h, &hl, &b, &bl);
	char *ct = hdr_get(h, hl, "Content-Type");
	char *t = mime_type(ct);
	free(ct);
	return t;
}

static void render_part(buf_t *out, const char *p, size_t len, int depth);

/* The part of a multipart/alternative that is shown: the plain text, unless
 * it is empty (only a line break or two, also in base64) and there is HTML;
 * else the HTML, else a nested multipart, else the first. */
static int alt_pick(span_t *parts, int n)
{
	int pick = -1, html = -1, multi = -1;
	for (int i = 0; i < n; i++) {
		char *t = part_type(parts[i]);
		if (!strcmp(t, "text/plain") && pick < 0) pick = i;
		else if (!strcmp(t, "text/html") && html < 0) html = i;
		else if (!strncmp(t, "multipart/", 10) && multi < 0) multi = i;
		free(t);
	}
	if (pick >= 0 && html >= 0) {
		const char *ph, *pb;
		size_t phl, pbl, blank = 0;
		split_entity(parts[pick].p, parts[pick].len, &ph, &phl, &pb, &pbl);
		for (size_t k = 0; k < pbl; k++)
			if (pb[k] == ' ' || pb[k] == '\t' || pb[k] == '\r' || pb[k] == '\n') blank++;
		if (pbl - blank <= 8) pick = html;
	}
	if (pick < 0) pick = html >= 0 ? html : multi >= 0 ? multi : 0;
	return pick;
}

static void render_summary(buf_t *out, const char *p, size_t len)
{
	static const char *names[] = {"From", "To", "Cc", "Date", "Subject", NULL};
	const char *h, *b;
	size_t hl, bl;
	split_entity(p, len, &h, &hl, &b, &bl);
	for (int i = 0; names[i]; i++) {
		char *v = hdr_get(h, hl, names[i]);
		if (!v) continue;
		char *d = mime_decode_words(v);
		buf_printf(out, "%-8s %s\n", names[i], d);
		free(d);
		free(v);
	}
	buf_addc(out, '\n');
}

void mime_human_size(char *dst, size_t n, size_t bytes)
{
	if (bytes < 1024) snprintf(dst, n, "%zu B", bytes);
	else if (bytes < 1024 * 1024) snprintf(dst, n, "%.1f KB", (double)bytes / 1024);
	else snprintf(dst, n, "%.1f MB", (double)bytes / (1024 * 1024));
}

static void render_part(buf_t *out, const char *p, size_t len, int depth)
{
	const char *h, *b;
	size_t hl, bl;
	split_entity(p, len, &h, &hl, &b, &bl);

	char *ct = hdr_get(h, hl, "Content-Type");
	char *type = mime_type(ct);
	char *cte = hdr_get(h, hl, "Content-Transfer-Encoding");
	char *disp = hdr_get(h, hl, "Content-Disposition");
	int attachment = disp && !strncasecmp(disp, "attachment", 10);

	if (cte) str_lower(cte);

	if (!strncmp(type, "multipart/", 10) && depth < MAX_DEPTH) {
		char *boundary = param_get(ct, "boundary");
		span_t *parts = NULL;
		int n = boundary ? split_multipart(b, bl, boundary, &parts) : 0;

		if (n == 0) {
			buf_adds(out, "[empty or malformed multipart]\n");
		} else if (!strcmp(type, "multipart/alternative")) {
			int pick = alt_pick(parts, n);
			render_part(out, parts[pick].p, parts[pick].len, depth + 1);
		} else {
			for (int i = 0; i < n; i++) {
				if (i) ensure_para(out);
				render_part(out, parts[i].p, parts[i].len, depth + 1);
			}
		}
		free(parts);
		free(boundary);
	} else if (!strcmp(type, "message/rfc822") && !attachment && depth < MAX_DEPTH) {
		buf_adds(out, "\n---------- Attached message ----------\n");
		render_summary(out, b, bl);
		render_part(out, b, bl, depth + 1);
	} else if (!attachment && (!strcmp(type, "text/plain") || !strcmp(type, "text/html") ||
	                           mime_is_markdown(type) || !strncmp(type, "multipart/", 10))) {
		buf_t dec = {0};
		if (cte && !strcmp(cte, "base64")) b64_decode(b, bl, &dec);
		else if (cte && !strcmp(cte, "quoted-printable")) qp_decode(b, bl, &dec, 0);
		else buf_add(&dec, b, bl);

		char *cs = param_get(ct, "charset");
		size_t ul;
		char *u = conv_to_utf8(cs, dec.d ? dec.d : "", dec.len, &ul);
		char *fmt = param_get(ct, "format");
		if (!strcmp(type, "text/html")) html_to_text(u, ul, out);
		else if (mime_is_markdown(type)) md_to_text(u, ul, out);
		else if (fmt && !strcasecmp(fmt, "flowed")) {
			char *ds = param_get(ct, "delsp");
			size_t fl;
			char *f = mime_unflow(u, ul, ds && !strcasecmp(ds, "yes"), &fl);
			buf_add(out, f, fl);
			free(f);
			free(ds);
		} else buf_add(out, u, ul);
		free(fmt);
		if (out->len && out->d[out->len - 1] != '\n') buf_addc(out, '\n');
		free(u);
		free(cs);
		buf_free(&dec);
	} else {
		char *fn = part_name(disp, ct);
		char *fnd = fn ? xstrdup(fn) : xstrdup("(unnamed)");
		size_t sz = (cte && !strcmp(cte, "base64")) ? bl * 3 / 4 : bl;
		char hs[32];
		mime_human_size(hs, sizeof hs, sz);
		buf_printf(out, "[Attachment: %s  (%s, %s)]\n", fnd, type, hs);
		free(fnd);
		free(fn);
	}

	free(ct);
	free(type);
	free(cte);
	free(disp);
}

/* Strip CR and control characters other than newline and tab. */
static char *sanitize(buf_t *in)
{
	buf_t out = {0};
	for (size_t i = 0; i < in->len; i++) {
		unsigned char c = (unsigned char)in->d[i];
		if (c == '\n' || c == '\t' || c >= 0x20) {
			if (c != 0x7F) buf_addc(&out, (char)c);
		}
	}
	/* drop trailing blank lines */
	while (out.len && (out.d[out.len - 1] == '\n' || out.d[out.len - 1] == ' '))
		out.len--;
	buf_addc(&out, '\n');
	return buf_steal(&out);
}

char *mime_render(const char *raw, size_t len, int all_headers)
{
	buf_t out = {0};

	if (all_headers) {
		const char *h, *b;
		size_t hl, bl;
		split_entity(raw, len, &h, &hl, &b, &bl);
		buf_add(&out, h, hl);
		buf_adds(&out, "\n\n");
	} else {
		render_summary(&out, raw, len);
	}
	render_part(&out, raw, len, 0);

	char *s = sanitize(&out);
	buf_free(&out);
	return s;
}

char *mime_render_body(const char *raw, size_t len)
{
	buf_t out = {0};
	render_part(&out, raw, len, 0);
	char *s = sanitize(&out);
	buf_free(&out);
	return s;
}

char *mime_decode_cte(const char *in, size_t len, const char *cte, size_t *outlen)
{
	buf_t b = {0};
	if (cte && !strcasecmp(cte, "base64")) {
		b64_decode(in, len, &b);
	} else if (cte && !strcasecmp(cte, "quoted-printable")) {
		qp_decode(in, len, &b, 0);
		if (b.len && (!len || in[len - 1] != '\n')) b.len--;   /* the newline qp_decode adds */
	} else {
		buf_add(&b, in, len);
	}
	*outlen = b.len;
	buf_cstr(&b);
	return buf_steal(&b);
}

/* Collect the attachments of entity p (the same choices as render_part). */
static void collect_atts(const char *p, size_t len, int depth, mime_att **out, int *n)
{
	const char *h, *b;
	size_t hl, bl;
	split_entity(p, len, &h, &hl, &b, &bl);
	char *ct = hdr_get(h, hl, "Content-Type");
	char *type = mime_type(ct);
	char *cte = hdr_get(h, hl, "Content-Transfer-Encoding");
	char *disp = hdr_get(h, hl, "Content-Disposition");
	int attachment = disp && !strncasecmp(disp, "attachment", 10);

	if (!strncmp(type, "multipart/", 10) && depth < MAX_DEPTH) {
		char *boundary = param_get(ct, "boundary");
		span_t *parts = NULL;
		int k = boundary ? split_multipart(b, bl, boundary, &parts) : 0;
		if (k && !strcmp(type, "multipart/alternative")) {
			int pick = alt_pick(parts, k);
			collect_atts(parts[pick].p, parts[pick].len, depth + 1, out, n);
		} else {
			for (int i = 0; i < k; i++) collect_atts(parts[i].p, parts[i].len, depth + 1, out, n);
		}
		free(parts);
		free(boundary);
	} else if (!strcmp(type, "message/rfc822") && !attachment && depth < MAX_DEPTH) {
		collect_atts(b, bl, depth + 1, out, n);
	} else if (!attachment && (!strcmp(type, "text/plain") || !strcmp(type, "text/html") ||
	                           mime_is_markdown(type) || !strncmp(type, "multipart/", 10))) {
		/* shown as text */
	} else {
		char *fn = part_name(disp, ct);
		*out = xrealloc(*out, sizeof **out * (size_t)(*n + 1));
		mime_att *a = &(*out)[(*n)++];
		a->name = fn ? xstrdup(fn) : xstrdup("(unnamed)");
		a->type = xstrdup(type);
		a->data = mime_decode_cte(b, bl, cte, &a->len);
		free(fn);
	}
	free(ct);
	free(type);
	free(cte);
	free(disp);
}

int mime_attachments(const char *raw, size_t len, mime_att **out)
{
	int n = 0;
	*out = NULL;
	collect_atts(raw, len, 0, out, &n);
	return n;
}

void mime_att_free(mime_att *a, int n)
{
	for (int i = 0; i < n; i++) {
		free(a[i].name);
		free(a[i].type);
		free(a[i].data);
	}
	free(a);
}

char *mime_summary(const char *hdr, size_t len)
{
	buf_t out = {0};
	render_summary(&out, hdr, len);
	return buf_steal(&out);
}

char *mime_unflow(const char *s, size_t n, int delsp, size_t *outlen)
{
	buf_t o = {0};
	int open = 0, odepth = 0;                 /* inside a flowed paragraph */
	for (size_t i = 0; i < n;) {
		const char *e = memchr(s + i, '\n', n - i);
		size_t ln = e ? (size_t)(e - (s + i)) : n - i;
		const char *l = s + i;
		i += ln + (e ? 1 : 0);
		if (ln && l[ln - 1] == '\r') ln--;

		int depth = 0;
		while (depth < (int)ln && l[depth] == '>') depth++;
		const char *t = l + depth;
		size_t tl = ln - (size_t)depth;
		if (tl && *t == ' ') { t++; tl--; }  /* space-stuffing */
		int sig = tl == 3 && !memcmp(t, "-- ", 3);
		int soft = !sig && tl && t[tl - 1] == ' ';

		if (open && depth != odepth) {        /* quote depth changed: paragraph ends */
			while (o.len && o.d[o.len - 1] == ' ') o.len--;
			buf_addc(&o, '\n');
			open = 0;
		}
		if (!open && depth) {
			for (int k = 0; k < depth; k++) buf_addc(&o, '>');
			if (tl) buf_addc(&o, ' ');
		}
		buf_add(&o, t, soft && delsp ? tl - 1 : tl);
		if (soft) {
			open = 1;
			odepth = depth;
		} else {
			buf_addc(&o, '\n');
			open = 0;
		}
	}
	if (open) {
		while (o.len && o.d[o.len - 1] == ' ') o.len--;
		buf_addc(&o, '\n');
	}
	if (outlen) *outlen = o.len;
	char *r = buf_steal(&o);
	return r ? r : xstrdup("");
}

int mime_is_markdown(const char *type)
{
	return !strcasecmp(type, "text/markdown") || !strcasecmp(type, "text/x-markdown");
}

char *mime_decode_text(const char *data, size_t len, int kind, const char *cte,
                       const char *charset, size_t *outlen)
{
	buf_t dec = {0};
	if (cte && !strcasecmp(cte, "base64")) b64_decode(data, len, &dec);
	else if (cte && !strcasecmp(cte, "quoted-printable")) qp_decode(data, len, &dec, 0);
	else buf_add(&dec, data, len);

	size_t ul;
	char *u = conv_to_utf8(charset, dec.d ? dec.d : "", dec.len, &ul);
	buf_free(&dec);
	if (kind == MIME_PLAIN) {
		if (outlen) *outlen = ul;
		return u;
	}
	buf_t out = {0};
	if (kind == MIME_HTML) html_to_text(u, ul, &out);
	else md_to_text(u, ul, &out);
	free(u);
	if (outlen) *outlen = out.len;
	return buf_steal(&out);
}

/* ------------------------------------------------------------------ */
/* Dates and addresses                                                 */

static int mon_idx(const char *s)
{
	static const char *m = "janfebmaraprmayjunjulaugsepoctnovdec";
	for (int i = 0; i < 12; i++)
		if (!strncasecmp(s, m + 3 * i, 3)) return i;
	return -1;
}

/* Parse RFC 2822 dates ("Thu, 12 Nov 2026 23:10:05 +0100") and
 * IMAP INTERNALDATE ("12-Nov-2026 23:10:05 +0100"). */
time_t mime_parse_date(const char *in)
{
	char s[128];
	snprintf(s, sizeof s, "%s", in);
	for (int i = 0; s[i] && i < 12; i++)
		if (s[i] == '-') s[i] = ' ';

	const char *p = s;
	while (isspace((unsigned char)*p)) p++;
	if (isalpha((unsigned char)*p)) {          /* weekday */
		const char *c = strchr(p, ',');
		p = c ? c + 1 : p + 3;
	}

	int day, year, hh, mm, ss = 0, n = 0;
	char mon[4];
	if (sscanf(p, " %d %3s %d %d:%d%n", &day, mon, &year, &hh, &mm, &n) < 5) return 0;
	p += n;
	if (*p == ':') ss = (int)strtol(p + 1, (char **)&p, 10);
	while (*p == ' ') p++;

	long off = 0;
	if (*p == '+' || *p == '-') {
		int v = atoi(p + 1);
		off = (*p == '-' ? -1 : 1) * ((v / 100) * 3600L + (v % 100) * 60L);
	} else {
		static const struct { const char *z; int h; } zones[] = {
			{"EST", -5}, {"EDT", -4}, {"CST", -6}, {"CDT", -5},
			{"MST", -7}, {"MDT", -6}, {"PST", -8}, {"PDT", -7},
		};
		for (size_t i = 0; i < sizeof zones / sizeof *zones; i++)
			if (!strncasecmp(p, zones[i].z, 3)) off = zones[i].h * 3600L;
	}

	int mi = mon_idx(mon);
	if (mi < 0) return 0;
	if (year < 50) year += 2000;
	else if (year < 1000) year += 1900;

	struct tm tm = {0};
	tm.tm_year = year - 1900;
	tm.tm_mon = mi;
	tm.tm_mday = day;
	tm.tm_hour = hh;
	tm.tm_min = mm;
	tm.tm_sec = ss;
	return timegm(&tm) - off;
}

/* The address in a From: header value (the one in <>, else the bare
 * one), or NULL when there is none. */
char *mime_addr_email(const char *v)
{
	if (!v) return NULL;
	const char *s = v, *e;
	const char *lt = strchr(v, '<'), *gt = lt ? strchr(lt, '>') : NULL;
	if (lt && gt) {
		s = lt + 1;
		e = gt;
	} else {
		/* "addr (Name)" or a bare address: the word with the @ */
		const char *at = strchr(v, '@');
		if (!at) return NULL;
		s = e = at;
		while (s > v && !strchr(" \t\r\n<(\",", s[-1])) s--;
		while (*e && !strchr(" \t\r\n>)\",", *e)) e++;
	}
	while (s < e && (*s == ' ' || *s == '\t')) s++;
	while (e > s && (e[-1] == ' ' || e[-1] == '\t')) e--;
	if (e <= s) return NULL;
	return xstrndup(s, (size_t)(e - s));
}

/* Who a From: header value is: the decoded display name, else the address. */
char *mime_addr_display(const char *v)
{
	if (!v) return xstrdup("");
	char *d = mime_decode_words(v);
	char *lt = strchr(d, '<');
	buf_t b = {0};
	if (lt) {
		const char *s = d;
		const char *e = lt;
		while (s < e && (*s == ' ' || *s == '\t' || *s == '"')) s++;
		while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '"')) e--;
		if (e > s) {
			for (const char *q = s; q < e; q++)
				if (*q != '\\') buf_addc(&b, *q);
		} else {
			const char *gt = strchr(lt, '>');
			buf_add(&b, lt + 1, gt ? (size_t)(gt - lt - 1) : strlen(lt + 1));
		}
	} else {
		/* "addr (Name)" or a bare address */
		char *lp = strchr(d, '('), *rp = lp ? strchr(lp, ')') : NULL;
		if (lp && rp && rp > lp + 1) buf_add(&b, lp + 1, (size_t)(rp - lp - 1));
		else buf_adds(&b, str_trim(d));
	}
	free(d);
	char *r = buf_steal(&b);
	if (!r) r = xstrdup("");
	return r;
}

/* A part's Content-Type that is shown in the text (not an attachment). */
static int shown_type(const char *t, size_t n)
{
	static const char *const ok[] = {"text/plain", "text/html", "text/markdown", "text/x-markdown",
	                                 "message/rfc822", NULL};
	if (n >= 10 && !strncasecmp(t, "multipart/", 10)) return 1;
	for (int i = 0; ok[i]; i++)
		if (strlen(ok[i]) == n && !strncasecmp(t, ok[i], n)) return 1;
	return 0;
}

int mime_has_attachment(const char *raw, size_t len)
{
	size_t i = 0;
	int top = 1, mixed = 0;
	while (i < len) {
		/* one header block: the message's, or a part's after a boundary */
		int disp_att = 0, disp_inline = 0, cid = 0, other = 0, blank = 0;
		while (i < len) {
			size_t e = i;
			while (e < len && raw[e] != '\n') e++;
			size_t ll = e - i;
			if (ll && raw[e - 1] == '\r') ll--;
			const char *l = raw + i;
			i = e < len ? e + 1 : e;
			if (!ll) { blank = 1; break; }
			if (ll > 13 && !strncasecmp(l, "Content-Type:", 13)) {
				const char *v = l + 13;
				while (v < l + ll && (*v == ' ' || *v == '\t')) v++;
				size_t tn = 0;
				while (v + tn < l + ll && v[tn] != ';' && v[tn] != ' ' && v[tn] != '\t') tn++;
				if (!shown_type(v, tn)) other = 1;
				if (top && tn == 15 && !strncasecmp(v, "multipart/mixed", 15)) mixed = 1;
			} else if (ll > 20 && !strncasecmp(l, "Content-Disposition:", 20)) {
				const char *v = l + 20;
				while (v < l + ll && (*v == ' ' || *v == '\t')) v++;
				if ((size_t)(l + ll - v) >= 10 && !strncasecmp(v, "attachment", 10)) disp_att = 1;
				if ((size_t)(l + ll - v) >= 6 && !strncasecmp(v, "inline", 6)) disp_inline = 1;
			} else if (ll > 11 && !strncasecmp(l, "Content-ID:", 11)) {
				cid = 1;
			}
		}
		/* an attachment: so marked, or a part that is not text and not
		 * an inline picture of an HTML text (those have a Content-ID) */
		if (disp_att || (other && !disp_inline && !cid)) return 1;
		if (top && !blank) return 0;
		if (top && i >= len) return mixed;      /* only the header was there */
		top = 0;
		/* to the next boundary line */
		while (i < len) {
			size_t e = i;
			while (e < len && raw[e] != '\n') e++;
			int bnd = e - i >= 3 && raw[i] == '-' && raw[i + 1] == '-' && raw[i + 2] != '-';
			i = e < len ? e + 1 : e;
			if (bnd) break;
		}
	}
	return 0;
}
