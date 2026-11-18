#!/usr/bin/env python3
"""Test Microsoft 365 IMAP/SMTP login with OAuth2 (device code flow).

Usage: python3 -I m365-oauth-test.py [--browser] CLIENT_ID [you@company.com]

--browser: sign in with the browser on this computer (authorization code +
PKCE, reply on http://localhost) instead of a device code.

Without an address the sign-in name from the token is used.

Read-only: lists folders, counts INBOX, checks SMTP AUTH. Sends nothing,
stores nothing; the token is never printed.
"""
import base64, hashlib, http.server, imaplib, secrets, webbrowser, json, smtplib, ssl, sys, time, urllib.error, urllib.parse, urllib.request

AUTH = "https://login.microsoftonline.com/organizations/oauth2/v2.0"
SCOPE = ("https://outlook.office.com/IMAP.AccessAsUser.All "
         "https://outlook.office.com/SMTP.Send offline_access")


def post(url, data):
    req = urllib.request.Request(url, urllib.parse.urlencode(data).encode())
    try:
        with urllib.request.urlopen(req, timeout=30) as r:
            return json.load(r)
    except urllib.error.HTTPError as e:
        return json.load(e)


def browser_login(client_id):
    verifier = secrets.token_urlsafe(64)
    challenge = base64.urlsafe_b64encode(hashlib.sha256(verifier.encode()).digest()).rstrip(b"=").decode()
    state = secrets.token_urlsafe(16)
    got = {}

    class H(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            q = urllib.parse.parse_qs(urllib.parse.urlparse(self.path).query)
            if "state" not in q and "error" not in q:
                self.send_response(404); self.end_headers(); return
            got.update({k: v[0] for k, v in q.items()})
            self.send_response(200)
            self.send_header("Content-Type", "text/plain; charset=utf-8")
            self.end_headers()
            self.wfile.write(b"Done, you can close this tab and go back to the terminal.")

        def log_message(self, *a):
            pass

    srv = http.server.HTTPServer(("127.0.0.1", 0), H)
    redirect = "http://localhost:%d" % srv.server_port
    url = AUTH + "/authorize?" + urllib.parse.urlencode({
        "client_id": client_id, "response_type": "code", "redirect_uri": redirect,
        "response_mode": "query", "scope": SCOPE, "state": state,
        "code_challenge": challenge, "code_challenge_method": "S256"})
    print("\nOpening the browser. If it does not open, visit:\n\n" + url + "\n")
    webbrowser.open(url)
    while not got:
        srv.handle_request()
    srv.server_close()
    if "error" in got:
        sys.exit("login failed: %s\n%s" % (got["error"], got.get("error_description")))
    if got.get("state") != state:
        sys.exit("login failed: state does not match")
    return post(AUTH + "/token", {
        "grant_type": "authorization_code", "client_id": client_id, "code": got["code"],
        "redirect_uri": redirect, "code_verifier": verifier, "scope": SCOPE})


def device_login(client_id):
    dc = post(AUTH + "/devicecode", {"client_id": client_id, "scope": SCOPE})
    if "device_code" not in dc:
        sys.exit("devicecode failed: %s\n%s" % (dc.get("error"), dc.get("error_description")))
    print("\n" + dc["message"] + "\n")

    interval = int(dc.get("interval", 5))
    while True:
        time.sleep(interval)
        tok = post(AUTH + "/token", {
            "grant_type": "urn:ietf:params:oauth:grant-type:device_code",
            "client_id": client_id, "device_code": dc["device_code"]})
        err = tok.get("error")
        if err == "authorization_pending":
            continue
        if err == "slow_down":
            interval += 5
            continue
        if err:
            sys.exit("login failed: %s\n%s" % (err, tok.get("error_description")))
        return tok


def main():
    args = sys.argv[1:]
    browser = "--browser" in args
    args = [a for a in args if a != "--browser"]
    if len(args) not in (1, 2):
        sys.exit(__doc__)
    client_id = args[0]
    user = args[1] if len(args) == 2 else None

    tok = browser_login(client_id) if browser else device_login(client_id)
    if "access_token" not in tok:
        sys.exit("login failed: %s\n%s" % (tok.get("error"), tok.get("error_description")))
    print("Token received, scopes:", tok.get("scope"))
    print("Refresh token:", "yes" if tok.get("refresh_token") else "no")

    # the sign-in name (UPN) from the token's claims; the token is not printed
    try:
        p = tok["access_token"].split(".")[1]
        claims = json.loads(base64.urlsafe_b64decode(p + "=" * (-len(p) % 4)))
        upn = claims.get("upn") or claims.get("unique_name") or claims.get("preferred_username")
    except (IndexError, ValueError):
        upn = None
    print("Signed in as:", upn or "(unknown)")
    if not user:
        if not upn:
            sys.exit("no address given and none found in the token")
        user = upn
    print("IMAP/SMTP user:", user)

    xoauth = "user=%s\x01auth=Bearer %s\x01\x01" % (user, tok["access_token"])
    ctx = ssl.create_default_context()

    print("\nIMAP outlook.office365.com:993 ...")
    try:
        im = imaplib.IMAP4_SSL("outlook.office365.com", 993, ssl_context=ctx)
        im.authenticate("XOAUTH2", lambda _: xoauth.encode())
        typ, folders = im.list()
        print("IMAP login OK, %d folders" % len(folders))
        typ, n = im.select("INBOX", readonly=True)
        print("INBOX: %s messages" % n[0].decode())
        im.logout()
    except imaplib.IMAP4.error as e:
        print("IMAP failed:", e)

    print("\nSMTP smtp.office365.com:587 ...")
    try:
        s = smtplib.SMTP("smtp.office365.com", 587, timeout=30)
        s.starttls(context=ctx)
        code, msg = s.docmd("AUTH", "XOAUTH2 " + base64.b64encode(xoauth.encode()).decode())
        print("SMTP AUTH: %d %s" % (code, msg.decode(errors="replace")))
        s.quit()
    except (smtplib.SMTPException, OSError) as e:
        print("SMTP failed:", e)


if __name__ == "__main__":
    main()
