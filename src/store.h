/* store.h - where the mail is: an IMAP server, a POP3 server, or local files
 *
 * One interface for the three kinds of account; the rest of mcm does not
 * need to know which it is. The calls mirror imap.h:
 *   IMAP   all folders are on the server;
 *   POP3   INBOX is on the server (messages are left there); Sent, Drafts
 *          and any other folders are local, in the account's folder path;
 *   LOCAL  Maildir and mbox folders on this computer (local.h).
 */
#ifndef MCM_STORE_H
#define MCM_STORE_H

#include "imap.h"

typedef struct store store_t;

enum { STORE_IMAP, STORE_POP3, STORE_LOCAL };

store_t    *store_new(int kind);
void        store_free(store_t *s);
int         store_kind(const store_t *s);
const char *store_error(store_t *s);
void        store_set_cert_check(store_t *s, net_cert_fn fn, void *ctx);

/* IMAP, POP3: connect (TLS) and log in. */
int  store_connect(store_t *s, const char *host, const char *port);
int  store_login(store_t *s, const char *user, const char *pass);
/* POP3, LOCAL: the local folders (created for POP3 when missing). For POP3,
 * state_file keeps which messages were seen or answered. */
int  store_open_local(store_t *s, const char *path, const char *state_file);
void store_logout(store_t *s);
/* The login password (memory only), "" for local folders. */
const char *store_pass(store_t *s);

int  store_list(store_t *s, folder_t **out, int *n);
int  store_select(store_t *s, const char *mbox, uint32_t *exists);
int  store_fetch_headers(store_t *s, uint32_t from, uint32_t to, msg_t **out, int *n);
/* Headers of n messages by sequence number (ascending); out[i] is seqs[i]. */
int  store_fetch_set(store_t *s, const uint32_t *seqs, int n, msg_t **out, int *nout);
int  store_status_unseen(store_t *s, const char *mbox, uint32_t *unseen);
int  store_fetch_structure(store_t *s, uint32_t uid, char **hdr, size_t *hlen, mpart_t **root);
int  store_fetch_partial(store_t *s, uint32_t uid, imap_partial_t *req, int n);
int  store_mark_seen(store_t *s, uint32_t uid);
int  store_add_flags(store_t *s, uint32_t uid, const char *flags);
/* Check for new mail; force = the user asked (F5). */
int  store_noop(store_t *s, int force);
int  store_changes(store_t *s, uint32_t *exists, const uint32_t **expunged, int *n);
/* Filter: sequence numbers (ascending, malloc'd) of the messages among the
 * n of the selected folder whose subject or sender contains text, ignoring
 * case. IMAP asks the server; other folders read the headers. */
int  store_search(store_t *s, const char *text, uint32_t n, uint32_t **out, int *nout);
int  store_append(store_t *s, const char *mbox, const char *flags, const char *data, size_t len);

#endif
