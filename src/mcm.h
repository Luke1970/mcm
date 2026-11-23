/* mcm.h - what the parts of mcm (main.c and the files next to it) share:
 * the program state A, the accounts, settings and window layout, and the
 * functions one part calls in another.
 *
 *   main.c     startup, command line, login, switching accounts, main loop
 *   config.c   mcm.conf: reading and writing it, the list of accounts
 *   folder.c   message list cache, opening and refreshing folders, filter
 *   screen.c   drawing the windows, column layout and borders, scroll bars
 *   msgwin.c   the message window: text, wrap, hex, ':' commands, save/print
 *   attach.c   attachments (in the K window)
 *   keys.c     keys, mouse clicks in the windows, help window
 *   certs.c    untrusted certificates
 *   account.c  accounts, stored passwords, Config menu, SMTP settings
 *   send.c     writing, sending and keeping mail
 *   pgp.c      OpenPGP through gpg: signatures, encryption
 */
#ifndef MCM_H
#define MCM_H

#include "compose.h"
#include "editor.h"
#include "store.h"
#include "mime.h"
#include "msgview.h"
#include "smtp.h"
#include "theme.h"
#include "ui.h"
#include "util.h"
#include "vault.h"

#include <ctype.h>
#include <wctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <locale.h>
#include <signal.h>
#include <stdarg.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#else
#include <sys/wait.h>
#endif

#define MCM_VERSION   "0.996"
#define LOAD_DELAY_MS  200      /* load message after selection rests this long */
#define CHUNK          100      /* message headers fetched per block */
#define MAX_CHUNKS     64       /* header blocks kept in memory (least recently used dropped) */

/* The windows: f folders, l email list, e message, a accounts. */
enum { COL_FOLDERS, COL_LIST, COL_MSG, COL_ACCTS };

/* Narrowest a window left of the message may be (columns). */
#define MIN_LEFT 25
/* Wrapped message lines are at most this wide (W); Left/Right move
 * unwrapped text this many columns. */
#define WRAP_VIEW 76
#define HSTEP 8
/* The letters on the message window's bottom border (keys, and clicks). */
#define HWXSP "HWXSPTK"

typedef struct { uint32_t off, len; } line_t;   /* one display line of the message text */

struct chunk {
	uint32_t      first;    /* sequence number of m[0]; 0 = slot unused */
	int           n;
	msg_t        *m;
	unsigned long used;
};

/* One mail account; all of them are kept in mcm.conf. */
typedef struct {
	char  name[64];            /* [account NAME] in the config file */
	int   type;                /* STORE_IMAP, STORE_POP3 or STORE_LOCAL (store.h) */
	char  path[512];           /* LOCAL: the mailboxes; POP3: where Sent/Drafts are kept */
	char  fullname[128];       /* display name in From: */
	char  host[256], port[16], user[256];
	char  smtp_host[256], smtp_port[16], smtp_user[256];
	char  smtp_login[600];     /* "user@host" of the IMAP login the SMTP settings belong to */
	char  smtp_security[16];   /* "tls", "starttls" or "" = by port (465 = tls) */
	int   store;               /* STORE_ASK / STORE_YES / STORE_NO: keep the password on disk */
	char *pass_enc;            /* encrypted IMAP password (vault.h), or NULL */
	char *smtp_pass_enc;       /* encrypted SMTP password, or NULL = use the IMAP one */
	char  pgp[24];             /* PGP (pgp.c): "" / "no" off, "yes" on, or what new mail gets:
	                            * "sign", "encrypt", "sign encrypt" */
	char  pgp_key[256];        /* key to sign with (and encrypt to yourself); "": the address */
} account_t;

enum { STORE_ASK, STORE_YES, STORE_NO };
#define MAX_ACCOUNTS 64

extern account_t  accts[MAX_ACCOUNTS];
extern int        naccts;
extern account_t *ac;              /* the account in use */
extern char       last_account[64];
extern char      *master_check;    /* encrypted known text: verifies the master password */
extern char      *master;          /* master password, in memory only once typed */

/* Settings of the program (mcm.conf, before the accounts) */
extern int wrap_width;             /* new mail is wrapped at this column */
extern int check_mail;             /* seconds between checks for new mail when idle; 0 = only F5 */
/* Programs from mcm.conf; empty: the default. pdf_viewer opens PDF files,
 * html_viewer web links (the editor: cfg_editor in ui.h). */
extern char cfg_pdf_viewer[256], cfg_html_viewer[256];
extern char cfg_pdf_viewer_console[256], cfg_html_viewer_console[256];
extern int col_pct[3];  /* window widths in %: [0] Folders (of
                                        * the screen), [1] the list (of what is
                                        * left of it), [2] Accounts (4 columns) */
extern int dragging;               /* mouse drag: DRAG_BORDER or DRAG_THUMB */
enum { DRAG_BORDER = 1, DRAG_THUMB };
extern int drag_border;            /* which border (1 or 2) is being dragged */
/* Scroll bars in the right border of each window (as last drawn), indexed
 * by COL_*: shown only when the window has more lines than rows. */
typedef struct {
	int x, y, h;     /* column, first row, rows (0: no scroll bar) */
	int t0, tlen;    /* track: first row and length (between the arrows) */
	int th, tsize;   /* thumb: first row and length */
	int grab;        /* while dragging the thumb: row offset of the grab point */
	int n, rows;     /* lines in the window, rows shown */
	int *top;        /* the window's first line */
	int horiz;       /* along the bottom border: x and columns instead of y and rows */
} sbar_t;
/* sbs[SB_HMSG]: the message's horizontal scroll bar (lines not wrapped) */
enum { SB_HMSG = 4, NSBS };
extern sbar_t sbs[NSBS];
extern int sb_drag;                /* the window whose thumb is dragged */

/* The state of the program. */
typedef struct {
	store_t   *im;
	char      theme[128];
	char      start_folder[512];   /* from the -f URL; empty = INBOX */
	int       type_given;          /* the command line said IMAP, POP3 or local */
	/* sending */
	char     *smtp_pass;           /* memory only; NULL = use the IMAP password */
	net_trust smtp_trust;

	folder_t *folders;
	int       nfolders, fsel, ftop, frows;
	int       cur_folder;            /* index into folders, -1 if none */
	/* the mouse wheel scrolled a list away from its selection: keep the
	 * view until a key or click moves the selection */
	int       ffree, mfree, afree;

	/* Filter (L): the email list shows only the messages whose subject or
	 * sender contains the filter text; row i is then fseq[nf - 1 - i]. */
	char      filter[256];           /* the text, as applied or being typed */
	int       fedit;                 /* typing in the filter bar */
	int       fon;                   /* a filter is applied */
	uint32_t *fseq;                  /* matching sequence numbers, ascending */
	int       nf;
	int       fbar_y, fbar_x, fbar_w;     /* the list's bottom border, as last drawn */
	int       fesc_x0, fesc_x1;           /* where "Esc=Leave" is on it (x1 = 0: not shown) */
	int       fcur_x;                     /* cursor while typing */

	/* Message list, newest first: row i is sequence number nmsgs - i
	 * (with a filter: see below).
	 * Only blocks of headers around the visible rows are kept in memory. */
	int           nmsgs, msel, mtop, mrows;
	long          unseen;            /* -1 = unknown */
	struct chunk  chunks[MAX_CHUNKS];
	unsigned long clock;

	int       focus;
	int       left;                  /* two columns: l, f or a on the left (Tab: l -> f -> a -> l) */
	int       side;                  /* three columns: f or a on the left (Tab switches them) */
	int       asel, atop, arows;     /* accounts window: selection, first row, rows */

	/* message pane */
	msgview_t mv;
	line_t   *lines;
	int       nlines, lines_cap, lines_width, more_line, text_dirty;
	int       vtop, vrows, show_headers, hex;
	int       nowrap;                /* W: lines are not wrapped (scroll sideways) */
	int       hoff, maxw;            /* first column shown; widest line (columns) */
	int       keep_line;             /* after W: show the line at text offset keep_line - 1 (0: none) */
	char      srch[128];             /* last :/ or :? search text */
	int       hls;                   /* its matches are highlighted (until :noh) */
	int       hwx_y, hwx_x;          /* H W X on the message's bottom border (x 0: not shown) */
	int       pending_load;
	/* the active link (Tab / Shift-Tab): bytes [lk_off, lk_off + lk_len)
	 * of mv.text, shown between > and <; lk_url NULL = none */
	size_t    lk_off, lk_len;
	char     *lk_url, *lk_text;
	int       lk_att;   /* 0, or k+1: it is the k-th attachment (lk_url its text) */
	int       reindex;               /* the lines must be wrapped again (markers moved) */

	/* background work requested by the last draw */
	int       need_list, need_more, need_hex, work_failed;
	size_t    need_hex_off, need_hex_len;

	char      status[256];
	int       newmail;           /* status is the new-mail notice: its count (highlighted) */
	time_t    last_poll;
} app_t;
extern app_t A;

/* The main screen's menu bar. */
extern const ui_menuitem folder_menu[];
extern const int nfolder_menu;

/* The visible columns, left to right: what each shows (COL_*), where and
 * how wide (see get_cols in screen.c). */
typedef struct { int n, col[4], x[4], w[4]; } cols_t;

/* config.c */
void config_path(char *p, size_t n, const char *file);
void set_secret(char **slot, char *v);
account_t *account_new(const char *name);
account_t *account_find(const char *name);
void account_name(account_t *a);
void config_load(void);
void config_mkdir(void);
void config_save(void);

/* folder.c */
void cache_clear(void);
msg_t *msg_at(int i);
void now_status(const char *fmt, ...) __attribute__((format(MCM_PRINTF, 1, 2)));
void load_selected(void);
void select_msg(int i);
void filter_off(void);
void sync_changes(void);
void filter_leave(void);
void open_folder(int idx);
void refresh_folder(int quiet);
int work_pending(void);
void do_work(void);
void filter_edit(void);
void reopen_folder(void);
int folders_reload(const char *sel);
int nrows(void);
msg_t *msg_load(int i);

/* pick.c */
int pick_count(void);
int pick_has(uint32_t uid);
void pick_clear(void);
void pick_toggle(void);
void pick_range(void);
void pick_extend(int dir);
void pick_window(void);
int pick_can_undo(void);
void pick_undo(void);

/* msgwin.c */
void close_message(void);
void draw_message(int x, int y, int w, int h, int focused);
void name_part(buf_t *b, const char *s, size_t max);
int downloads_dir(char *dir, size_t n);
FILE *new_file(buf_t *path, const char *ext);
void save_message(int print);
void handle_key_wrap(void);
void handle_key_raw(void);    /* H: the raw message over the whole screen */
int msg_colon(void);
int msg_colon_with(const char *init);
void links_window(int key);
void msg_toggle_plain(void);   /* T */
int  att_temp_file(const char *name, const char *data, size_t len, char *out, size_t n);
void compose_draft(void);     /* E in the Drafts folder */
void link_step(int dir);
int link_active_visible(void);
int link_open_active(void);
void link_clear(void);
int msg_click(int x, int y);

/* screen.c */
void clamp_top(int *top, int rows, int n);
void ensure_visible(int *sel, int *top, int rows, int n);
void draw_scrollbar(int col, int x, int y, int rows, int n, int *top);
void draw_hscrollbar(int col, int x, int y, int len, int n, int cols, int *left);
void scroll_thumb_to(int row);
int scrollbar_press(int x, int y);
int wide(void);
int full(void);
void get_cols(cols_t *c);
int *border_pct(int b);
void set_border(int b, int x);
int border_at(int x);
void draw(void);
void draw_bg(void);
void login_bg(void);

/* attach.c */
void att_cleanup(void);
/* The private folder for opened attachments (and pgp.c's temporary files). */
int att_private_dir(char *dir, size_t n);
int open_with_viewer(const char *path);
int open_url(const char *url);
int graphical(void);
const char *viewer_for(int kind);
/* The links window's share: the open message's attachments as list items
 * ("name  (type, size)", malloc'd; returns how many), Open/Save for one of
 * them, and freeing them again. */
int att_collect(char ***items);
void att_choose(int i, const char *item);
void att_release(void);
/* The k-th "[Attachment: ...]" of the text (name: its file name, item: the
 * text shown): Open or Save. */
void att_open_shown(int k, const char *name, const char *item);

/* keys.c */
void show_help(void);
void show_cmdline_help(void);
void show_about(void);
void focus_to(int col);
void click_window(const cols_t *cl, int x, int y);
int handle_key(int ch);

/* menu.c: F1, F10 - the menu window; 0 = quit */
int menu_run(void);

/* fldops.c: Folders > Create, Delete, Rename, Move */
void folder_create(void);
void folder_delete(void);
void folder_rename(void);
void folder_move(void);
char fld_delim(void);
void fld_prefix(char *out, size_t n);
char *fld_shown(const char *raw);
char *fld_stored(const char *typed);

/* xfer.c: Account > export (to a new Maildir) and import (into a new folder) */
void folder_export(void);
void folder_import(void);
/* the work, also for mcm --export DIR / --import PATH (msg: what happened) */
struct local;
const char   *xfer_export_check(const char *dir, char *why, size_t n);
int           xfer_export(const char *dir, int only, buf_t *msg);
struct local *xfer_import_open(const char *path, buf_t *msg);
int           xfer_import(struct local *src, buf_t *msg);

/* certs.c */
int cert_check(const net_certinfo *ci, void *ctx);

/* account.c */
void account_open(int i);
void login_address(char *out, size_t n);
void smtp_derive(void);
void aad_smtp(const account_t *a, char *o, size_t n);
int get_master(const char *why, ui_bg_fn bg);
int store_password(const char *pass, ui_bg_fn bg);
void offer_store(const char *pass);
int unlock_password(char *pass, size_t n, const char **err);
int pick_account(const char *title, int def, int mark, ui_bg_fn bg);
void load_theme(theme_t *t, char *msg, size_t n);
void config_menu(void);
void account_menu(void);
void account_edit(void);
void account_remove(void);
int smtp_settings(const char *err, ui_bg_fn bg);

/* send.c */
void compose_start(int kind);

/* main.c */
void set_status(const char *fmt, ...) __attribute__((format(MCM_PRINTF, 1, 2)));
void expand_home(char *out, size_t n, const char *in);
void switch_account(const char *name);

/* pgp.c */
enum { PGP_SIGN = COMPOSE_SIGN, PGP_ENCRYPT = COMPOSE_ENCRYPT };
int  pgp_on(const account_t *a);
/* The key of account a: pgp_key, else its address. */
void pgp_key(const account_t *a, char *out, size_t n);
const char *pgp_program(void);
int  pgp_found(void);          /* gpg can be run */
/* Check a detached signature / decrypt (or check clearsigned text): a note
 * for the reader, malloc'd. */
char *pgp_verify(const char *data, size_t n, const char *sig, size_t sn, const char *from);
char *pgp_decrypt(const char *in, size_t n, buf_t *out, int *encrypted, const char *from);
/* Sign and/or encrypt a MIME entity (header lines, blank line, body;
 * CRLF) into out: a multipart/signed or multipart/encrypted entity. */
int  pgp_wrap(int how, const char *key, const buf_t *entity, const char *const *rcpt, int nrcpt,
              const char *const *hidden, int nhidden, buf_t *out, char *err, size_t errlen);
extern const mv_pgp_t pgp_hooks;

#endif
