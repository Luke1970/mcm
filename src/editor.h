/* editor.h - full-screen vim-like message editor
 *
 * The text is the header block, an empty line and the body. They are shown
 * in two windows: the header window opens first; Esc closes it and the body
 * is edited below the (still visible) header; Esc again gives command mode
 * with the menu  EdIt Send Keep Header Wrap aTtach  and vim keys. aTtach (T)
 * picks a file and adds an "Attach: PATH" header line for it. Edit modes show
 * "Press Esc" and the message's To and Subject in the menu bar. With Wrap on
 * (menu item, W), body lines are wrapped at wrap_width columns while typing;
 * the space left at the end of a wrapped line marks a soft break
 * (format=flowed, RFC 3676). gq re-wraps a paragraph.
 */
#ifndef MCM_EDITOR_H
#define MCM_EDITOR_H

#include <stddef.h>

/* An action on the whole text. Return 1 to close the editor, 0 to keep
 * editing; on a problem fill msg and set *line (0-based) or leave it -1. */
typedef int (*editor_fn)(void *ctx, const char *text, size_t len, int *line, char *msg,
                         size_t msglen);

typedef struct {
	const char *title;     /* "New message", "Reply", "Forward" */
	editor_fn   send;      /* S */
	editor_fn   keep;      /* K: save as draft */
	void       *ctx;
	int         wrap;        /* Wrap on at the start (new mail); W toggles it */
	int         wrap_width;  /* column to wrap at (config wrap_width, default 72) */
} editor_cfg;

/* Edit until sent, kept or abandoned. Starts in the header window;
 * start_line (0-based in the whole text) places the cursor there. */
void editor_run(const editor_cfg *cfg, const char *text, int start_line);

/* If a message is being edited and was changed, save its text in
 * ~/.local/share/mcm/unsent-DATE.txt (path returned; "" when nothing was
 * saved). Used when Ctrl-Q or Ctrl-C quits mcm from inside the editor. */
int editor_save_unsent(char *path, size_t n);

/* Redraw the editor screen (background for dialogs opened by callbacks). */
void editor_redraw(void);

#endif
