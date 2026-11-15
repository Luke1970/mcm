/* compose.c - turn the editor text into an Internet message (see compose.h) */
#include "compose.h"
#include "mime.h"

#include <sys/types.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#endif
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <openssl/rand.h>
#include <sys/stat.h>

#define FOLD_AT   78     /* RFC 5322 2.1.1: SHOULD */
#define LINE_MAX_ 998    /* RFC 5322 2.1.1: MUST (octets, without CRLF) */

/* ------------------------------------------------------------------ */
/* Character classes                                                   */

/* RFC 5322 atext, extended with UTF8-non-ascii by RFC 6532 */
static int is_atext(unsigned char c)
{
	return isalnum(c) || (c && strchr("!#$%&'*+-/=?^_`{|}~", c)) || c >= 0x80;
}

static int is_ascii(const char *s)
{
	for (; *s; s++)
		if ((unsigned char)*s >= 0x80) return 0;
	return 1;
}

/* Valid UTF-8 without C0/C1 controls other than TAB. */
static int valid_text(const char *s, size_t n)
{
	size_t i = 0;
	while (i < n) {
		size_t cl;
		unsigned char c = (unsigned char)s[i];
		if (c < 0x80) {
			if ((c < 0x20 && c != '\t') || c == 0x7F) return 0;
			i++;
			continue;
		}
		wchar_t w = utf8_next(s + i, n - i, &cl);
		if (w == 0xFFFD && cl == 1) return 0;
		if (w >= 0x80 && w < 0xA0) return 0;
		i += cl;
	}
	return 1;
}

static int dot_atom_ok(const char *s)
{
	if (!*s || *s == '.') return 0;
	for (const char *p = s; *p; p++) {
		if (*p == '.') {
			if (p[1] == '.' || p[1] == '\0') return 0;
		} else if (!is_atext((unsigned char)*p)) {
			return 0;
		}
	}
	return 1;
}

/* ------------------------------------------------------------------ */
/* Lexer for structured header values                                  */

typedef struct {
	const char *p;
	char err[200];
} lex_t;

static int lerr(lex_t *l, const char *fmt, ...) __attribute__((format(MCM_PRINTF, 2, 3)));
static int lerr(lex_t *l, const char *fmt, ...)
{
	if (!*l->err) {
		va_list ap;
		va_start(ap, fmt);
		vsnprintf(l->err, sizeof l->err, fmt, ap);
		va_end(ap);
	}
	return -1;
}

/* Skip white space and (nested) comments. */
static int skip_cfws(lex_t *l)
{
	for (;;) {
		while (*l->p == ' ' || *l->p == '\t' || *l->p == '\r' || *l->p == '\n') l->p++;
		if (*l->p != '(') return 0;
		int depth = 0;
		do {
			if (!*l->p) return lerr(l, "unclosed comment '('");
			if (*l->p == '\\' && l->p[1]) l->p++;
			else if (*l->p == '(') depth++;
			else if (*l->p == ')') depth--;
			l->p++;
		} while (depth);
	}
}

static char *get_atom(lex_t *l)
{
	const char *s = l->p;
	while (is_atext((unsigned char)*l->p)) l->p++;
	return l->p > s ? xstrndup(s, (size_t)(l->p - s)) : NULL;
}

/* At '"': return the unescaped content. */
static char *get_qstring(lex_t *l)
{
	buf_t b = {0};
	l->p++;
	for (;;) {
		char c = *l->p;
		if (!c) { buf_free(&b); lerr(l, "unclosed quote '\"'"); return NULL; }
		l->p++;
		if (c == '"') break;
		if (c == '\\') {
			if (!*l->p) continue;
			c = *l->p++;
		}
		if (c == '\r' || c == '\n') continue;
		buf_addc(&b, c);
	}
	return buf_steal(&b);
}

/* A word (atom or quoted-string); encoded-words in atoms are decoded when
 * `decode` is set. *quoted tells which it was. */
static char *get_word(lex_t *l, int decode, int *quoted)
{
	if (skip_cfws(l) < 0) return NULL;
	*quoted = 0;
	if (*l->p == '"') {
		*quoted = 1;
		return get_qstring(l);
	}
	char *a = get_atom(l);
	if (a && decode && !strncmp(a, "=?", 2)) {
		char *d = mime_decode_words(a);
		free(a);
		a = d;
	}
	return a;
}

/* ------------------------------------------------------------------ */
/* Addresses (RFC 5322 3.4, RFC 6532, RFC 6854)                        */

typedef struct {
	char *name;     /* display name (UTF-8) or NULL */
	char *local;    /* local-part, unquoted */
	char *domain;
} mbox_t;

typedef struct {
	char   *group;  /* group display name, or NULL for a plain mailbox */
	mbox_t *m;
	int     n;
} entry_t;

typedef struct {
	entry_t *e;
	int      n;
} alist_t;

static void alist_free(alist_t *a)
{
	for (int i = 0; i < a->n; i++) {
		for (int j = 0; j < a->e[i].n; j++) {
			free(a->e[i].m[j].name);
			free(a->e[i].m[j].local);
			free(a->e[i].m[j].domain);
		}
		free(a->e[i].m);
		free(a->e[i].group);
	}
	free(a->e);
	memset(a, 0, sizeof *a);
}

/* local-part = dot-atom / quoted-string (obs: words separated by dots) */
static char *parse_local(lex_t *l)
{
	buf_t b = {0};
	for (;;) {
		int q;
		char *w = get_word(l, 0, &q);
		if (!w) { buf_free(&b); return NULL; }
		buf_adds(&b, w);
		free(w);
		if (skip_cfws(l) < 0) { buf_free(&b); return NULL; }
		if (*l->p != '.') break;
		buf_addc(&b, '.');
		l->p++;
		if (skip_cfws(l) < 0) { buf_free(&b); return NULL; }
		if (*l->p != '"' && !is_atext((unsigned char)*l->p)) {
			lerr(l, "two dots in a row, or a dot just before '@', in '%.60s'", b.d);
			buf_free(&b);
			return NULL;
		}
	}
	return buf_steal(&b);
}

static char *parse_domain(lex_t *l)
{
	if (skip_cfws(l) < 0) return NULL;
	if (*l->p == '[') {
		const char *s = l->p;
		while (*l->p && *l->p != ']') {
			if (*l->p == '[' && l->p != s) { lerr(l, "bad domain literal"); return NULL; }
			l->p++;
		}
		if (*l->p != ']') { lerr(l, "unclosed domain literal '['"); return NULL; }
		l->p++;
		return xstrndup(s, (size_t)(l->p - s));
	}
	buf_t b = {0};
	for (;;) {
		if (skip_cfws(l) < 0) break;
		char *a = get_atom(l);
		if (!a) break;
		buf_adds(&b, a);
		free(a);
		if (skip_cfws(l) < 0) break;
		if (*l->p != '.') return buf_steal(&b);
		buf_addc(&b, '.');
		l->p++;
	}
	buf_free(&b);
	lerr(l, "missing or bad domain after '@'");
	return NULL;
}

/* Check an address for SMTP (RFC 5321bis 4.1.2, 4.5.3.1): lengths,
 * LDH domain labels (U-labels allowed, they need SMTPUTF8), literals. */
static int check_addr(lex_t *l, const mbox_t *m)
{
	if (!*m->local) return lerr(l, "empty local part before '@'");
	if (strlen(m->local) > 64) return lerr(l, "local part longer than 64 octets: %.40s...", m->local);
	const char *d = m->domain;
	if (strlen(d) > 255) return lerr(l, "domain longer than 255 octets");
	if (*d == '[') {
		char in[256];
		snprintf(in, sizeof in, "%.*s", (int)strlen(d) - 2, d + 1);
		unsigned char a[16];
		if (!strncasecmp(in, "IPv6:", 5) ? inet_pton(AF_INET6, in + 5, a) == 1
		                                  : inet_pton(AF_INET, in, a) == 1)
			return 0;
		return lerr(l, "bad address literal %s", d);
	}
	const char *s = d;
	for (;;) {
		const char *e = strchr(s, '.');
		size_t n = e ? (size_t)(e - s) : strlen(s);
		if (n == 0) return lerr(l, "empty label in domain %s", d);
		if (n > 63) return lerr(l, "domain label longer than 63 octets in %s", d);
		int ascii = 1;
		for (size_t i = 0; i < n; i++)
			if ((unsigned char)s[i] >= 0x80) ascii = 0;
		if (ascii) {
			for (size_t i = 0; i < n; i++)
				if (!isalnum((unsigned char)s[i]) && s[i] != '-')
					return lerr(l, "domain %s: only letters, digits and '-' are allowed", d);
			if (s[0] == '-' || s[n - 1] == '-')
				return lerr(l, "domain %s: a label may not start or end with '-'", d);
		}
		if (!e) break;
		s = e + 1;
	}
	return 0;
}

/* addr-spec after the local part was read: '@' domain */
static int finish_addr(lex_t *l, mbox_t *m, char *local)
{
	if (*l->p != '@') {
		free(local);
		return lerr(l, "missing '@' in address");
	}
	l->p++;
	char *dom = parse_domain(l);
	if (!dom) { free(local); return -1; }
	m->local = local;
	m->domain = dom;
	return check_addr(l, m);
}

/* phrase = 1*word (obs-phrase: dots too); returns NULL if no words */
static char *parse_phrase(lex_t *l)
{
	buf_t b = {0};
	for (;;) {
		if (skip_cfws(l) < 0) break;
		if (*l->p == '.' && b.len) {     /* obs-phrase: "John Q. Public" */
			buf_addc(&b, '.');
			l->p++;
			continue;
		}
		if (*l->p != '"' && !is_atext((unsigned char)*l->p)) break;
		int q;
		char *w = get_word(l, 1, &q);
		if (!w) break;
		if (b.len) buf_addc(&b, ' ');
		buf_adds(&b, w);
		free(w);
	}
	if (*l->err || !b.len) { buf_free(&b); return NULL; }
	return buf_steal(&b);
}

static int parse_mailbox(lex_t *l, mbox_t *m)
{
	memset(m, 0, sizeof *m);
	const char *save = l->p;

	/* addr-spec on its own: local@domain followed by , ; or end */
	if (skip_cfws(l) < 0) return -1;
	if (*l->p != '<') {
		char *local = parse_local(l);
		if (!local && *l->err && !strpbrk(save, "<")) return -1;
		if (local && *l->p == '@') {
			if (finish_addr(l, m, local) < 0) return -1;
			if (skip_cfws(l) < 0) return -1;
			if (!*l->p || *l->p == ',' || *l->p == ';') return 0;
			free(m->local);
			free(m->domain);
			memset(m, 0, sizeof *m);
		} else {
			free(local);
		}
		l->p = save;
		l->err[0] = '\0';
	}

	/* name-addr: [phrase] '<' addr-spec '>' */
	m->name = parse_phrase(l);
	if (*l->err) return -1;
	if (skip_cfws(l) < 0) return -1;
	if (*l->p != '<') {
		if (m->name && *l->p == '@')
			return lerr(l, "put the address in <>, e.g. Name <user@example.org>");
		if (m->name) return lerr(l, "'%.60s' is not an address (expected name <user@domain>)", m->name);
		return lerr(l, "expected an address");
	}
	l->p++;
	if (skip_cfws(l) < 0) return -1;
	if (*l->p == '@') return lerr(l, "source routes (<@host:...>) are not supported");
	char *local = parse_local(l);
	if (!local) return lerr(l, "missing address inside <>");
	if (finish_addr(l, m, local) < 0) return -1;
	if (skip_cfws(l) < 0) return -1;
	if (*l->p != '>') return lerr(l, "missing '>' after address");
	l->p++;
	return skip_cfws(l);
}

static void entry_add(entry_t *e, mbox_t *m)
{
	e->m = xrealloc(e->m, sizeof *e->m * (size_t)(e->n + 1));
	e->m[e->n++] = *m;
}

/* address-list; groups allowed when allow_group. Empty list is fine. */
static int parse_alist(const char *s, alist_t *out, int allow_group, char *err, size_t errlen)
{
	lex_t l = {.p = s};
	memset(out, 0, sizeof *out);
	for (;;) {
		if (skip_cfws(&l) < 0) goto fail;
		if (!*l.p) break;
		if (*l.p == ',') { l.p++; continue; }

		entry_t e = {0};
		const char *save = l.p;
		char *ph = parse_phrase(&l);
		if (*l.err) { free(ph); goto fail; }
		if (skip_cfws(&l) < 0) { free(ph); goto fail; }
		if (ph && *l.p == ':') {
			if (!allow_group) { free(ph); lerr(&l, "a group (Name: ...;) is not allowed here"); goto fail; }
			l.p++;
			e.group = ph;
			for (;;) {
				if (skip_cfws(&l) < 0) break;
				if (*l.p == ';') { l.p++; break; }
				if (*l.p == ',') { l.p++; continue; }
				if (!*l.p) { lerr(&l, "group '%.60s' is missing its closing ';'", ph); break; }
				mbox_t m;
				if (parse_mailbox(&l, &m) < 0) {
					free(m.name); free(m.local); free(m.domain);
					break;
				}
				entry_add(&e, &m);
			}
		} else {
			free(ph);
			l.p = save;
			mbox_t m;
			if (parse_mailbox(&l, &m) < 0) {
				free(m.name); free(m.local); free(m.domain);
			} else {
				entry_add(&e, &m);
			}
		}
		out->e = xrealloc(out->e, sizeof *out->e * (size_t)(out->n + 1));
		out->e[out->n++] = e;
		if (*l.err) goto fail;
		if (skip_cfws(&l) < 0) goto fail;
		if (*l.p && *l.p != ',') {
			lerr(&l, "unexpected '%.20s' (separate addresses with ',')", l.p);
			goto fail;
		}
	}
	return 0;
fail:
	snprintf(err, errlen, "%s", l.err);
	alist_free(out);
	return -1;
}

static int alist_count(const alist_t *a)
{
	int n = 0;
	for (int i = 0; i < a->n; i++) n += a->e[i].n;
	return n;
}

static char *addr_str(const mbox_t *m)
{
	buf_t b = {0};
	if (dot_atom_ok(m->local)) {
		buf_adds(&b, m->local);
	} else {
		buf_addc(&b, '"');
		for (const char *p = m->local; *p; p++) {
			if (*p == '"' || *p == '\\') buf_addc(&b, '\\');
			buf_addc(&b, *p);
		}
		buf_addc(&b, '"');
	}
	buf_printf(&b, "@%s", m->domain);
	return buf_steal(&b);
}

/* ------------------------------------------------------------------ */
/* Header output with folding                                          */

typedef struct {
	buf_t *b;
	size_t col;     /* octets on the current physical line */
	int    any;     /* a piece has been written after the name */
} hw_t;

static void hw_start(hw_t *w, buf_t *b, const char *name)
{
	w->b = b;
	w->col = strlen(name) + 1;
	w->any = 0;
	buf_printf(b, "%s:", name);
}

/* Add a piece that must not be split, preceded by white space (a folding
 * point) unless glue is set. */
static void hw_piece(hw_t *w, const char *s, size_t n, int glue)
{
	if (!glue) {
		if (w->any && w->col + 1 + n > FOLD_AT) {
			buf_adds(w->b, "\r\n ");
			w->col = 1;
		} else {
			buf_addc(w->b, ' ');
			w->col++;
		}
	}
	buf_add(w->b, s, n);
	w->col += n;
	w->any = 1;
}

static void hw_end(hw_t *w) { buf_adds(w->b, "\r\n"); }

static const char b64chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void b64_add(buf_t *b, const unsigned char *s, size_t n)
{
	for (size_t i = 0; i < n; i += 3) {
		unsigned v = (unsigned)s[i] << 16;
		if (i + 1 < n) v |= (unsigned)s[i + 1] << 8;
		if (i + 2 < n) v |= s[i + 2];
		buf_addc(b, b64chars[v >> 18 & 63]);
		buf_addc(b, b64chars[v >> 12 & 63]);
		buf_addc(b, i + 1 < n ? b64chars[v >> 6 & 63] : '=');
		buf_addc(b, i + 2 < n ? b64chars[v & 63] : '=');
	}
}

/* RFC 2047 encoded-words (UTF-8, B), each <= 75 chars, split at character
 * boundaries; written as separate pieces so they can be folded. */
static void hw_encoded(hw_t *w, const char *s, int glue_first)
{
	size_t n = strlen(s), i = 0;
	int first = 1;
	while (i < n) {
		size_t take = 0;
		while (i + take < n) {
			size_t cl;
			utf8_next(s + i + take, n - i - take, &cl);
			if (take + cl > 45) break;          /* 45 octets -> 60 base64 chars */
			take += cl;
		}
		if (!take) take = 1;
		buf_t e = {0};
		buf_adds(&e, "=?UTF-8?B?");
		b64_add(&e, (const unsigned char *)s + i, take);
		buf_adds(&e, "?=");
		hw_piece(w, e.d, e.len, first && glue_first);
		buf_free(&e);
		i += take;
		first = 0;
	}
}

/* Unstructured text (Subject etc.): words separated by white space. */
static void hw_text(hw_t *w, const char *s, int utf8)
{
	if (!is_ascii(s) && !utf8) {
		hw_encoded(w, s, 0);
		return;
	}
	const char *p = s;
	while (*p) {
		while (*p == ' ' || *p == '\t') p++;
		const char *e = p;
		while (*e && *e != ' ' && *e != '\t') e++;
		if (e > p) hw_piece(w, p, (size_t)(e - p), 0);
		p = e;
	}
}

/* A display name: atoms if possible, else a quoted-string, or encoded-words
 * for non-ASCII text when UTF-8 headers are not in use. */
static void hw_phrase(hw_t *w, const char *name, int utf8)
{
	if (!is_ascii(name) && !utf8) {
		hw_encoded(w, name, 0);
		return;
	}
	int atoms = 1;
	for (const char *p = name; *p; p++)
		if (*p != ' ' && !is_atext((unsigned char)*p)) atoms = 0;
	if (atoms && strstr(name, "=?")) atoms = 0;    /* would look like an encoded-word */
	if (atoms) {
		hw_text(w, name, 1);
		return;
	}
	buf_t q = {0};
	buf_addc(&q, '"');
	for (const char *p = name; *p; p++) {
		if (*p == '"' || *p == '\\') buf_addc(&q, '\\');
		buf_addc(&q, *p);
	}
	buf_addc(&q, '"');
	hw_piece(w, q.d, q.len, 0);
	buf_free(&q);
}

static void hw_alist(hw_t *w, const alist_t *a, int utf8)
{
	int total = 0;
	for (int i = 0; i < a->n; i++) total += a->e[i].group ? 1 : a->e[i].n;
	int k = 0;
	for (int i = 0; i < a->n; i++) {
		const entry_t *e = &a->e[i];
		if (e->group) {
			hw_phrase(w, e->group, utf8);
			hw_piece(w, ":", 1, 1);
		}
		for (int j = 0; j < e->n; j++) {
			const mbox_t *m = &e->m[j];
			char *as = addr_str(m);
			buf_t p = {0};
			if (m->name && *m->name) {
				hw_phrase(w, m->name, utf8);
				buf_printf(&p, "<%s>", as);
			} else {
				buf_adds(&p, as);
			}
			int last_in_group = j == e->n - 1;
			if (!e->group ? k + 1 < total : !last_in_group) buf_addc(&p, ',');
			hw_piece(w, p.d, p.len, 0);
			buf_free(&p);
			free(as);
		}
		if (e->group) {
			hw_piece(w, k + 1 < total ? ";," : ";", k + 1 < total ? 2 : 1, 1);
			k++;
		} else {
			k += e->n;
		}
	}
}

/* ------------------------------------------------------------------ */
/* Date (RFC 5322 3.3)                                                 */

static const char *const DAYS[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
static const char *const MONS[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                   "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

const char *compose_date(time_t t)
{
	static char s[64];
	struct tm tm;
	localtime_r(&t, &tm);
#ifdef _WIN32
	struct tm u = tm;
	long off = (long)((_mkgmtime(&u) - t) / 60);   /* no tm_gmtoff */
#else
	long off = tm.tm_gmtoff / 60;
#endif
	char sign = off < 0 ? '-' : '+';
	if (off < 0) off = -off;
	snprintf(s, sizeof s, "%s, %d %s %d %02d:%02d:%02d %c%02ld%02ld", DAYS[tm.tm_wday], tm.tm_mday,
	         MONS[tm.tm_mon], tm.tm_year + 1900, tm.tm_hour, tm.tm_min, tm.tm_sec, sign, off / 60,
	         off % 60);
	return s;
}

static int check_date(const char *v, char *err, size_t errlen)
{
	lex_t l = {.p = v};
	int wday = -1;
	skip_cfws(&l);
	for (int i = 0; i < 7; i++)
		if (!strncasecmp(l.p, DAYS[i], 3) && !isalpha((unsigned char)l.p[3])) {
			wday = i;
			l.p += 3;
			skip_cfws(&l);
			if (*l.p != ',') goto bad;
			l.p++;
			break;
		}
	skip_cfws(&l);
	char *e;
	long day = strtol(l.p, &e, 10);
	if (e == l.p || e - l.p > 2 || day < 1 || day > 31) goto bad;
	l.p = e;
	skip_cfws(&l);
	int mon = -1;
	for (int i = 0; i < 12; i++)
		if (!strncasecmp(l.p, MONS[i], 3) && !isalpha((unsigned char)l.p[3])) mon = i;
	if (mon < 0) goto bad;
	l.p += 3;
	skip_cfws(&l);
	long year = strtol(l.p, &e, 10);
	if (e - l.p < 4 || year < 1900) goto bad;
	l.p = e;
	skip_cfws(&l);
	int hh, mm, ss = 0, n = 0;
	if (sscanf(l.p, "%2d:%2d%n", &hh, &mm, &n) != 2) goto bad;
	l.p += n;
	if (*l.p == ':') {
		if (sscanf(l.p, ":%2d%n", &ss, &n) != 1) goto bad;
		l.p += n;
	}
	if (hh > 23 || mm > 59 || ss > 60) goto bad;
	skip_cfws(&l);
	if (*l.p == '+' || *l.p == '-') {
		if (!isdigit((unsigned char)l.p[1]) || strspn(l.p + 1, "0123456789") != 4) goto bad;
		int z = atoi(l.p + 1);
		if (z % 100 > 59) goto bad;
		l.p += 5;
	} else {
		static const char *const obs[] = {"UT", "GMT", "EST", "EDT", "CST", "CDT",
		                                  "MST", "MDT", "PST", "PDT", NULL};
		int ok = 0;
		for (int i = 0; obs[i]; i++) {
			size_t k = strlen(obs[i]);
			if (!strncasecmp(l.p, obs[i], k) && !isalpha((unsigned char)l.p[k])) {
				l.p += k;
				ok = 1;
				break;
			}
		}
		if (!ok) goto bad;
	}
	skip_cfws(&l);
	if (*l.p) goto bad;

	struct tm tm = {.tm_year = (int)year - 1900, .tm_mon = mon, .tm_mday = (int)day, .tm_hour = 12};
	time_t t = timegm(&tm);
	struct tm chk;
	gmtime_r(&t, &chk);
	if (chk.tm_mday != day) {
		snprintf(err, errlen, "Date: %s %ld has no day %ld", MONS[mon], year, day);
		return -1;
	}
	if (wday >= 0 && chk.tm_wday != wday) {
		snprintf(err, errlen, "Date: %ld %s %ld is a %s, not a %s", day, MONS[mon], year,
		         DAYS[chk.tm_wday], DAYS[wday]);
		return -1;
	}
	return 0;
bad:
	snprintf(err, errlen, "Date: expected e.g. \"%s\"", compose_date(time(NULL)));
	return -1;
}

/* ------------------------------------------------------------------ */
/* Message-ID (RFC 5322 3.6.4)                                         */

/* One or more <id-left@id-right>, separated by white space. */
static int check_msgids(const char *v, int many, char *err, size_t errlen)
{
	lex_t l = {.p = v};
	int n = 0;
	for (;;) {
		if (skip_cfws(&l) < 0) break;
		if (!*l.p) break;
		if (*l.p != '<') goto bad;
		const char *e = strchr(l.p, '>');
		if (!e) goto bad;
		char *id = xstrndup(l.p + 1, (size_t)(e - l.p - 1));
		char *at = strrchr(id, '@');
		int ok = at && at > id;
		if (ok) {
			*at = '\0';
			const char *right = at + 1;
			ok = dot_atom_ok(id) &&
			     (dot_atom_ok(right) || (right[0] == '[' && right[strlen(right) - 1] == ']'));
		}
		free(id);
		if (!ok) goto bad;
		l.p = e + 1;
		n++;
	}
	if (n == 0 || (!many && n > 1)) goto bad;
	return 0;
bad:
	snprintf(err, errlen, "expected a message id like <unique@example.org>");
	return -1;
}

static void make_msgid(buf_t *b, const char *domain)
{
	unsigned char r[12];
	if (RAND_bytes(r, (int)sizeof r) != 1)   /* never expected: then unique enough */
		for (size_t i = 0; i < sizeof r; i++) r[i] = (unsigned char)(time(NULL) >> (8 * (i % 4)) ^ getpid() >> (i % 3) ^ i * 151);
	buf_printf(b, "<%llx.", (unsigned long long)time(NULL));
	for (size_t i = 0; i < sizeof r; i++) buf_printf(b, "%02x", r[i]);
	buf_printf(b, "@%s>", domain);
}

/* ------------------------------------------------------------------ */
/* Body (RFC 2045 6.7 quoted-printable)                                */

static void qp_line(buf_t *out, const char *s, size_t n)
{
	size_t col = 0;
	for (size_t i = 0; i < n; i++) {
		unsigned char c = (unsigned char)s[i];
		int last = i + 1 == n;
		int enc = c == '=' || c >= 0x7F || (c < 0x20 && c != '\t') ||
		          ((c == ' ' || c == '\t') && last) ||
		          (col == 0 && c == '.') ||                       /* safe for broken relays */
		          (col == 0 && c == 'F' && n - i >= 5 && !memcmp(s + i, "From ", 5));
		size_t w = enc ? 3 : 1;
		if (col + w > 75) {              /* 75 + "=" soft break = 76 */
			buf_adds(out, "=\r\n");
			col = 0;
			if (c == '.') enc = 1, w = 3;
		}
		if (enc) buf_printf(out, "=%02X", c);
		else buf_addc(out, (char)c);
		col += w;
	}
	buf_adds(out, "\r\n");
}

/* ------------------------------------------------------------------ */
/* Building the message                                                */

typedef struct {
	char *name;
	char *value;
	int   line;
} field_t;

static const char *const UNIQUE[] = {"From", "Sender", "Reply-To", "To", "Cc", "Bcc", "Subject",
                                     "Date", "Message-ID", "In-Reply-To", "References", NULL};
static const char *const GENERATED[] = {"MIME-Version", "Content-Type", "Content-Transfer-Encoding",
                                        "Content-Disposition", "Content-ID", "User-Agent",
                                        "Return-Path", "Received", NULL};

/* ------------------------------------------------------------------ */
/* Attachments (RFC 2045/2046 multipart/mixed, RFC 2183 disposition,   */
/* RFC 2231 file name parameters)                                      */

#define ATTACH_MAX (100u * 1024 * 1024)   /* all attachments together */

typedef struct {
	char       *name;      /* file name shown to the receiver (UTF-8) */
	const char *type;
	buf_t       data;
} att_t;

static const char *mime_type_of(const char *name)
{
	static const struct { const char *ext, *type; } t[] = {
		{"pdf", "application/pdf"}, {"txt", "text/plain"}, {"text", "text/plain"},
		{"csv", "text/csv"}, {"html", "text/html"}, {"htm", "text/html"},
		{"md", "text/markdown"}, {"ics", "text/calendar"}, {"vcf", "text/vcard"},
		{"xml", "application/xml"}, {"json", "application/json"},
		{"jpg", "image/jpeg"}, {"jpeg", "image/jpeg"}, {"png", "image/png"},
		{"gif", "image/gif"}, {"webp", "image/webp"}, {"svg", "image/svg+xml"},
		{"heic", "image/heic"}, {"tif", "image/tiff"}, {"tiff", "image/tiff"},
		{"mp3", "audio/mpeg"}, {"ogg", "audio/ogg"}, {"wav", "audio/wav"},
		{"flac", "audio/flac"}, {"mp4", "video/mp4"}, {"mov", "video/quicktime"},
		{"webm", "video/webm"}, {"zip", "application/zip"}, {"gz", "application/gzip"},
		{"tgz", "application/gzip"}, {"xz", "application/x-xz"}, {"7z", "application/x-7z-compressed"},
		{"tar", "application/x-tar"}, {"odt", "application/vnd.oasis.opendocument.text"},
		{"ods", "application/vnd.oasis.opendocument.spreadsheet"},
		{"odp", "application/vnd.oasis.opendocument.presentation"},
		{"doc", "application/msword"}, {"xls", "application/vnd.ms-excel"},
		{"ppt", "application/vnd.ms-powerpoint"},
		{"docx", "application/vnd.openxmlformats-officedocument.wordprocessingml.document"},
		{"xlsx", "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"},
		{"pptx", "application/vnd.openxmlformats-officedocument.presentationml.presentation"},
		{"eml", "message/rfc822"},
	};
	const char *dot = strrchr(name, '.');
	if (dot && dot[1])
		for (size_t i = 0; i < sizeof t / sizeof *t; i++)
			if (!strcasecmp(dot + 1, t[i].ext)) {
				/* message/rfc822 may not be base64 (RFC 2046 5.2.1): send it as a file */
				return strcmp(t[i].type, "message/rfc822") ? t[i].type : "application/octet-stream";
			}
	return "application/octet-stream";
}

/* Read the file named in an Attach: line ("~/" = home). */
static int read_attachment(const char *v, att_t *a, size_t *total, char *err, size_t errlen)
{
	char path[4096];
	const char *home = getenv("HOME");
	size_t vl = strlen(v);
	if (vl >= 2 && ((v[0] == '"' && v[vl - 1] == '"') || (v[0] == '<' && v[vl - 1] == '>'))) {
		v++;
		vl -= 2;
	}
	if (v[0] == '~' && (vl == 1 || v[1] == '/') && home)
		snprintf(path, sizeof path, "%s%.*s", home, (int)vl - 1, v + 1);
	else
		snprintf(path, sizeof path, "%.*s", (int)vl, v);
	if (!*path) {
		snprintf(err, errlen, "Attach: give the path of a file");
		return -1;
	}
	struct stat st;
	if (stat(path, &st) != 0) {
		snprintf(err, errlen, "Attach: %.300s: %s", path, strerror(errno));
		return -1;
	}
	if (!S_ISREG(st.st_mode)) {
		snprintf(err, errlen, "Attach: %.300s is not a file", path);
		return -1;
	}
	if ((unsigned long long)st.st_size + *total > ATTACH_MAX) {
		snprintf(err, errlen, "Attach: the attachments are larger than %u MB together",
		         ATTACH_MAX / (1024 * 1024));
		return -1;
	}
	FILE *f = fopen(path, "rb");
	if (!f) {
		snprintf(err, errlen, "Attach: %.300s: %s", path, strerror(errno));
		return -1;
	}
	char tmp[65536];
	size_t got;
	while ((got = fread(tmp, 1, sizeof tmp, f)) > 0) buf_add(&a->data, tmp, got);
	int bad = ferror(f);
	fclose(f);
	if (bad) {
		snprintf(err, errlen, "Attach: cannot read %.300s", path);
		buf_free(&a->data);
		return -1;
	}
	*total += a->data.len;
	const char *base = strrchr(path, '/');
	a->name = xstrdup(base ? base + 1 : path);
	a->type = mime_type_of(a->name);
	return 0;
}

/* ; name="..." as RFC 2231 when it is long or not plain ASCII:
 * name*0*=utf-8''%C3%A9t..; name*1*=... */
static void name_param(buf_t *b, const char *param, const char *v)
{
	int plain = strlen(v) <= 60;
	for (const unsigned char *p = (const unsigned char *)v; *p && plain; p++)
		if (*p < 0x20 || *p >= 0x7F || *p == '"' || *p == '\\') plain = 0;
	if (plain) {
		buf_printf(b, ";\r\n\t%s=\"%s\"", param, v);
		return;
	}
	static const char keep[] = "!#$&+-.^_`|~";
	buf_t enc = {0};
	for (const unsigned char *p = (const unsigned char *)v; *p; p++) {
		if (isalnum(*p) || strchr(keep, *p)) buf_addc(&enc, (char)*p);
		else buf_printf(&enc, "%%%02X", *p);
	}
	int part = 0;
	for (size_t i = 0; i < enc.len; part++) {
		size_t n = enc.len - i > 60 ? 60 : enc.len - i;
		while (n > 1 && i + n < enc.len && (enc.d[i + n - 1] == '%' || enc.d[i + n - 2] == '%'))
			n--;                                      /* do not split a %XX */
		buf_printf(b, ";\r\n\t%s*%d*=%s%.*s", param, part, part ? "" : "utf-8''", (int)n, enc.d + i);
		i += n;
	}
	buf_free(&enc);
}

static void write_attachment(buf_t *b, const att_t *a, const char *boundary)
{
	buf_printf(b, "\r\n--%s\r\nContent-Type: %s", boundary, a->type);
	name_param(b, "name", a->name);
	buf_adds(b, "\r\nContent-Disposition: attachment");
	name_param(b, "filename", a->name);
	buf_adds(b, "\r\nContent-Transfer-Encoding: base64\r\n\r\n");
	const unsigned char *d = (const unsigned char *)a->data.d;
	for (size_t i = 0; i < a->data.len; i += 57) {
		b64_add(b, d + i, a->data.len - i < 57 ? a->data.len - i : 57);
		buf_adds(b, "\r\n");
	}
}

static int in_list(const char *const *list, const char *s)
{
	for (int i = 0; list[i]; i++)
		if (!strcasecmp(list[i], s)) return 1;
	return 0;
}

static field_t *find(field_t *f, int n, const char *name)
{
	for (int i = 0; i < n; i++)
		if (!strcasecmp(f[i].name, name)) return &f[i];
	return NULL;
}

static int fail(compose_out *o, int line, const char *fmt, ...) __attribute__((format(MCM_PRINTF, 3, 4)));
static int fail(compose_out *o, int line, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(o->err, sizeof o->err, fmt, ap);
	va_end(ap);
	o->err_line = line;
	return -1;
}

static void add_rcpt(compose_out *o, const alist_t *a)
{
	for (int i = 0; i < a->n; i++)
		for (int j = 0; j < a->e[i].n; j++) {
			char *s = addr_str(&a->e[i].m[j]);
			int dup = 0;
			for (int k = 0; k < o->nrcpt; k++)
				if (!strcasecmp(o->rcpt[k], s)) dup = 1;
			if (dup) { free(s); continue; }
			o->rcpt = xrealloc(o->rcpt, sizeof *o->rcpt * (size_t)(o->nrcpt + 1));
			o->rcpt[o->nrcpt++] = s;
		}
}

static int alist_utf8(const alist_t *a)
{
	for (int i = 0; i < a->n; i++)
		for (int j = 0; j < a->e[i].n; j++)
			if (!is_ascii(a->e[i].m[j].local) || !is_ascii(a->e[i].m[j].domain)) return 1;
	return 0;
}

void compose_out_free(compose_out *o)
{
	buf_free(&o->msg);
	buf_free(&o->copy);
	free(o->mail_from);
	for (int i = 0; i < o->nrcpt; i++) free(o->rcpt[i]);
	free(o->rcpt);
	free(o->subject);
	memset(o, 0, sizeof *o);
	o->err_line = -1;
}

#define NADDR 6
static const char *const ADDR_HDRS[NADDR] = {"From", "Sender", "Reply-To", "To", "Cc", "Bcc"};

int compose_build(const char *text, size_t len, const compose_opts *o, compose_out *out)
{
	memset(out, 0, sizeof *out);
	out->err_line = -1;

	/* ---- split header block / body ---- */
	field_t *f = NULL;
	int nf = 0, line = 0, rc = -1;
	size_t pos = 0;
	alist_t al[NADDR] = {{0}};
	int parsed[NADDR] = {0};
	int body_line = -1;
	att_t *atts = NULL;              /* files from Attach: lines */
	int natt = 0;
	size_t att_total = 0;
	buf_t fbody = {0};               /* body after space-stuffing */

	while (pos < len) {
		const char *s = text + pos;
		const char *nl = memchr(s, '\n', len - pos);
		size_t n = nl ? (size_t)(nl - s) : len - pos;
		pos += n + (nl ? 1 : 0);
		if (n && s[n - 1] == '\r') n--;

		if (n == 0) { body_line = line + 1; break; }
		if (!valid_text(s, n)) {
			fail(out, line, "line %d: invalid character in header", line + 1);
			goto done;
		}
		if (s[0] == ' ' || s[0] == '\t') {
			if (!nf) { fail(out, line, "line %d: continuation line without a header", line + 1); goto done; }
			buf_t b = {0};
			buf_adds(&b, f[nf - 1].value);
			buf_addc(&b, ' ');
			buf_add(&b, s, n);
			free(f[nf - 1].value);
			f[nf - 1].value = str_trim(buf_steal(&b));
		} else {
			const char *c = memchr(s, ':', n);
			size_t k = 0;
			while (k < n && s[k] > 32 && s[k] < 127 && s[k] != ':') k++;
			if (!c || k == 0 || s + k != c) {
				fail(out, line, "line %d: not a header (Name: value); leave an empty line "
				                "before the message text", line + 1);
				goto done;
			}
			f = xrealloc(f, sizeof *f * (size_t)(nf + 1));
			f[nf].name = xstrndup(s, k);
			char *v = xstrndup(c + 1, n - k - 1);
			for (char *t = v; *t; t++)
				if (*t == '\t') *t = ' ';
			f[nf].value = str_trim(v);
			f[nf].line = line;
			nf++;
		}
		line++;
	}

	/* ---- check fields ---- */
	for (int i = 0; i < nf; i++) {
		if (in_list(GENERATED, f[i].name)) {
			fail(out, f[i].line, "%s: is set by mcm; remove that line", f[i].name);
			goto done;
		}
		if (in_list(UNIQUE, f[i].name))
			for (int j = 0; j < i; j++)
				if (!strcasecmp(f[i].name, f[j].name)) {
					fail(out, f[i].line, "%s: appears twice (lines %d and %d)", f[i].name,
					     f[j].line + 1, f[i].line + 1);
					goto done;
				}
	}

	int need_utf8 = 0;
	for (int h = 0; h < NADDR; h++) {
		field_t *fd = find(f, nf, ADDR_HDRS[h]);
		if (!fd || !*fd->value) continue;
		char err[256];
		/* RFC 6854: groups are allowed in From: and Sender: as well */
		if (parse_alist(fd->value, &al[h], 1, err, sizeof err) < 0) {
			if (o->draft) continue;          /* kept as plain text */
			fail(out, fd->line, "%s: %s", fd->name, err);
			goto done;
		}
		parsed[h] = 1;
		if (alist_utf8(&al[h])) need_utf8 = 1;
	}
	alist_t *from = &al[0], *sender = &al[1], *to = &al[3], *cc = &al[4], *bcc = &al[5];
	field_t *ff = find(f, nf, "From");

	if (!o->draft) {
		if (!ff || !*ff->value) {
			fail(out, ff ? ff->line : 0, "From: is required (your address)");
			goto done;
		}
		int nfrom = alist_count(from);
		int from_group = 0;
		for (int i = 0; i < from->n; i++)
			if (from->e[i].group) from_group = 1;
		if (find(f, nf, "Sender")) {
			if (alist_count(sender) != 1 || sender->e[0].group) {
				fail(out, find(f, nf, "Sender")->line, "Sender: must be exactly one address");
				goto done;
			}
		} else if (nfrom != 1 || from_group) {
			/* RFC 5322 3.6.2 / RFC 6854 */
			fail(out, ff->line, "From: with %s needs a Sender: line with your own address",
			     from_group ? "a group" : "several addresses");
			goto done;
		}
		if (alist_count(to) + alist_count(cc) + alist_count(bcc) == 0) {
			field_t *t = find(f, nf, "To");
			fail(out, t ? t->line : 0, "no recipients: fill in To:, Cc: or Bcc:");
			goto done;
		}
		if (need_utf8 && !o->smtputf8) {
			fail(out, -1, "an address contains non-ASCII characters, but the server "
			              "does not support SMTPUTF8 (RFC 6531)");
			goto done;
		}
	}
	int utf8 = need_utf8 && o->smtputf8;

	char date[128];
	field_t *fdate = find(f, nf, "Date");
	if (!fdate || !*fdate->value || (o->auto_date && !strcmp(fdate->value, o->auto_date))) {
		snprintf(date, sizeof date, "%s", compose_date(time(NULL)));
	} else {
		char err[256];
		if (check_date(fdate->value, err, sizeof err) < 0) {
			fail(out, fdate->line, "%s", err);
			goto done;
		}
		snprintf(date, sizeof date, "%s", fdate->value);
	}

	static const char *const IDS[] = {"Message-ID", "In-Reply-To", "References"};
	for (int i = 0; i < 3; i++) {
		field_t *fd = find(f, nf, IDS[i]);
		char err[256];
		if (fd && *fd->value && check_msgids(fd->value, i > 0, err, sizeof err) < 0) {
			fail(out, fd->line, "%s: %s", fd->name, err);
			goto done;
		}
	}

	field_t *fsub = find(f, nf, "Subject");
	out->no_subject = !fsub || !*fsub->value;
	out->subject = xstrdup(fsub ? fsub->value : "");

	/* ---- body ---- */
	const char *body = body_line >= 0 ? text + pos : "";
	size_t blen = body_line >= 0 ? len - pos : 0;
	while (blen && (body[blen - 1] == '\n' || body[blen - 1] == '\r')) blen--;

	/* format=flowed (RFC 3676): a line ending in a space continues on the
	 * next line (the editor's soft breaks); other lines are hard breaks.
	 * Lines starting with a space or "From " get one extra space
	 * (space-stuffing), which the receiver removes. '>' starts a quote. */
	for (size_t i = 0; i < blen;) {
		const char *e = memchr(body + i, '\n', blen - i);
		size_t n = e ? (size_t)(e - (body + i)) : blen - i;
		if (n && body[i + n - 1] == '\r') n--;
		if (n && (body[i] == ' ' || (n >= 5 && !memcmp(body + i, "From ", 5))))
			buf_addc(&fbody, ' ');
		buf_add(&fbody, body + i, n);
		buf_addc(&fbody, '\n');
		i += (size_t)((e ? e + 1 : body + blen) - (body + i));
	}
	if (fbody.len) fbody.len--;                       /* last '\n' */
	body = buf_cstr(&fbody);
	blen = fbody.len;
	int ascii = 1, ctl = 0;
	size_t maxl = 0, cur = 0;
	for (size_t i = 0; i < blen; i++) {
		unsigned char c = (unsigned char)body[i];
		if (c == '\n') { cur = 0; continue; }
		if (c >= 0x80) ascii = 0;
		else if ((c < 0x20 && c != '\t' && c != '\r') || c == 0x7F) ctl = 1;
		if (++cur > maxl) maxl = cur;
	}
	{
		size_t i = 0;
		int bl = body_line;
		while (i < blen) {
			const char *e = memchr(body + i, '\n', blen - i);
			size_t n = e ? (size_t)(e - (body + i)) : blen - i;
			if (!valid_text(body + i, n) && !ctl) {
				fail(out, bl, "line %d: not valid UTF-8 text", bl + 1);
				goto done;
			}
			i += n + 1;
			bl++;
		}
	}
	const char *cte;
	if (ascii && !ctl && maxl <= LINE_MAX_) cte = "7bit";
	else if (!ctl && maxl <= LINE_MAX_ && (o->eightbit || utf8)) cte = "8bit";
	else cte = "quoted-printable";
	out->eightbit = !strcmp(cte, "8bit");
	out->smtputf8 = utf8;

	/* ---- envelope ---- */
	if (!o->draft) {
		const alist_t *env = find(f, nf, "Sender") ? sender : from;
		out->mail_from = addr_str(&env->e[0].m[0]);
		add_rcpt(out, to);
		add_rcpt(out, cc);
		add_rcpt(out, bcc);
	}

	/* ---- attachments: Attach: lines name the files ---- */
	for (int i = 0; i < nf; i++) {
		if (strcasecmp(f[i].name, "Attach")) continue;
		atts = xrealloc(atts, sizeof *atts * (size_t)(natt + 1));
		memset(&atts[natt], 0, sizeof *atts);
		char err[600];
		if (read_attachment(f[i].value, &atts[natt], &att_total, err, sizeof err) != 0) {
			fail(out, f[i].line, "%s", err);
			goto done;
		}
		natt++;
	}
	out->nattach = natt;
	out->attach_bytes = att_total;
	char boundary[64] = "";
	if (natt) {
		unsigned char r[12];
		if (RAND_bytes(r, (int)sizeof r) != 1)   /* never expected: then unique enough */
			for (size_t i = 0; i < sizeof r; i++) r[i] = (unsigned char)(time(NULL) >> (8 * (i % 4)) ^ getpid() >> (i % 3) ^ i * 151);
		int k = snprintf(boundary, sizeof boundary, "=_mcm_");
		for (size_t i = 0; i < sizeof r; i++) k += snprintf(boundary + k, sizeof boundary - (size_t)k, "%02x", r[i]);
	}

	/* ---- write header twice: for sending (no Bcc) and for the copy ---- */
	const char *domain = "localhost";
	if (sender->n && sender->e[0].n) domain = sender->e[0].m[0].domain;
	else if (from->n && from->e[0].n) domain = from->e[0].m[0].domain;
	if (!is_ascii(domain) || *domain == '[') domain = "mcm.invalid";
	buf_t mid = {0};
	field_t *fmid = find(f, nf, "Message-ID");
	if (fmid && *fmid->value) buf_adds(&mid, fmid->value);
	else make_msgid(&mid, domain);

	for (int pass = 0; pass < 2; pass++) {
		buf_t *b = pass ? &out->copy : &out->msg;
		hw_t w;
		hw_start(&w, b, "Date");
		hw_text(&w, date, 1);
		hw_end(&w);
		for (int h = 0; h < NADDR; h++) {
			if (h == 5 && !pass) continue;          /* Bcc only in the copy */
			field_t *fd = find(f, nf, ADDR_HDRS[h]);
			if (!fd || !*fd->value) continue;
			hw_start(&w, b, ADDR_HDRS[h]);
			if (parsed[h]) hw_alist(&w, &al[h], utf8);
			else hw_text(&w, fd->value, utf8);
			hw_end(&w);
		}
		if (fsub && *fsub->value) {
			hw_start(&w, b, "Subject");
			hw_text(&w, fsub->value, utf8);
			hw_end(&w);
		}
		hw_start(&w, b, "Message-ID");
		hw_text(&w, mid.d, 1);
		hw_end(&w);
		for (int i = 1; i < 3; i++) {
			field_t *fd = find(f, nf, IDS[i]);
			if (!fd || !*fd->value) continue;
			hw_start(&w, b, IDS[i]);
			hw_text(&w, fd->value, 1);
			hw_end(&w);
		}
		for (int i = 0; i < nf; i++) {         /* other headers, in the user's order */
			int known = in_list(UNIQUE, f[i].name) || !strcasecmp(f[i].name, "Attach");
			if (known || !*f[i].value) continue;
			hw_start(&w, b, f[i].name);
			hw_text(&w, f[i].value, utf8);
			hw_end(&w);
		}
		hw_start(&w, b, "User-Agent");
		hw_text(&w, o->user_agent, 1);
		hw_end(&w);
		buf_adds(b, "MIME-Version: 1.0\r\n");
		if (natt) {
			buf_printf(b, "Content-Type: multipart/mixed;\r\n\tboundary=\"%s\"\r\n", boundary);
			if (!strcmp(cte, "8bit")) buf_adds(b, "Content-Transfer-Encoding: 8bit\r\n");
			buf_printf(b, "\r\nThis is a message in MIME format with %d attachment%s.\r\n"
			              "\r\n--%s\r\n", natt, natt == 1 ? "" : "s", boundary);
		}
		buf_printf(b, "Content-Type: text/plain; charset=%s; format=flowed\r\n",
		           ascii ? "us-ascii" : "utf-8");
		buf_printf(b, "Content-Transfer-Encoding: %s\r\n\r\n", cte);

		/* body */
		size_t i = 0;
		while (i < blen) {
			const char *e = memchr(body + i, '\n', blen - i);
			size_t n = e ? (size_t)(e - (body + i)) : blen - i;
			size_t m = n;
			if (m && body[i + m - 1] == '\r') m--;
			if (!strcmp(cte, "quoted-printable")) {
				qp_line(b, body + i, m);
			} else {
				buf_add(b, body + i, m);
				buf_adds(b, "\r\n");
			}
			i += n + 1;
		}
		if (blen == 0) buf_adds(b, "\r\n");
		for (int k = 0; k < natt; k++) write_attachment(b, &atts[k], boundary);
		if (natt) buf_printf(b, "\r\n--%s--\r\n", boundary);
	}
	buf_free(&mid);

	/* RFC 5322 2.1.1: no line may exceed 998 octets */
	const char *p = out->msg.d, *end = out->msg.d + out->msg.len;
	while (p < end) {
		const char *e = memchr(p, '\n', (size_t)(end - p));
		if (!e) e = end;
		if (e - p > LINE_MAX_ + 1) {
			fail(out, -1, "a header line is longer than 998 octets and cannot be folded");
			goto done;
		}
		p = e + 1;
	}
	rc = 0;

done:
	for (int k = 0; k < natt; k++) {
		free(atts[k].name);
		buf_free(&atts[k].data);
	}
	free(atts);
	buf_free(&fbody);
	for (int h = 0; h < NADDR; h++) alist_free(&al[h]);
	for (int i = 0; i < nf; i++) {
		free(f[i].name);
		free(f[i].value);
	}
	free(f);
	if (rc != 0) {
		char err[512];
		int el = out->err_line;
		memcpy(err, out->err, sizeof err);
		compose_out_free(out);
		memcpy(out->err, err, sizeof err);
		out->err_line = el;
	}
	return rc;
}

/* ------------------------------------------------------------------ */
/* Helpers for reply / forward                                         */

char *compose_addrs(const char *raw, const char *skip)
{
	if (!raw) return NULL;
	alist_t a;
	char err[256];
	if (parse_alist(raw, &a, 1, err, sizeof err) < 0) return NULL;
	buf_t b = {0};
	for (int i = 0; i < a.n; i++)
		for (int j = 0; j < a.e[i].n; j++) {
			mbox_t *m = &a.e[i].m[j];
			char *as = addr_str(m);
			if (skip && !strcasecmp(as, skip)) { free(as); continue; }
			if (b.len) buf_adds(&b, ", ");
			if (m->name && *m->name) {
				int atoms = 1;
				for (const char *p = m->name; *p; p++)
					if (*p != ' ' && !is_atext((unsigned char)*p)) atoms = 0;
				if (atoms) buf_adds(&b, m->name);
				else {
					buf_addc(&b, '"');
					for (const char *p = m->name; *p; p++) {
						if (*p == '"' || *p == '\\') buf_addc(&b, '\\');
						buf_addc(&b, *p);
					}
					buf_addc(&b, '"');
				}
				buf_printf(&b, " <%s>", as);
			} else {
				buf_adds(&b, as);
			}
			free(as);
		}
	alist_free(&a);
	if (!b.len) { buf_free(&b); return NULL; }
	return buf_steal(&b);
}

char *compose_first_addr(const char *raw)
{
	if (!raw) return NULL;
	alist_t a;
	char err[256];
	if (parse_alist(raw, &a, 1, err, sizeof err) < 0) return NULL;
	char *r = NULL;
	for (int i = 0; i < a.n && !r; i++)
		if (a.e[i].n) r = addr_str(&a.e[i].m[0]);
	alist_free(&a);
	return r;
}
