# Vendored dependencies

`yyjson` 0.12.0 is vendored unmodified from https://github.com/ibireme/yyjson/tree/0.12.0.
Its MIT license is in `yyjson/LICENSE`. No dependency is downloaded at build time.

SHA-256:

- `yyjson.h`: `175867c5493a5df648cec566717fa1c29aa2f6096f5f0cf1efad0b65e1f6d7b3`
- `yyjson.c`: `ac2e9bbb2e2d9149d90878d40506a1d624fa0b33c979a11b61075c54782c6d6a`
- `LICENSE`: `45e384d3d52c73cba3a64d6e6c25d47cd738cd8a55c30629e3201046eda62947`

libcurl and SQLite are external system dependencies. Record their runtime versions
when comparing benchmark results. The store uses rollback journaling with FULL
synchronization and a single owner; it does not depend on concurrent WAL writers.
