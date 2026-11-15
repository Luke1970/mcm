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
};

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
	return s->im ? imap_select(s->im, mbox, exists) : local_select(s->loc, mbox, exists);
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
