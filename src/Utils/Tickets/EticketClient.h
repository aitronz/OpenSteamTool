#pragma once

#include "Steam/Types.h"

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace EticketClient {

    // On-demand encrypted-app-ticket mint.
    //
    // Strict Denuvo titles bind their encrypted app ticket to a nonce they pass
    // into RequestEncryptedAppTicket (pData) AT LAUNCH, and reject any pre-baked
    // / stale ticket with 88500012. A ticket written to the credential store
    // before launch can never carry that nonce, so for those titles we POST
    // {app_id, nonce} to a user-configured backend (see seteticketurl() in Lua
    // config), which is expected to mint a FRESH ticket from an owning pool
    // account with userdata=nonce — matching the exact challenge the running
    // game validates. Disabled (empty URL) is the default; the DLL then serves
    // the static credential-store ticket exactly as a stock build does.
    //
    // Returns the fresh ticket bytes, or nullopt on any failure (disabled,
    // backend down, bad response). Callers fall back to the static credential
    // store so titles that don't need this keep working unchanged.
    std::optional<std::vector<uint8_t>> FetchFreshEticket(AppId_t appId, std::span<const uint8_t> nonce);

    // Same backend mint, but returns the signed app-OWNERSHIP ticket instead of
    // the eticket. Both come from ONE /eticket call (one pool account) and are
    // cached per app, so the eticket served at the IPC layer and the ownership
    // ticket spoofed at the netpacket layer always match the same account —
    // required by Denuvo titles that verify ownership over the network
    // (k_EMsgClientGetAppOwnershipTicket, e.g. Suicide Squad: KTJL).
    // nonce is only used on the first fetch for an app.
    std::optional<std::vector<uint8_t>> FetchOwnershipTicket(AppId_t appId, std::span<const uint8_t> nonce);

} // namespace EticketClient
