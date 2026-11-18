/* fldops.c - Folders > creaTe, Delete, reName and moVe (the menu, F1)
 *
 * They work on the folder the folders window points at. Names are typed as
 * they are shown, with the folder's own separator ("Archive/2025" or
 * "INBOX.Archive"); IMAP names and Maildir++ folders are stored in modified
 * UTF-7 (the folder list decodes them again).
 */
#include "mcm.h"

static int is_inbox(int i) { return !strcasecmp(A.folders[i].name, "INBOX"); }

/* The hierarchy separator: INBOX's, else the first folder's that has one. */
static char delim(void)
{
	for (int i = 0; i < A.nfolders; i++)
		if (is_inbox(i) && A.folders[i].delim) return A.folders[i].delim;
	for (int i = 0; i < A.nfolders; i++)
		if (A.folders[i].delim) return A.folders[i].delim;
	return '/';
}

static int encoded(void) { return store_kind(A.im) == STORE_IMAP || delim() == '.'; }

static char *shown(const char *raw) { return encoded() ? mutf7_decode(raw) : xstrdup(raw); }
static char *stored(const char *typed) { return encoded() ? mutf7_encode(typed) : xstrdup(typed); }

/* IMAP servers that keep every folder under INBOX ("INBOX.Name"): that
 * prefix, else "". */
static void prefix(char *out, size_t n)
{
	int under = 0, others = 0;
	char d = delim();
	*out = '\0';
	if (store_kind(A.im) != STORE_IMAP) return;
	for (int i = 0; i < A.nfolders; i++) {
		if (is_inbox(i)) continue;
		others++;
		if (!strncasecmp(A.folders[i].name, "INBOX", 5) && A.folders[i].name[5] == d) under++;
	}
	if (others && under == others) snprintf(out, n, "INBOX%c", d);
}

/* A name typed for a folder (whole: parts may be separated) or one part
 * of it; NULL when it can be used, else why not. */
static const char *bad_name(const char *s, int whole)
{
	char d = delim();
	if (!*s) return "Type a name";
	for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
		if (*p < 0x20 || *p == 0x7F) return "No control characters in a folder name";
		if (*p == '*' || *p == '%') return "No * or % in a folder name";
		if (*p == (unsigned char)d) {
			if (!whole) return d == '.' ? "No '.' in a folder name here" : "No '/' in a folder name";
			if (p == (const unsigned char *)s || !p[1] || p[1] == d) return "An empty part in the name";
		}
	}
	return NULL;
}

static int find_raw(const char *raw)
{
	for (int i = 0; i < A.nfolders; i++)
		if (!strcmp(A.folders[i].name, raw)) return i;
	return -1;
}

/* Is folder j inside folder i? */
static int inside(int j, int i)
{
	size_t n = strlen(A.folders[i].name);
	return j != i && !strncmp(A.folders[j].name, A.folders[i].name, n) &&
	       A.folders[j].name[n] == A.folders[i].delim && A.folders[i].delim;
}

static int has_children(int i)
{
	for (int j = 0; j < A.nfolders; j++)
		if (inside(j, i)) return 1;
	return 0;
}

/* The folder the folders window points at, if it may be changed. */
static int target(const char *what)
{
	int i = A.fsel;
	if (i < 0 || i >= A.nfolders) {
		set_status("No folder selected (Folders > select)");
		return -1;
	}
	if (is_inbox(i)) {
		set_status("INBOX cannot be %s", what);
		return -1;
	}
	return i;
}

/* Folder i is about to go: when it is open, open INBOX. 0 = done. */
static int leave_open(int i)
{
	if (A.cur_folder != i) return 0;
	for (int k = 0; k < A.nfolders; k++)
		if (is_inbox(k)) {
			open_folder(k);
			return A.cur_folder == k ? 0 : -1;
		}
	set_status("Open another folder first");
	return -1;
}

void folder_create(void)
{
	char name[200] = "", hint[120], pre[64];
	prefix(pre, sizeof pre);
	char d = delim();
	snprintf(hint, sizeof hint, "A subfolder: Parent%cName%s%s", d, *pre ? "   (made under " : "",
	         *pre ? "INBOX)" : "");
	const char *err = NULL;
	for (;;) {
		ui_field f[] = {{"Folder name", name, sizeof name, 0, 0}};
		if (ui_input_dialog("Create folder", f, 1, err ? err : hint, draw_bg) < 0) return;
		err = bad_name(str_trim(name), 1);
		if (!err) break;
	}
	char *t = str_trim(name);
	buf_t full = {0};
	if (*pre && strncasecmp(t, pre, strlen(pre)) && strcasecmp(t, "INBOX")) buf_adds(&full, pre);
	buf_adds(&full, t);
	char *raw = stored(buf_cstr(&full));
	now_status("Creating %s ...", full.d);
	if (store_create(A.im, raw) != 0) {
		set_status("Cannot create %s: %s", full.d, store_error(A.im));
	} else if (folders_reload(raw) == 0) {
		set_status("Folder %s created", full.d);
	}
	free(raw);
	buf_free(&full);
}

void folder_delete(void)
{
	int i = target("deleted");
	if (i < 0) return;
	char *show = shown(A.folders[i].name);
	if (has_children(i)) {
		set_status("%s has subfolders: delete or move those first", show);
		free(show);
		return;
	}
	char l0[300];
	snprintf(l0, sizeof l0, "Delete folder %.200s and all messages in it?", show);
	const char *lines[] = {l0, "This cannot be undone."};
	static const ui_menuitem b[] = {{"Cancel", 0, 'c'}, {"Delete", 0, 'd'}};
	if (ui_choice_dialog("Delete folder", lines, 2, b, 2, 0, 1, draw_bg) != 1 || leave_open(i) != 0) {
		free(show);
		return;
	}
	char *raw = xstrdup(A.folders[i].name);
	now_status("Deleting %s ...", show);
	if (store_delete_folder(A.im, raw) != 0) {
		set_status("Cannot delete %s: %s", show, store_error(A.im));
		folders_reload(raw);
	} else if (folders_reload(NULL) == 0) {
		set_status("Folder %s deleted", show);
	}
	free(raw);
	free(show);
}

/* Rename folder i to raw name to; an open folder is opened again under
 * its new name. */
static void rename_to(int i, const char *to)
{
	char *from = xstrdup(A.folders[i].name), *sf = shown(from), *st = shown(to);
	int was_open = A.cur_folder == i;
	if (find_raw(to) >= 0 && !A.folders[find_raw(to)].noselect) {
		set_status("There is already a folder %s", st);
	} else if (leave_open(i) == 0) {
		now_status("Renaming %s ...", sf);
		if (store_rename_folder(A.im, from, to) != 0) {
			set_status("Cannot rename %s: %s", sf, store_error(A.im));
			folders_reload(from);
			if (was_open && find_raw(from) >= 0) open_folder(find_raw(from));
		} else if (folders_reload(to) == 0) {
			if (was_open && find_raw(to) >= 0) open_folder(find_raw(to));
			set_status("%s is now %s", sf, st);
		}
	}
	free(from);
	free(sf);
	free(st);
}

void folder_rename(void)
{
	int i = target("renamed");
	if (i < 0) return;
	const char *raw = A.folders[i].name;
	char d = A.folders[i].delim;
	const char *last = d ? strrchr(raw, d) : NULL;
	size_t plen = last ? (size_t)(last - raw) + 1 : 0;   /* the parent, with the separator */
	char *leaf = shown(raw + plen), *show = shown(raw);
	char name[200], msg[300];
	snprintf(name, sizeof name, "%s", leaf);
	snprintf(msg, sizeof msg, "Rename %.200s (Folders > moVe puts it elsewhere)", show);
	const char *err = NULL;
	int ok = 0;
	for (;;) {
		ui_field f[] = {{"New name", name, sizeof name, 0, 0}};
		if (ui_input_dialog("Rename folder", f, 1, err ? err : msg, draw_bg) < 0) break;
		err = bad_name(str_trim(name), 0);
		if (!err) { ok = 1; break; }
	}
	if (ok && strcmp(str_trim(name), leaf)) {
		char *enc = stored(str_trim(name));
		buf_t to = {0};
		buf_add(&to, raw, plen);
		buf_adds(&to, enc);
		rename_to(i, buf_cstr(&to));
		buf_free(&to);
		free(enc);
	}
	free(leaf);
	free(show);
}

void folder_move(void)
{
	int i = target("moved");
	if (i < 0) return;
	const char *raw = A.folders[i].name;
	char d = A.folders[i].delim ? A.folders[i].delim : delim(), pre[64];
	const char *last = strrchr(raw, d);
	const char *leaf = last ? last + 1 : raw;
	size_t plen = last ? (size_t)(last - raw) : 0;
	prefix(pre, sizeof pre);

	/* the places it can go: the top level (unless everything is under
	 * INBOX), or any folder but itself, what is inside it and where it is */
	int *idx = xmalloc(sizeof *idx * (size_t)(A.nfolders + 1));
	char **items = xmalloc(sizeof *items * (size_t)(A.nfolders + 1));
	int n = 0;
	if (!*pre && plen) {
		items[n] = xstrdup("(top level)");
		idx[n++] = -1;
	}
	for (int j = 0; j < A.nfolders; j++) {
		const char *jn = A.folders[j].name;
		if (j == i || inside(j, i) || (plen && strlen(jn) == plen && !strncmp(jn, raw, plen))) continue;
		if (!plen && *pre && !strcasecmp(jn, "INBOX")) continue;   /* (already under INBOX: no) */
		items[n] = shown(jn);
		idx[n++] = j;
	}
	char *show = shown(raw), title[300];
	snprintf(title, sizeof title, "Move %.200s into", show);
	int r = n ? ui_list_dialog(title, (const char *const *)items, n, 0, "Enter move   Esc cancel", draw_bg)
	          : -1;
	if (!n) set_status("There is no other place for %s", show);
	if (r >= 0) {
		buf_t to = {0};
		if (idx[r] >= 0) {
			buf_adds(&to, A.folders[idx[r]].name);
			buf_addc(&to, d);
		}
		buf_adds(&to, leaf);
		rename_to(i, buf_cstr(&to));
		buf_free(&to);
	}
	for (int k = 0; k < n; k++) free(items[k]);
	free(items);
	free(idx);
	free(show);
}
