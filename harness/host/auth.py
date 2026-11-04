#!/usr/bin/env python3
"""Persistent ChatGPT plan login; no command prints a bearer or refresh token."""

import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
from .login import login
from .oauth import AuthError
from .session import DEFAULT_PROFILE, SessionManager, UNUSABLE
from .vault import KeychainVault, VaultError


def execute(manager, command):
    if command and command[0] == "--":
        command = command[1:]
    if not command:
        raise AuthError("usage", "A child command is required")
    token = manager.access_token()
    env = dict(os.environ, AH_ACCESS_TOKEN=token)
    env.pop("OPENAI_API_KEY", None)
    return subprocess.call(command, env=env)


def main():
    parser = argparse.ArgumentParser(
        description="Persistent ChatGPT plan authorization in macOS Keychain"
    )
    parser.add_argument("--profile", type=Path, default=DEFAULT_PROFILE)
    sub = parser.add_subparsers(dest="command", required=True)
    connect = sub.add_parser("login")
    connect.add_argument("--no-open", action="store_true")
    connect.add_argument("--force", action="store_true")
    connect.add_argument("--then", nargs=argparse.REMAINDER)
    sub.add_parser("status")
    sub.add_parser("logout")
    run = sub.add_parser("exec")
    run.add_argument("argv", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    try:
        manager = SessionManager(KeychainVault(), args.profile)
        if args.command == "status":
            print(json.dumps(manager.status()))
        elif args.command == "logout":
            print(json.dumps(manager.logout()))
        elif args.command == "exec":
            return execute(manager, args.argv)
        else:
            connected = False
            if not args.force:
                try:
                    manager.access_token()
                    connected = True
                except AuthError as error:
                    if error.code != "login_required" and error.code not in UNUSABLE:
                        raise
            if not connected:
                login(
                    manager,
                    lambda url: print(
                        json.dumps({"login_url": url, "storage": "macOS Keychain"}),
                        flush=True,
                    ),
                    open_browser=not args.no_open,
                )
            print(json.dumps({**manager.status(), "reused": connected}), flush=True)
            if args.then:
                return execute(manager, args.then)
        return 0
    except (AuthError, VaultError) as error:
        print(
            json.dumps(
                {"error": getattr(error, "code", "keychain"), "message": str(error)}
            ),
            file=sys.stderr,
        )
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
