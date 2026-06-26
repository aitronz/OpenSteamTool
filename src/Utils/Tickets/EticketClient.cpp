#include "EticketClient.h"

#include "OSTPlatform/include/Http.h"
#include "Utils/Config/LuaConfig.h"
#include "Utils/Logging/Log.h"

#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace EticketClient {
namespace {

    // On-demand mint endpoint, sourced from LuaConfig::GetEticketUrl() — set in
    // user Lua config via seteticketurl("..."). The expected backend POSTs
    // {app_id, nonce(hex)} and returns {eticket, appticket}. Empty URL disables
    // the feature entirely; the DLL then falls back to the static credential
    // store ticket (original behaviour, identical to a stock build).

    // Short connect timeouts so a down/unreachable backend fails fast and the
    // caller falls back; generous recv because the backend mints via a live
    // Steam CM round-trip (~1-5s).
    constexpr uint32_t kResolveMs = 2000;
    constexpr uint32_t kConnectMs = 2000;
    constexpr uint32_t kSendMs    = 3000;
    constexpr uint32_t kRecvMs    = 8000;

    struct CachedTickets {
        std::vector<uint8_t> eticket;
        std::vector<uint8_t> ownership;
    };

    std::mutex g_mutex;
    std::unordered_map<AppId_t, CachedTickets> g_cache;  // only successful fetches are cached

    std::string ToHex(std::span<const uint8_t> bytes) {
        static const char digits[] = "0123456789ABCDEF";
        std::string out;
        out.reserve(bytes.size() * 2);
        for (uint8_t b : bytes) {
            out.push_back(digits[b >> 4]);
            out.push_back(digits[b & 0x0F]);
        }
        return out;
    }

    int HexNibble(char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }

    bool FromHex(std::string_view hex, std::vector<uint8_t>& out) {
        if (hex.empty() || (hex.size() % 2) != 0) return false;
        out.clear();
        out.reserve(hex.size() / 2);
        for (size_t i = 0; i < hex.size(); i += 2) {
            int hi = HexNibble(hex[i]);
            int lo = HexNibble(hex[i + 1]);
            if (hi < 0 || lo < 0) return false;
            out.push_back(static_cast<uint8_t>((hi << 4) | lo));
        }
        return true;
    }

    // Extract a string field ("key":"VALUE") from our own backend's JSON.
    // Returns false when the key is absent or its value is null/empty.
    bool ExtractStringField(std::string_view body, std::string_view key, std::string& out) {
        const std::string needle = std::string("\"") + std::string(key) + "\"";
        size_t k = body.find(needle);
        if (k == std::string_view::npos) return false;
        size_t colon = body.find(':', k + needle.size());
        if (colon == std::string_view::npos) return false;
        size_t q1 = body.find('"', colon + 1);
        if (q1 == std::string_view::npos) return false;
        // A null value (e.g. "appticket":null) has no opening quote before the
        // next delimiter — guard against grabbing a later field's quote.
        size_t delim = body.find_first_of(",}", colon + 1);
        if (delim != std::string_view::npos && q1 > delim) return false;
        size_t q2 = body.find('"', q1 + 1);
        if (q2 == std::string_view::npos) return false;
        out = std::string(body.substr(q1 + 1, q2 - q1 - 1));
        return !out.empty();
    }

    // Single backend mint → both tickets. Cached per app on success; failures are
    // not cached so the next call (the game retries ownership/eticket) re-attempts.
    bool EnsureFetched(AppId_t appId, std::span<const uint8_t> nonce, CachedTickets& out) {
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            auto it = g_cache.find(appId);
            if (it != g_cache.end()) { out = it->second; return true; }
        }

        const std::string& url = LuaConfig::GetEticketUrl();
        if (url.empty()) return false;

        const std::string nonceHex = ToHex(nonce);
        const std::string reqBody =
            "{\"app_id\":\"" + std::to_string(appId) + "\",\"nonce\":\"" + nonceHex + "\"}";

        auto r = OSTPlatform::Http::Execute(
            L"POST", url.c_str(),
            reqBody.data(), static_cast<uint32_t>(reqBody.size()),
            L"Content-Type: application/json\r\n",
            kResolveMs, kConnectMs, kSendMs, kRecvMs);

        if (!r.ok || r.status != 200) {
            LOG_IPC_WARN("EticketClient: on-demand fetch failed appid={} status={} ok={} (fallback to credential store)",
                         appId, r.status, r.ok);
            return false;
        }

        CachedTickets fetched;
        std::string hex;
        if (ExtractStringField(r.body, "eticket", hex)) {
            if (!FromHex(hex, fetched.eticket)) fetched.eticket.clear();
        }
        if (ExtractStringField(r.body, "appticket", hex)) {
            if (!FromHex(hex, fetched.ownership)) fetched.ownership.clear();
        }

        if (fetched.eticket.empty() && fetched.ownership.empty()) {
            LOG_IPC_WARN("EticketClient: backend returned no usable tickets appid={} bytes={}", appId, r.body.size());
            return false;
        }

        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_cache[appId] = fetched;
            out = fetched;
        }
        LOG_IPC_INFO("EticketClient: minted appid={} eticket_bytes={} ownership_bytes={} nonce_bytes={}",
                     appId, fetched.eticket.size(), fetched.ownership.size(), nonce.size());
        return true;
    }

} // namespace

std::optional<std::vector<uint8_t>> FetchFreshEticket(AppId_t appId, std::span<const uint8_t> nonce) {
    CachedTickets t;
    if (!EnsureFetched(appId, nonce, t) || t.eticket.empty()) return std::nullopt;
    return t.eticket;
}

std::optional<std::vector<uint8_t>> FetchOwnershipTicket(AppId_t appId, std::span<const uint8_t> nonce) {
    CachedTickets t;
    if (!EnsureFetched(appId, nonce, t) || t.ownership.empty()) return std::nullopt;
    return t.ownership;
}

} // namespace EticketClient
