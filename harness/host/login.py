"""Loopback PKCE login. Authorization URLs containing ID hints never enter logs."""

import base64
import hashlib
import http.server
import secrets
import socketserver
import threading
import urllib.parse
import webbrowser
from oauth import AuthError, ISSUER, RESOURCE, SCOPE


class LoopbackServer(http.server.ThreadingHTTPServer):
    def server_bind(self):
        # The callback is always a numeric loopback address; reverse DNS is both
        # unnecessary and can block listener startup on managed macOS hosts.
        socketserver.TCPServer.server_bind(self)
        self.server_name = "127.0.0.1"
        self.server_port = self.server_address[1]


def login(manager, on_url, *, open_browser=True, timeout=600):
    profile = manager.profile()
    state = secrets.token_urlsafe(32)
    nonce = secrets.token_urlsafe(32)
    verifier = secrets.token_urlsafe(48)
    entry = "/login/" + secrets.token_urlsafe(24)
    outcome = {}
    finished = threading.Event()
    rendered = threading.Event()
    callback_lock = threading.Lock()

    class Handler(http.server.BaseHTTPRequestHandler):
        timeout = 5

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
                rendered.set()
                return
            if parsed.path != "/auth/callback" or finished.is_set():
                self.send_error(404)
                return
            query = urllib.parse.parse_qs(parsed.query)
            if len(query.get("state", [])) != 1 or not secrets.compare_digest(
                query["state"][0], state
            ):
                self.send_error(400, "Invalid login state")
                return
            if not callback_lock.acquire(blocking=False):
                self.send_error(409, "A login callback is already being processed")
                return
            try:
                if finished.is_set():
                    self.send_error(409, "Login is already complete")
                    return
                if query.get("error"):
                    raise AuthError("access_denied", "OpenAI sign-in was not completed")
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
            finally:
                finished.set()
                callback_lock.release()

    server = LoopbackServer(("127.0.0.1", 0), Handler)
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
    # Only ID-token hints require an opaque local redirect. Fresh login can use
    # the official URL directly, avoiding an unnecessary loopback round trip.
    browser_url = local_url if parameters.get("id_token_hint") else authorize
    serving = threading.Thread(target=server.serve_forever, daemon=True)
    serving.start()
    try:
        on_url(browser_url)
        if open_browser:
            webbrowser.open(browser_url)
        if not finished.wait(timeout):
            raise AuthError(
                "timeout", "Sign-in timed out; the saved registration remains available"
            )
        if outcome.get("error"):
            raise outcome["error"]
        rendered.wait(2)
    finally:
        server.shutdown()
        server.server_close()
        serving.join()
