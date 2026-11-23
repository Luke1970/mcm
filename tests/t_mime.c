/* t_mime.c - mime.c: encodings, headers, dates, addresses, rendering */
#include "check.h"
#include "../src/util.h"
#include "../src/mime.h"

static char *cte(const char *in, const char *how)
{
	size_t n;
	return mime_decode_cte(in, strlen(in), how, &n);
}

static void t_cte(void)
{
	CHECK_STR_FREE(cte("caf=C3=A9 =\r\nend=3D", "quoted-printable"), "caf\xC3\xA9 end=");
	CHECK_STR_FREE(cte("soft=\nbreak", "quoted-printable"), "softbreak");
	CHECK_STR_FREE(cte("lower=c3=a9", "quoted-printable"), "lower\xC3\xA9");
	CHECK_STR_FREE(cte("bad=ZZ", "Quoted-Printable"), "bad=ZZ");     /* kept as written */
	CHECK_STR_FREE(cte("ends soft=", "quoted-printable"), "ends soft");
	CHECK_STR_FREE(cte("line\r\n", "quoted-printable"), "line\r\n");
	CHECK_STR_FREE(cte("aGVsbG8g\r\nd29ybGQ=", "base64"), "hello world");
	CHECK_STR_FREE(cte("aGVsbG8", "BASE64"), "hello");                /* no padding */
	CHECK_STR_FREE(cte("a G V s\nb G 8 =", "base64"), "hello");      /* spaces */
	CHECK_STR_FREE(cte("plain *text*", "8bit"), "plain *text*");
	CHECK_STR_FREE(cte("plain", NULL), "plain");
	size_t n;
	char *z = mime_decode_cte("AAEC", 4, "base64", &n);               /* binary */
	CHECK_INT(n, 3);
	CHECK(z[0] == 0 && z[1] == 1 && z[2] == 2);
	free(z);
}

static void t_charset(void)
{
	size_t n;
	CHECK_STR_FREE(conv_to_utf8("iso-8859-1", "caf\xE9", 4, &n), "caf\xC3\xA9");
	CHECK_STR_FREE(conv_to_utf8("latin1", "\x80", 1, &n), "\xE2\x82\xAC");   /* read as 1252 */
	CHECK_STR_FREE(conv_to_utf8("Windows-1252", "\x93q\x94", 3, &n), "\xE2\x80\x9Cq\xE2\x80\x9D");
	CHECK_STR_FREE(conv_to_utf8("UTF-8", "caf\xC3\xA9", 5, &n), "caf\xC3\xA9");
	CHECK_STR_FREE(conv_to_utf8(NULL, "x", 1, &n), "x");
	/* says UTF-8 (or nothing, or a charset iconv does not know) but is
	 * Latin-1 / Windows-1252: read as Windows-1252 */
	CHECK_STR_FREE(conv_to_utf8("utf-8", "caf\xE9", 4, &n), "caf\xC3\xA9");
	CHECK_STR_FREE(conv_to_utf8("us-ascii", "\x80 5", 3, &n), "\xE2\x82\xAC 5");
	CHECK_STR_FREE(conv_to_utf8(NULL, "\x93hi\x94", 4, &n), "\xE2\x80\x9Chi\xE2\x80\x9D");
	CHECK_STR_FREE(conv_to_utf8("x-nonsense", "caf\xE9", 4, &n), "caf\xC3\xA9");
	CHECK_STR_FREE(conv_to_utf8("x-nonsense", "caf\xC3\xA9", 5, &n), "caf\xC3\xA9");
	CHECK_STR_FREE(conv_to_utf8("iso-8859-15", "\xA4", 1, &n), "\xE2\x82\xAC");
	CHECK_STR_FREE(conv_to_utf8("koi8-r", "\xC1", 1, &n), "\xD0\xB0");          /* а */
	CHECK_STR_FREE(conv_to_utf8("utf-8*en", "x", 1, &n), "x");                    /* RFC 2231 language */
	/* a byte that is not valid in the charset: U+FFFD, the rest goes on */
	char *s = conv_to_utf8("utf-16le", "a\0b", 3, &n);
	CHECK_HAS(s, "a");
	free(s);
}

static void t_words(void)
{
	CHECK_STR_FREE(mime_decode_words("=?utf-8?Q?caf=C3=A9_au_lait?="), "caf\xC3\xA9 au lait");
	CHECK_STR_FREE(mime_decode_words("=?ISO-8859-1?B?6Q==?="), "\xC3\xA9");
	/* white space between two encoded words goes, other text stays */
	CHECK_STR_FREE(mime_decode_words("=?utf-8?Q?a?= =?utf-8?Q?b?= c"), "ab c");
	CHECK_STR_FREE(mime_decode_words("x =?utf-8?Q?y?="), "x y");
	CHECK_STR_FREE(mime_decode_words("no words here"), "no words here");
	CHECK_STR_FREE(mime_decode_words(""), "");
	/* broken words are kept as written */
	CHECK_STR_FREE(mime_decode_words("=?utf-8?Q?open"), "=?utf-8?Q?open");
	CHECK_STR_FREE(mime_decode_words("=?utf-8?X?abc?="), "=?utf-8?X?abc?=");
	/* a multi-byte character split over two words */
	CHECK_STR_FREE(mime_decode_words("=?utf-8?B?w6k=?= =?utf-8?Q?=C3?= =?utf-8?Q?=A9?="),
	               "\xC3\xA9\xC3\xA9");
	/* not joined when the charset differs */
	CHECK_STR_FREE(mime_decode_words("=?utf-8?Q?a?= =?iso-8859-1?Q?=E9?="), "a\xC3\xA9");
	/* raw 8-bit header text (not allowed, but sent): Windows-1252 */
	CHECK_STR_FREE(mime_decode_words("Caf\xE9 \x93x\x94"), "Caf\xC3\xA9 \xE2\x80\x9Cx\xE2\x80\x9D");
	CHECK_STR_FREE(mime_decode_words("Caf\xC3\xA9"), "Caf\xC3\xA9");
}

static void t_params(void)
{
	const char *at[] = {"filename*0*", "filename*1"}, *vl[] = {"utf-8''caf%C3%A9", ".txt"};
	CHECK_STR_FREE(mime_param_decode(at, vl, 2, "filename"), "caf\xC3\xA9.txt");
	const char *at2[] = {"name", "filename"}, *vl2[] = {"n.pdf", "=?utf-8?Q?r=C3=A9sum=C3=A9.pdf?="};
	CHECK_STR_FREE(mime_param_decode(at2, vl2, 2, "filename"), "r\xC3\xA9sum\xC3\xA9.pdf");
	CHECK_STR_FREE(mime_param_decode(at2, vl2, 2, "name"), "n.pdf");
	CHECK_STR_FREE(mime_param_decode(at2, vl2, 2, "charset"), NULL);
	const char *at3[] = {"filename*"}, *vl3[] = {"iso-8859-1'en'%E9t%E9.doc"};
	CHECK_STR_FREE(mime_param_decode(at3, vl3, 1, "filename"), "\xC3\xA9t\xC3\xA9.doc");
}

static void t_dates(void)
{
	time_t t = mime_parse_date("Thu, 12 Nov 2026 23:10:05 +0100");
	CHECK_INT(t, 1794521405);
	CHECK_INT(mime_parse_date("12-Nov-2026 23:10:05 +0100"), t);
	CHECK_INT(mime_parse_date("12 Nov 2026 22:10:05 GMT"), t);          /* no weekday, zone name */
	CHECK_INT(mime_parse_date("Thu, 12 Nov 2026 17:10:05 -0500 (EST)"), t);
	CHECK_INT(mime_parse_date("Thu, 12 Nov 2026 22:10:05 +0000"), t);
	CHECK_INT(mime_parse_date("Thu, 1 Jan 1970 00:00:00 +0000"), 0);
	CHECK_INT(mime_parse_date("garbage"), 0);
	CHECK_INT(mime_parse_date(""), 0);
}

static void t_addr(void)
{
	CHECK_STR_FREE(mime_addr_display("=?utf-8?Q?Jos=C3=A9?= <jose@example.org>"), "Jos\xC3\xA9");
	CHECK_STR_FREE(mime_addr_display("\"Doe, John\" <john@example.org>"), "Doe, John");
	CHECK_STR_FREE(mime_addr_display("plain@example.org"), "plain@example.org");
	CHECK_STR_FREE(mime_addr_display("<only@example.org>"), "only@example.org");
	CHECK_STR_FREE(mime_addr_email("\"Doe, John\" <john@example.org>"), "john@example.org");
	CHECK_STR_FREE(mime_addr_email("plain@example.org"), "plain@example.org");
	char h[32];
	mime_human_size(h, sizeof h, 0);
	CHECK_STR(h, "0 B");
	mime_human_size(h, sizeof h, 1536);
	CHECK_STR(h, "1.5 KB");
	mime_human_size(h, sizeof h, 5u * 1024 * 1024);
	CHECK_STR(h, "5.0 MB");
}

static void t_unflow(void)
{
	size_t n;
	const char *s = "one \r\ntwo\r\n> q1 \r\n> q2\r\n";
	CHECK_STR_FREE(mime_unflow(s, strlen(s), 0, &n), "one two\n> q1 q2\n");
	s = "a \nb\n";
	CHECK_STR_FREE(mime_unflow(s, strlen(s), 1, &n), "ab\n");
	s = " stuffed\n-- \nsig\n";             /* "-- " is the signature line, not soft */
	CHECK_STR_FREE(mime_unflow(s, strlen(s), 0, &n), "stuffed\n-- \nsig\n");
}

#define MSG_MIXED \
	"From: A <a@x.org>\r\nTo: b@y.org\r\nSubject: =?utf-8?Q?Hi_=C3=A9?=\r\n" \
	"Date: Thu, 12 Nov 2026 23:10:05 +0100\r\nMIME-Version: 1.0\r\n" \
	"Content-Type: multipart/mixed;\r\n boundary=\"BB\"\r\n\r\npreamble\r\n" \
	"--BB\r\nContent-Type: text/plain; charset=iso-8859-1\r\n" \
	"Content-Transfer-Encoding: quoted-printable\r\n\r\nCaf=E9 body\r\n" \
	"--BB\r\nContent-Type: application/pdf; name=\"doc.pdf\"\r\n" \
	"Content-Disposition: attachment; filename=\"doc.pdf\"\r\n" \
	"Content-Transfer-Encoding: base64\r\n\r\nJVBERi0=\r\n--BB--\r\nepilogue\r\n"

#define MSG_ALT \
	"Subject: alt\r\nContent-Type: multipart/alternative; boundary=x\r\n\r\n" \
	"--x\r\nContent-Type: text/plain\r\n\r\nPLAIN version\r\n" \
	"--x\r\nContent-Type: text/html; charset=utf-8\r\n\r\n<p>HTML <b>version</b></p>\r\n--x--\r\n"

/* the plain text is (almost) empty: the HTML is shown */
#define MSG_ALT_EMPTY \
	"Content-Type: multipart/alternative; boundary=x\r\n\r\n" \
	"--x\r\nContent-Type: text/plain\r\n\r\n \r\n\r\n" \
	"--x\r\nContent-Type: text/html\r\n\r\n<p>HTML <b>version</b></p>\r\n--x--\r\n"

static void t_headers(void)
{
	const char *m = MSG_MIXED;
	size_t n = strlen(m);
	CHECK_STR_FREE(mime_header(m, n, "subject"), "Hi \xC3\xA9");
	CHECK_STR_FREE(mime_header(m, n, "SUBJECT"), "Hi \xC3\xA9");
	CHECK_STR_FREE(mime_header_raw(m, n, "subject"), "=?utf-8?Q?Hi_=C3=A9?=");
	char *ct = mime_header(m, n, "content-type");        /* unfolded */
	CHECK_HAS(ct, "boundary=\"BB\"");
	CHECK_NOT_HAS(ct, "\n");
	free(ct);
	CHECK_STR_FREE(mime_header(m, n, "x-missing"), NULL);
	CHECK_STR_FREE(mime_header(m, n, "Content-Disposition"), NULL);   /* only top level */
	const char *nohdr = "just a body without header";
	CHECK_STR_FREE(mime_header(nohdr, strlen(nohdr), "subject"), NULL);
}

static void t_render(void)
{
	const char *m = MSG_MIXED;
	char *r = mime_render(m, strlen(m), 0);
	CHECK_HAS(r, "From     A <a@x.org>");
	CHECK_HAS(r, "Subject  Hi \xC3\xA9");
	CHECK_HAS(r, "Caf\xC3\xA9 body");
	CHECK_HAS(r, "[Attachment: doc.pdf  (application/pdf, 6 B)]");
	CHECK_NOT_HAS(r, "preamble");
	CHECK_NOT_HAS(r, "epilogue");
	CHECK_NOT_HAS(r, "JVBERi0");
	CHECK(utf8_valid(r, strlen(r)));
	free(r);

	r = mime_render(m, strlen(m), 1);                  /* all headers */
	CHECK_HAS(r, "MIME-Version: 1.0");
	free(r);

	const char *a = MSG_ALT;
	r = mime_render_body(a, strlen(a));                /* the plain text */
	CHECK_HAS(r, "PLAIN version");
	CHECK_NOT_HAS(r, "HTML");
	free(r);
	const char *e = MSG_ALT_EMPTY;
	r = mime_render_body(e, strlen(e));                /* empty plain text: the HTML */
	CHECK_HAS(r, "HTML version");
	free(r);
	mime_set_plain(1);                                 /* T: the plain text as sent */
	r = mime_render_body(e, strlen(e));
	CHECK_NOT_HAS(r, "HTML version");
	free(r);
	mime_set_plain(0);
	/* a "plain text" that is really an HTML page: the HTML part */
	const char *pg = "Content-Type: multipart/alternative; boundary=x\r\n\r\n"
	    "--x\r\nContent-Type: text/plain\r\n\r\n<!DOCTYPE html><html><body>junk</body></html>\r\n"
	    "--x\r\nContent-Type: text/html\r\n\r\n<p>the real one</p>\r\n--x--\r\n";
	r = mime_render_body(pg, strlen(pg));
	CHECK_HAS(r, "the real one");
	free(r);
	CHECK(mime_has_plain(a, strlen(a)));
	CHECK(!mime_has_attachment(a, strlen(a)));
}

static void t_render_kinds(void)
{
	/* Markdown in text/plain is shown as Markdown (0.996) */
	const char *md = "Subject: md\r\nContent-Type: text/plain; charset=utf-8\r\n\r\n"
	                 "# Title\r\n\r\nSome **bold** and `code`.\r\n";
	char *r = mime_render_body(md, strlen(md));
	CHECK_HAS(r, "Title\n=====");
	CHECK_HAS(r, "Some bold and code.");
	free(r);
	/* text/markdown */
	const char *md2 = "Content-Type: text/markdown\r\n\r\n*only* one kind\r\n";
	r = mime_render_body(md2, strlen(md2));
	CHECK_HAS(r, "only one kind");
	free(r);
	/* a body that says UTF-8 but is Latin-1, and a header word likewise */
	const char *l1 = "Subject: =?utf-8?Q?Caf=E9?=\r\nContent-Type: text/plain; charset=utf-8\r\n"
	                 "Content-Transfer-Encoding: 8bit\r\n\r\nCaf\xE9 cr\xE8me\r\n";
	r = mime_render(l1, strlen(l1), 0);
	CHECK_HAS(r, "Subject  Caf\xC3\xA9");
	CHECK_HAS(r, "Caf\xC3\xA9 cr\xC3\xA8me");
	CHECK(utf8_valid(r, strlen(r)));
	free(r);
	/* base64 UTF-8 body, no MIME-Version */
	const char *b = "Content-Type: text/plain; charset=utf-8\r\n"
	                "Content-Transfer-Encoding: base64\r\n\r\nY2Fmw6kK\r\n";
	r = mime_render_body(b, strlen(b));
	CHECK_HAS(r, "caf\xC3\xA9");
	free(r);
	/* format=flowed */
	const char *f = "Content-Type: text/plain; format=flowed\r\n\r\nlong \r\nline\r\n";
	r = mime_render_body(f, strlen(f));
	CHECK_HAS(r, "long line");
	free(r);
	/* no header at all, and empty input */
	r = mime_render("", 0, 0);
	CHECK(r != NULL);
	free(r);
	r = mime_render_body("\r\n", 2);
	CHECK(r != NULL);
	free(r);
	/* a multipart without its closing boundary */
	const char *open = "Content-Type: multipart/mixed; boundary=q\r\n\r\n--q\r\n"
	                   "Content-Type: text/plain\r\n\r\nstill shown\r\n";
	r = mime_render_body(open, strlen(open));
	CHECK_HAS(r, "still shown");
	free(r);
	/* nested: mixed( alternative(plain, html), attachment ) */
	const char *nest = "Content-Type: multipart/mixed; boundary=o\r\n\r\n--o\r\n"
	    "Content-Type: multipart/alternative; boundary=i\r\n\r\n--i\r\n"
	    "Content-Type: text/plain\r\n\r\ninner plain\r\n--i\r\n"
	    "Content-Type: text/html\r\n\r\n<div>inner html</div>\r\n--i--\r\n--o\r\n"
	    "Content-Type: image/png; name=a.png\r\nContent-Transfer-Encoding: base64\r\n\r\n"
	    "iVBORw0KGgo=\r\n--o--\r\n";
	r = mime_render_body(nest, strlen(nest));
	CHECK_HAS(r, "inner plain");
	CHECK_NOT_HAS(r, "inner html");
	CHECK_HAS(r, "[Attachment: a.png  (image/png, 9 B)]");
	free(r);
	CHECK(mime_has_attachment(nest, strlen(nest)));
}

static void t_attachments(void)
{
	const char *m = MSG_MIXED;
	mime_att *a = NULL;
	int n = mime_attachments(m, strlen(m), &a);
	CHECK_INT(n, 1);
	if (n == 1) {
		CHECK_STR(a[0].name, "doc.pdf");
		CHECK_STR(a[0].type, "application/pdf");
		CHECK_INT(a[0].len, 5);
		CHECK(!memcmp(a[0].data, "%PDF-", 5));
	}
	mime_att_free(a, n);
	CHECK(mime_has_attachment(m, strlen(m)));
	/* only the header: multipart/mixed counts */
	const char *h = "Content-Type: multipart/mixed; boundary=z\r\n\r\n";
	CHECK(mime_has_attachment(h, strlen(h)));

	mime_span *sp = NULL;
	CHECK_INT(mime_split(m, strlen(m), &sp), 2);
	free(sp);
}

static void up_hook(void *ctx, char **t, size_t *n)
{
	(*(int *)ctx)++;
	for (size_t i = 0; i < *n; i++)
		if ((*t)[i] == 'x') (*t)[i] = 'X';
}

static char *part(const char *data, mime_textopt o)
{
	size_t n;
	char *t = mime_text_part(data, strlen(data), &o, &n);
	CHECK_INT(n, strlen(t));
	return t;
}

/* the text pipeline both ways of showing a message use */
static void t_text_part(void)
{
	mime_textopt o = {.kind = MIME_PLAIN};
	CHECK_STR_FREE(part("plain\r\ntext\r\n", o), "plain\r\ntext\r\n");
	o.cte = "quoted-printable";
	o.charset = "iso-8859-1";
	CHECK_STR_FREE(part("caf=E9", o), "caf\xC3\xA9");
	o = (mime_textopt){.kind = MIME_PLAIN, .cte = "BASE64"};
	CHECK_STR_FREE(part("aGk=", o), "hi");
	/* format=flowed, with and without delsp */
	o = (mime_textopt){.kind = MIME_PLAIN, .flowed = 1};
	CHECK_STR_FREE(part("soft \nbreak\n", o), "soft break\n");
	o.delsp = 1;
	CHECK_STR_FREE(part("soft \nbreak\n", o), "softbreak\n");
	/* Markdown found in plain text; invisible characters, also as entities */
	o = (mime_textopt){.kind = MIME_PLAIN};
	CHECK_STR_FREE(part("# T\n\n**b** and `c`\n", o), "T\n=\n\nb and c\n");
	CHECK_STR_FREE(part("a\xE2\x80\x8B &zwnj;b\n", o), "a b\n");
	/* T: as sent (only decoded and unflowed) */
	o = (mime_textopt){.kind = MIME_PLAIN, .as_sent = 1, .flowed = 1};
	CHECK_STR_FREE(part("# T\n\n**b** \nx &zwnj;\n", o), "# T\n\n**b** x &zwnj;\n");
	/* the hook runs on plain text before unflowing, never on HTML */
	int calls = 0;
	o = (mime_textopt){.kind = MIME_PLAIN, .flowed = 1, .hook = up_hook, .ctx = &calls};
	CHECK_STR_FREE(part("x \nx\n", o), "X X\n");
	CHECK_INT(calls, 1);
	o.kind = MIME_HTML;
	CHECK_STR_FREE(part("<p>x</p>", o), "x\n\n");
	CHECK_INT(calls, 1);
	/* HTML and Markdown: entities in HTML are decoded, not stripped as text;
	 * T does not change them */
	o = (mime_textopt){.kind = MIME_HTML, .as_sent = 1};
	CHECK_STR_FREE(part("<b>a</b>&nbsp;&amp;\xE2\x80\x8B", o), "a &\n");
	o = (mime_textopt){.kind = MIME_MARKDOWN};
	CHECK_STR_FREE(part("*one* kind\n", o), "one kind\n");
	CHECK_STR_FREE(part("", o), "");
}

static void t_alt_choose(void)
{
	/* plain, html, multi, poor, as_sent */
	CHECK_INT(mime_alt_choose(0, 1, -1, 0, 0), 0);      /* plain text */
	CHECK_INT(mime_alt_choose(0, 1, -1, 1, 0), 1);      /* poor plain: HTML */
	CHECK_INT(mime_alt_choose(0, 1, -1, 1, 1), 0);      /* T: plain anyway */
	CHECK_INT(mime_alt_choose(0, -1, -1, 1, 0), 0);     /* poor, but no HTML */
	CHECK_INT(mime_alt_choose(-1, 1, 0, 0, 0), 1);      /* no plain: HTML */
	CHECK_INT(mime_alt_choose(-1, -1, 2, 0, 0), 2);     /* a multipart */
	CHECK_INT(mime_alt_choose(-1, -1, -1, 0, 0), 0);    /* the first */
	buf_t b = {0};
	mime_att_line(&b, "a.pdf", "application/pdf", 1024, "base64");
	mime_att_line(&b, NULL, "image/png", 10, "7bit");
	CHECK_STR(buf_cstr(&b), "[Attachment: a.pdf  (application/pdf, 768 B)]\n"
	                        "[Attachment: (unnamed)  (image/png, 10 B)]\n");
	buf_free(&b);
}

static void t_detect(void)
{
	const char *h = "\n\n  <!DOCTYPE html><html>";
	CHECK(mime_looks_like_html(h, strlen(h)));
	CHECK(!mime_looks_like_html("Hello <b>", 9));
	const char *css = "a{b:c} d{e:f} g{h:i} text";
	CHECK(mime_plain_is_bad(css, strlen(css)));
	CHECK(!mime_plain_is_bad("Dear all,\nsee {this}.\n", 22));
	CHECK(mime_is_markdown("text/markdown"));
	CHECK(mime_is_markdown("TEXT/X-MARKDOWN"));
	CHECK(!mime_is_markdown("text/plain"));
}

void run_mime(void)
{
	RUN(t_cte);
	RUN(t_charset);
	RUN(t_words);
	RUN(t_params);
	RUN(t_dates);
	RUN(t_addr);
	RUN(t_unflow);
	RUN(t_headers);
	RUN(t_render);
	RUN(t_render_kinds);
	RUN(t_attachments);
	RUN(t_detect);
	RUN(t_text_part);
	RUN(t_alt_choose);
}
