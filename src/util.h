/* util.h - small helpers: allocation, growable buffers, UTF-8 */
#ifndef MCM_UTIL_H
#define MCM_UTIL_H

#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

/* printf-style checking: MinGW's C99 printf is "gnu_printf" (compat.h) */
#ifndef MCM_PRINTF
#define MCM_PRINTF printf
#endif

/* Set by Ctrl-C (SIGINT): network waits stop, and mcm quits at the next
 * key read (ui_getch). */
extern volatile sig_atomic_t quit_requested;

typedef struct {
	char  *d;
	size_t len, cap;
} buf_t;

void *xmalloc(size_t n);
void *xcalloc(size_t n, size_t sz);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);   /* binary safe, always NUL-terminated */

void  buf_add(buf_t *b, const void *p, size_t n);
void  buf_adds(buf_t *b, const char *s);
void  buf_addc(buf_t *b, char c);
void  buf_printf(buf_t *b, const char *fmt, ...) __attribute__((format(MCM_PRINTF, 2, 3)));
char *buf_cstr(buf_t *b);                 /* NUL-terminate, return data */
char *buf_steal(buf_t *b);                /* return malloc'd string, reset buffer */
void  buf_free(buf_t *b);

void   utf8_put(buf_t *b, uint32_t cp);
/* Decode UTF-8 into a NUL-terminated wide string; invalid bytes become U+FFFD. */
size_t utf8_to_wcs(const char *s, size_t len, wchar_t **out);
/* Decode one character at s; *clen = bytes consumed (>= 1). */
wchar_t utf8_next(const char *s, size_t len, size_t *clen);

/* Windows delivers a character above U+FFFF as two UTF-16 halves: call
 * this with each; it returns 0 while it holds the first half, otherwise 1
 * with *cp the whole character. */
int  utf16_join(uint32_t *cp);
/* Windows-1252 character for 0x80-0x9F (0 if undefined); others unchanged. */
uint32_t cp1252_char(uint32_t c);
/* Text that is surely valid UTF-8: bytes that are not UTF-8 are read as
 * Windows-1252, C1 controls (U+0080-U+009F) too; other control characters
 * than newline and tab are left out. Returns a new string. */
char *utf8_clean(const char *s, size_t n, size_t *outlen);

char *str_trim(char *s);
void  str_lower(char *s);

#endif
