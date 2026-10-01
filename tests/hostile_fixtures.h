#pragma once

// Test-only hostile data used to prove diagnostics do not serialize customer or rig material.
namespace HostileFixtures {

inline constexpr char RigAddress[] = "203.0.113.77";
inline constexpr char RigAddressWithPort[] = "203.0.113.77:48010";
inline constexpr char Pin[] = "4821";
inline constexpr char Bearer[] = "tok-secret-abc123";
inline constexpr char PemHeader[] = "-----BEGIN CERTIFICATE-----";
inline constexpr char Email[] = "a.b@example.test";
inline constexpr char Phone[] = "+962790000000";
inline constexpr char PairingSecret[] = "clientpairingsecret=deadbeefcafebabe";
inline constexpr char ResponseFragment[] = "{\"access_token\":\"response-secret\"}";

} // namespace HostileFixtures
