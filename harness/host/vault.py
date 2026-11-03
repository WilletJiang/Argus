"""Native macOS Keychain storage. No plaintext fallback or token command line."""

import json
import sys

SERVICE = "Argus Harness / ChatGPT"


class VaultError(RuntimeError):
    pass


class KeychainVault:
    def __init__(self, service=SERVICE):
        if sys.platform != "darwin":
            raise VaultError("This host adapter requires macOS Keychain")
        try:
            from keyring.backends.macOS import Keyring

            self.backend = Keyring()
        except ImportError as error:
            raise VaultError("Install the optional host requirements first") from error
        self.service = service

    def load(self, client_id):
        try:
            value = self.backend.get_password(self.service, client_id)
            return json.loads(value) if value is not None else None
        except Exception as error:
            raise VaultError(
                "Could not read the ChatGPT session from macOS Keychain"
            ) from error

    def save(self, client_id, record):
        value = json.dumps(
            record, ensure_ascii=False, separators=(",", ":"), allow_nan=False
        )
        try:
            self.backend.set_password(self.service, client_id, value)
        except Exception as error:
            raise VaultError(
                "Could not save the ChatGPT session in macOS Keychain"
            ) from error

    def delete(self, client_id):
        try:
            if self.backend.get_password(self.service, client_id) is not None:
                self.backend.delete_password(self.service, client_id)
        except Exception as error:
            raise VaultError(
                "Could not remove the ChatGPT session from macOS Keychain"
            ) from error
