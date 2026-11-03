"""Loopback PKCE login. Authorization URLs containing ID hints never enter logs."""

import base64
import hashlib
import http.server
import secrets
import time
import urllib.parse
import webbrowser
from oauth import AuthError, ISSUER, RESOURCE, SCOPE


def login(manager, on_url, *, open_browser=True, timeout=600):
    profile = manager.profile()
    state = secrets.token_urlsafe(32)
    nonce = secrets.token_urlsafe(32)
    verifier = secrets.token_urlsafe(48)
    entry = "/login/" + secrets.token_urlsafe(24)
    outcome = {}

    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass

        def redirect(self, url):
            self.send_response(303)
            self.send_header("Location", url)
            self.send_header("Cache-Control", "no-store")
            self.send_header("Referrer-Policy", "no-referrer")
            self.end_headers()

        def do_GET(self):
            parsed = urllib.parse.urlparse(self.path)
            if parsed.path == entry:
                self.redirect(authorize)
                return
            if parsed.path == "/complete" and outcome.get("success"):
                self.send_response(200)
                self.send_header("Content-Type", "text/plain; charset=utf-8")
                self.send_header("Cache-Control", "no-store")
                self.end_headers()
                self.wfile.write(
                    b"Connected. Your ChatGPT session is saved in macOS Keychain. You may close this tab."
                )
                return
            if parsed.path != "/auth/callback" or outcome:
                self.send_error(404)
                return
            query = urllib.parse.parse_qs(parsed.query)
            if len(query.get("state", [])) != 1 or not secrets.compare_digest(
                query["state"][0], state
            ):
                self.send_error(400, "Invalid login state")
                return
            if query.get("error"):
                outcome["error"] = AuthError(
                    "access_denied", "OpenAI sign-in was not completed"
                )
                self.send_error(403, "Sign-in was not completed")
                return
            try:
                if (
                    len(query.get("code", [])) != 1
                    or len(query.get("client_id", [None])) != 1
                ):
                    raise AuthError("protocol", "Invalid login callback")
                client = query.get("client_id", [profile.get("client_id")])[0]
                manager.remember_client(client)
                value = manager.api.exchange(
                    client, query["code"][0], verifier, redirect_uri
                )
                manager.accept_login(client, value, nonce)
                outcome["success"] = True
                self.redirect("/complete")
            except Exception as error:
                outcome["error"] = error
                self.send_error(
                    400, "Could not save a validated login session; return to the app"
                )

    server = http.server.HTTPServer(("127.0.0.1", 0), Handler)
    server.timeout = 0.5
    redirect_uri = f"http://127.0.0.1:{server.server_port}/auth/callback"
    parameters = {
        "client_id": profile.get("client_id", "dynamic_agent_client"),
        "ext_agent_host_id": profile["host_id"],
        "response_type": "code",
        "redirect_uri": redirect_uri,
        "scope": SCOPE,
        "resource": RESOURCE,
        "state": state,
        "nonce": nonce,
        "code_challenge_method": "S256",
        "code_challenge": base64.urlsafe_b64encode(
            hashlib.sha256(verifier.encode()).digest()
        )
        .rstrip(b"=")
        .decode(),
    }
    if not profile.get("client_id"):
        parameters["agent_name_hint"] = "Argus Harness"
    else:
        try:
            hint = manager.login_hint()
            if hint:
                parameters["id_token_hint"] = hint
        except AuthError:
            pass
    authorize = ISSUER + "/api/accounts/authorize?" + urllib.parse.urlencode(parameters)
    local_url = f"http://127.0.0.1:{server.server_port}" + entry
    on_url(local_url)
    if open_browser:
        webbrowser.open(local_url)
    started = time.monotonic()
    try:
        while not outcome and time.monotonic() - started < timeout:
            server.handle_request()
        if not outcome:
            raise AuthError(
                "timeout", "Sign-in timed out; the saved registration remains available"
            )
        if outcome.get("error"):
            raise outcome["error"]
        # Allow the browser's final non-secret redirect to render.
        server.handle_request()
    finally:
        server.server_close()
