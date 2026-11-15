/* ui.h - ncurses drawing primitives and dialogs */
#ifndef MCM_UI_H
#define MCM_UI_H

#ifndef NCURSES_WIDECHAR
#define NCURSES_WIDECHAR 1
#endif
#include <curses.h>
#include <stddef.h>

/* Older ncurses (before 6.1): no extended color pairs
 * (only needed for 24-bit color), and no wheel-down button. */
#if !defined(NCURSES_VERSION_MAJOR) || NCURSES_VERSION_MAJOR < 6 || \
    (NCURSES_VERSION_MAJOR == 6 && NCURSES_VERSION_MINOR < 1)
#define init_extended_pair(p, f, b) init_pair((short)(p), (short)(f), (short)(b))
#endif
#ifndef BUTTON5_PRESSED
#define BUTTON5_PRESSED 0
#endif
#include <wchar.h>

#include "theme.h"

#define UI_HOT_CAPS (-2)
#define UI_HOT_KEYNAME (-3)   /* hotkey is the text before '=', e.g. "F1=Help" */

/* Ctrl-Q and Ctrl-C quit mcm from anywhere: every key is read with
 * ui_getch() or ui_get_wch(), which call the function set here (it does not
 * return) on Ctrl-Q, or once Ctrl-C has set quit_requested (util.h). Ctrl-C
 * also stops network waits; a second Ctrl-C leaves at once. */
void ui_set_quit(void (*fn)(void));
int  ui_getch(void);
int  ui_get_wch(wint_t *wc);

/* Key codes of Ctrl-Left and Ctrl-Right (set by ui_init). */
extern int ui_key_cleft, ui_key_cright;
/* Shift-Space, for terminals that report it (kitty, xterm modifyOtherKeys). */
extern int ui_key_sspace;

/* While dragging with the mouse: also report movement with the button held
 * (on = 1 at the press, 0 at the release). */
void ui_mouse_drag(int on);

enum { CP_NORMAL = 1, CP_HIGHLIGHT, CP_MENU, CP_HOTKEY, CP_HOTKEY_HL };

typedef void (*ui_bg_fn)(void);   /* redraws the screen behind a dialog */

typedef struct {
	const char *label;
	char       *buf;
	size_t      size;
	int         secret;
	int         optional;   /* may be left empty */
} ui_field;

typedef struct {
	const char *label;
	int         hot;    /* index of the hotkey letter, -1 none, UI_HOT_CAPS all capitals */
	int         key;    /* key code a mouse click on this item produces */
} ui_menuitem;

void ui_init(const theme_t *t);
void ui_end(void);

int  ui_wcwidth(wchar_t c);
/* Print text clipped/padded to exactly `width` columns. */
int  ui_putw(int y, int x, const wchar_t *s, int width, attr_t attr, short pair);
int  ui_puts(int y, int x, const char *s, int width, attr_t attr, short pair);
void ui_fill(int y, int x, int h, int w, short pair);
void ui_box(int y, int x, int h, int w, short pair, const char *title, attr_t tattr, short tpair);
/* Key hint in the bottom border of a box, right-aligned. */
void ui_box_hint(int y, int x, int h, int w, const char *hint, short pair);

/* Top menu bar with hotkeys and clock on the right. */
/* active: bit i set = item i is drawn in inverted colors (a mode that is on) */
void ui_menubar(const ui_menuitem *items, int n, unsigned active);
/* Key of the menu item at screen column x on the menu bar, or 0. */
int  ui_menu_hit(const ui_menuitem *items, int n, int x);

/* Modal dialogs. */
int  ui_input_dialog(const char *title, ui_field *f, int nf, const char *msg, ui_bg_fn bg);
void ui_progress(const char *msg, ui_bg_fn bg);
void ui_message(const char *title, const char *msg, ui_bg_fn bg);
int  ui_confirm(const char *title, const char *msg, ui_bg_fn bg);
void ui_text_dialog(const char *title, const char *const *lines, int n, ui_bg_fn bg);
/* Text with a row of buttons. Arrows/Tab move, Enter picks, a button's hot
 * letter or a mouse click picks it directly. Returns the button index, or
 * -1 for Esc. `warn` draws the title in the highlight color. */
int  ui_choice_dialog(const char *title, const char *const *lines, int n,
                      const ui_menuitem *btn, int nb, int def, int warn, ui_bg_fn bg);
/* Pick one of n lines: arrows, Enter or a click picks, Esc gives -1. */
int  ui_list_dialog(const char *title, const char *const *items, int n, int def, const char *hint,
                    ui_bg_fn bg);

/* Pick a file: folders open, a file is chosen (its full path, malloc'd);
 * NULL for Esc. dir is where to start ("" = the current directory) and is
 * left at the chosen file's folder. */
char *ui_file_dialog(const char *title, char *dir, size_t dirsize, ui_bg_fn bg);

#endif
