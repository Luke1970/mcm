/* check.h - a very small test framework for "make test"
 *
 * A test is a function void t_name(void) that uses the CHECK macros; each
 * file lists its tests in a run_xxx() function (called from tests/main.c).
 * A failed check prints file:line, what was expected and what came out, and
 * the test goes on; main returns 1 when any check failed.
 */
#ifndef MCM_CHECK_H
#define MCM_CHECK_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int checks_run, checks_failed;
extern const char *test_name;

void check_fail(const char *file, int line, const char *what);
void check_str_fail(const char *file, int line, const char *expr, const char *want, const char *got);
void check_free(void *p);

#define RUN(t) do { test_name = #t; t(); } while (0)

#define CHECK(c) do { checks_run++; \
	if (!(c)) check_fail(__FILE__, __LINE__, #c); } while (0)

#define CHECK_INT(a, b) do { long long a_ = (a), b_ = (b); checks_run++; \
	if (a_ != b_) { char m_[256]; \
		snprintf(m_, sizeof m_, "%s == %s: %lld, wanted %lld", #a, #b, a_, b_); \
		check_fail(__FILE__, __LINE__, m_); } } while (0)

/* Compare a string with the expected one; got may be NULL. */
#define CHECK_STR(got, want) do { const char *g_ = (got), *w_ = (want); checks_run++; \
	if (!g_ || !w_ ? g_ != w_ : strcmp(g_, w_)) \
		check_str_fail(__FILE__, __LINE__, #got, w_, g_); } while (0)

/* The same, and free got (a malloc'd result). */
#define CHECK_STR_FREE(got, want) do { char *f_ = (got); CHECK_STR(f_, want); free(f_); } while (0)

/* got contains want */
#define CHECK_HAS(got, want) do { const char *g_ = (got), *w_ = (want); checks_run++; \
	if (!g_ || !strstr(g_, w_)) check_str_fail(__FILE__, __LINE__, #got " contains", w_, g_); } while (0)

#define CHECK_NOT_HAS(got, want) do { const char *g_ = (got), *w_ = (want); checks_run++; \
	if (g_ && strstr(g_, w_)) check_str_fail(__FILE__, __LINE__, #got " does not contain", w_, g_); } while (0)

/* Valid UTF-8 (no bytes that are not part of a correct sequence) */
int utf8_valid(const char *s, size_t n);

void run_util(void);
void run_mime(void);
void run_textconv(void);
void run_config(void);
void run_vault(void);
void run_fuzz(void);

#endif
