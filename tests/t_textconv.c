/* t_textconv.c - textconv.c: Markdown and HTML as text, styles, md_detect */
#include "check.h"
#include "../src/util.h"
#include "../src/textconv.h"

static char *md(const char *s)
{
	buf_t b = {0};
	md_to_text(s, strlen(s), &b);
	return buf_steal(&b);
}

static char *html(const char *s)
{
	buf_t b = {0};
	html_to_text(s, strlen(s), &b);
	return buf_steal(&b);
}

static void t_markdown(void)
{
	CHECK_STR_FREE(md("# Title\n"), "Title\n=====\n");
	CHECK_STR_FREE(md("## Sub\n"), "Sub\n---\n");
	CHECK_STR_FREE(md("Some **bold**, *it*, __b__, _i_ and `co*de`.\n"),
	               "Some bold, it, b, i and co*de.\n");
	CHECK_STR_FREE(md("~~gone~~ text\n"), "gone text\n");
	CHECK_STR_FREE(md("2 * 3 * 4\n"), "2 * 3 * 4\n");           /* not emphasis */
	CHECK_STR_FREE(md("snake_case_name\n"), "snake_case_name\n");
	CHECK_STR_FREE(md("see [the site](http://e.org)\n"),
	               "see the site [1]\n\nLinks:\n[1] http://e.org\n");
	char *r = md("```\nx = *1*\n```\n");
	CHECK_HAS(r, "    x = *1*");                                 /* as written, indented */
	free(r);
	r = md("- one\n- two\n\n> quoted **b**\n");
	CHECK_HAS(r, "- one\n- two");
	CHECK_HAS(r, "> quoted b");
	free(r);
	CHECK_STR_FREE(md(""), "");
	r = md("**never closed\n");
	CHECK_HAS(r, "never closed");
	free(r);
}

static void t_styles(void)
{
	buf_t b = {0};
	textconv_styles(1);
	md_to_text("a **b** *i* ~~s~~ `c`\n", 21, &b);
	textconv_styles(0);
	ts_span *sp = NULL;
	int n = 0;
	size_t keep = 4;
	size_t len = ts_strip(b.d, b.len, &sp, &n, &keep);
	CHECK_INT(len, strlen("a b i s c\n"));
	CHECK(!memcmp(b.d, "a b i s c\n", len));
	CHECK_INT(n, 4);
	if (n == 4) {
		CHECK_INT(sp[0].off, 2); CHECK_INT(sp[0].len, 1); CHECK_INT(sp[0].style, 1 << TS_BOLD);
		CHECK_INT(sp[1].off, 4); CHECK_INT(sp[1].style, 1 << TS_ITALIC);
		CHECK_INT(sp[2].off, 6); CHECK_INT(sp[2].style, 1 << TS_STRIKE);
		CHECK_INT(sp[3].off, 8); CHECK_INT(sp[3].style, 1 << TS_CODE);
	}
	CHECK(keep < len);                    /* moved along with the text */
	free(sp);
	buf_free(&b);

	/* styles off (the default): no markers at all */
	md_to_text("a **b**\n", 8, &b);
	CHECK_STR(buf_cstr(&b), "a b\n");
	CHECK_NOT_HAS(b.d, "\xEF\xA3");
	buf_free(&b);

	/* headings 3 and 4 get arrows */
	textconv_styles(1);
	md_to_text("### three\n#### four\n", 20, &b);
	textconv_styles(0);
	sp = NULL;
	keep = (size_t)-1;
	len = ts_strip(b.d, b.len, &sp, &n, &keep);
	b.d[len] = 0;
	CHECK_HAS(b.d, "--> three");
	CHECK_HAS(b.d, "-> four");
	CHECK(n >= 1 && (sp[0].style & 1 << TS_HEAD));
	free(sp);
	buf_free(&b);

	/* text without markers */
	char t[] = "plain";
	sp = (ts_span *)1;
	CHECK_INT(ts_strip(t, 5, &sp, &n, NULL), 5);
	CHECK(sp == NULL && n == 0);
}

static void t_md_detect(void)
{
	const char *yes[] = {
		"# Title\n\nSome **bold** text\n",
		"# One\n## Two\n",
		"Use `make` and *then* go\n",
		"[link](http://e.org) and **b**\n",
		"```\ncode\n```\nand ~~old~~\n",
	};
	const char *no[] = {
		"Just text.\n",
		"just **bold** text\n",
		"> # quoted\n> **b**\n",
		"Price: 2 * 3 = 6, see #5\n",
		"",
	};
	for (size_t i = 0; i < sizeof yes / sizeof *yes; i++) CHECK(md_detect(yes[i], strlen(yes[i])));
	for (size_t i = 0; i < sizeof no / sizeof *no; i++) CHECK(!md_detect(no[i], strlen(no[i])));
}

static void t_html(void)
{
	char *r = html("<html><head><title>x</title><style>p{x:y}</style></head><body>"
	               "<h1>T</h1><p>a&amp;b&nbsp;c &lt;d&gt; &#233;&eacute;</p>"
	               "<script>alert(1)</script></body></html>");
	CHECK_HAS(r, "T\n=");
	CHECK_HAS(r, "a&b c <d> \xC3\xA9\xC3\xA9");         /* &nbsp; a space */
	CHECK_NOT_HAS(r, "p{x:y}");
	CHECK_NOT_HAS(r, "alert");
	free(r);
	r = html("<p>see <a href=\"http://e.org/a?b=1&amp;c=2\">here</a></p>");
	CHECK_HAS(r, "here [1]");
	CHECK_HAS(r, "[1] http://e.org/a?b=1&c=2");
	free(r);
	r = html("<ul><li>one<ul><li>sub</li></ul></li><li>two</li></ul><ol><li>first</li></ol>");
	CHECK_HAS(r, "* one");
	CHECK_HAS(r, "* sub");
	CHECK_HAS(r, "* two");
	CHECK_HAS(r, "1. first");
	free(r);
	r = html("<table><tr><td>a</td><td>bb</td></tr><tr><td>ccc</td><td>d</td></tr></table>");
	CHECK_HAS(r, "a    bb\nccc  d");
	free(r);
	r = html("<pre>  keep   this\n  as is</pre>");
	CHECK_HAS(r, "  keep   this\n  as is");
	free(r);
	r = html("line<br>break<blockquote>quoted</blockquote>");
	CHECK_HAS(r, "line\nbreak");
	CHECK_HAS(r, "> quoted");
	free(r);
	r = html("<img src=x alt=\"a picture\">");
	CHECK_HAS(r, "a picture");
	free(r);
	/* broken HTML: no crash, the text is still there */
	r = html("<p>unclosed <b>bold <a href=\"x");
	CHECK_HAS(r, "unclosed");
	free(r);
	r = html("&#xFFFFFFFF; &#0; &bogus; &");
	CHECK(utf8_valid(r, strlen(r)));
	free(r);
	CHECK_STR_FREE(html(""), "");
}

static void t_invisible(void)
{
	buf_t b = {0};
	strip_invisible("a\xE2\x80\x8B" "b\xC2\xAD" "c\xEF\xBB\xBF" "d\n", 13, 0, &b);
	CHECK_STR(buf_cstr(&b), "abcd\n");
	buf_free(&b);
	strip_invisible("x &zwnj; &#8204; y\n", 19, 1, &b);
	CHECK_STR(buf_cstr(&b), "x y\n");
	buf_free(&b);
	strip_invisible("end \xE2\x80\x8B\n", 8, 0, &b);    /* no space left at the end */
	CHECK_STR(buf_cstr(&b), "end\n");
	buf_free(&b);
	strip_invisible("x &zwnj; y\n", 11, 0, &b);           /* entities only when asked */
	CHECK_HAS(buf_cstr(&b), "&zwnj;");
	buf_free(&b);
}

void run_textconv(void)
{
	RUN(t_markdown);
	RUN(t_styles);
	RUN(t_md_detect);
	RUN(t_html);
	RUN(t_invisible);
}
