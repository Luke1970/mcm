/* send.c - writing mail (new, reply, forward) and sending or keeping it
 */
#include "mcm.h"

#define USER_AGENT "mcm/" MCM_VERSION " (Mail Commander)"

static ui_bg_fn smtp_bg = NULL;      /* dialog background while sending */

static void from_line(char *out, size_t n);
static const folder_t *special_folder(int kind);
static int save_to(int kind, const buf_t *msg, const char *flags, char *note, size_t nl);
static int keep_cb(void *ctx, const char *text, size_t len, int *line, char *msg, size_t ml);
static int send_cb(void *ctx, const char *text, size_t len, int *line, char *msg, size_t ml);
static void add_hdr(buf_t *b, const char *name, const char *v);
static void quote_flowed(buf_t *t, const char *l, size_t n);

/* From: "Full Name <address>", the name quoted when it needs to be. */
static void from_line(char *out, size_t n)
{
	char addr[512];
	login_address(addr, sizeof addr);
	if (!*addr) {                 /* not known yet: the user fills it in */
		snprintf(out, n, "%s", "");
		return;
	}
	if (!*ac->fullname) {
		snprintf(out, n, "%s", addr);
		return;
	}
	int plain = 1;
	for (const unsigned char *p = (const unsigned char *)ac->fullname; *p; p++)
		if (!(isalnum(*p) || *p >= 0x80 || *p == ' ' || strchr("!#$%&'*+-/=?^_`{|}~", *p)))
			plain = 0;
	buf_t b = {0};
	if (plain) buf_adds(&b, ac->fullname);
	else {
		buf_addc(&b, '"');
		for (const char *p = ac->fullname; *p; p++) {
			if (*p == '"' || *p == '\\') buf_addc(&b, '\\');
			buf_addc(&b, *p);
		}
		buf_addc(&b, '"');
	}
	snprintf(out, n, "%s <%s>", buf_cstr(&b), addr);
	buf_free(&b);
}

/* Find the folder with a special-use attribute, or by a usual name. */
static const folder_t *special_folder(int kind)
{
	for (int i = 0; i < A.nfolders; i++)
		if (A.folders[i].special == kind && !A.folders[i].noselect) return &A.folders[i];
	static const char *const sent[] = {"Sent", "Sent Items", "Sent Messages", "Sent Mail",
	                                   "INBOX.Sent", "INBOX/Sent", NULL};
	static const char *const drafts[] = {"Drafts", "Draft", "INBOX.Drafts", "INBOX/Drafts", NULL};
	const char *const *names = kind == FOLDER_SENT ? sent : drafts;
	for (int k = 0; names[k]; k++)
		for (int i = 0; i < A.nfolders; i++) {
			char *u = mutf7_decode(A.folders[i].name);
			int hit = !strcasecmp(u, names[k]) && !A.folders[i].noselect;
			free(u);
			if (hit) return &A.folders[i];
		}
	return NULL;
}

typedef struct {
	char     auto_date[64];
	uint32_t reply_uid;            /* original message to flag \Answered, or 0 */
} compose_ctx;

static int save_to(int kind, const buf_t *msg, const char *flags, char *note, size_t nl)
{
	const folder_t *f = special_folder(kind);
	const char *what = kind == FOLDER_SENT ? "Sent" : "Drafts";
	if (!f) {
		snprintf(note, nl, "no %s folder found, no copy saved", what);
		return -1;
	}
	if (store_append(A.im, f->name, flags, msg->d, msg->len) != 0) {
		snprintf(note, nl, "could not save in %s: %s", f->display, store_error(A.im));
		return -1;
	}
	if (A.cur_folder >= 0 && !strcmp(A.folders[A.cur_folder].name, f->name)) A.last_poll = 0;
	snprintf(note, nl, "%s", f->display);
	return 0;
}

static int keep_cb(void *ctx, const char *text, size_t len, int *line, char *msg, size_t ml)
{
	compose_ctx *c = ctx;
	compose_opts o = {.user_agent = USER_AGENT, .auto_date = c->auto_date,
	                  .smtputf8 = 1, .eightbit = 1, .draft = 1};
	compose_out out;
	if (compose_build(text, len, &o, &out) != 0) {
		snprintf(msg, ml, "%s", out.err);
		*line = out.err_line;
		return 0;
	}
	ui_progress("Saving draft ...", editor_redraw);
	char note[300];
	int rc = save_to(FOLDER_DRAFTS, &out.copy, "\\Draft \\Seen", note, sizeof note);
	compose_out_free(&out);
	if (rc != 0) {
		snprintf(msg, ml, "Draft not saved: %s", note);
		return 0;
	}
	set_status("Draft saved in %s", note);
	return 1;
}

static int send_cb(void *ctx, const char *text, size_t len, int *line, char *msg, size_t ml)
{
	compose_ctx *c = ctx;
	compose_opts o = {.user_agent = USER_AGENT, .auto_date = c->auto_date,
	                  .smtputf8 = 1, .eightbit = 1};
	compose_out out;

	/* 1. syntax check before anything goes over the network */
	if (compose_build(text, len, &o, &out) != 0) {
		snprintf(msg, ml, "Not sent: %s", out.err);
		*line = out.err_line;
		return 0;
	}
	char who[300] = "";
	for (int i = 0; i < out.nrcpt && strlen(who) < 200; i++)
		snprintf(who + strlen(who), sizeof who - strlen(who), "%s%s", i ? ", " : "", out.rcpt[i]);
	char q[4][400];
	const char *ql[4];
	int nq = 0;
	snprintf(q[nq], sizeof q[0], "Send to %s%s?", who, strlen(who) >= 200 ? " ..." : "");
	ql[nq] = q[nq];
	nq++;
	if (out.nattach) {
		char hs[32];
		mime_human_size(hs, sizeof hs, out.attach_bytes);
		snprintf(q[nq], sizeof q[0], "With %d attachment%s (%s).", out.nattach,
		         out.nattach == 1 ? "" : "s", hs);
		ql[nq] = q[nq];
		nq++;
	}
	if (out.no_subject) {
		snprintf(q[nq], sizeof q[0], "Note: the message has no subject.");
		ql[nq] = q[nq];
		nq++;
	}
	char me[512];
	login_address(me, sizeof me);
	if (strcasecmp(out.mail_from, me)) {
		/* the server will usually refuse a sender that is not the login */
		snprintf(q[nq], sizeof q[0], "Note: sender %.150s is not your login address %.150s.",
		         out.mail_from, me);
		ql[nq] = q[nq];
		nq++;
	}
	compose_out_free(&out);
	static const ui_menuitem yn[] = {{"Send", 0, 's'}, {"Cancel", 0, 'c'}};
	if (ui_choice_dialog("Send message", ql, nq, yn, 2, 0, 0, editor_redraw) != 0)
		return 0;

	/* 2. connect */
	smtp_derive();
	smtp_bg = editor_redraw;
	A.smtp_trust.fn = cert_check;
	A.smtp_trust.ctx = &smtp_bg;
	smtp_caps caps;
	smtp_t *s = NULL;
	char err[512];
	for (;;) {
		char pm[400];
		snprintf(pm, sizeof pm, "Connecting to %s:%s ...", ac->smtp_host, ac->smtp_port);
		ui_progress(pm, editor_redraw);
		if (!A.smtp_pass && ac->smtp_pass_enc &&
		    get_master("For the stored SMTP password", editor_redraw) == 0) {
			char aad[600];
			aad_smtp(ac, aad, sizeof aad);
			A.smtp_pass = vault_decrypt(master, aad, ac->smtp_pass_enc);
		}
		const char *pass = A.smtp_pass ? A.smtp_pass : store_pass(A.im);
		int implicit = *ac->smtp_security ? !strcasecmp(ac->smtp_security, "tls")
		                                : !strcmp(ac->smtp_port, "465");
		s = smtp_open(ac->smtp_host, ac->smtp_port, implicit, ac->smtp_user, pass, &A.smtp_trust, 1,
		              &caps, err, sizeof err);
		if (s) break;
		if (smtp_settings(err, editor_redraw) != 0) {
			snprintf(msg, ml, "Not sent: %s", err);
			return 0;
		}
	}

	/* 3. build for this server: SMTPUTF8 / 8BITMIME decide the encodings */
	o.smtputf8 = caps.smtputf8;
	o.eightbit = caps.eightbitmime;
	if (compose_build(text, len, &o, &out) != 0) {
		smtp_close(s);
		snprintf(msg, ml, "Not sent: %s", out.err);
		*line = out.err_line;
		return 0;
	}
	ui_progress("Sending ...", editor_redraw);
	int rc = smtp_send(s, out.mail_from, out.rcpt, out.nrcpt, out.msg.d, out.msg.len, out.smtputf8,
	                   out.eightbit, err, sizeof err);
	smtp_close(s);
	if (rc != 0) {
		compose_out_free(&out);
		snprintf(msg, ml, "Not sent: %s", err);
		return 0;
	}

	/* 4. copy to Sent, flag the original as answered */
	ui_progress("Saving a copy ...", editor_redraw);
	char note[300];
	int saved = save_to(FOLDER_SENT, &out.copy, "\\Seen", note, sizeof note);
	if (c->reply_uid) store_add_flags(A.im, c->reply_uid, "\\Answered");
	set_status("Sent to %s; %s%s", who, saved == 0 ? "copy saved in " : "", note);
	compose_out_free(&out);
	return 1;
}

static void add_hdr(buf_t *b, const char *name, const char *v)
{
	buf_printf(b, "%s: %s\n", name, v ? v : "");
}

/* Quote one unwrapped (paragraph) line of a flowed message: one more '>'
 * and the text wrapped at 72 columns, each soft break ending in a space. */
static void quote_flowed(buf_t *t, const char *l, size_t n)
{
	size_t d = 0;
	while (d < n && l[d] == '>') d++;
	const char *s = l + d;
	size_t sl = n - d;
	if (sl && *s == ' ') { s++; sl--; }
	while (sl && (s[sl - 1] == ' ' || s[sl - 1] == '\r')) sl--;   /* also "-- " */
	char pre[40];
	size_t pl = d + 1 < 30 ? d + 1 : 30;
	memset(pre, '>', pl);
	pre[pl] = '\0';
	if (!sl) {
		buf_printf(t, "%s\n", pre);
		return;
	}
	int width = 72 - (int)pl - 1;
	if (width < 30) width = 30;
	while (sl) {
		/* take characters up to the width, then back up to a space */
		size_t i = 0, last_sp = 0;
		int col = 0;
		while (i < sl && col < width) {
			if (s[i] == ' ') last_sp = i;
			size_t cl;
			utf8_next(s + i, sl - i, &cl);
			i += cl;
			col++;
		}
		size_t cut;
		if (i >= sl) cut = sl;
		else if (s[i] == ' ') cut = i + 1;
		else if (last_sp) cut = last_sp + 1;
		else {                                     /* one long word */
			while (i < sl && s[i] != ' ') i++;
			cut = i < sl ? i + 1 : sl;
		}
		buf_printf(t, "%s ", pre);
		buf_add(t, s, cut);                        /* ends in a space if soft */
		buf_addc(t, '\n');
		s += cut;
		sl -= cut;
	}
}

/* Start the editor: 'm' new mail, 'r' reply, 'f' forward. */
void compose_start(int kind)
{
	compose_ctx c = {0};
	snprintf(c.auto_date, sizeof c.auto_date, "%s", compose_date(time(NULL)));
	char from[800];
	from_line(from, sizeof from);

	char *ohdr = NULL, *obody = NULL;
	size_t ohlen = 0;
	if (kind != 'm') {
		const msg_t *m = msg_at(A.msel);
		if (!m || !m->uid) {
			set_status("Select a message to %s first", kind == 'r' ? "reply to" : "forward");
			return;
		}
		if (!A.mv.open || A.mv.uid != m->uid) load_selected();
		if (!A.mv.open) return;
		ohdr = A.mv.hdr;
		ohlen = A.mv.hlen;
		obody = mv_body(&A.mv);
		if (kind == 'r') c.reply_uid = m->uid;
	}

	buf_t t = {0};
	int start = 1;                                   /* To: line */
	add_hdr(&t, "From", from);
	if (kind == 'r') {
		char *rt = mime_header_raw(ohdr, ohlen, "Reply-To");
		char *fr = mime_header_raw(ohdr, ohlen, "From");
		char *to = compose_addrs(rt ? rt : fr, NULL);
		add_hdr(&t, "To", to);
		free(to);
		free(rt);
		free(fr);
	} else {
		add_hdr(&t, "To", "");
	}
	add_hdr(&t, "Cc", "");
	add_hdr(&t, "Bcc", "");

	char *osub = kind != 'm' ? mime_header(ohdr, ohlen, "Subject") : NULL;
	if (kind == 'r') {
		buf_t sub = {0};
		if (!osub || strncasecmp(osub, "Re:", 3)) buf_adds(&sub, "Re: ");
		buf_adds(&sub, osub ? osub : "");
		add_hdr(&t, "Subject", sub.d);
		buf_free(&sub);
	} else if (kind == 'f') {
		buf_t sub = {0};
		buf_printf(&sub, "Fwd: %s", osub ? osub : "");
		add_hdr(&t, "Subject", sub.d);
		buf_free(&sub);
	} else {
		add_hdr(&t, "Subject", "");
	}
	add_hdr(&t, "Date", c.auto_date);

	if (kind == 'r') {
		char *mid = mime_header_raw(ohdr, ohlen, "Message-ID");
		char *refs = mime_header_raw(ohdr, ohlen, "References");
		if (mid && *mid) {
			add_hdr(&t, "In-Reply-To", mid);
			buf_t r = {0};
			if (refs && *refs) buf_printf(&r, "%s ", refs);
			buf_adds(&r, mid);
			add_hdr(&t, "References", r.d);
			buf_free(&r);
		}
		free(mid);
		free(refs);
	}
	buf_addc(&t, '\n');

	if (kind == 'r') {
		char *fr = mime_header(ohdr, ohlen, "From");
		char *dt = mime_header(ohdr, ohlen, "Date");
		buf_printf(&t, "On %s, %s wrote:\n", dt ? dt : "an earlier date", fr ? fr : "someone");
		free(fr);
		free(dt);
		/* A flowed original was unwrapped for display; quote it as flowed
		 * text again (wrapped at 72 with soft breaks). Other text is quoted
		 * line by line, without trailing spaces (they would mean "flowed"). */
		const char *p = obody;
		while (p && *p) {
			const char *e = strchr(p, '\n');
			size_t n = e ? (size_t)(e - p) : strlen(p);
			if (A.mv.flowed) {
				quote_flowed(&t, p, n);
			} else {
				size_t m = n;
				while (m && (p[m - 1] == ' ' || p[m - 1] == '\t' || p[m - 1] == '\r')) m--;
				buf_adds(&t, m && p[0] == '>' ? ">" : m ? "> " : ">");
				buf_add(&t, p, m);
				buf_addc(&t, '\n');
			}
			p += n + (e ? 1 : 0);
		}
		buf_adds(&t, "\n\n");                         /* blank line, then the reply */
		start = -1;                                  /* last line */
	} else if (kind == 'f') {
		buf_adds(&t, "\n\n---------- Forwarded message ----------\n");
		static const char *const fh[] = {"From", "Date", "Subject", "To", "Cc", NULL};
		for (int i = 0; fh[i]; i++) {
			char *v = mime_header(ohdr, ohlen, fh[i]);
			if (v) buf_printf(&t, "%s: %s\n", fh[i], v);
			free(v);
		}
		buf_printf(&t, "\n%s\n", obody ? obody : "");
	}
	free(osub);
	free(obody);

	/* The original may hold text that is not valid UTF-8 (a wrong or missing
	 * charset, Windows-1252 bytes, control characters): clean it here, or it
	 * could not be sent. */
	{
		size_t cl;
		char *ct = utf8_clean(t.d ? t.d : "", t.len, &cl);
		buf_free(&t);
		buf_add(&t, ct, cl);
		free(ct);
	}
	int nlines = 0;
	for (size_t i = 0; i < t.len; i++) nlines += t.d[i] == '\n';
	if (start < 0) start = nlines > 0 ? nlines - 1 : 0;
	if (!*from) start = 0;          /* no address for this account: From: first */
	if (t.len && t.d[t.len - 1] == '\n') t.len--;

	editor_cfg ec = {
		.title = kind == 'r' ? "Reply" : kind == 'f' ? "Forward" : "New message",
		.send = send_cb,
		.keep = keep_cb,
		.ctx = &c,
		.wrap = kind == 'm',                         /* replies/forwards: W turns it on */
		.wrap_width = wrap_width,
	};
	editor_run(&ec, buf_cstr(&t), start);
	buf_free(&t);
	focus_to(COL_LIST);            /* back to the email list and the message */
}
