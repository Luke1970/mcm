/* certs.c - certificates that are not trusted: asked about, or always
 * accepted (~/.config/mcm/certs)
 */
#include "mcm.h"

static int cert_lookup(const net_certinfo *ci);
static int cert_store(const net_certinfo *ci);

/* ------------------------------------------------------------------ */
/* Untrusted certificates                                              */
/* ~/.config/mcm/certs holds "host port sha256" for certificates the   */
/* user chose to always accept.                                        */

/* 1 = this exact certificate is stored, -1 = a different one is stored
 * for this server (it changed), 0 = nothing stored. */
static int cert_lookup(const net_certinfo *ci)
{
	char p[1100], line[512], h[256], po[16], fp[128];
	config_path(p, sizeof p, "certs");
	FILE *f = fopen(p, "r");
	if (!f) return 0;
	int r = 0;
	while (fgets(line, sizeof line, f))
		if (sscanf(line, "%255s %15s %127s", h, po, fp) == 3 &&
		    !strcasecmp(h, ci->host) && !strcmp(po, ci->port)) {
			if (!strcasecmp(fp, ci->sha256)) { r = 1; break; }
			r = -1;
		}
	fclose(f);
	return r;
}

/* Store the certificate, replacing any earlier one for the same server. */
static int cert_store(const net_certinfo *ci)
{
	char p[1100], tmp[1110], line[512], h[256], po[16];
	config_mkdir();
	config_path(p, sizeof p, "certs");
	snprintf(tmp, sizeof tmp, "%s.new", p);
	FILE *out = fopen(tmp, "w");
	if (!out) return -1;
	chmod(tmp, 0600);
	FILE *in = fopen(p, "r");
	if (in) {
		while (fgets(line, sizeof line, in))
			if (!(sscanf(line, "%255s %15s", h, po) == 2 && !strcasecmp(h, ci->host) &&
			      !strcmp(po, ci->port)))
				fputs(line, out);
		fclose(in);
	} else {
		fputs("# certificates accepted in mcm: host port sha256-fingerprint\n", out);
	}
	fprintf(out, "%s %s %s\n", ci->host, ci->port, ci->sha256);
	int bad = ferror(out);
	if (fclose(out) != 0 || bad) { unlink(tmp); return -1; }
	return rename(tmp, p);
}

int cert_check(const net_certinfo *ci, void *ctx)
{
	ui_bg_fn bg = ctx ? *(ui_bg_fn *)ctx : login_bg;
	int known = cert_lookup(ci);
	if (known == 1) return 1;

	char l[12][160];
	int n = 0;
	if (known < 0) {
		snprintf(l[n++], sizeof l[0], "THE CERTIFICATE OF %s:%s HAS CHANGED", ci->host, ci->port);
		snprintf(l[n++], sizeof l[0], "since you accepted it. Someone may be intercepting the connection.");
	} else {
		snprintf(l[n++], sizeof l[0], "The certificate of %s:%s is not trusted:", ci->host, ci->port);
	}
	snprintf(l[n++], sizeof l[0], "  %.150s", ci->problem);
	l[n++][0] = '\0';
	snprintf(l[n++], sizeof l[0], "Subject   %.140s", ci->subject);
	snprintf(l[n++], sizeof l[0], "Issuer    %.140s", ci->issuer);
	if (*ci->names) snprintf(l[n++], sizeof l[0], "Names     %.140s", ci->names);
	snprintf(l[n++], sizeof l[0], "Valid     %s to %s", ci->not_before, ci->not_after);
	snprintf(l[n++], sizeof l[0], "SHA-256   %.47s", ci->sha256);
	snprintf(l[n++], sizeof l[0], "          %s", strlen(ci->sha256) > 48 ? ci->sha256 + 48 : "");
	l[n++][0] = '\0';
	snprintf(l[n++], sizeof l[0], "Accept only if you know this is the server's real certificate.");
	const char *lines[12];
	for (int i = 0; i < n; i++) lines[i] = l[i];

	static const ui_menuitem btn[] = {
		{"Reject", 0, 'r'}, {"Accept once", 7, 'o'}, {"Always accept", 0, 'a'},
	};
	int c = ui_choice_dialog(known < 0 ? "WARNING: certificate changed" : "Certificate warning",
	                         lines, n, btn, 3, 0, 1, bg);
	if (c == 2 && cert_store(ci) != 0)
		ui_message("Certificate", "Could not save the certificate; accepted for this session only.",
		           bg);
	return c == 1 || c == 2;
}
