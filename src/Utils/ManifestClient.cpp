#include "ManifestClient.h"
#include "Config.h"
#include "Log.h"
#include "LuaConfig.h"
#include "WinHttp.h"
#include <algorithm>
#include <charconv>
#include <mutex>
#include <string_view>

namespace ManifestClient {

    // ── parsers ────────────────────────────────────────────────────
    using Parser = bool (*)(std::string_view body, uint64_t* out);

    static bool ParsePlainUint(std::string_view body, uint64_t* out) {
        uint64_t code = 0;
        auto [_, ec] = std::from_chars(body.data(), body.data() + body.size(), code);
        if (ec != std::errc{}) return false;
        *out = code;
        return true;
    }

    static bool ParseSteamRunJson(std::string_view body, uint64_t* out) {
        size_t key = body.find("\"content\"");
        if (key == std::string_view::npos) return false;
        size_t q1 = body.find('"', key + 9);
        if (q1 == std::string_view::npos) return false;
        size_t q2 = body.find('"', q1 + 1);
        if (q2 == std::string_view::npos) return false;
        return ParsePlainUint(body.substr(q1 + 1, q2 - q1 - 1), out);
    }

    // ── provider table ────────────────────────────────────────────
    //
    // Adding a new provider: add one row to kProviders below.
    // host / port / tls / path are all derived from the URL template
    // by Make() at compile time.

    struct Provider {
        std::string_view name;          // matches [manifest] url = "..."
        const char*      urlTemplate;   // full literal with one %llu — for log & path
        std::string_view host;          // ASCII slice into urlTemplate
        const char*      pathFormat;    // suffix of urlTemplate, null-terminated, e.g. "/path/%llu"
        INTERNET_PORT    port;
        bool             tls;
        Parser           parse;
    };

    consteval Provider Make(std::string_view name, const char* url, Parser parse) {
        std::string_view sv{url};
        bool tls = false;
        INTERNET_PORT port = INTERNET_DEFAULT_HTTP_PORT;
        if (sv.starts_with("https://")) { 
            tls = true; 
            port = INTERNET_DEFAULT_HTTPS_PORT; 
            sv.remove_prefix(8); 
        }
        else if (sv.starts_with("http://")) {
            sv.remove_prefix(7);
        }

        size_t slash = sv.find('/');
        std::string_view hostPart = sv.substr(0, slash);
        // sv.data() points into url; the suffix at slash is null-terminated
        // because url itself is a null-terminated literal.
        const char* pathFormat = (slash == std::string_view::npos) ? "/" : (sv.data() + slash);

        std::string_view host = hostPart;
        if (size_t colon = hostPart.find(':'); colon != std::string_view::npos) {
            host = hostPart.substr(0, colon);
            INTERNET_PORT custom = 0;
            for (char c : hostPart.substr(colon + 1))
                custom = static_cast<INTERNET_PORT>(custom * 10 + (c - '0'));
            port = custom;
        }
        return {name, url, host, pathFormat, port, tls, parse};
    }

    static constexpr Provider kProviders[] = {
        Make("opensteamtool", "https://manifest.opensteamtool.com/%llu",       ParsePlainUint),
        Make("wudrm",         "http://gmrc.wudrm.com/manifest/%llu",           ParsePlainUint),
        Make("steamrun",      "https://manifest.steam.run/api/manifest/%llu",  ParseSteamRunJson),
    };

    static const Provider* g_active = &kProviders[0];   // opensteamtool

    bool SetProvider(std::string_view name) {
        for (const auto& p : kProviders)
            if (p.name == name) { 
                g_active = &p; 
                return true; 
            }
        return false;
    }

    const char* ActiveProviderName() { 
        return g_active->name.data(); 
    }

    // ── connection ────────────────────────────────────────────────

    static std::mutex      g_mutex;
    static HINTERNET       g_hSession  = nullptr;
    static HINTERNET       g_hConnect  = nullptr;
    static const Provider* g_connected = nullptr;   // provider g_hConnect is tied to

    static void CloseConnection() {
        if (g_hConnect) { WinHttpCloseHandle(g_hConnect); g_hConnect = nullptr; }
        if (g_hSession) { WinHttpCloseHandle(g_hSession); g_hSession = nullptr; }
        g_connected = nullptr;
    }

    // Widen an ASCII string_view into a wide stack buffer (null-terminated).
    template <size_t N>
    static void WidenAscii(std::string_view s, wchar_t (&dst)[N]) {
        size_t n = (std::min)(s.size(), N - 1);   // parens defeat the windows.h min macro
        for (size_t i = 0; i < n; ++i) dst[i] = static_cast<wchar_t>(s[i]);
        dst[n] = 0;
    }

    static void EnsureConnection() {
        if (g_hConnect && g_connected == g_active) return;
        CloseConnection();

        g_hSession = WinHttpOpen(L"OpenSteamTool/1.0",
            WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!g_hSession) return;

        WinHttpSetTimeouts(g_hSession,
            Config::manifestTimeoutResolve, Config::manifestTimeoutConnect,
            Config::manifestTimeoutSend,    Config::manifestTimeoutRecv);

        wchar_t hostW[256];
        WidenAscii(g_active->host, hostW);

        g_hConnect = WinHttpConnect(g_hSession, hostW, g_active->port, 0);
        if (!g_hConnect) {
            WinHttpCloseHandle(g_hSession);
            g_hSession = nullptr;
            return;
        }
        g_connected = g_active;
    }

    void Shutdown() {
        std::lock_guard<std::mutex> lock(g_mutex);
        CloseConnection();
    }

    // ── fetch ─────────────────────────────────────────────────────

    static bool FetchActive(uint64_t gid, uint64_t* outCode) {
        EnsureConnection();
        if (!g_hConnect) return false;
        const Provider& p = *g_active;

        char pathN[160];
        std::snprintf(pathN, sizeof(pathN), p.pathFormat, gid);
        wchar_t pathW[160];
        WidenAscii(pathN, pathW);

        char urlLog[256];
        std::snprintf(urlLog, sizeof(urlLog), p.urlTemplate, gid);

        auto r = WinHttp::ExecuteEx(g_hSession, g_hConnect, p.tls,
                                    L"GET", pathW, nullptr, 0, nullptr, urlLog);
        if (!r.ok) CloseConnection();

        LOG_MANIFEST_INFO("Manifest {} status={} gid={}", p.name, r.status, gid);

        if (!r.ok || r.status != 200) return false;
        return p.parse(r.body, outCode);
    }

    // ── public ────────────────────────────────────────────────────

    bool FetchManifestRequestCode(uint64_t manifestGid, uint64_t* outRequestCode,
                                  AppId_t appId, AppId_t depotId)
    {
        std::lock_guard<std::mutex> lock(g_mutex);

        if (appId && depotId && LuaConfig::HasManifestCodeFuncEx()) {
            if (LuaConfig::CallManifestFetchCodeEx(appId, depotId, manifestGid, outRequestCode)) {
                LOG_MANIFEST_INFO("Manifest gid={} resolved via fetch_manifest_code_ex", manifestGid);
                return true;
            }
            LOG_MANIFEST_WARN("Manifest gid={} fetch_manifest_code_ex returned nil, trying fetch_manifest_code", manifestGid);
        }

        if (LuaConfig::HasManifestCodeFunc()) {
            if (LuaConfig::CallManifestFetchCode(manifestGid, outRequestCode)) {
                LOG_MANIFEST_INFO("Manifest gid={} resolved via manifest.lua", manifestGid);
                return true;
            }
            LOG_MANIFEST_WARN("Manifest gid={} lua returned nil, falling back to config", manifestGid);
        }

        return FetchActive(manifestGid, outRequestCode);
    }
}
