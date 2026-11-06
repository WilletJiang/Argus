"""Persistent account state and serialized refresh-token rotation."""

from datetime import datetime
import hashlib
import math
from pathlib import Path
import time
from .oauth import AuthError, ISSUER, OAuthClient
from .account import AccountProfile

DEFAULT_PROFILE = Path.home() / ".config/argus-harness-evaluation/registration.json"
UNUSABLE = {
    "invalid_grant",
    "invalid_refresh_token",
    "token_expired",
    "refresh_token_expired",
    "refresh_token_invalidated",
    "refresh_token_reused",
}


def earliest(value):
    if (
        isinstance(value, (int, float))
        and not isinstance(value, bool)
        and math.isfinite(value)
    ):
        return float(value)
    if isinstance(value, str):
        try:
            parsed = datetime.fromisoformat(value.replace("Z", "+00:00"))
            return parsed.timestamp() if parsed.tzinfo else 0
        except ValueError:
            pass
    return 0


class SessionManager:
    def __init__(self, vault, profile=DEFAULT_PROFILE, api=None, clock=time.time):
        self.vault = vault
        self.account = AccountProfile(profile)
        self.api = api or OAuthClient()
        self.clock = clock

    def profile(self):
        return self.account.profile()

    def remember_client(self, client_id):
        self.account.remember_client(client_id)

    def login_hint(self):
        with self.account.locked():
            try:
                return self._load(self.account.load()).get("id_token")
            except AuthError:
                return None

    def _record(self, profile, value, previous=None, nonce=None):
        previous = previous or {}
        for field in ("access_token", "refresh_token"):
            token = value.get(field)
            if (
                not isinstance(token, str)
                or not token
                or any(c.isspace() or ord(c) < 32 for c in token)
            ):
                raise AuthError(
                    "protocol", "OAuth did not return a complete replacement token set"
                )
        try:
            lifetime = float(value["expires_in"])
        except (KeyError, TypeError, ValueError, OverflowError) as error:
            raise AuthError(
                "protocol", "OAuth did not return a valid token lifetime"
            ) from error
        if (
            isinstance(value["expires_in"], bool)
            or not math.isfinite(lifetime)
            or lifetime <= 0
        ):
            raise AuthError("protocol", "Invalid access-token lifetime")
        scopes = value.get("scope")
        scopes = (
            set(scopes.split())
            if isinstance(scopes, str)
            else set(previous.get("scopes", []))
        )
        if "chatgpt.tokens.use.direct" not in scopes:
            raise AuthError("permission", "ChatGPT plan usage was not granted")
        id_token = value.get("id_token")
        if id_token:
            claims = self.api.identity(
                id_token,
                profile["client_id"],
                nonce=nonce,
                subject_hash=profile.get("subject_hash"),
            )
            subject = claims["sub"]
        elif previous and nonce is None:
            subject = previous["subject"]
            id_token = previous["id_token"]
        else:
            raise AuthError("identity", "OpenAI did not return an identity token")
        return {
            "version": 1,
            "issuer": ISSUER,
            "client_id": profile["client_id"],
            "host_id": profile["host_id"],
            "subject": subject,
            "access_token": value["access_token"],
            "refresh_token": value["refresh_token"],
            "id_token": id_token,
            "scopes": sorted(scopes),
            "saved_at": self.clock(),
            "expires_at": self.clock() + lifetime,
            "earliest_refresh_at": value.get("earliest_refresh_at"),
        }

    def accept_login(self, client_id, value, nonce):
        with self.account.locked():
            profile = self.account.load()
            if profile.get("client_id") != client_id:
                raise AuthError("identity", "OAuth client mismatch")
            record = self._record(profile, value, nonce=nonce)
            self.vault.save(client_id, record)
            profile["subject_hash"] = hashlib.sha256(
                record["subject"].encode()
            ).hexdigest()
            self.account.save(profile)

    def _load(self, profile):
        client = profile.get("client_id")
        record = self.vault.load(client) if client else None
        if record is None:
            raise AuthError(
                "login_required",
                "Run python -m host.auth login once to connect ChatGPT",
            )
        if (
            not isinstance(record, dict)
            or record.get("client_id") != client
            or record.get("issuer") != ISSUER
            or record.get("host_id") != profile["host_id"]
        ):
            raise AuthError(
                "identity", "Stored session does not match this host and client"
            )
        subject = record.get("subject")
        if not isinstance(subject, str) or hashlib.sha256(
            subject.encode()
        ).hexdigest() != profile.get("subject_hash"):
            raise AuthError(
                "identity", "Stored session does not match the selected account"
            )
        if type(record.get("version")) is not int or record["version"] != 1:
            raise AuthError("storage", "Unsupported stored credential format")
        for field in ("access_token", "refresh_token", "id_token"):
            value = record.get(field)
            if (
                not isinstance(value, str)
                or not value
                or any(c.isspace() or ord(c) < 32 for c in value)
            ):
                raise AuthError("storage", "Stored credential set is incomplete")
        expires = record.get("expires_at")
        if (
            isinstance(expires, bool)
            or not isinstance(expires, (int, float))
            or not math.isfinite(expires)
        ):
            raise AuthError("storage", "Stored credential expiry is invalid")
        if not isinstance(record.get("scopes"), list):
            raise AuthError("storage", "Stored grant scopes are invalid")
        if "chatgpt.tokens.use.direct" not in record.get("scopes", []):
            raise AuthError(
                "permission", "Stored session lacks ChatGPT plan permission"
            )
        return record

    def access_token(self, min_validity=120):
        with self.account.locked():
            profile = self.account.load()
            record = self._load(profile)
            now = self.clock()
            if record["expires_at"] > now + min_validity:
                return record["access_token"]
            if earliest(record.get("earliest_refresh_at")) > now:
                if record["expires_at"] > now:
                    return record["access_token"]
                raise AuthError(
                    "refresh_deferred",
                    "Provider refresh window has not opened",
                    retryable=True,
                )
            try:
                value = self.api.refresh(profile["client_id"], record["refresh_token"])
            except AuthError as error:
                if error.code in UNUSABLE:
                    self.vault.delete(profile["client_id"])
                elif error.earliest_refresh_at is not None:
                    record["earliest_refresh_at"] = error.earliest_refresh_at
                    self.vault.save(profile["client_id"], record)
                    if record["expires_at"] > now:
                        return record["access_token"]
                raise
            replacement = self._record(profile, value, previous=record)
            self.vault.save(profile["client_id"], replacement)
            return replacement["access_token"]

    def status(self):
        with self.account.locked():
            profile = self.account.load()
            try:
                record = self._load(profile)
            except AuthError as error:
                if error.code == "login_required":
                    return {
                        "connected": False,
                        "storage": "macOS Keychain",
                        "client_registered": bool(profile.get("client_id")),
                    }
                raise
            return {
                "connected": True,
                "storage": "macOS Keychain",
                "expires_in_seconds": max(0, int(record["expires_at"] - self.clock())),
                "refresh_available": bool(record.get("refresh_token")),
            }

    def logout(self):
        with self.account.locked():
            profile = self.account.load()
            try:
                record = self._load(profile)
            except AuthError as error:
                if error.code == "login_required":
                    return {"local_cleared": True, "remote_revoked": None}
                raise
            remote = True
            try:
                self.api.revoke(profile["client_id"], record["refresh_token"])
            except AuthError:
                remote = False
            self.vault.delete(profile["client_id"])
            return {"local_cleared": True, "remote_revoked": remote}
