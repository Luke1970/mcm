/* msgview.c - an open message, loaded piece by piece from the server */
#include "msgview.h"
#include "mime.h"
#include "textconv.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define TEXT_BLOCK  (64 * 1024)   /* bytes of a text part fetched at a time */
#define HEX_BLOCK   (16 * 1024)   /* hex window granularity */
#define HEX_MAX     (64 * 1024)   /* largest hex window kept */
#define PGP_MAX     (64u << 20)   /* larger PGP/MIME messages are not loaded to check */

const mv_pgp_t *mv_pgp;

/* ------------------------------------------------------------------ */
/* Output helpers                                                      */

/* Append text, dropping CR and control characters other than LF and TAB. */
static void add_clean(buf_t *out, const char *s, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		unsigned char c = (unsigned char)s[i];
		if (c == '\n' || c == '\t' || (c >= 0x20 && c != 0x7F)) buf_addc(out, (char)c);
	}
}

static void ensure_nl(buf_t *out)
{
	if (out->len && out->d[out->len - 1] != '\n') buf_addc(out, '\n');
}

static void ensure_para(buf_t *out)
{
	ensure_nl(out);
	if (out->len >= 2 && out->d[out->len - 2] != '\n') buf_addc(out, '\n');
}

/* ------------------------------------------------------------------ */
/* Which parts are shown                                               */

static int is(const mpart_t *p, const char *type, const char *sub)
{
	return !strcmp(p->type, type) && (!sub || !strcmp(p->subtype, sub));
}

static int is_markdown(const mpart_t *p)
{
	return is(p, "text", "markdown") || is(p, "text", "x-markdown");
}

/* Parts whose content is displayed inline (everything else is listed). */
static int inline_part(const mpart_t *p)
{
	if (p->attachment) return 0;
	return is(p, "text", "plain") || is(p, "text", "html") || is_markdown(p) ||
	       is(p, "message", "rfc822");
}

/* PGP/MIME's signature and version parts: neither shown nor listed. */
static int pgp_part(const mpart_t *p)
{
	return is(p, "application", "pgp-signature") || is(p, "application", "pgp-encrypted");
}

static mv_leaf *find_leaf(msgview_t *mv, const mpart_t *p)
{
	for (int i = 0; i < mv->nleaves; i++)
		if (mv->leaves[i].part == p) return &mv->leaves[i];
	return NULL;
}

/* The message as one text: the header block and what was fetched of the
 * body (the fallback leaf l). */
static void whole_message(const msgview_t *mv, const mv_leaf *l, buf_t *msg)
{
	buf_add(msg, mv->hdr, mv->hlen);
	/* the header block normally ends in its blank line already */
	if (!(mv->hlen >= 2 && mv->hdr[mv->hlen - 1] == '\n' &&
	      (mv->hdr[mv->hlen - 2] == '\n' ||
	       (mv->hlen >= 3 && mv->hdr[mv->hlen - 2] == '\r' && mv->hdr[mv->hlen - 3] == '\n'))))
		buf_adds(msg, "\r\n");
	buf_add(msg, l->raw.d ? l->raw.d : "", l->raw.len);
}

/* Does the whole message (fallback leaf l) have a plain-text part? */
static int hdr_plain(const msgview_t *mv, const mv_leaf *l)
{
	buf_t msg = {0};
	whole_message(mv, l, &msg);
	int r = mime_has_plain(msg.d, msg.len);
	buf_free(&msg);
	return r;
}

/* Remove the CRs of text t in place. */
static void drop_cr(char *t, size_t *n)
{
	size_t k = 0;
	for (size_t i = 0; i < *n; i++)
		if (t[i] != '\r') t[k++] = t[i];
	*n = k;
	t[k] = '\0';
}

/* Replace the inline PGP blocks of t (malloc'd) by what they became. */
static void pgp_subst(const msgview_t *mv, char **t, size_t *n)
{
	for (int i = 0; i < mv->npgp_blk; i++) {
		const mv_pgpblk *b = &mv->pgp_blk[i];
		size_t al = strlen(b->armor);
		const char *at = al ? memmem(*t, *n, b->armor, al) : NULL;
		if (!at) continue;
		buf_t r = {0};
		buf_add(&r, *t, (size_t)(at - *t));
		buf_adds(&r, b->result);
		buf_add(&r, at + al, *n - (size_t)(at - *t) - al);
		free(*t);
		*n = r.len;
		*t = r.d ? buf_steal(&r) : xstrdup("");
	}
}

/* mime_text_part's hook: inline PGP blocks replaced (in plain text) */
static void pgp_hook(void *ctx, char **t, size_t *n)
{
	drop_cr(*t, n);
	pgp_subst(ctx, t, n);
}

static void render_leaf(msgview_t *mv, mpart_t *p, buf_t *out)
{
	mv_leaf *l = find_leaf(mv, p);
	if (!l) return;
	const char *data = l->raw.d ? l->raw.d : "";

	if (p == &mv->fallback) {
		char *ct = mime_header_raw(mv->hdr, mv->hlen, "Content-Type");
		if (ct && strcasestr(ct, "format=flowed")) mv->flowed = 1;
		free(ct);
		/* whole body as one blob: let the raw parser handle it */
		buf_t msg = {0};
		whole_message(mv, l, &msg);
		mime_set_plain(mv->plain);               /* T */
		char *t = mime_render(msg.d, msg.len, 0);
		mime_set_plain(0);
		if (mv->npgp_blk) {
			size_t tn = strlen(t);
			pgp_subst(mv, &t, &tn);
		}
		/* mime_render prints its own header summary; skip it */
		char *body = strstr(t, "\n\n");
		buf_adds(out, body ? body + 2 : t);
		free(t);
		buf_free(&msg);
	} else if (is(p, "message", "rfc822")) {
		ensure_para(out);
		buf_adds(out, "---------- Attached message ----------\n");
		char *t = mime_render(data, l->raw.len, 0);
		buf_adds(out, t);
		free(t);
	} else {
		/* the same text pipeline as mime_render (mime.c) */
		mime_textopt o = {
			.kind = is(p, "text", "html") ? MIME_HTML : is_markdown(p) ? MIME_MARKDOWN : MIME_PLAIN,
			.cte = p->cte, .charset = p->charset,
			.flowed = p->flowed && is(p, "text", "plain"), .delsp = p->delsp,
			.as_sent = mv->plain,
			.hook = mv->npgp_blk ? pgp_hook : NULL, .ctx = mv,
		};
		if (o.flowed) mv->flowed = 1;
		size_t n;
		char *t = mime_text_part(data, l->raw.len, &o, &n);
		add_clean(out, t, n);
		free(t);
	}
	ensure_nl(out);

	if (!l->done) {
		char hs[32];
		mime_human_size(hs, sizeof hs, p->size > l->raw.len ? p->size - l->raw.len : 0);
		if (mv->more_off == MV_NO_MORE) mv->more_off = out->len;
		buf_printf(out, "[... %s more - scroll down to load ...]\n", hs);
	}
}

/* The part of a multipart/alternative that is shown: the plain text, but
 * not an empty one (some newsletters send one, with all the text in the
 * HTML part) or one that turned out to be an HTML page or full of CSS
 * (html_inside, see mv_repick) when there is HTML; else HTML, a multipart,
 * the first. mv->plain (T): the plain text whenever there is one. */
static int alt_pick(const msgview_t *mv, const mpart_t *p)
{
	int pick = -1, html = -1, multi = -1;
	for (int i = 0; i < p->nkids; i++) {
		const mpart_t *k = &p->kids[i];
		if (is(k, "text", "plain") && pick < 0) pick = i;
		else if (is(k, "text", "html") && html < 0) html = i;
		else if (is(k, "multipart", NULL) && multi < 0) multi = i;
	}
	/* the same rule as mime_render (mime.c) */
	int poor = pick >= 0 && (p->kids[pick].size <= MIME_EMPTY_PART || p->kids[pick].html_inside);
	return mime_alt_choose(pick, html, multi, poor, mv->plain);
}

/* Walk the structure. With out == NULL, only collect the displayed leaves. */
static void walk(msgview_t *mv, mpart_t *p, buf_t *out)
{
	if (is(p, "multipart", NULL)) {
		if (!p->nkids) return;
		if (is(p, "multipart", "alternative")) {
			walk(mv, &p->kids[alt_pick(mv, p)], out);
		} else {
			for (int i = 0; i < p->nkids; i++) {
				if (out && i) ensure_para(out);
				walk(mv, &p->kids[i], out);
			}
		}
		return;
	}

	if (pgp_part(p)) return;
	if (inline_part(p) || p == &mv->fallback) {
		if (out) {
			render_leaf(mv, p, out);
		} else {
			mv->leaves = xrealloc(mv->leaves, sizeof *mv->leaves * (size_t)(mv->nleaves + 1));
			mv->leaves[mv->nleaves++] = (mv_leaf){.part = p};
		}
	} else if (out) {
		char type[100];
		snprintf(type, sizeof type, "%s/%s", p->type, p->subtype);
		ensure_nl(out);
		mime_att_line(out, p->name, type, p->size, p->cte);
	}
}

/* ------------------------------------------------------------------ */
/* Loading                                                             */

/* Fetch the next block of the given leaves in one round trip. */
static int fetch_blocks(msgview_t *mv, store_t *im, mv_leaf **ls, int n)
{
	if (n == 0) return 0;
	imap_partial_t *req = xcalloc((size_t)n, sizeof *req);
	for (int i = 0; i < n; i++) {
		req[i].section = ls[i]->part->section;
		req[i].off = ls[i]->raw.len;
		req[i].len = TEXT_BLOCK;
		req[i].out = &ls[i]->raw;
	}
	int rc = store_fetch_partial(im, mv->uid, req, n);
	if (rc == 0)
		for (int i = 0; i < n; i++) {
			mv_leaf *l = ls[i];
			/* short read = end of part; also stop if the size says so */
			if (req[i].got < TEXT_BLOCK || (l->part->size && l->raw.len >= l->part->size))
				l->done = 1;
		}
	free(req);
	return rc;
}

/* Walk the structure again (after html_inside or mv->plain changed) and
 * fetch the parts that are new; the ones fetched already are kept. */
static int rewalk(msgview_t *mv, store_t *im)
{
	mv_leaf *old = mv->leaves;
	int nold = mv->nleaves;
	mv->leaves = NULL;
	mv->nleaves = 0;
	walk(mv, mv->root, NULL);
	mv_leaf **ls = xcalloc((size_t)mv->nleaves + 1, sizeof *ls);
	int n = 0;
	for (int i = 0; i < mv->nleaves; i++) {
		mv_leaf *l = &mv->leaves[i];
		for (int k = 0; k < nold; k++)          /* fetched already: keep it */
			if (old[k].part == l->part) {
				*l = old[k];
				old[k].raw = (buf_t){0};
			}
		if (l->part->size == 0) l->done = 1;
		else if (!l->done && !l->raw.len) ls[n++] = l;
	}
	for (int k = 0; k < nold; k++) buf_free(&old[k].raw);
	free(old);
	int rc = fetch_blocks(mv, im, ls, n);
	free(ls);
	return rc;
}

/* A plain-text part that is an HTML page or full of CSS: show the HTML
 * part of its multipart/alternative instead (unless T asks for the plain
 * text). Checked on what was fetched of each plain leaf. */
int mv_repick(msgview_t *mv, store_t *im)
{
	if (!mv->root) return 0;
	int again = mv->plain_changed;
	mv->plain_changed = 0;
	for (int i = 0; !mv->plain && i < mv->nleaves; i++) {
		mv_leaf *l = &mv->leaves[i];
		if (!is(l->part, "text", "plain") || !l->raw.len || l->part->html_inside) continue;
		size_t dn;
		char *t = mime_decode_text(l->raw.d, l->raw.len, l->part->cte, l->part->charset, &dn);
		if (mime_plain_is_bad(t, dn)) {
			l->part->html_inside = 1;
			again = 1;
		}
		free(t);
	}
	return again ? rewalk(mv, im) : 0;
}

/* Is there a plain-text part that T can show (in a multipart/alternative,
 * or the message itself)? */
static int has_plain(const mpart_t *p)
{
	if (is(p, "text", "plain") && !p->attachment) return 1;
	if (!is(p, "multipart", NULL)) return 0;
	for (int i = 0; i < p->nkids; i++)
		if (has_plain(&p->kids[i])) return 1;
	return 0;
}

int mv_set_plain(msgview_t *mv, store_t *im, int on)
{
	if (!mv->root) {                         /* the whole message: mime_render */
		mv_leaf *l = find_leaf(mv, &mv->fallback);
		if (on && !(l && hdr_plain(mv, l))) return 1;
		mv->plain = on;
		return 0;
	}
	if (on && !has_plain(mv->root)) return 1;
	if (mv->plain == on) return 0;
	mv->plain = on;
	mv->plain_changed = 1;
	return mv_repick(mv, im) == 0 ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* PGP                                                                 */

int mv_raw(msgview_t *mv, store_t *im, buf_t *out)
{
	mv_leaf *l = mv->pgp_decrypted ? find_leaf(mv, &mv->fallback) : NULL;
	if (l) {
		whole_message(mv, l, out);
		return 0;
	}
	imap_partial_t req = {.section = "", .off = 0, .len = (size_t)-1 / 2, .out = out};
	return store_fetch_partial(im, mv->uid, &req, 1);
}

static char *sender(const msgview_t *mv)
{
	char *f = mime_header_raw(mv->hdr, mv->hlen, "From");
	char *a = f ? mime_addr_email(f) : NULL;
	free(f);
	return a;
}

/* What kind of PGP/MIME entity a header block is: 's' signed, 'e'
 * encrypted, 0 neither. */
static int pgp_kind(const char *hdr, size_t hlen)
{
	char *ct = mime_header_raw(hdr, hlen, "Content-Type");
	int k = 0;
	if (ct) {
		const char *v = ct;
		while (*v == ' ' || *v == '\t') v++;
		if (!strncasecmp(v, "multipart/signed", 16) && strcasestr(v, "application/pgp-signature")) k = 's';
		if (!strncasecmp(v, "multipart/encrypted", 19) && strcasestr(v, "application/pgp-encrypted")) k = 'e';
	}
	free(ct);
	return k;
}

static void note_add(buf_t *note, char *n)
{
	if (!n) return;
	if (note->len) {
		buf_adds(note, "; ");
		n[0] = (char)tolower((unsigned char)n[0]);
	}
	buf_adds(note, n);
	free(n);
}

/* The signature of multipart/signed message raw. */
static void pgp_signed(const char *raw, size_t len, const char *from, buf_t *note)
{
	mime_span *ps;
	int n = mime_split(raw, len, &ps);
	if (n < 2) {
		note_add(note, xstrdup("Signed, but the parts of the message are not complete"));
		free(ps);
		return;
	}
	buf_t data = {0};                      /* the signed part as sent: CRLF line ends */
	for (size_t i = 0; i < ps[0].len; i++) {
		char c = ps[0].p[i];
		if (c == '\n' && (i == 0 || ps[0].p[i - 1] != '\r')) buf_addc(&data, '\r');
		buf_addc(&data, c);
	}
	const char *b;
	size_t bl, sl;
	mime_body_of(ps[1].p, ps[1].len, &b, &bl);
	char *cte = mime_header_raw(ps[1].p, ps[1].len, "Content-Transfer-Encoding");
	char *sig = mime_decode_cte(b, bl, cte, &sl);
	note_add(note, mv_pgp->verify(data.d ? data.d : "", data.len, sig, sl, from));
	free(cte);
	free(sig);
	buf_free(&data);
	free(ps);
}

/* Header block hdr without its Content-* fields and the blank line. */
static void hdr_without_content(const char *h, size_t hl, buf_t *out)
{
	int skip = 0;
	for (size_t i = 0; i < hl;) {
		const char *e = memchr(h + i, '\n', hl - i);
		size_t n = e ? (size_t)(e - (h + i)) + 1 : hl - i;
		const char *l = h + i;
		size_t ll = n;
		while (ll && (l[ll - 1] == '\n' || l[ll - 1] == '\r')) ll--;
		if (!ll) break;                                /* the blank line */
		if (*l != ' ' && *l != '\t') skip = ll >= 8 && !strncasecmp(l, "Content-", 8);
		if (!skip) {
			buf_add(out, l, ll);
			buf_adds(out, "\r\n");
		}
		i += n;
	}
}

/* A PGP/MIME message: check its signature, or decrypt it; a decrypted
 * message is shown from then on like one without a structure (the
 * fallback leaf holds its body, hdr its header block). */
static void pgp_mime(msgview_t *mv, store_t *im)
{
	int kind = pgp_kind(mv->hdr, mv->hlen);
	if (!kind) return;
	char *from = sender(mv);
	buf_t note = {0}, raw = {0};
	if (mv->size > PGP_MAX) {
		note_add(&note, xstrdup(kind == 's' ? "Signed; not checked, the message is too large"
		                                     : "Encrypted; not decrypted, the message is too large"));
		goto out;
	}
	if (mv_raw(mv, im, &raw) != 0) {
		note_add(&note, xstrdup("The message could not be loaded to check it"));
		goto out;
	}
	if (kind == 'e') {
		mime_span *ps;
		int n = mime_split(raw.d, raw.len, &ps);
		mv->pgp_enc = 1;
		if (n < 2) {
			note_add(&note, xstrdup("Encrypted, but the encrypted part is missing"));
			free(ps);
			goto out;
		}
		const char *b;
		size_t bl, el;
		mime_body_of(ps[1].p, ps[1].len, &b, &bl);
		char *cte = mime_header_raw(ps[1].p, ps[1].len, "Content-Transfer-Encoding");
		char *enc = mime_decode_cte(b, bl, cte, &el);
		free(cte);
		free(ps);
		buf_t plain = {0};
		int was;
		note_add(&note, mv_pgp->decrypt(enc, el, &plain, &was, from));
		free(enc);
		if (!plain.len) goto out;
		/* the message again: its own header lines, then the decrypted entity */
		buf_t msg = {0};
		hdr_without_content(mv->hdr, mv->hlen, &msg);
		const char *pd = plain.d;
		size_t pl = plain.len;
		size_t c = strcspn(pd, ":\n");
		if (pd[c] != ':' || memchr(pd, ' ', c)) buf_adds(&msg, "Content-Type: text/plain; charset=utf-8\r\n\r\n");
		buf_add(&msg, pd, pl);
		buf_free(&plain);
		const char *e1 = memmem(msg.d, msg.len, "\r\n\r\n", 4), *e2 = memmem(msg.d, msg.len, "\n\n", 2);
		size_t hl = e1 && (!e2 || e1 < e2) ? (size_t)(e1 - msg.d) + 4 : e2 ? (size_t)(e2 - msg.d) + 2 : msg.len;
		free(mv->hdr);
		mv->hdr = xmalloc(hl + 1);
		memcpy(mv->hdr, msg.d, hl);
		mv->hdr[hl] = '\0';
		mv->hlen = hl;
		for (int i = 0; i < mv->nleaves; i++) buf_free(&mv->leaves[i].raw);
		free(mv->leaves);
		mpart_free(mv->root);
		mv->root = NULL;
		memset(&mv->fallback, 0, sizeof mv->fallback);
		snprintf(mv->fallback.section, sizeof mv->fallback.section, "TEXT");
		snprintf(mv->fallback.type, sizeof mv->fallback.type, "text");
		snprintf(mv->fallback.subtype, sizeof mv->fallback.subtype, "plain");
		mv->fallback.size = (uint32_t)(msg.len - hl);
		mv->leaves = xcalloc(1, sizeof *mv->leaves);
		mv->leaves[0].part = &mv->fallback;
		buf_add(&mv->leaves[0].raw, msg.d + hl, msg.len - hl);
		mv->leaves[0].done = 1;
		mv->nleaves = 1;
		mv->pgp_decrypted = 1;
		/* signed and then encrypted (RFC 3156 6.1) */
		if (pgp_kind(mv->hdr, mv->hlen) == 's') pgp_signed(msg.d, msg.len, from, &note);
		buf_free(&msg);
	} else {
		pgp_signed(raw.d, raw.len, from, &note);
	}
out:
	buf_free(&raw);
	free(from);
	mv->pgp_note = note.len ? buf_steal(&note) : NULL;
	buf_free(&note);
}

/* Find the next inline PGP block in t from *pos: its start and length. */
static int next_armor(const char *t, size_t n, size_t *pos, size_t *len)
{
	static const char *const begin[] = {"-----BEGIN PGP MESSAGE-----", "-----BEGIN PGP SIGNED MESSAGE-----"};
	static const char *const end[] = {"-----END PGP MESSAGE-----", "-----END PGP SIGNATURE-----"};
	for (size_t i = *pos; i < n;) {
		for (int k = 0; k < 2; k++) {
			size_t bl = strlen(begin[k]);
			if (n - i >= bl && !memcmp(t + i, begin[k], bl)) {
				size_t el = strlen(end[k]);
				const char *e = memmem(t + i, n - i, end[k], el);
				if (!e) return 0;
				*pos = i;
				*len = (size_t)(e - (t + i)) + el;
				return 1;
			}
		}
		const char *nl = memchr(t + i, '\n', n - i);
		if (!nl) break;
		i = (size_t)(nl - t) + 1;
	}
	return 0;
}

/* Inline PGP in the loaded text parts: decrypt or check every block once
 * (when its part is loaded completely). */
static void pgp_inline(msgview_t *mv)
{
	char *from = NULL;
	for (int i = 0; i < mv->nleaves; i++) {
		mv_leaf *l = &mv->leaves[i];
		mpart_t *p = l->part;
		if (!l->done || !l->raw.len) continue;
		char *t;
		size_t n;
		if (p == &mv->fallback) {
			buf_t msg = {0};
			whole_message(mv, l, &msg);
			mime_set_plain(1);
			t = mime_render_body(msg.d, msg.len);
			mime_set_plain(0);
			n = strlen(t);
			buf_free(&msg);
		} else if (is(p, "text", "plain")) {
			t = mime_decode_text(l->raw.d, l->raw.len, p->cte, p->charset, &n);
			drop_cr(t, &n);
		} else continue;
		size_t pos = 0, al;
		while (next_armor(t, n, &pos, &al)) {
			int seen = 0;
			for (int k = 0; k < mv->npgp_blk; k++)
				if (strlen(mv->pgp_blk[k].armor) == al && !memcmp(mv->pgp_blk[k].armor, t + pos, al)) seen = 1;
			if (!seen) {
				if (!from) from = sender(mv);
				char *armor = xmalloc(al + 2);
				memcpy(armor, t + pos, al);
				armor[al] = '\0';
				buf_t out = {0};
				int enc = 0;
				char *note = mv_pgp->decrypt(armor, al, &out, &enc, from);
				if (enc) mv->pgp_enc = 1;
				buf_t r = {0};
				buf_printf(&r, "[PGP: %s]\n", note);
				free(note);
				if (out.len) {
					size_t ol = out.len;
					char *o = buf_steal(&out);
					drop_cr(o, &ol);
					while (ol && o[ol - 1] == '\n') ol--;
					buf_add(&r, o, ol);
					free(o);
				} else {
					buf_adds(&r, armor);
				}
				buf_free(&out);
				mv->pgp_blk = xrealloc(mv->pgp_blk, sizeof *mv->pgp_blk * (size_t)(mv->npgp_blk + 1));
				mv->pgp_blk[mv->npgp_blk++] = (mv_pgpblk){armor, buf_steal(&r)};
			}
			pos += al;
		}
		free(t);
	}
	free(from);
}

int mv_open(msgview_t *mv, store_t *im, uint32_t uid, uint32_t size)
{
	memset(mv, 0, sizeof *mv);
	mv->uid = uid;
	mv->size = size;
	mv->more_off = MV_NO_MORE;

	if (store_fetch_structure(im, uid, &mv->hdr, &mv->hlen, &mv->root) != 0) return -1;

	if (mv->root) {
		walk(mv, mv->root, NULL);
	} else {
		/* no structure: fetch the body text as one blob */
		snprintf(mv->fallback.section, sizeof mv->fallback.section, "TEXT");
		snprintf(mv->fallback.type, sizeof mv->fallback.type, "text");
		snprintf(mv->fallback.subtype, sizeof mv->fallback.subtype, "plain");
		mv->fallback.size = size > mv->hlen ? (uint32_t)(size - mv->hlen) : 0;
		mv->leaves = xcalloc(1, sizeof *mv->leaves);
		mv->leaves[0].part = &mv->fallback;
		mv->nleaves = 1;
	}

	mv_leaf **ls = xcalloc((size_t)mv->nleaves + 1, sizeof *ls);
	int n = 0;
	for (int i = 0; i < mv->nleaves; i++) {
		if (mv->leaves[i].part->size == 0 && mv->root) mv->leaves[i].done = 1;
		else ls[n++] = &mv->leaves[i];
	}
	int rc = fetch_blocks(mv, im, ls, n);
	free(ls);

	if (rc == 0) rc = mv_repick(mv, im);
	if (rc == 0 && mv_pgp) {
		pgp_mime(mv, im);
		pgp_inline(mv);
	}
	mv->open = 1;
	if (rc != 0) {
		mv_close(mv);
		return -1;
	}
	return 0;
}

/* The parts walk() lists as attachments. */
static void walk_atts(msgview_t *mv, mpart_t *p, mpart_t ***out, int *n)
{
	if (is(p, "multipart", NULL)) {
		if (!p->nkids) return;
		if (is(p, "multipart", "alternative")) {
			walk_atts(mv, &p->kids[alt_pick(mv, p)], out, n);
		} else {
			for (int i = 0; i < p->nkids; i++) walk_atts(mv, &p->kids[i], out, n);
		}
	} else if (!inline_part(p) && p != &mv->fallback && !pgp_part(p)) {
		*out = xrealloc(*out, sizeof **out * (size_t)(*n + 1));
		(*out)[(*n)++] = p;
	}
}

int mv_attachments(msgview_t *mv, mpart_t ***out)
{
	int n = 0;
	*out = NULL;
	if (mv->open && mv->root) walk_atts(mv, mv->root, out, &n);
	return n;
}

int mv_fetch_part(msgview_t *mv, store_t *im, const mpart_t *p, buf_t *out)
{
	enum { BLOCK = 1024 * 1024 };
	for (;;) {
		imap_partial_t req = {.section = p->section, .off = out->len, .len = BLOCK, .out = out};
		if (store_fetch_partial(im, mv->uid, &req, 1) != 0) return -1;
		if (req.got < BLOCK) return 0;
	}
}

int mv_more(msgview_t *mv, store_t *im)
{
	for (int i = 0; i < mv->nleaves; i++) {
		if (!mv->leaves[i].done) {
			mv_leaf *l = &mv->leaves[i];
			int rc = fetch_blocks(mv, im, &l, 1);
			if (rc == 0 && l->done && mv_pgp) pgp_inline(mv);
			return rc;
		}
	}
	return 0;
}

void mv_render(msgview_t *mv, int all_headers)
{
	buf_t out = {0};
	free(mv->text);
	mv->more_off = MV_NO_MORE;

	if (all_headers) {
		add_clean(&out, mv->hdr, mv->hlen);
		ensure_para(&out);
	} else {
		char *s = mime_summary(mv->hdr, mv->hlen);
		buf_adds(&out, s);
		free(s);
	}
	mv->hdr_end = out.len;               /* not the blank line(s) after it */
	while (mv->hdr_end && out.d[mv->hdr_end - 1] == '\n') mv->hdr_end--;
	if (mv->pgp_note) {
		ensure_para(&out);
		buf_printf(&out, "[PGP: %s]\n\n", mv->pgp_note);
	}
	textconv_styles(1);                  /* Markdown: bold, italic, ... marked */
	walk(mv, mv->root ? mv->root : &mv->fallback, &out);
	textconv_styles(0);
	free(mv->sty);
	mv->sty = NULL;
	mv->nsty = 0;
	if (out.d) out.len = ts_strip(out.d, out.len, &mv->sty, &mv->nsty, &mv->more_off);

	/* trim trailing blank lines, but never cut into the "more" marker */
	size_t min = mv->more_off == MV_NO_MORE ? 0 : mv->more_off;
	while (out.len > min && (out.d[out.len - 1] == '\n' || out.d[out.len - 1] == ' '))
		out.len--;
	buf_addc(&out, '\n');
	mv->textlen = out.len;
	mv->text = buf_steal(&out);
}

char *mv_body(msgview_t *mv)
{
	buf_t out = {0};
	size_t saved = mv->more_off;
	mv->more_off = MV_NO_MORE;
	walk(mv, mv->root ? mv->root : &mv->fallback, &out);
	if (mv->more_off != MV_NO_MORE) {
		out.len = mv->more_off;
		buf_adds(&out, "[...]\n");
	}
	mv->more_off = saved;
	while (out.len && (out.d[out.len - 1] == '\n' || out.d[out.len - 1] == ' ')) out.len--;
	return buf_steal(&out);
}

const unsigned char *mv_hex_peek(msgview_t *mv, size_t off, size_t len)
{
	if (off + len > mv->size) len = off < mv->size ? mv->size - off : 0;
	if (len == 0) return (const unsigned char *)"";
	if (off >= mv->hex_off && off + len <= mv->hex_off + mv->hex.len)
		return (const unsigned char *)mv->hex.d + (off - mv->hex_off);
	return NULL;
}

int mv_hex_load(msgview_t *mv, store_t *im, size_t off, size_t len)
{
	size_t start = off / HEX_BLOCK * HEX_BLOCK;
	size_t end = (off + len + HEX_BLOCK - 1) / HEX_BLOCK * HEX_BLOCK;
	if (end - start > HEX_MAX) end = start + HEX_MAX;

	buf_free(&mv->hex);
	mv->hex_off = start;
	imap_partial_t req = {.section = "", .off = start, .len = end - start, .out = &mv->hex};
	int rc = store_fetch_partial(im, mv->uid, &req, 1);
	/* the server's size may differ slightly from RFC822.SIZE; trust what it sent */
	if (rc == 0 && req.got < end - start && start + req.got < mv->size)
		mv->size = (uint32_t)(start + req.got);
	return rc;
}

void mv_close(msgview_t *mv)
{
	for (int i = 0; i < mv->nleaves; i++) buf_free(&mv->leaves[i].raw);
	free(mv->leaves);
	free(mv->hdr);
	free(mv->text);
	free(mv->sty);
	mpart_free(mv->root);
	buf_free(&mv->hex);
	free(mv->pgp_note);
	for (int i = 0; i < mv->npgp_blk; i++) {
		free(mv->pgp_blk[i].armor);
		free(mv->pgp_blk[i].result);
	}
	free(mv->pgp_blk);
	memset(mv, 0, sizeof *mv);
	mv->more_off = MV_NO_MORE;
}
