/* main.c - "make test": runs all tests; exit status 1 when a check failed
 *
 *   tests/run            all tests
 *   tests/run mime fuzz  only these groups
 *   FUZZ=100000 tests/run fuzz   more fuzz rounds (default 3000)
 */
#include "check.h"
#include "../src/mcm.h"

int checks_run, checks_failed;
const char *test_name = "";

/* The shared state that main.c of mcm defines (config.c uses it). */
account_t  accts[MAX_ACCOUNTS];
int        naccts;
account_t *ac;
char       last_account[64];
char      *master_check;
int wrap_width = 72;
int check_mail = 60;
char cfg_pdf_viewer[256], cfg_html_viewer[256];
char cfg_pdf_viewer_console[256], cfg_html_viewer_console[256];
char cfg_editor[256];
int col_pct[3] = {0, 45, 0};
app_t A;

static void show(const char *s)
{
	if (!s) { fputs("NULL", stderr); return; }
	fputc('"', stderr);
	for (; *s; s++)
		if (*s == '\n') fputs("\\n", stderr);
		else if (*s == '\r') fputs("\\r", stderr);
		else if (*s == '\t') fputs("\\t", stderr);
		else if ((unsigned char)*s < 32) fprintf(stderr, "\\x%02x", (unsigned char)*s);
		else fputc(*s, stderr);
	fputc('"', stderr);
}

void check_fail(const char *file, int line, const char *what)
{
	checks_failed++;
	fprintf(stderr, "%s:%d: %s: FAILED: %s\n", file, line, test_name, what);
}

void check_str_fail(const char *file, int line, const char *expr, const char *want, const char *got)
{
	checks_failed++;
	fprintf(stderr, "%s:%d: %s: FAILED: %s\n    wanted ", file, line, test_name, expr);
	show(want);
	fputs("\n    got    ", stderr);
	show(got);
	fputc('\n', stderr);
}

int utf8_valid(const char *s, size_t n)
{
	const unsigned char *p = (const unsigned char *)s;
	for (size_t i = 0; i < n; ) {
		unsigned c = p[i];
		size_t k = c < 0x80 ? 0 : (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : 9;
		if (k == 9 || (k && c < 0xC2) || i + k >= n + (k ? 0 : 1)) return 0;
		for (size_t j = 1; j <= k; j++)
			if ((p[i + j] & 0xC0) != 0x80) return 0;
		i += k + 1;
	}
	return 1;
}

static const struct { const char *name; void (*run)(void); } groups[] = {
	{"util", run_util}, {"mime", run_mime}, {"textconv", run_textconv},
	{"config", run_config}, {"vault", run_vault}, {"fuzz", run_fuzz},
};

int main(int argc, char **argv)
{
	int ran = 0;
	for (size_t g = 0; g < sizeof groups / sizeof *groups; g++) {
		int want = argc < 2;
		for (int i = 1; i < argc; i++)
			if (!strcmp(argv[i], groups[g].name)) want = 1;
		if (!want) continue;
		int before = checks_failed, n = checks_run;
		groups[g].run();
		printf("  %-9s %5d checks  %s\n", groups[g].name, checks_run - n,
		       checks_failed > before ? "FAILED" : "ok");
		ran++;
	}
	if (!ran) { fprintf(stderr, "no such test group\n"); return 2; }
	printf("%d checks, %d failed\n", checks_run, checks_failed);
	return checks_failed ? 1 : 0;
}
