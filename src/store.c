/* store.c - one interface for IMAP, POP3 and local folders (see store.h) */
#include "store.h"
#include "local.h"
#include "pop3.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>

struct store {
	int      kind;
	imap_t  *im;
	pop3_t  *pop;
	local_t *loc;
	char     err[512];
	char    *selected;         /* the folder store_select opened */
	/* the last copy or move, for store_undo */
	struct {
		int       kind;        /* 0: none, 'c' copied, 'm' moved */
		char     *src, *dst;
		int       n;
		uint32_t *old;         /* IMAP move that could not expunge: the originals, */
		int       marked;      /* still in src and marked \Deleted */
		uint32_t *uids;        /* IMAP: the copies in dst (COPYUID) ... */
		int       nuids;
		uint32_t  next, validity;   /* ... or the UIDNEXT dst had before */
		char    **ids;         /* IMAP: the Message-IDs of the messages (sorted) */
		int       nids;
	} u;
	char    **ids;             /* noted before a copy or move */
	int       nids;
};

static void ids_free(char **ids, int n)
{
	for (int i = 0; i < n; i++) free(ids[i]);
	free(ids);
}

static int strpcmp(const void *a, const void *b)
{
	return strcmp(*(char *const *)a, *(char *const *)b);
}

/* IMAP: note the Message-IDs of the messages about to be copied or moved,
 * to check later that undo finds the same messages. */
static void ids_note(store_t *s, const uint32_t *uids, int n)
{
	ids_free(s->ids, s->nids);
	s->ids = NULL;
	s->nids = 0;
	uint32_t *u;
	if (s->im && imap_message_ids(s->im, uids, n, &s->ids, &u, &s->nids) == 0) {
		qsort(s->ids, (size_t)s->nids, sizeof *s->ids, strpcmp);
		free(u);
	}
}

static void undo_clear(store_t *s)
{
	ids_free(s->u.ids, s->u.nids);
	free(s->u.src);
	free(s->u.dst);
	free(s->u.old);
	free(s->u.uids);
	memset(&s->u, 0, sizeof s->u);
}

store_t *store_new(int kind)
{
	store_t *s = xcalloc(1, sizeof *s);
	s->kind = kind;
	if (kind == STORE_IMAP) s->im = imap_new();
	else s->loc = local_new();
	if (kind == STORE_POP3) s->pop = pop3_new();
	return s;
}

void store_free(store_t *s)
{
	if (!s) return;
	undo_clear(s);
	ids_free(s->ids, s->nids);
	free(s->selected);
	imap_free(s->im);
	local_free(s->loc);
	pop3_free(s->pop);
	free(s);
}

int store_kind(const store_t *s) { return s->kind; }

const char *store_error(store_t *s)
{
	if (s->err[0]) return s->err;
	if (s->im) return imap_error(s->im);
	return local_error(s->loc);
}

/* Every call starts with no error of its own. */
#define CLR(s) ((s)->err[0] = '\0')

void store_set_cert_check(store_t *s, net_cert_fn fn, void *ctx)
{
	if (s->im) imap_set_cert_check(s->im, fn, ctx);
	if (s->pop) pop3_set_cert_check(s->pop, fn, ctx);
}

int store_connect(store_t *s, const char *host, const char *port)
{
	CLR(s);
	if (s->im) return imap_connect(s->im, host, port);
	if (s->pop) {
		if (pop3_connect(s->pop, host, port) == 0) return 0;
		snprintf(s->err, sizeof s->err, "%s", pop3_error(s->pop));
		return -1;
	}
	snprintf(s->err, sizeof s->err, "local folders need no server");
	return -1;
}

int store_login(store_t *s, const char *user, const char *pass)
{
	CLR(s);
	if (s->im) return imap_login(s->im, user, pass);
	if (s->pop) {
		if (pop3_login(s->pop, user, pass) == 0) return 0;
		snprintf(s->err, sizeof s->err, "%s", pop3_error(s->pop));
		return -1;
	}
	return 0;
}

int store_open_local(store_t *s, const char *path, const char *state_file)
{
	CLR(s);
	if (!s->loc) return 0;
	if (local_open(s->loc, path, s->pop != NULL) != 0) return -1;
	if (s->pop) local_set_pop3(s->loc, s->pop, state_file);
	return 0;
}

void store_logout(store_t *s)
{
	if (s->im) imap_logout(s->im);
	if (s->pop) pop3_quit(s->pop);
}

const char *store_pass(store_t *s)
{
	if (s->im) return imap_password(s->im);
	if (s->pop) return pop3_password(s->pop);
	return "";
}

int store_list(store_t *s, folder_t **out, int *n)
{
	CLR(s);
	return s->im ? imap_list(s->im, out, n) : local_list(s->loc, out, n);
}

int store_select(store_t *s, const char *mbox, uint32_t *exists)
{
	CLR(s);
	free(s->selected);
	s->selected = NULL;
	int rc = s->im ? imap_select(s->im, mbox, exists) : local_select(s->loc, mbox, exists);
	if (rc == 0) s->selected = xstrdup(mbox);
	return rc;
}

int store_fetch_headers(store_t *s, uint32_t from, uint32_t to, msg_t **out, int *n)
{
	CLR(s);
	return s->im ? imap_fetch_headers(s->im, from, to, out, n)
	             : local_fetch_headers(s->loc, from, to, out, n);
}

int store_fetch_set(store_t *s, const uint32_t *seqs, int n, msg_t **out, int *nout)
{
	CLR(s);
	if (s->im) return imap_fetch_set(s->im, seqs, n, out, nout);
	*out = NULL;
	*nout = 0;
	if (n <= 0) return 0;
	msg_t *m = xcalloc((size_t)n, sizeof *m);
	for (int i = 0; i < n;) {             /* runs of consecutive numbers */
		int j = i;
		while (j + 1 < n && seqs[j + 1] == seqs[j] + 1) j++;
		msg_t *part;
		int k;
		if (local_fetch_headers(s->loc, seqs[i], seqs[j], &part, &k) != 0) {
			msgs_free(m, n);
			return -1;
		}
		for (int x = 0; x < k && i + x <= j; x++) m[i + x] = part[x];
		for (int x = j - i + 1; x < k; x++) {   /* not expected */
			free(part[x].from);
			free(part[x].subject);
			free(part[x].addr);
			free(part[x].msgid);
		}
		free(part);
		i = j + 1;
	}
	for (int i = 0; i < n; i++) {
		if (!m[i].subject) m[i].subject = xstrdup("");
		if (!m[i].from) m[i].from = xstrdup("");
		if (!m[i].seq) m[i].seq = seqs[i];
	}
	*out = m;
	*nout = n;
	return 0;
}

int store_status_unseen(store_t *s, const char *mbox, uint32_t *unseen)
{
	CLR(s);
	return s->im ? imap_status_unseen(s->im, mbox, unseen) : local_status_unseen(s->loc, mbox, unseen);
}

int store_fetch_structure(store_t *s, uint32_t uid, char **hdr, size_t *hlen, mpart_t **root)
{
	CLR(s);
	return s->im ? imap_fetch_structure(s->im, uid, hdr, hlen, root)
	             : local_fetch_structure(s->loc, uid, hdr, hlen, root);
}

int store_fetch_partial(store_t *s, uint32_t uid, imap_partial_t *req, int n)
{
	CLR(s);
	return s->im ? imap_fetch_partial(s->im, uid, req, n) : local_fetch_partial(s->loc, uid, req, n);
}

int store_mark_seen(store_t *s, uint32_t uid)
{
	CLR(s);
	return s->im ? imap_mark_seen(s->im, uid) : local_mark_seen(s->loc, uid);
}

int store_add_flags(store_t *s, uint32_t uid, const char *flags)
{
	CLR(s);
	return s->im ? imap_add_flags(s->im, uid, flags) : local_add_flags(s->loc, uid, flags);
}

int store_noop(store_t *s, int force)
{
	CLR(s);
	return s->im ? imap_noop(s->im) : local_noop(s->loc, force);
}

int store_changes(store_t *s, uint32_t *exists, const uint32_t **expunged, int *n)
{
	return s->im ? imap_changes(s->im, exists, expunged, n) : local_changes(s->loc, exists, expunged, n);
}

int store_append(store_t *s, const char *mbox, const char *flags, const char *data, size_t len)
{
	CLR(s);
	return s->im ? imap_append(s->im, mbox, flags, data, len)
	             : local_append(s->loc, mbox, flags, data, len);
}

/* Remember a copy or move of n messages to dest that went (partly) well. */
static void undo_note(store_t *s, int kind, const uint32_t *uids, int n, const char *dest, int marked)
{
	undo_clear(s);
	if (!s->selected) return;
	s->u.kind = kind;
	s->u.src = xstrdup(s->selected);
	s->u.dst = xstrdup(dest);
	s->u.n = n;
	if (s->im) {
		imap_copied(s->im, &s->u.uids, &s->u.nuids, &s->u.next, &s->u.validity);
		s->u.ids = s->ids;
		s->u.nids = s->nids;
		s->ids = NULL;
		s->nids = 0;
		/* cannot find the copies, or not check them */
		if ((!s->u.nuids && !s->u.next) || s->u.nids != n) undo_clear(s);
		else if (marked) {
			s->u.marked = 1;
			s->u.old = xmalloc(sizeof *uids * (size_t)n);
			memcpy(s->u.old, uids, sizeof *uids * (size_t)n);
		}
	} else {
		const char *d;
		s->u.n = local_copies(s->loc, &d);
		if (!s->u.n) undo_clear(s);
	}
}

int store_copy(store_t *s, const uint32_t *uids, int n, const char *dest)
{
	CLR(s);
	ids_note(s, uids, n);
	int done, rc = s->im ? imap_copy(s->im, uids, n, dest) : local_copy(s->loc, uids, n, dest, &done);
	if (rc == 0 || !s->im) undo_note(s, 'c', uids, n, dest, 0);
	return rc;
}

int store_move(store_t *s, const uint32_t *uids, int n, const char *dest)
{
	CLR(s);
	if (s->im) {
		ids_note(s, uids, n);
		int rc = imap_move(s->im, uids, n, dest);
		if (rc >= 0) undo_note(s, 'm', uids, n, dest, rc > 0);
		else undo_clear(s);
		return rc;
	}
	int done;
	int rc = local_copy(s->loc, uids, n, dest, &done);
	if (done > 0 && local_remove(s->loc, uids, done) != 0) {
		undo_note(s, 'c', uids, n, dest, 0);    /* copied, not removed: undo removes the copies */
		return -1;
	}
	undo_note(s, 'm', uids, n, dest, 0);
	return rc;
}

int store_delete(store_t *s, const uint32_t *uids, int n)
{
	CLR(s);
	undo_clear(s);          /* gone for good: nothing to undo */
	return s->im ? imap_expunge(s->im, uids, n) : local_remove(s->loc, uids, n);
}

int store_undo_info(store_t *s, int *n, const char **src, const char **dst)
{
	*n = s->u.n;
	*src = s->u.src;
	*dst = s->u.dst;
	return s->u.kind;
}

/* IMAP: select dst and find the copies there. */
static int find_copies(store_t *s, uint32_t **uids, int *n)
{
	uint32_t ex;
	*uids = NULL;
	*n = 0;
	if (store_select(s, s->u.dst, &ex) != 0) return -1;
	uint32_t v = imap_uidvalidity(s->im);
	if (s->u.validity && v && v != s->u.validity) {
		snprintf(s->err, sizeof s->err, "%s was recreated since: its messages have other numbers",
		         s->u.dst);
		return -1;
	}
	uint32_t *cand;
	int ncand;
	if (s->u.nuids) {                     /* the uids the server named (COPYUID) */
		cand = xmalloc(sizeof *cand * (size_t)s->u.nuids);
		memcpy(cand, s->u.uids, sizeof *cand * (size_t)s->u.nuids);
		ncand = s->u.nuids;
	} else if (imap_uids_from(s->im, s->u.next, &cand, &ncand) != 0) {
		return -1;                        /* (else: all that arrived since the copy) */
	}
	/* the copies are the candidates with the Message-IDs noted before */
	char **ids;
	uint32_t *idu;
	int nids;
	int rc = imap_message_ids(s->im, cand, ncand, &ids, &idu, &nids);
	free(cand);
	if (rc != 0) return -1;
	*uids = xmalloc(sizeof **uids * (size_t)(s->u.nids ? s->u.nids : 1));
	int ok = 1;
	for (int i = 0; ok && i < s->u.nids; i++) {
		const char *want = s->u.ids[i];
		if (i && !strcmp(want, s->u.ids[i - 1])) continue;     /* (counted already) */
		int need = 0, have = 0;
		for (int j = i; j < s->u.nids && !strcmp(s->u.ids[j], want); j++) need++;
		for (int j = 0; j < nids; j++) have += !strcmp(ids[j], want);
		if (have != need) ok = 0;         /* missing, or more than one could be it */
		for (int j = 0; ok && j < nids; j++)
			if (!strcmp(ids[j], want)) (*uids)[(*n)++] = idu[j];
	}
	ids_free(ids, nids);
	free(idu);
	if (ok && *n == s->u.n) return 0;
	snprintf(s->err, sizeof s->err, "mcm cannot find the %s messages in %s by their Message-IDs: "
	         "nothing was undone", s->u.kind == 'c' ? "copied" : "moved", s->u.dst);
	free(*uids);
	*uids = NULL;
	*n = 0;
	return -1;
}

int store_undo(store_t *s)
{
	CLR(s);
	if (!s->u.kind) {
		snprintf(s->err, sizeof s->err, "there is nothing to undo");
		return -1;
	}
	int rc;
	if (!s->im) {
		rc = local_uncopy(s->loc, s->u.kind == 'm' ? s->u.src : NULL);
		if (rc == 0) undo_clear(s);
		return rc;
	}
	uint32_t *uids;
	int n;
	if (find_copies(s, &uids, &n) != 0) return -1;     /* (checked before anything changes) */
	if (s->u.marked) {
		/* moved without expunge: the originals are still there */
		uint32_t ex;
		if (store_select(s, s->u.src, &ex) != 0 ||
		    imap_unflag(s->im, s->u.old, s->u.n, "\\Deleted") != 0 ||
		    store_select(s, s->u.dst, &ex) != 0) {
			free(uids);
			return -1;
		}
	}
	if (s->u.kind == 'c' || s->u.marked) rc = n ? imap_expunge(s->im, uids, n) : 0;
	else rc = n ? imap_move(s->im, uids, n, s->u.src) : 0;
	free(uids);
	/* not again: the copies are gone or back (or only marked deleted, rc 1) */
	if (rc >= 0) undo_clear(s);
	return rc;
}

int store_create(store_t *s, const char *name)
{
	CLR(s);
	if (s->im) return imap_create(s->im, name);
	return s->loc ? local_create(s->loc, name) : 0;
}

int store_delete_folder(store_t *s, const char *name)
{
	CLR(s);
	undo_clear(s);                     /* the undo may point at it */
	if (s->im) return imap_delete(s->im, name);
	return local_delete_folder(s->loc, name);
}

int store_rename_folder(store_t *s, const char *from, const char *to)
{
	CLR(s);
	undo_clear(s);
	if (s->im) return imap_rename(s->im, from, to);
	return local_rename(s->loc, from, to);
}

/* Lower-case wide copy of UTF-8 text (malloc'd). */
static wchar_t *lower_w(const char *s)
{
	wchar_t *w;
	size_t n = utf8_to_wcs(s ? s : "", s ? strlen(s) : 0, &w);
	for (size_t i = 0; i < n; i++) w[i] = (wchar_t)towlower((wint_t)w[i]);
	return w;
}

int store_search(store_t *s, const char *text, uint32_t n, uint32_t **out, int *nout)
{
	CLR(s);
	if (s->im) return imap_search(s->im, text, out, nout);
	*out = NULL;
	*nout = 0;
	wchar_t *want = lower_w(text);
	uint32_t *v = NULL;
	int cnt = 0, cap = 0;
	for (uint32_t from = 1; from <= n; from += 500) {
		uint32_t to = from + 499 > n ? n : from + 499;
		msg_t *m;
		int k;
		if (local_fetch_headers(s->loc, from, to, &m, &k) != 0) {
			free(v);
			free(want);
			return -1;
		}
		for (int i = 0; i < k; i++) {
			wchar_t *a = lower_w(m[i].subject), *b = lower_w(m[i].from);
			if (m[i].uid && (wcsstr(a, want) || wcsstr(b, want))) {
				if (cnt == cap) {
					cap = cap ? cap * 2 : 256;
					v = xrealloc(v, sizeof *v * (size_t)cap);
				}
				v[cnt++] = from + (uint32_t)i;
			}
			free(a);
			free(b);
		}
		msgs_free(m, k);
	}
	free(want);
	*out = v;
	*nout = cnt;
	return 0;
}
