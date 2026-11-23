/* t_util.c - util.c: buffers, UTF-8, strings */
#include "check.h"
#include "../src/util.h"

static void t_buf(void)
{
	buf_t b = {0};
	CHECK_STR(buf_cstr(&b), "");
	buf_adds(&b, "abc");
	buf_addc(&b, '-');
	buf_add(&b, "xyz", 2);
	buf_printf(&b, "%d|%s", 42, "end");
	CHECK_STR(buf_cstr(&b), "abc-xy42|end");
	CHECK_INT(b.len, 12);
	for (int i = 0; i < 10000; i++) buf_addc(&b, 'x');     /* growing */
	CHECK_INT(b.len, 10012);
	char *s = buf_steal(&b);
	CHECK_INT(strlen(s), 10012);
	CHECK(b.d == NULL && b.len == 0);
	free(s);
	buf_free(&b);
}

static void t_strings(void)
{
	char a[] = "  \t hi there \r\n";
	CHECK_STR(str_trim(a), "hi there");
	char e[] = "   ";
	CHECK_STR(str_trim(e), "");
	char l[] = "MiXeD 123";
	str_lower(l);
	CHECK_STR(l, "mixed 123");
	CHECK_STR_FREE(xstrndup("abcdef", 3), "abc");
	char *z = xstrndup("a\0b", 3);                    /* binary safe */
	CHECK(z[0] == 'a' && z[1] == 0 && z[2] == 'b' && z[3] == 0);
	free(z);
}

static void t_utf8(void)
{
	buf_t b = {0};
	utf8_put(&b, 'A');
	utf8_put(&b, 0xE9);        /* é */
	utf8_put(&b, 0x20AC);      /* € */
	utf8_put(&b, 0x1F600);     /* 😀 */
	CHECK_STR(buf_cstr(&b), "A\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80");
	size_t cl;
	CHECK_INT(utf8_next(b.d + 1, b.len - 1, &cl), 0xE9);
	CHECK_INT(cl, 2);
	CHECK_INT(utf8_next(b.d + 6, b.len - 6, &cl), 0x1F600);
	CHECK_INT(cl, 4);
	wchar_t *w;
	CHECK_INT(utf8_to_wcs(b.d, b.len, &w), 4);
	CHECK(w[0] == 'A' && w[1] == 0xE9 && w[2] == 0x20AC && w[3] == 0x1F600 && w[4] == 0);
	free(w);
	buf_free(&b);

	/* invalid bytes: U+FFFD, and never more than was there */
	CHECK_INT(utf8_to_wcs("a\xFF" "b\xC3", 4, &w), 4);
	CHECK(w[0] == 'a' && w[1] == 0xFFFD && w[2] == 'b' && w[3] == 0xFFFD);
	free(w);
	CHECK_INT(utf8_next("\xE2\x82", 2, &cl), 0xFFFD);      /* cut off */
	CHECK(cl >= 1 && cl <= 2);

	CHECK_INT(cp1252_char(0x80), 0x20AC);
	CHECK_INT(cp1252_char(0x92), 0x2019);
	CHECK_INT(cp1252_char(0x41), 0x41);
}

static void t_utf8_clean(void)
{
	size_t n;
	char *s = utf8_clean("ok \x80 \xE9 caf\xC3\xA9 \x01x\ty\n", 18, &n);
	CHECK_STR(s, "ok \xE2\x82\xAC \xC3\xA9 caf\xC3\xA9 x\ty\n");
	CHECK_INT(n, strlen(s));
	CHECK(utf8_valid(s, n));
	free(s);
	s = utf8_clean("", 0, &n);
	CHECK_STR(s, "");
	free(s);
	/* overlong and surrogate encodings are not valid UTF-8 */
	s = utf8_clean("\xC0\xAF\xED\xA0\x80", 5, &n);
	CHECK(utf8_valid(s, n));
	free(s);
}

static void t_utf8_repair(void)
{
	size_t n;
	/* valid UTF-8: unchanged, control characters too */
	const char *ok = "caf\xC3\xA9\r\n\x0C\xF0\x9F\x98\x80";
	char *s = utf8_repair(ok, strlen(ok), &n);
	CHECK_STR(s, ok);
	CHECK_INT(n, strlen(ok));
	free(s);
	/* Latin-1 / Windows-1252 in "UTF-8" text, with the controls kept */
	s = utf8_repair("caf\xE9 \x93q\x94\r\n", 10, &n);
	CHECK_STR(s, "caf\xC3\xA9 \xE2\x80\x9Cq\xE2\x80\x9D\r\n");
	CHECK(utf8_valid(s, n));
	free(s);
	/* mixed: the valid sequences stay, the stray bytes are converted */
	s = utf8_repair("\xC3\xA9-\xE9-\xE2\x82\xAC-\x80", 10, &n);
	CHECK_STR(s, "\xC3\xA9-\xC3\xA9-\xE2\x82\xAC-\xE2\x82\xAC");
	free(s);
	/* cut off at the end, overlong, surrogate */
	s = utf8_repair("a\xC3", 2, &n);
	CHECK_STR(s, "a\xC3\x83");
	free(s);
	s = utf8_repair("\xC0\xAF\xED\xA0\x80", 5, &n);
	CHECK(utf8_valid(s, n));
	free(s);
	s = utf8_repair("", 0, &n);
	CHECK_STR(s, "");
	CHECK_INT(n, 0);
	free(s);
}

static void t_utf16(void)
{
	uint32_t c = 0xD83D;
	CHECK_INT(utf16_join(&c), 0);
	c = 0xDE00;
	CHECK_INT(utf16_join(&c), 1);
	CHECK_INT(c, 0x1F600);
	c = 'x';
	CHECK_INT(utf16_join(&c), 1);
	CHECK_INT(c, 'x');
}

void run_util(void)
{
	RUN(t_buf);
	RUN(t_strings);
	RUN(t_utf8);
	RUN(t_utf8_clean);
	RUN(t_utf8_repair);
	RUN(t_utf16);
}
