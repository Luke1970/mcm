/* util.c - small helpers: allocation, growable buffers, UTF-8 */
#include "util.h"

volatile sig_atomic_t quit_requested;

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void oom(void)
{
	fputs("emsg: out of memory\n", stderr);
	abort();
}

void *xmalloc(size_t n)
{
	void *p = malloc(n ? n : 1);
	if (!p) oom();
	return p;
}

void *xcalloc(size_t n, size_t sz)
{
	void *p = calloc(n ? n : 1, sz ? sz : 1);
	if (!p) oom();
	return p;
}

void *xrealloc(void *p, size_t n)
{
	p = realloc(p, n ? n : 1);
	if (!p) oom();
	return p;
}

char *xstrdup(const char *s)
{
	return xstrndup(s, strlen(s));
}

char *xstrndup(const char *s, size_t n)
{
	char *p = xmalloc(n + 1);
	memcpy(p, s, n);
	p[n] = '\0';
	return p;
}

static void buf_reserve(buf_t *b, size_t extra)
{
	if (b->len + extra + 1 <= b->cap) return;
	size_t cap = b->cap ? b->cap : 64;
	while (cap < b->len + extra + 1) cap *= 2;
	b->d = xrealloc(b->d, cap);
	b->cap = cap;
}

void buf_add(buf_t *b, const void *p, size_t n)
{
	buf_reserve(b, n);
	if (n) memcpy(b->d + b->len, p, n);
	b->len += n;
	b->d[b->len] = '\0';
}

void buf_adds(buf_t *b, const char *s) { buf_add(b, s, strlen(s)); }
void buf_addc(buf_t *b, char c) { buf_add(b, &c, 1); }

void buf_printf(buf_t *b, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(NULL, 0, fmt, ap);
	va_end(ap);
	if (n < 0) return;
	buf_reserve(b, (size_t)n);
	va_start(ap, fmt);
	vsnprintf(b->d + b->len, (size_t)n + 1, fmt, ap);
	va_end(ap);
	b->len += (size_t)n;
}

char *buf_cstr(buf_t *b)
{
	buf_reserve(b, 0);
	b->d[b->len] = '\0';
	return b->d;
}

char *buf_steal(buf_t *b)
{
	char *s = buf_cstr(b);
	b->d = NULL;
	b->len = b->cap = 0;
	return s;
}

void buf_free(buf_t *b)
{
	free(b->d);
	b->d = NULL;
	b->len = b->cap = 0;
}

void utf8_put(buf_t *b, uint32_t cp)
{
	char t[4];
	if ((cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) cp = 0xFFFD;   /* not a character */
	if (cp < 0x80) {
		buf_addc(b, (char)cp);
	} else if (cp < 0x800) {
		t[0] = (char)(0xC0 | (cp >> 6));
		t[1] = (char)(0x80 | (cp & 0x3F));
		buf_add(b, t, 2);
	} else if (cp < 0x10000) {
		t[0] = (char)(0xE0 | (cp >> 12));
		t[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
		t[2] = (char)(0x80 | (cp & 0x3F));
		buf_add(b, t, 3);
	} else if (cp <= 0x10FFFF) {
		t[0] = (char)(0xF0 | (cp >> 18));
		t[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
		t[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
		t[3] = (char)(0x80 | (cp & 0x3F));
		buf_add(b, t, 4);
	}
}

size_t utf8_to_wcs(const char *s, size_t len, wchar_t **out)
{
	const unsigned char *u = (const unsigned char *)s;
	wchar_t *w = xmalloc((len + 1) * sizeof(wchar_t));
	size_t i = 0, n = 0;

	while (i < len) {
		unsigned c = u[i];
		uint32_t cp, min;
		int extra;

		if (c < 0x80)                { cp = c;        extra = 0; min = 0; }
		else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; extra = 1; min = 0x80; }
		else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra = 2; min = 0x800; }
		else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; extra = 3; min = 0x10000; }
		else { w[n++] = 0xFFFD; i++; continue; }

		int ok = 1;
		for (int k = 1; k <= extra; k++) {
			if (i + k >= len || (u[i + k] & 0xC0) != 0x80) { ok = 0; break; }
			cp = (cp << 6) | (u[i + k] & 0x3F);
		}
		if (!ok || cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
			w[n++] = 0xFFFD;
			i++;
			continue;
		}
#if WCHAR_MAX <= 0xFFFF
		if (cp > 0xFFFF) cp = 0xFFFD;      /* Windows: one wchar_t holds only up to U+FFFF */
#endif
		w[n++] = (wchar_t)cp;
		i += (size_t)extra + 1;
	}
	w[n] = 0;
	*out = w;
	return n;
}

wchar_t utf8_next(const char *s, size_t len, size_t *clen)
{
	const unsigned char *u = (const unsigned char *)s;
	unsigned c = u[0];
	uint32_t cp, min;
	size_t extra;

	*clen = 1;
	if (c < 0x80) return (wchar_t)c;
	if ((c & 0xE0) == 0xC0)      { cp = c & 0x1F; extra = 1; min = 0x80; }
	else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra = 2; min = 0x800; }
	else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; extra = 3; min = 0x10000; }
	else return 0xFFFD;
	if (extra >= len) return 0xFFFD;
	for (size_t k = 1; k <= extra; k++) {
		if ((u[k] & 0xC0) != 0x80) return 0xFFFD;
		cp = (cp << 6) | (u[k] & 0x3F);
	}
	if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return 0xFFFD;
	*clen = extra + 1;
#if WCHAR_MAX <= 0xFFFF
	if (cp > 0xFFFF) return 0xFFFD;    /* valid, but does not fit (*clen tells it apart) */
#endif
	return (wchar_t)cp;
}

int utf16_join(uint32_t *cp)
{
	static uint32_t hi;
	if (*cp >= 0xD800 && *cp < 0xDC00) {
		hi = *cp;
		return 0;
	}
	if (*cp >= 0xDC00 && *cp < 0xE000) {
		*cp = hi ? 0x10000 + ((hi - 0xD800) << 10) + (*cp - 0xDC00) : 0xFFFD;
		hi = 0;
		return 1;
	}
	hi = 0;
	return 1;
}

/* Windows-1252 characters for the bytes 0x80-0x9F (0: not defined). */
static const uint16_t cp1252[32] = {
	0x20AC, 0, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
	0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0, 0x017D, 0,
	0, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
	0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0, 0x017E, 0x0178,
};

uint32_t cp1252_char(uint32_t c)
{
	if (c >= 0x80 && c < 0xA0) return cp1252[c - 0x80];
	return c;
}

char *utf8_clean(const char *s, size_t n, size_t *outlen)
{
	buf_t b = {0};
	const unsigned char *u = (const unsigned char *)s;
	size_t i = 0;
	while (i < n) {
		unsigned c = u[i];
		if (c < 0x80) {
			if (c == '\n' || c == '\t' || (c >= 0x20 && c != 0x7F)) buf_addc(&b, (char)c);
			i++;
			continue;
		}
		/* decode one sequence (as utf8_next, without the wchar_t limit) */
		uint32_t cp = 0, min = 0;
		size_t extra = 0, k;
		if ((c & 0xE0) == 0xC0)      { cp = c & 0x1F; extra = 1; min = 0x80; }
		else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra = 2; min = 0x800; }
		else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; extra = 3; min = 0x10000; }
		int ok = extra > 0;
		for (k = 1; ok && k <= extra; k++) {
			if (i + k >= n || (u[i + k] & 0xC0) != 0x80) ok = 0;
			else cp = (cp << 6) | (u[i + k] & 0x3F);
		}
		if (ok && (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))) ok = 0;
		if (!ok) {
			cp = cp1252_char(c);      /* a stray byte: most likely Windows-1252 */
			i++;
		} else {
			cp = cp1252_char(cp);     /* C1 controls meant as Windows-1252 */
			i += extra + 1;
		}
		if (cp) utf8_put(&b, cp);
	}
	if (outlen) *outlen = b.len;
	if (!b.d) return xstrdup("");
	return buf_steal(&b);
}

char *str_trim(char *s)
{
	char *p = s;
	while (*p && isspace((unsigned char)*p)) p++;
	if (p != s) memmove(s, p, strlen(p) + 1);   /* in place: pointer stays freeable */
	size_t n = strlen(s);
	while (n && isspace((unsigned char)s[n - 1])) s[--n] = '\0';
	return s;
}

void str_lower(char *s)
{
	for (; *s; s++) *s = (char)tolower((unsigned char)*s);
}
