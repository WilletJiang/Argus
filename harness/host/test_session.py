"""Offline persistence, token rotation and concurrent-refresh contract tests."""

import copy
import hashlib
import json
import multiprocessing
from pathlib import Path
import tempfile
import time
import unittest

from oauth import AuthError
from session import SessionManager, earliest


class MemoryVault:
    def __init__(self):
        self.records = {}

    def load(self, key):
        return copy.deepcopy(self.records.get(key))

    def save(self, key, value):
        self.records[key] = copy.deepcopy(value)

    def delete(self, key):
        self.records.pop(key, None)


class FakeOAuth:
    def __init__(self):
        self.refreshes = []
        self.error = None
        self.revoked = False

    def identity(self, token, client_id, *, nonce=None, subject_hash=None):
        if nonce not in (None, "nonce") or (
            subject_hash and subject_hash != hashlib.sha256(b"user").hexdigest()
        ):
            raise AuthError("identity", "Account or nonce mismatch")
        return {"sub": "user"}

    def tokens(self, suffix="one"):
        return dict(
            access_token="access-" + suffix,
            refresh_token="refresh-" + suffix,
            id_token="id-" + suffix,
            expires_in=3600,
            scope="openid offline_access chatgpt.tokens.use.direct",
        )

    def refresh(self, client, token):
        self.refreshes.append((client, token))
        if self.error:
            raise self.error
        return self.tokens("two")

    def revoke(self, client, token):
        self.revoked = True
        if self.error:
            raise self.error


# Synthetic tokens only. This backend exists solely to test real interprocess locking.
class FileVault:
    def __init__(self, path):
        self.path = Path(path)

    def load(self, key):
        return json.loads(self.path.read_text()) if self.path.exists() else None

    def save(self, key, value):
        self.path.write_text(json.dumps(value))


class CountingOAuth(FakeOAuth):
    def __init__(self, counter):
        super().__init__()
        self.counter = Path(counter)

    def refresh(self, client, token):
        with self.counter.open("a") as output:
            output.write("refresh\n")
        time.sleep(0.15)
        return super().refresh(client, token)


def refresh_worker(profile, vault, counter, ready, start, results):
    manager = SessionManager(
        FileVault(vault), profile, CountingOAuth(counter), clock=lambda: 5000
    )
    ready.put(True)
    start.wait(5)
    results.put(manager.access_token())


class SessionTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.path = Path(self.directory.name) / "registration.json"
        self.vault = MemoryVault()
        self.api = FakeOAuth()
        self.now = 1000
        self.manager = SessionManager(
            self.vault, self.path, self.api, clock=lambda: self.now
        )
        self.manager.remember_client("issued-client")
        self.manager.accept_login("issued-client", self.api.tokens(), "nonce")

    def test_reopen_reuses_and_public_profile_has_no_credentials(self):
        other = SessionManager(self.vault, self.path, self.api, clock=lambda: self.now)
        self.assertEqual(other.access_token(), "access-one")
        self.assertFalse(self.api.refreshes)
        profile = json.loads(self.path.read_text())
        self.assertEqual(set(profile), {"host_id", "client_id", "subject_hash"})
        self.assertNotIn("access-one", json.dumps(other.status()))
        self.assertEqual(other.login_hint(), "id-one")

    def test_refresh_rotates_whole_record_and_reuses_it(self):
        self.now = 4600
        self.assertEqual(self.manager.access_token(), "access-two")
        self.assertEqual(
            self.vault.load("issued-client")["refresh_token"], "refresh-two"
        )
        self.assertEqual(self.manager.access_token(), "access-two")
        self.assertEqual(self.api.refreshes, [("issued-client", "refresh-one")])

    def test_unusable_grant_clears_only_tokens(self):
        self.now = 5000
        self.api.error = AuthError("refresh_token_reused", "Invalid refresh token")
        with self.assertRaises(AuthError):
            self.manager.access_token()
        self.assertIsNone(self.vault.load("issued-client"))
        self.assertEqual(self.manager.profile()["client_id"], "issued-client")
        self.assertFalse(self.manager.status()["connected"])

    def test_transient_error_preserves_credentials(self):
        self.now = 5000
        self.api.error = AuthError("network", "Unavailable", retryable=True)
        before = self.vault.load("issued-client")
        with self.assertRaises(AuthError):
            self.manager.access_token()
        self.assertEqual(before, self.vault.load("issued-client"))

    def test_refresh_window(self):
        self.now = 4500
        self.api.error = AuthError("too_early", "Wait", earliest_refresh_at=4550)
        self.assertEqual(self.manager.access_token(), "access-one")
        self.assertEqual(self.manager.access_token(), "access-one")
        self.assertEqual(len(self.api.refreshes), 1)
        self.api.error = None
        self.now = 4551
        self.assertEqual(self.manager.access_token(), "access-two")
        self.assertEqual(earliest("1970-01-01T01:00:00Z"), 3600)
        self.assertEqual(earliest("invalid"), 0)

    def test_invalid_replacement_does_not_overwrite(self):
        before = self.vault.load("issued-client")
        for field, value in [
            ("refresh_token", None),
            ("expires_in", True),
            ("scope", "openid"),
        ]:
            tokens = self.api.tokens("bad")
            tokens[field] = value
            with self.subTest(field=field), self.assertRaises(AuthError):
                self.manager.accept_login("issued-client", tokens, "nonce")
            self.assertEqual(before, self.vault.load("issued-client"))
        with self.assertRaises(AuthError):
            self.manager.accept_login("issued-client", self.api.tokens(), "wrong-nonce")

    def test_corrupt_record_rejected(self):
        for field, value in [
            ("version", True),
            ("expires_at", float("nan")),
            ("access_token", "bad token"),
            ("host_id", "other"),
            ("subject", "other"),
        ]:
            original = self.vault.load("issued-client")
            changed = dict(original, **{field: value})
            self.vault.save("issued-client", changed)
            with self.subTest(field=field), self.assertRaises(AuthError):
                self.manager.access_token()
            self.vault.save("issued-client", original)

    def test_logout_clears_local_even_when_revocation_unavailable(self):
        self.api.error = AuthError("network", "Offline")
        self.assertEqual(
            self.manager.logout(), {"local_cleared": True, "remote_revoked": False}
        )
        self.assertTrue(self.api.revoked)
        self.assertEqual(
            self.manager.logout(), {"local_cleared": True, "remote_revoked": None}
        )

    def test_processes_refresh_once(self):
        root = Path(self.directory.name)
        vault, counter = root / "fake-vault.json", root / "count.txt"
        FileVault(vault).save("issued-client", self.vault.load("issued-client"))
        ctx = multiprocessing.get_context("spawn")
        ready, results, start = ctx.Queue(), ctx.Queue(), ctx.Event()
        processes = [
            ctx.Process(
                target=refresh_worker,
                args=(self.path, vault, counter, ready, start, results),
            )
            for _ in range(3)
        ]
        try:
            for process in processes:
                process.start()
            for _ in processes:
                self.assertTrue(ready.get(timeout=10))
            start.set()
            self.assertEqual(
                [results.get(timeout=10) for _ in processes], ["access-two"] * 3
            )
            for process in processes:
                process.join(10)
                self.assertEqual(process.exitcode, 0)
            self.assertEqual(counter.read_text(), "refresh\n")
        finally:
            for process in processes:
                if process.is_alive():
                    process.terminate()
                process.join()
            ready.close()
            results.close()


if __name__ == "__main__":
    unittest.main()
