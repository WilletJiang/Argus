"""OpenAI public-client OAuth and verified identity handling."""

import hashlib
import json
import urllib.error
import urllib.parse
import urllib.request

ISSUER = "https://auth.openai.com"
RESOURCE = "https://api.openai.com/v1"
SCOPE = "openid profile email offline_access resource.invoke chatgpt.tokens.use.direct"


class AuthError(RuntimeError):
    def __init__(self, code, message, *, retryable=False, earliest_refresh_at=None):
        super().__init__(message)
        self.code = code
        self.retryable = retryable
        self.earliest_refresh_at = earliest_refresh_at


class OAuthClient:
    def __init__(self):
        self.discovery = None
        self.jwks = None

    def _request(self, url, values=None):
        body = urllib.parse.urlencode(values).encode() if values is not None else None
        headers = {"Content-Type": "application/x-www-form-urlencoded"} if body else {}
        request = urllib.request.Request(url, data=body, headers=headers)
        try:
            with urllib.request.urlopen(request, timeout=30) as response:
                raw = response.read(65537)
                if len(raw) > 65536:
                    raise AuthError(
                        "protocol", "OAuth response exceeded its size limit"
                    )
                value = json.loads(raw)
        except urllib.error.HTTPError as error:
            try:
                data = json.loads(error.read(65536))
            except (ValueError, OSError):
                data = {}
            detail = data.get("error") if isinstance(data, dict) else None
            code = detail.get("code") if isinstance(detail, dict) else detail
            if (
                not isinstance(code, str)
                or len(code) > 64
                or not code.replace("_", "").isalnum()
            ):
                code = f"http_{error.code}"
            earliest = (
                data.get("earliest_refresh_at") if isinstance(data, dict) else None
            )
            raise AuthError(
                code,
                f"OpenAI authorization failed ({code})",
                retryable=error.code in (429, 500, 502, 503, 504),
                earliest_refresh_at=earliest,
            ) from None
        except (urllib.error.URLError, TimeoutError, OSError) as error:
            raise AuthError(
                "network", "OpenAI authorization network request failed", retryable=True
            ) from error
        except (ValueError, UnicodeError) as error:
            raise AuthError(
                "protocol", "OpenAI authorization returned invalid JSON"
            ) from error
        if not isinstance(value, dict):
            raise AuthError("protocol", "OAuth object response required")
        return value

    def configuration(self):
        if self.discovery is None:
            value = self._request(ISSUER + "/.well-known/openid-configuration")
            if value.get("issuer") != ISSUER:
                raise AuthError("identity", "Unexpected OpenAI issuer")
            for field in ("jwks_uri", "revocation_endpoint"):
                url = value.get(field)
                if url and (
                    urllib.parse.urlparse(url).scheme != "https"
                    or urllib.parse.urlparse(url).hostname != "auth.openai.com"
                ):
                    raise AuthError("identity", "Unexpected authorization endpoint")
            self.discovery = value
        return self.discovery

    def identity(self, token, client_id, *, nonce=None, subject_hash=None):
        try:
            import jwt
        except ImportError as error:
            raise AuthError(
                "dependency", "Install the optional host requirements first"
            ) from error
        discovery = self.configuration()
        try:
            if self.jwks is None:
                self.jwks = jwt.PyJWKClient(discovery["jwks_uri"])
            key = self.jwks.get_signing_key_from_jwt(token)
            required = ["exp", "iss", "aud", "sub"] + (
                ["nonce"] if nonce is not None else []
            )
            claims = jwt.decode(
                token,
                key.key,
                algorithms=["RS256", "ES256"],
                issuer=ISSUER,
                audience=client_id,
                options={"require": required},
            )
        except Exception as error:
            raise AuthError(
                "identity", "OpenAI identity token validation failed"
            ) from error
        if nonce is not None and claims.get("nonce") != nonce:
            raise AuthError("identity", "Sign-in nonce mismatch")
        digest = hashlib.sha256(claims["sub"].encode()).hexdigest()
        if subject_hash and digest != subject_hash:
            raise AuthError(
                "identity", "Sign-in account differs from the selected registration"
            )
        return claims

    def exchange(self, client_id, code, verifier, redirect):
        return self._request(
            ISSUER + "/api/accounts/oauth/token",
            {
                "grant_type": "authorization_code",
                "client_id": client_id,
                "code": code,
                "code_verifier": verifier,
                "redirect_uri": redirect,
                "resource": RESOURCE,
            },
        )

    def refresh(self, client_id, token):
        return self._request(
            ISSUER + "/api/accounts/oauth/token",
            {
                "grant_type": "refresh_token",
                "client_id": client_id,
                "refresh_token": token,
                "resource": RESOURCE,
            },
        )

    def revoke(self, client_id, token):
        endpoint = self.configuration().get("revocation_endpoint")
        if not endpoint:
            raise AuthError("protocol", "OpenAI did not publish a revocation endpoint")
        body = urllib.parse.urlencode(
            {"token": token, "token_type_hint": "refresh_token", "client_id": client_id}
        ).encode()
        try:
            with urllib.request.urlopen(
                urllib.request.Request(
                    endpoint,
                    data=body,
                    headers={"Content-Type": "application/x-www-form-urlencoded"},
                ),
                timeout=30,
            ) as response:
                if response.status != 200:
                    raise AuthError("revocation", "OpenAI did not confirm sign-out")
        except (urllib.error.URLError, TimeoutError, OSError) as error:
            raise AuthError(
                "revocation", "Remote sign-out could not be confirmed", retryable=True
            ) from error
