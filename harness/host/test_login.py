"""Loopback PKCE and optional real JWT verification without network access."""

import base64
import hashlib
import json
from pathlib import Path
import tempfile
import socket
import threading
import time
import unittest
import urllib.error
import urllib.parse
import urllib.request
from types import SimpleNamespace
from unittest.mock import patch

from login import login
from oauth import AuthError, ISSUER, OAuthClient
from session import SessionManager
from test_session import FakeOAuth, MemoryVault


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, *args):
        return None


class LoginTests(unittest.TestCase):
    def test_pkce_state_and_hint_stays_out_of_public_url(self):
        with (
            tempfile.TemporaryDirectory() as directory,
            patch(
                "socket.getfqdn",
                side_effect=AssertionError("Loopback login must not resolve DNS"),
            ),
        ):
            api = FakeOAuth()
            manager = SessionManager(
                MemoryVault(), Path(directory) / "account.json", api
            )
            manager.remember_client("issued-client")
            manager.accept_login("issued-client", api.tokens(), "nonce")
            url_ready, errors, addresses = threading.Event(), [], []

            def on_url(value):
                addresses.append(value)
                url_ready.set()

            def run():
                try:
                    login(manager, on_url, open_browser=False, timeout=5)
                except Exception as error:
                    errors.append(error)

            thread = threading.Thread(target=run)
            thread.start()
            self.assertTrue(url_ready.wait(5))
            local = addresses[0]
            self.assertNotIn("id-one", local)
            # Browsers open idle speculative connections before sending GET.
            # A single-threaded HTTPServer blocks every later callback here.
            address = urllib.parse.urlparse(local)
            idle = socket.create_connection((address.hostname, address.port), timeout=5)
            self.addCleanup(idle.close)
            opener = urllib.request.build_opener(NoRedirect)
            with self.assertRaises(urllib.error.HTTPError) as redirect:
                opener.open(local, timeout=5)
            self.assertEqual(redirect.exception.code, 303)
            target = urllib.parse.urlparse(redirect.exception.headers["Location"])
            self.assertEqual(target.hostname, "auth.openai.com")
            query = urllib.parse.parse_qs(target.query)
            self.assertEqual(query["id_token_hint"], ["id-one"])
            self.assertEqual(query["code_challenge_method"], ["S256"])
            callback = query["redirect_uri"][0]
            with self.assertRaises(urllib.error.HTTPError) as wrong:
                urllib.request.urlopen(callback + "?state=wrong&code=secret", timeout=5)
            self.assertEqual(wrong.exception.code, 400)

            def exchange(client, code, verifier, uri):
                self.assertEqual(client, "issued-client")
                self.assertEqual(code, "synthetic-code")
                self.assertEqual(uri, callback)
                challenge = (
                    base64.urlsafe_b64encode(hashlib.sha256(verifier.encode()).digest())
                    .rstrip(b"=")
                    .decode()
                )
                self.assertEqual(query["code_challenge"], [challenge])
                return api.tokens("new")

            def identity(token, client, **kwargs):
                self.assertEqual(kwargs["nonce"], query["nonce"][0])
                return {"sub": "user"}

            api.exchange, api.identity = exchange, identity
            with urllib.request.urlopen(
                callback
                + "?"
                + urllib.parse.urlencode(
                    {"state": query["state"][0], "code": "synthetic-code"}
                ),
                timeout=5,
            ) as response:
                self.assertIn(b"saved in macOS Keychain", response.read())
            thread.join(5)
            self.assertFalse(thread.is_alive())
            self.assertFalse(errors)
            self.assertEqual(manager.access_token(), "access-new")

    def test_signed_identity_checks(self):
        try:
            import jwt
            from cryptography.hazmat.primitives.asymmetric import rsa
        except ImportError:
            self.skipTest("Optional host JWT dependencies are not installed")
        key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
        api = OAuthClient()
        api.discovery = {"issuer": ISSUER}
        api.jwks = SimpleNamespace(
            get_signing_key_from_jwt=lambda token: SimpleNamespace(key=key.public_key())
        )
        claims = {
            "iss": ISSUER,
            "aud": "issued",
            "sub": "user",
            "exp": int(time.time()) + 60,
            "nonce": "nonce",
        }

        def token(changes):
            return jwt.encode(dict(claims, **changes), key, algorithm="RS256")

        valid = token({})
        self.assertEqual(api.identity(valid, "issued", nonce="nonce")["sub"], "user")
        for changes in (
            {"iss": "wrong"},
            {"aud": "wrong"},
            {"exp": 1},
            {"nonce": "wrong"},
        ):
            with self.subTest(changes=changes), self.assertRaises(AuthError):
                api.identity(token(changes), "issued", nonce="nonce")
        with self.assertRaises(AuthError):
            api.identity(
                valid, "issued", subject_hash=hashlib.sha256(b"other").hexdigest()
            )

    def test_oauth_error_redaction(self):
        import io

        error = urllib.error.HTTPError(
            ISSUER,
            400,
            "bad",
            {},
            io.BytesIO(
                json.dumps(
                    {"error": "invalid_grant", "error_description": "secret-token"}
                ).encode()
            ),
        )
        with patch("urllib.request.urlopen", side_effect=error):
            with self.assertRaises(AuthError) as caught:
                OAuthClient().refresh("client", "refresh-secret")
        error.close()
        self.assertEqual(caught.exception.code, "invalid_grant")
        self.assertNotIn("secret", str(caught.exception))


if __name__ == "__main__":
    unittest.main()
