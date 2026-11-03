# Persistent ChatGPT login for a macOS host

This optional adapter implements public-client Sign in with ChatGPT, independent
of the C runtime. It uses the user's subscription grant, not a Platform API key
and not a copy of Codex's credentials. The native Keychain backend is required;
there is no plaintext fallback. Linux hosts can provide a different vault adapter.

From the harness directory:

```sh
python3 -m pip install -r host/requirements.txt
python3 -B host/auth.py login
python3 -B host/auth.py status
python3 -B host/auth.py exec -- ./build/ah new --db /private/path/agent.db --model MODEL_SLUG
```

`login` reuses an existing session or refreshes it when necessary. Only a missing
or unusable grant starts browser authorization. `--force` explicitly reconnects.
`login --then COMMAND ...` starts a trusted child after connecting; `exec -- COMMAND ...`
reuses the same session in later processes. Neither command prints a token.

Credentials are stored as one replacement record under Keychain service
`Argus Harness / ChatGPT`, account = issued OAuth client ID. The record contains
access, refresh and identity tokens plus grant metadata. Public registration at
`~/.config/argus-harness-evaluation/registration.json` contains only host ID,
client ID and account subject hash; the historical path preserves the existing
registration. A companion owner-only lock serializes refresh across processes.
Use one profile path per registration. `--profile PATH` selects another account
profile; it is not a credential export.

The login callback checks state, PKCE and signed identity claims including issuer,
audience, expiry, nonce and the selected account. Fresh login opens the official
authorization URL directly. When reusing an identity-token hint, a local opaque
entry URL keeps that token out of process arguments and application logs. The
concurrent loopback listener tolerates idle browser preconnections while serializing
authorization-code exchange. Refresh
replaces the entire rotated credential set under the same lock. Transient failures
retain the prior record; permanently invalid refresh grants clear unusable tokens
and leave public registration intact for reconnection. An explicit `logout`
attempts remote revocation and deletes local tokens, reporting both outcomes.
Removing evaluation artifacts never logs out.

`exec` gives only the short-lived access token to the trusted child via
`AH_ACCESS_TOKEN`; it does not expose refresh or identity tokens. The child must
keep that bearer out of tool environments. Refresh occurs before starting the
child, not during a running child's lifetime. Long-lived application hosts should
call `SessionManager.access_token()` at request boundaries and pass the result to
`ah_responses_set_token()` between calls. This adapter is not yet wired into the
Argus app's UI or a background credential broker.

Offline tests cover persisted reuse, rotated refresh, process serialization,
invalid/transient grants, public metadata, callback state and PKCE, and error
redaction. Signed JWT tests run when optional JWT dependencies are installed;
other tests need only Python's standard library. Real Keychain behavior is checked
locally with a synthetic item, which is deleted afterward. Offline tests make no
network calls and never access the user's actual Keychain account.

Protocol: [sign-in](https://developers.openai.com/siwc/token-sharing-open-source/sign-in),
[accounts and sessions](https://developers.openai.com/siwc/token-sharing-open-source/profiles-and-sessions),
[token reference](https://developers.openai.com/siwc/token-sharing-open-source/token-reference).
