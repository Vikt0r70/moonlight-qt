#pragma once

// The production pairing handshake: upstream Moonlight's own pairing crypto, driven against the
// address the control plane supplied (03-RESEARCH.md §Pairing sequence step 3: "reuse upstream
// Moonlight pairing crypto; do not reimplement salt/certificate/AES handshake logic in SeatHub").
//
// This is the only file in `app/seathub/` that includes `app/backend/`. It calls public upstream
// API and edits no upstream file, so the D-28 diff gate (`app/streaming/`) and the fork boundary
// are both untouched - which is why the interface exists in this shape at all.
//
// The sequence it runs, all of it upstream's:
//
//   1. `NvHTTP` over the host address. The address comes from the control plane
//      (`docs/spec/openapi.yaml:1797` `host_address`, `:1802-1813` the port set), not from mDNS.
//      That substitution is the whole trick: mDNS is only upstream's usual way of learning an
//      address, and `NvHTTP` has a public constructor that takes one directly
//      (`app/backend/nvhttp.h:112`).
//   2. `NvHTTP::getServerInfo()` - unauthenticated, over HTTP 47989 - for `<HttpsPort>`,
//      `<appversion>`, the host `uniqueid` and `PairState`. With no pinned certificate and
//      httpsPort 0 this is the same pre-pairing path upstream's own address-only host add takes
//      (`app/backend/computermanager.cpp:805`).
//   3. `NvComputer(NvHTTP&, QString serverInfo)` - the same constructor that path uses
//      (`app/backend/computermanager.cpp:821`).
//   4. `NvPairingManager::pair()` - the five-phase handshake
//      (`app/backend/nvpairingmanager.cpp`): `getservercert` over HTTP, `clientchallenge`,
//      `serverchallengeresp` (which verifies the host's signature), the PIN check, and
//      `clientpairingsecret` + `pairchallenge` over HTTPS 47984. Success is `<paired>1</paired>`.
//   5. The client identity: SHA-256 over this client's own certificate, which is the value a
//      host-side reader of Sunshine's `GET /api/clients/list` can match to this client's pairing
//      record (see `pairing_seam.h` for why the Sunshine UUID itself is not available here).

#include "pairing_seam.h"

/// Run one handshake against `target.hostAddress` with `target.pairingPin`. Blocks its caller
/// (it is called on a worker thread) and never throws.
///
/// On success `ok` is true and `clientIdentity` is the fingerprint of this client's certificate.
/// On any failure `ok` is false, `clientIdentity` is empty, and `engineError` carries upstream's
/// own text for the log only - it is never rendered (D-51) and never contains the PIN.
PairingHandshakeResult runUpstreamPairingHandshake(const PairingTarget& target);
