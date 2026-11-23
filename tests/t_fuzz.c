/* t_fuzz.c - damaged input must never crash the parsers
 *
 * Sample messages are changed at random (bytes flipped, cut off, pieces of
 * MIME syntax inserted) and fed to everything that reads mail. The random
 * numbers are fixed (same seed), so a failure can be repeated. FUZZ=n
 * sets the number of rounds (default 3000). Build with "make test
 * SANITIZE=1" to also catch reads and writes outside a buffer.
 */
#include "check.h"
#include "../src/util.h"
#include "../src/mime.h"
#include "../src/textconv.h"

static unsigned long long rng = 0x9E3779B97F4A7C15ull;

static unsigned rnd(unsigned n)
{
	rng ^= rng << 13;
	rng ^= rng >> 7;
	rng ^= rng << 17;
	return n ? (unsigned)(rng % n) : 0;
}

static const char *seeds[] = {
	"From: =?utf-8?Q?A=C3=A9?= <a@x.org>\r\nSubject: =?iso-8859-1?B?6Q==?= x\r\n"
	"Date: Thu, 12 Nov 2026 23:10:05 +0100\r\n"
	"Content-Type: multipart/mixed; boundary=\"BB\"\r\n\r\n--BB\r\n"
	"Content-Type: multipart/alternative; boundary=CC\r\n\r\n--CC\r\n"
	"Content-Type: text/plain; charset=utf-8; format=flowed; delsp=yes\r\n"
	"Content-Transfer-Encoding: quoted-printable\r\n\r\nsoft =\r\nline=C3=A9 \r\nnext\r\n--CC\r\n"
	"Content-Type: text/html; charset=windows-1252\r\n\r\n<table><tr><td>a<td>b</table>"
	"<a href=\"x\">l</a>&eacute;&#x41;<pre>p</pre>\r\n--CC--\r\n--BB\r\n"
	"Content-Type: application/octet-stream; name*0*=utf-8''a%C3%A9; name*1=.bin\r\n"
	"Content-Disposition: attachment\r\nContent-Transfer-Encoding: base64\r\n\r\nAAECAwQF\r\n--BB--\r\n",

	"Subject: md\r\nContent-Type: text/plain\r\n\r\n# H1\r\n## H2\r\n### H3\r\n"
	"**b** *i* ~~s~~ `c` [l](u)\r\n```\r\ncode\r\n```\r\n> q\r\n- li\r\n",

	"Content-Type: message/rfc822\r\n\r\nSubject: inner\r\nContent-Type: text/html\r\n\r\n"
	"<html><head><style>a{b:c}</style></head><body><ul><li>x<ol><li>y</ol></ul>"
	"<blockquote>q</blockquote><img alt=i></body></html>\r\n",
};

static const char *bits[] = {
	"\r\n", "\r\n\r\n", "--BB", "--BB--", "--CC", "=?utf-8?Q?", "?=", "=?x?B?", "=",
	"=C3", "<", ">", "<a href=\"", "&", "&#", ";", "**", "*", "`", "```", "[", "](",
	"#", "~~", "\"", ";", "boundary=", "charset=", "\xC3", "\xEF\xA3\xA0", "\0", " ",
	"Content-Type: multipart/mixed; boundary=BB\r\n", "<table><tr><td>", "<pre>",
};

static void check_text(const char *what, char *s)
{
	checks_run++;
	if (!s) { check_fail(__FILE__, __LINE__, what); return; }
	(void)strlen(s);                 /* NUL-terminated (the sanitizer checks the read) */
	free(s);
}

static void feed(const char *m, size_t n)
{
	check_text("mime_render", mime_render(m, n, 0));
	check_text("mime_render all", mime_render(m, n, 1));
	check_text("mime_render_body", mime_render_body(m, n));
	mime_set_plain(1);
	check_text("mime_render_body plain", mime_render_body(m, n));
	mime_set_plain(0);
	mime_att *a = NULL;
	int na = mime_attachments(m, n, &a);
	mime_att_free(a, na);
	(void)mime_has_attachment(m, n);
	(void)mime_has_plain(m, n);
	mime_span *sp = NULL;
	(void)mime_split(m, n, &sp);
	free(sp);
	free(mime_header(m, n, "subject"));
	char *from = mime_header_raw(m, n, "from");
	if (from) {
		free(mime_addr_display(from));
		free(mime_addr_email(from));
		free(mime_decode_words(from));
		(void)mime_parse_date(from);
		free(from);
	}
	size_t ol;
	free(mime_unflow(m, n, rnd(2), &ol));
	free(mime_decode_cte(m, n, rnd(2) ? "base64" : "quoted-printable", &ol));
	free(utf8_clean(m, n, &ol));

	buf_t b = {0};
	textconv_styles(rnd(2));
	md_to_text(m, n, &b);
	textconv_styles(0);
	ts_span *ts = NULL;
	int nts;
	size_t keep = n ? rnd((unsigned)n) : (size_t)-1;
	if (b.d) {
		size_t len = ts_strip(b.d, b.len, &ts, &nts, &keep);
		checks_run++;
		if (len > b.len) check_fail(__FILE__, __LINE__, "ts_strip grew the text");
		for (int i = 0; i < nts; i++)
			if (ts[i].off + ts[i].len > len) { check_fail(__FILE__, __LINE__, "span past the end"); break; }
	}
	free(ts);
	buf_free(&b);
	html_to_text(m, n, &b);
	buf_free(&b);
	(void)md_detect(m, n);
	strip_invisible(m, n, 1, &b);
	buf_free(&b);
}

static void t_fuzz(void)
{
	long rounds = getenv("FUZZ") ? atol(getenv("FUZZ")) : 3000;
	buf_t m = {0};
	for (long r = 0; r < rounds; r++) {
		const char *seed = seeds[rnd(sizeof seeds / sizeof *seeds)];
		m.len = 0;
		buf_adds(&m, seed);
		int changes = 1 + rnd(8);
		for (int c = 0; c < changes && m.len; c++) {
			size_t at = rnd((unsigned)m.len);
			switch (rnd(4)) {
			case 0: m.d[at] = (char)rnd(256); break;                     /* flip a byte */
			case 1: m.len = at; break;                                    /* cut off */
			case 2: {                                                     /* insert */
				const char *s = bits[rnd(sizeof bits / sizeof *bits)];
				size_t k = *s ? strlen(s) : 1;
				buf_add(&m, s, k);                                        /* make room */
				memmove(m.d + at + k, m.d + at, m.len - k - at);
				memcpy(m.d + at, s, k);
				break;
			}
			default: {                                                    /* delete */
				size_t k = rnd(40);
				if (k > m.len - at) k = m.len - at;
				memmove(m.d + at, m.d + at + k, m.len - at - k);
				m.len -= k;
			}
			}
		}
		/* an exact-size copy, so that reading one byte too far shows */
		char *copy = xmalloc(m.len ? m.len : 1);
		memcpy(copy, m.d, m.len);
		feed(copy, m.len);
		free(copy);
	}
	buf_free(&m);
	/* the seeds as they are, and the empty message */
	for (size_t i = 0; i < sizeof seeds / sizeof *seeds; i++) feed(seeds[i], strlen(seeds[i]));
	feed("", 0);
}

void run_fuzz(void)
{
	RUN(t_fuzz);
}
