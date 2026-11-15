/* theme.c - color themes loaded from NAME.theme files
 *
 * File format, one setting per line ('#' starts a comment):
 *
 *     BACKGROUND   = blue
 *     HIGHLIGHTBG  = cyan
 *     HIGHLIGHTTXT = black
 *     TEXT         = white
 *     MENUBG       = lightgray
 *     MENUTXT      = black
 *
 * Colors: black red green yellow(brown) blue magenta cyan white,
 * with an optional "bright"/"light" prefix, gray/darkgray, darkblue, orange, "default"
 * (terminal default), a number 0-255, or #RRGGBB.
 *
 * #RRGGBB (and darkblue, #000040) is shown exactly on a 24-bit color terminal
 * (see ui_init); elsewhere it is the nearest of the 256 colors.
 */
#define NCURSES_WIDECHAR 1
#include "theme.h"
#include "ui.h"
#include "util.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef _WIN32
#undef DATADIR
#define DATADIR win_exe_dir()          /* themes\ next to mcm.exe */
#endif
#ifndef DATADIR
#define DATADIR "/usr/local/share/mcm"
#endif

void theme_defaults(theme_t *t)
{
	/* PC Tools inspired: white on blue, cyan selection bar, gray menus */
	t->background = DARKBLUE;         /* #000040; blue on 8-color terminals */
	t->text = COLOR_WHITE;
	t->highlightbg = COLOR_CYAN;
	t->highlighttxt = COLOR_BLACK;
	t->menubg = COLOR_WHITE;
	t->menutxt = COLOR_BLACK;
	t->hotkey = 208;                  /* orange; yellow on 8-color terminals */
}

static int parse_color(const char *in, int *out)
{
	char s[32];
	size_t n = 0;
	for (; *in && n < sizeof s - 1; in++)
		if (!isspace((unsigned char)*in) && *in != '_' && *in != '-')
			s[n++] = (char)toupper((unsigned char)*in);
	s[n] = '\0';
	if (!n) return -1;

	if (s[0] == '#') {
		char *e;
		long v = strtol(s + 1, &e, 16);
		if (n != 7 || *e || v < 0) return -1;
		*out = THEME_RGB | (int)v;
		return 0;
	}
	if (isdigit((unsigned char)s[0])) {
		char *e;
		long v = strtol(s, &e, 10);
		if (*e || v < 0 || v > 255) return -1;
		*out = (int)v;
		return 0;
	}

	static const struct { const char *name; int v; } names[] = {
		{"BLACK", 0}, {"RED", 1}, {"GREEN", 2}, {"YELLOW", 3}, {"BROWN", 3},
		{"BLUE", 4}, {"MAGENTA", 5}, {"CYAN", 6}, {"WHITE", 15}, {"LIGHTGRAY", 7},
		{"LIGHTGREY", 7}, {"GRAY", 7}, {"GREY", 7}, {"DARKGRAY", 8}, {"DARKGREY", 8},
		{"DARKBLUE", DARKBLUE}, {"NAVY", 18}, {"ORANGE", 208},
		{"DEFAULT", -1},
	};
	const char *p = s;
	int bright = 0;
	if (!strncmp(p, "BRIGHT", 6)) { bright = 8; p += 6; }
	else if (!strncmp(p, "LIGHT", 5) && strcmp(p, "LIGHTGRAY") && strcmp(p, "LIGHTGREY")) {
		bright = 8; p += 5;
	}

	for (size_t i = 0; i < sizeof names / sizeof *names; i++) {
		if (!strcmp(p, names[i].name)) {
			int v = names[i].v;
			if (v == 15 && !bright) v = 7;      /* plain WHITE = color 7 */
			else if (v >= 0 && v < 8) v += bright;
			*out = v;
			return 0;
		}
	}
	return -1;
}

int theme_load(theme_t *t, const char *path, char *err, size_t errlen)
{
	FILE *f = fopen(path, "r");
	if (!f) {
		snprintf(err, errlen, "cannot open theme %s", path);
		return -1;
	}

	char line[256];
	int lineno = 0, rc = 0;
	while (fgets(line, sizeof line, f)) {
		lineno++;
		char *s = str_trim(line);
		if (!*s || *s == '#') continue;

		char *k = s;
		size_t kl = strcspn(k, " \t=:#");
		char *v = k + kl;
		while (*v && strchr(" \t=:", *v)) v++;
		k[kl] = '\0';
		/* '#' starts a comment, except in a #RRGGBB value */
		char *h = strchr(v + (*v == '#' && strspn(v + 1, "0123456789abcdefABCDEF") == 6), '#');
		if (h) *h = '\0';
		v = str_trim(v);

		int *slot = NULL;
		if (!strcasecmp(k, "BACKGROUND")) slot = &t->background;
		else if (!strcasecmp(k, "HIGHLIGHTBG")) slot = &t->highlightbg;
		else if (!strcasecmp(k, "HIGHLIGHTTXT")) slot = &t->highlighttxt;
		else if (!strcasecmp(k, "TEXT")) slot = &t->text;
		else if (!strcasecmp(k, "MENUBG")) slot = &t->menubg;
		else if (!strcasecmp(k, "MENUTXT")) slot = &t->menutxt;
		else if (!strcasecmp(k, "HOTKEY")) slot = &t->hotkey;

		int c;
		if (!slot) {
			snprintf(err, errlen, "%s:%d: unknown setting '%s'", path, lineno, k);
			rc = -1;
		} else if (parse_color(v, &c) < 0) {
			snprintf(err, errlen, "%s:%d: invalid color '%s'", path, lineno, v);
			rc = -1;
		} else {
			*slot = c;
		}
	}
	fclose(f);
	return rc;
}

static int readable(const char *p) { return access(p, R_OK) == 0; }

char *theme_find(const char *name)
{
	char p[1024];
	const char *home = getenv("HOME");
	const char *xdg = getenv("XDG_CONFIG_HOME");

	if (strchr(name, '/') || (strlen(name) > 6 && !strcmp(name + strlen(name) - 6, ".theme")))
		return readable(name) ? xstrdup(name) : NULL;

	if (xdg && *xdg) {
		snprintf(p, sizeof p, "%s/mcm/%s.theme", xdg, name);
		if (readable(p)) return xstrdup(p);
	} else if (home) {
		snprintf(p, sizeof p, "%s/.config/mcm/%s.theme", home, name);
		if (readable(p)) return xstrdup(p);
	}
	snprintf(p, sizeof p, "themes/%s.theme", name);
	if (readable(p)) return xstrdup(p);
	snprintf(p, sizeof p, "%s/themes/%s.theme", DATADIR, name);
	if (readable(p)) return xstrdup(p);
	return NULL;
}

static const char DEFAULT_TEXT[] =
	"# mcm color theme. mcm reads this file every time it starts:\n"
	"# change a color here and start mcm again.\n"
	"#\n"
	"# Colors: black red green yellow blue magenta cyan white,\n"
	"#         optional \"bright\"/\"light\" prefix, gray, darkgray, darkblue, navy,\n"
	"#         orange, default (terminal default), a number 0-255 or #RRGGBB.\n"
	"BACKGROUND   = darkblue    # screen background\n"
	"TEXT         = white       # normal text\n"
	"HIGHLIGHTBG  = cyan        # selection bar\n"
	"HIGHLIGHTTXT = black\n"
	"MENUBG       = lightgray   # menu bar, status line and dialogs\n"
	"MENUTXT      = black\n"
	"HOTKEY       = orange      # shortcut letters\n";

static int copy_file(const char *from, const char *to)
{
	FILE *in = fopen(from, "r");
	if (!in) return -1;
	FILE *out = fopen(to, "wx");
	if (!out) { fclose(in); return -1; }
	char b[4096];
	size_t n;
	while ((n = fread(b, 1, sizeof b, in)) > 0) fwrite(b, 1, n, out);
	fclose(in);
	int bad = ferror(out);
	if (fclose(out) != 0 || bad) { unlink(to); return -1; }
	return 0;
}

char *theme_user_file(const char *name, int *created)
{
	*created = 0;
	if (!*name || strchr(name, '/') || (strlen(name) > 6 && !strcmp(name + strlen(name) - 6, ".theme")))
		return NULL;
	const char *home = getenv("HOME"), *xdg = getenv("XDG_CONFIG_HOME");
	char dir[900], p[1024];
	if (xdg && *xdg) snprintf(dir, sizeof dir, "%s/mcm", xdg);
	else if (home) {
		snprintf(dir, sizeof dir, "%s/.config", home);
		mkdir(dir, 0700);
		snprintf(dir, sizeof dir, "%s/.config/mcm", home);
	} else return NULL;
	snprintf(p, sizeof p, "%s/%s.theme", dir, name);
	if (readable(p)) return xstrdup(p);

	mkdir(dir, 0700);
	char src[1024];
	snprintf(src, sizeof src, "themes/%s.theme", name);
	if (!readable(src)) snprintf(src, sizeof src, "%s/themes/%s.theme", DATADIR, name);
	if (readable(src)) {
		if (copy_file(src, p) != 0) return NULL;
	} else if (!strcmp(name, "default")) {
		FILE *f = fopen(p, "wx");
		if (!f) return NULL;
		fputs(DEFAULT_TEXT, f);
		if (fclose(f) != 0) { unlink(p); return NULL; }
	} else {
		return NULL;
	}
	*created = 1;
	return xstrdup(p);
}

/* Nearest xterm-256 color to 0xRRGGBB. */
static int nearest256(int rgb)
{
	static const int lv[6] = {0, 95, 135, 175, 215, 255};
	int c[3] = {rgb >> 16 & 255, rgb >> 8 & 255, rgb & 255}, q[3];
	long dc = 0, dg;
	for (int i = 0; i < 3; i++) {
		int best = 0;
		for (int j = 1; j < 6; j++)
			if (abs(lv[j] - c[i]) < abs(lv[best] - c[i])) best = j;
		q[i] = best;
		dc += (long)(lv[best] - c[i]) * (lv[best] - c[i]);
	}
	int avg = (c[0] + c[1] + c[2]) / 3, g = avg < 8 ? 0 : (avg - 8) / 10;
	if (g > 23) g = 23;
	int gv = 8 + 10 * g;
	dg = 0;
	for (int i = 0; i < 3; i++) dg += (long)(gv - c[i]) * (gv - c[i]);
	return dg < dc ? 232 + g : 16 + 36 * q[0] + 6 * q[1] + q[2];
}

/* Nearest of the 8 basic colors for an xterm-256 color number. */
static int basic8(int c)
{
	if (c < 16) return c & 7;
	if (c >= 232) return c < 244 ? COLOR_BLACK : COLOR_WHITE;   /* gray ramp */
	c -= 16;                                                      /* 6x6x6 cube */
	int r = c / 36 >= 2, g = c / 6 % 6 >= 2, b = c % 6 >= 2;
	if (!r && !g && !b) r = c / 36 > 0, g = c / 6 % 6 > 0, b = c % 6 > 0;
	return r | g << 1 | b << 2;
}

/* 0xRRGGBB of xterm-256 color c (8-255). */
static int xterm_rgb(int c)
{
	static const int bright[8] = {0x808080, 0xff0000, 0x00ff00, 0xffff00,
	                              0x5c5cff, 0xff00ff, 0x00ffff, 0xffffff};
	static const int lv[6] = {0, 95, 135, 175, 215, 255};
	if (c < 16) return bright[c - 8];
	if (c >= 232) { int g = 8 + 10 * (c - 232); return g << 16 | g << 8 | g; }
	c -= 16;
	return lv[c / 36] << 16 | lv[c / 6 % 6] << 8 | lv[c % 6];
}

/* Color number for theme color c on this terminal. */
static int fit(int c)
{
	if (c < 0) return -1;
	if (COLORS >= 0x1000000) {               /* direct color: 0-7 palette, else RGB */
		if (c & THEME_RGB) return c & 0xffffff;
		return c < 8 ? c : xterm_rgb(c);
	}
	if (c & THEME_RGB) c = nearest256(c & 0xffffff);
	if (c >= COLORS) c = basic8(c);
	return c;
}

void theme_apply(const theme_t *t)
{
	if (!has_colors()) return;
	start_color();
	use_default_colors();
	init_extended_pair(CP_NORMAL, fit(t->text), fit(t->background));
	init_extended_pair(CP_HIGHLIGHT, fit(t->highlighttxt), fit(t->highlightbg));
	init_extended_pair(CP_MENU, fit(t->menutxt), fit(t->menubg));
	init_extended_pair(CP_HOTKEY, fit(t->hotkey), fit(t->menubg));
	init_extended_pair(CP_HOTKEY_HL, fit(t->hotkey), fit(t->highlightbg));
}
