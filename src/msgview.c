/* msgview.c - an open message, loaded piece by piece from the server */
#include "msgview.h"
#include "mime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEXT_BLOCK  (64 * 1024)   /* bytes of a text part fetched at a time */
#define HEX_BLOCK   (16 * 1024)   /* hex window granularity */
#define HEX_MAX     (64 * 1024)   /* largest hex window kept */

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

static mv_leaf *find_leaf(msgview_t *mv, const mpart_t *p)
{
	for (int i = 0; i < mv->nleaves; i++)
		if (mv->leaves[i].part == p) return &mv->leaves[i];
	return NULL;
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
		buf_add(&msg, mv->hdr, mv->hlen);
		/* the header block normally ends in its blank line already */
		if (!(mv->hlen >= 2 && mv->hdr[mv->hlen - 1] == '\n' &&
		      (mv->hdr[mv->hlen - 2] == '\n' ||
		       (mv->hlen >= 3 && mv->hdr[mv->hlen - 2] == '\r' && mv->hdr[mv->hlen - 3] == '\n'))))
			buf_adds(&msg, "\r\n");
		buf_add(&msg, data, l->raw.len);
		char *t = mime_render(msg.d, msg.len, 0);
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
		size_t n;
		int kind = is(p, "text", "html") ? MIME_HTML : is_markdown(p) ? MIME_MARKDOWN : MIME_PLAIN;
		char *t = mime_decode_text(data, l->raw.len, kind, p->cte, p->charset, &n);
		if (p->flowed && is(p, "text", "plain")) {   /* RFC 3676: join soft breaks */
			char *u = mime_unflow(t, n, p->delsp, &n);
			free(t);
			t = u;
			mv->flowed = 1;
		}
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

/* A text part this small (encoded octets) is empty: at most a line break
 * or two, also in base64. */
#define EMPTY_PART 8

/* The part of a multipart/alternative that is shown: the plain text, but
 * not an empty one (some newsletters send one, with all the text in the
 * HTML part) when there is HTML; else HTML, a multipart, the first. */
static int alt_pick(const mpart_t *p)
{
	int pick = -1, html = -1, multi = -1;
	for (int i = 0; i < p->nkids; i++) {
		const mpart_t *k = &p->kids[i];
		if (is(k, "text", "plain") && pick < 0) pick = i;
		else if (is(k, "text", "html") && html < 0) html = i;
		else if (is(k, "multipart", NULL) && multi < 0) multi = i;
	}
	if (pick >= 0 && p->kids[pick].size <= EMPTY_PART && html >= 0) pick = html;
	if (pick < 0) pick = html >= 0 ? html : multi >= 0 ? multi : 0;
	return pick;
}

/* Walk the structure. With out == NULL, only collect the displayed leaves. */
static void walk(msgview_t *mv, mpart_t *p, buf_t *out)
{
	if (is(p, "multipart", NULL)) {
		if (!p->nkids) return;
		if (is(p, "multipart", "alternative")) {
			walk(mv, &p->kids[alt_pick(p)], out);
		} else {
			for (int i = 0; i < p->nkids; i++) {
				if (out && i) ensure_para(out);
				walk(mv, &p->kids[i], out);
			}
		}
		return;
	}

	if (inline_part(p) || p == &mv->fallback) {
		if (out) {
			render_leaf(mv, p, out);
		} else {
			mv->leaves = xrealloc(mv->leaves, sizeof *mv->leaves * (size_t)(mv->nleaves + 1));
			mv->leaves[mv->nleaves++] = (mv_leaf){.part = p};
		}
	} else if (out) {
		char hs[32];
		size_t sz = !strcmp(p->cte, "base64") ? (size_t)p->size * 3 / 4 : p->size;
		mime_human_size(hs, sizeof hs, sz);
		ensure_nl(out);
		buf_printf(out, "[Attachment: %s  (%s/%s, %s)]\n", p->name ? p->name : "(unnamed)",
		           p->type, p->subtype, hs);
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
			walk_atts(mv, &p->kids[alt_pick(p)], out, n);
		} else {
			for (int i = 0; i < p->nkids; i++) walk_atts(mv, &p->kids[i], out, n);
		}
	} else if (!inline_part(p) && p != &mv->fallback) {
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
			return fetch_blocks(mv, im, &l, 1);
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
	walk(mv, mv->root ? mv->root : &mv->fallback, &out);

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
	mpart_free(mv->root);
	buf_free(&mv->hex);
	memset(mv, 0, sizeof *mv);
	mv->more_off = MV_NO_MORE;
}
