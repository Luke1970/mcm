/* theme.h - color themes loaded from NAME.theme files */
#ifndef MCM_THEME_H
#define MCM_THEME_H

#include <stddef.h>

#define THEME_RGB 0x1000000            /* flag: the value is 0xRRGGBB */
#define DARKBLUE  (THEME_RGB | 0x000040)

typedef struct {
	int background;    /* BACKGROUND   */
	int highlightbg;   /* HIGHLIGHTBG  */
	int highlighttxt;  /* HIGHLIGHTTXT */
	int text;          /* TEXT         */
	int menubg;        /* MENUBG       */
	int menutxt;       /* MENUTXT      */
	int hotkey;        /* HOTKEY: shortcut letters (default orange) */
} theme_t;

void  theme_defaults(theme_t *t);
/* Find NAME.theme in the usual places (or use NAME as a path). malloc'd or NULL. */
char *theme_find(const char *name);
/* The user's editable copy ~/.config/mcm/NAME.theme. Created when missing:
 * copied from the installed theme NAME, or the built-in colors for
 * "default". malloc'd path, or NULL (NAME is a path or unknown). */
char *theme_user_file(const char *name, int *created);
/* Load a theme file on top of the current values. Returns 0, or -1 with err set. */
int   theme_load(theme_t *t, const char *path, char *err, size_t errlen);
/* Initialise curses color pairs from the theme (after initscr). */
void  theme_apply(const theme_t *t);

#endif
