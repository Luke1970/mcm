/* compat.h - what Windows (MinGW-w64) lacks of POSIX.
 *
 * Built for Windows, this header is included first in every file (gcc
 * -include). It pulls in the C library headers that declare mkdir, rename and
 * the like, and only then replaces those names, so the declarations stay as
 * they are. The functions live in win32.c. On other systems it is empty. */
#ifndef MCM_COMPAT_H
#define MCM_COMPAT_H

#ifdef _WIN32

#ifndef _POSIX_THREAD_SAFE_FUNCTIONS
#define _POSIX_THREAD_SAFE_FUNCTIONS 200112L   /* localtime_r, gmtime_r */
#endif

#include <direct.h>
#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <process.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>

/* curses.h: PDCurses with wide characters, UTF-8, ncurses mouse calls and
 * ANSI color numbers (red = 1), linked in (the Makefile builds it the same) */
#define PDC_WIDE 1
#define PDC_FORCE_UTF8 1
#define PDC_NCMOUSE 1
#define PDC_RGB 1

#define MCM_PRINTF gnu_printf

/* Maildir: ':' cannot be in a Windows file name; "!" is what other Windows
 * Maildir programs use. */
#define MAILDIR_SEP '!'

#define mkdir(p, m) _mkdir(p)
#define fsync(fd) _commit(fd)
#define lstat stat
#ifndef S_ISLNK
#define S_ISLNK(m) 0
#endif
#define timegm _mkgmtime
#define realpath(p, r) win_realpath(p, r)
#define rename(a, b) win_rename(a, b)
#define pread(fd, b, n, o) win_pread(fd, b, n, o)

int     win_rename(const char *from, const char *to);   /* replaces an existing file */
char   *win_realpath(const char *path, char *resolved);
ssize_t win_pread(int fd, void *buf, size_t n, off_t at);
int     win_gethostname(char *buf, size_t n);   /* the computer name */
#define getline win_getline
#define strcasestr win_strcasestr
#define memmem win_memmem
#define wcwidth win_wcwidth
#define asprintf win_asprintf
#define vasprintf win_vasprintf
ssize_t win_getline(char **line, size_t *cap, FILE *f);
char   *win_strcasestr(const char *hay, const char *needle);
void   *win_memmem(const void *hay, size_t hn, const void *needle, size_t nn);
int     win_wcwidth(wchar_t c);
int     win_asprintf(char **out, const char *fmt, ...) __attribute__((format(gnu_printf, 2, 3)));
int     win_vasprintf(char **out, const char *fmt, va_list ap) __attribute__((format(gnu_printf, 2, 0)));

/* Directory of mcm.exe, with '/' (themes are looked for there). */
const char *win_exe_dir(void);
/* Run a command line (program and arguments, as typed) and wait for it;
 * returns its exit code, or -1 if it could not be started. */
int win_run(const char *cmdline);
/* Lock a whole file for writing (waits); unlock again. */
int win_lock(int fd);
void win_unlock(int fd);

#endif
#endif
