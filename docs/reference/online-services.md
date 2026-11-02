# Optional online-service backend

This backend is independent of a game library or authorization interface. I extracted the engine changes from my originating fork for the [scope discussion](https://github.com/seedds/nxnative/issues/951). Default builds remain offline. No game, firmware, service certificate, account document, password, OAuth token or observed leaf certificate is distributed here.

### Native client TLS

- **Symptom:** an enabled native socket connects, but the SDK's TLS context is still an unavailable-service cut.
- **Cause:** a raw TCP transport does not implement the SSL service, its object ABI, certificate verification or asynchronous socket ownership.
- **Fix:** `hle_ssl.c` adapts the C/C++ service entry points to `hle_ssl_apple.m`. Opaque guest objects key bounded owned registries. The backend duplicates a guest descriptor with its generation, preserving the underlying connection through guest close/reuse; destroy/finalize invalidate the owned objects. Blocking methods use the guest-aware HLE wait path. The implementation handles real handshake/read/write/poll, nonblocking progress, server-certificate buffers, client PKCS#12 data and ALPN. Unsupported operations fail rather than claiming a session. Contexts accept normal system PKI and explicitly imported server anchors, always retaining hostname verification.
- **Private PKI:** a configured peer may supply a private anchor, scoped to that peer/connection. `hle_nextendo_x509.c` uses strict X.509 purpose, SAN, chain, current-time and strength checks without default trust paths or a verification bypass. Optional explicit leaf enrollment accepts only identical DER for one enrolled logical identity, with self-signature, validity, server purpose and key/signature strength checks. It neither adopts an observed certificate automatically nor changes system trust. A numeric NEX handoff is allowed only by an explicit alias paired to the same exact identity, address and enrolled certificate.
- **Build:** `dm/tools/apple_crypto.py` pins OpenSSL source by SHA-256 and builds static libcrypto for the selected Apple SDK. the DM_OPENSSL_ROOT build variable may point to an already prepared target-compatible prefix. The iOS source/flags generator propagates the static archive to both real and smoke links; a Mac archive is not an iPhone dependency. The [OpenSSL license and provenance](../../dm/third_party/notices/OpenSSL/PROVENANCE.md) must accompany redistributed linked applications. No OpenSSL binary is committed.
- **Check:** I exercised real loopback TLS through the guest's registered C/C++ gates using ephemeral test certificates: SNI/hostname failures, nonblocking progression, data, polling, certificate buffers, descriptor generation/close ownership, context IDs, ALPN and lifecycle. The private verifier separately rejects malformed DER, wrong purposes/names/anchors, weak/invalid signatures, expired/future leaves and an unpaired/rotated enrolled certificate. No user account or public game server is involved in these fixtures.


## Validation scope

I tested the mechanism with synthetic/loopback fixtures. I did not repeat the five-game graphical regression on this rebased backend branch. No new gameplay qualification is claimed.
