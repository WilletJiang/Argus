"""Non-secret account metadata and cross-process refresh lock."""

from contextlib import contextmanager
import fcntl
import json
import os
from pathlib import Path
import stat
import tempfile
import uuid
from oauth import AuthError


class AccountProfile:
    def __init__(self, path):
        self.path = Path(path)

    @contextmanager
    def locked(self):
        self.path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
        fd = os.open(
            str(self.path) + ".lock",
            os.O_CREAT | os.O_RDWR | os.O_CLOEXEC | os.O_NOFOLLOW,
            0o600,
        )
        try:
            info = os.fstat(fd)
            if (
                not stat.S_ISREG(info.st_mode)
                or info.st_uid != os.getuid()
                or info.st_mode & 0o077
            ):
                raise AuthError(
                    "profile", "Profile lock must be an owner-only regular file"
                )
            fcntl.flock(fd, fcntl.LOCK_EX)
            yield
        finally:
            os.close(fd)

    def save(self, value):
        fd, name = tempfile.mkstemp(prefix=".auth-", dir=self.path.parent)
        try:
            with os.fdopen(fd, "w") as output:
                json.dump(value, output)
                output.flush()
                os.fsync(output.fileno())
            os.replace(name, self.path)
        finally:
            if os.path.exists(name):
                os.unlink(name)

    def load(self):
        if not self.path.exists():
            value = {"host_id": "urn:uuid:" + str(uuid.uuid4())}
            self.save(value)
            return value
        try:
            value = json.loads(self.path.read_text())
            if not isinstance(value, dict) or not isinstance(value.get("host_id"), str):
                raise ValueError
            if any(
                key in value for key in ("access_token", "refresh_token", "id_token")
            ):
                raise ValueError
            for key in ("client_id", "subject_hash"):
                if key in value and (not isinstance(value[key], str) or not value[key]):
                    raise ValueError
            return value
        except (ValueError, OSError) as error:
            raise AuthError(
                "profile",
                "Invalid public account profile; credentials belong in Keychain",
            ) from error

    def profile(self):
        with self.locked():
            return self.load()

    def remember_client(self, client_id):
        if (
            not isinstance(client_id, str)
            or not client_id
            or client_id == "dynamic_agent_client"
        ):
            raise AuthError("identity", "An issued OAuth client ID is required")
        with self.locked():
            profile = self.load()
            if profile.get("client_id") and profile["client_id"] != client_id:
                raise AuthError(
                    "identity", "OAuth client differs from the selected registration"
                )
            profile["client_id"] = client_id
            self.save(profile)
