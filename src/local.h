/* local.h - mailboxes on this computer (Maildir, mbox, MH, sdbox) and the POP3 inbox
 *
 * The folder root can be:
 *   - an mbox file: one folder, INBOX;
 *   - a Maildir (cur/ new/ tmp/): INBOX, with Maildir++ subfolders (.Name);
 *   - an MH folder (numbered files, .mh_sequences): INBOX, subdirectories
 *     are subfolders;
 *   - a directory: every mbox file, Maildir and MH folder below it is a
 *     folder (subdirectories give the hierarchy), plus $MAIL as INBOX;
 *   - a Dovecot sdbox root (mailboxes/NAME/dbox-Mails/u.UID): only read,
 *     flags from Dovecot's index (dbox.h); "seen" set here lasts until mcm
 *     exits. An mdbox root (with storage/) is refused.
 * Sent, Drafts and Deleted Items (unless there is a Trash or Deleted folder)
 * are listed even before they exist and are created by the first message
 * saved there (of the kind most folders there are).
 *
 * Flags: a Maildir keeps them in the file names (S seen, R answered, F
 * flagged, T deleted); changes are written there. MH keeps them in the
 * sequences unseen, replied and flagged in .mh_sequences (rewritten as a new
 * file, other sequences kept). An mbox file is only read
 * (and appended to): its Status/X-Status headers give the flags, and "seen"
 * set here lasts until mcm exits. POP3 has no flags: seen and answered are
 * kept per message id in a state file.
 *
 * The functions mirror the IMAP client (imap.h); messages have sequence
 * numbers 1..n in folder order and uids that stay the same while mcm runs.
 */
#ifndef MCM_LOCAL_H
#define MCM_LOCAL_H

#include "imap.h"
#include "pop3.h"

typedef struct local local_t;

local_t    *local_new(void);
void        local_free(local_t *l);
const char *local_error(local_t *l);

/* Use the folders at root (created as a directory when missing and create
 * is set). */
int  local_open(local_t *l, const char *root, int create);
/* Serve INBOX from a POP3 session (logged in); flags go to state_file. */
int  local_set_pop3(local_t *l, pop3_t *p, const char *state_file);

int  local_list(local_t *l, folder_t **out, int *n);
int  local_select(local_t *l, const char *name, uint32_t *exists);
int  local_fetch_headers(local_t *l, uint32_t from, uint32_t to, msg_t **out, int *n);
int  local_status_unseen(local_t *l, const char *name, uint32_t *unseen);
int  local_fetch_structure(local_t *l, uint32_t uid, char **hdr, size_t *hlen, mpart_t **root);
int  local_fetch_partial(local_t *l, uint32_t uid, imap_partial_t *req, int n);
int  local_mark_seen(local_t *l, uint32_t uid);
int  local_add_flags(local_t *l, uint32_t uid, const char *flags);
/* Look for changes in the open folder; force: also start a new POP3 session
 * (otherwise done at most every few minutes). */
int  local_noop(local_t *l, int force);
int  local_changes(local_t *l, uint32_t *exists, const uint32_t **expunged, int *n);
int  local_append(local_t *l, const char *name, const char *flags, const char *data, size_t len);
/* Copy n messages of the open folder to folder dest. *done: how many of
 * uids were dealt with (copied, or no longer there), also after an error. */
int  local_copy(local_t *l, const uint32_t *uids, int n, const char *dest, int *done);
/* Remove n messages from the open folder: Maildir and MH files are deleted,
 * the POP3 inbox's messages are deleted on the server (DELE); an mbox file
 * is not changed (error). The removals show up in local_changes(). */
int  local_remove(local_t *l, const uint32_t *uids, int n);
/* The copies the last local_copy made: how many, and in which folder. */
int  local_copies(local_t *l, const char **dest);
/* Undo the last local_copy: remove the copies again; when back is not
 * NULL, first put them in folder back (undoing a move; flags are kept). */
int  local_uncopy(local_t *l, const char *back);

/* Folders: create one (of the kind most folders here are; Sent, Drafts,
 * Deleted Items that are listed but not there yet are made), rename one
 * (its subfolders go along; not INBOX, not the open folder), delete one
 * with its messages (not one with subfolders). */
int  local_create(local_t *l, const char *name);
int  local_rename(local_t *l, const char *from, const char *to);
int  local_delete_folder(local_t *l, const char *name);

#endif
