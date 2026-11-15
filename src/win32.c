/* win32.c - POSIX functions emsg uses that Windows lacks (see compat.h).
 * Only built for Windows. */
#ifdef _WIN32

#include <winsock2.h>
#include <windows.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void slashes(char *s)
{
	for (; *s; s++)
		if (*s == '\\') *s = '/';
}

/* An environment variable from Windows, with '/' as separator. */
static void env_from(const char *name, const char *from, const char *sub)
{
	const char *v = getenv(name);
	if (v && *v) return;
	const char *f = getenv(from);
	if (!f || !*f) return;
	char buf[1024];
	snprintf(buf, sizeof buf, "%s%s", f, sub);
	slashes(buf);
	_putenv_s(name, buf);
}

/* Before main: files are binary (no \n -> \r\n), the console speaks UTF-8,
 * the configuration goes to %APPDATA%\emsg, data to %LOCALAPPDATA%\emsg,
 * and sockets work. */
__attribute__((constructor)) static void win_start(void)
{
	_fmode = _O_BINARY;
	_setmode(0, _O_BINARY);
	_setmode(1, _O_BINARY);
	_setmode(2, _O_BINARY);
	SetConsoleOutputCP(CP_UTF8);
	SetConsoleCP(CP_UTF8);
	env_from("HOME", "USERPROFILE", "");
	env_from("XDG_CONFIG_HOME", "APPDATA", "");
	env_from("XDG_DATA_HOME", "LOCALAPPDATA", "");
	WSADATA wd;
	WSAStartup(MAKEWORD(2, 2), &wd);
}

const char *win_exe_dir(void)
{
	static char dir[MAX_PATH * 3];
	if (!*dir) {
		DWORD n = GetModuleFileNameA(NULL, dir, sizeof dir);
		if (n == 0 || n >= sizeof dir) snprintf(dir, sizeof dir, ".");
		slashes(dir);
		char *sl = strrchr(dir, '/');
		if (sl) *sl = '\0';
	}
	return dir;
}

int win_rename(const char *from, const char *to)
{
	if (MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return 0;
	DWORD e = GetLastError();
	errno = e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND ? ENOENT
	      : e == ERROR_ACCESS_DENIED || e == ERROR_SHARING_VIOLATION ? EACCES : EIO;
	return -1;
}

char *win_realpath(const char *path, char *resolved)
{
	struct stat st;
	if (stat(path, &st) != 0) return NULL;
	if (!_fullpath(resolved, path, 4096)) return NULL;
	slashes(resolved);
	return resolved;
}

ssize_t win_pread(int fd, void *buf, size_t n, off_t at)
{
	HANDLE h = (HANDLE)_get_osfhandle(fd);
	if (h == INVALID_HANDLE_VALUE) { errno = EBADF; return -1; }
	OVERLAPPED ov = {0};
	ov.Offset = (DWORD)((unsigned long long)at & 0xffffffffu);
	ov.OffsetHigh = (DWORD)((unsigned long long)at >> 32);
	DWORD got = 0;
	if (!ReadFile(h, buf, (DWORD)n, &got, &ov)) {
		if (GetLastError() == ERROR_HANDLE_EOF) return 0;
		errno = EIO;
		return -1;
	}
	return (ssize_t)got;
}

int win_gethostname(char *buf, size_t n)
{
	DWORD k = (DWORD)n;
	if (!GetComputerNameA(buf, &k)) {
		snprintf(buf, n, "localhost");
		return -1;
	}
	return 0;
}

ssize_t win_getline(char **line, size_t *cap, FILE *f)
{
	size_t n = 0;
	int c;
	if (!*line || !*cap) {
		*cap = 128;
		*line = realloc(*line, *cap);
		if (!*line) return -1;
	}
	while ((c = getc(f)) != EOF) {
		if (n + 2 > *cap) {
			char *p = realloc(*line, *cap * 2);
			if (!p) return -1;
			*line = p;
			*cap *= 2;
		}
		(*line)[n++] = (char)c;
		if (c == '\n') break;
	}
	(*line)[n] = '\0';
	return n ? (ssize_t)n : -1;
}

char *win_strcasestr(const char *hay, const char *needle)
{
	size_t nl = strlen(needle);
	for (; *hay; hay++)
		if (!_strnicmp(hay, needle, nl)) return (char *)hay;
	return nl ? NULL : (char *)hay;
}

void *win_memmem(const void *hay, size_t hn, const void *needle, size_t nn)
{
	const char *h = hay;
	if (!nn) return (void *)h;
	for (size_t i = 0; i + nn <= hn; i++)
		if (h[i] == *(const char *)needle && !memcmp(h + i, needle, nn)) return (void *)(h + i);
	return NULL;
}

/* PDCurses gives every character one screen cell, so does this. */
int win_wcwidth(wchar_t c)
{
	return c < 32 || c == 0x7F ? -1 : 1;
}

int win_vasprintf(char **out, const char *fmt, va_list ap)
{
	va_list cp;
	va_copy(cp, ap);
	int n = vsnprintf(NULL, 0, fmt, cp);
	va_end(cp);
	*out = NULL;
	if (n < 0) return -1;
	*out = malloc((size_t)n + 1);
	if (!*out) return -1;
	return vsnprintf(*out, (size_t)n + 1, fmt, ap);
}

int win_asprintf(char **out, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int n = win_vasprintf(out, fmt, ap);
	va_end(ap);
	return n;
}

int win_run(const char *cmdline)
{
	STARTUPINFOA si = {.cb = sizeof si};
	PROCESS_INFORMATION pi;
	char *cl = _strdup(cmdline);
	if (!cl) return -1;
	BOOL ok = CreateProcessA(NULL, cl, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi);
	free(cl);
	if (!ok) return -1;
	WaitForSingleObject(pi.hProcess, INFINITE);
	DWORD code = 1;
	GetExitCodeProcess(pi.hProcess, &code);
	CloseHandle(pi.hProcess);
	CloseHandle(pi.hThread);
	return (int)code;
}

int win_lock(int fd)
{
	HANDLE h = (HANDLE)_get_osfhandle(fd);
	OVERLAPPED ov = {0};
	if (h != INVALID_HANDLE_VALUE && LockFileEx(h, LOCKFILE_EXCLUSIVE_LOCK, 0, MAXDWORD, MAXDWORD, &ov))
		return 0;
	errno = EACCES;
	return -1;
}

void win_unlock(int fd)
{
	HANDLE h = (HANDLE)_get_osfhandle(fd);
	OVERLAPPED ov = {0};
	if (h != INVALID_HANDLE_VALUE) UnlockFileEx(h, 0, MAXDWORD, MAXDWORD, &ov);
}

#endif
