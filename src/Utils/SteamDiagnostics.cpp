#include "SteamDiagnostics.h"
#include "Hash.h"
#include "Log.h"

#include <cstdint>
#include <thread>
#include <utility>
#include <windows.h>

namespace SteamDiagnostics {

namespace {

    struct Snapshot {
        std::string buildID = "(unavailable)";
        std::string steamclientPath;
        std::string steamclientSha256 = "(unavailable)";
        std::string steamUIPath;
        std::string steamUISha256 = "(unavailable)";
    };

    Snapshot g_snapshot;

    static std::string DetectSteamBuildID()
    {
        using GetBootstrapperVersion_t = int64_t (*)();

        const HMODULE steam = GetModuleHandleA("steam.exe");
        if (!steam) {
            LOG_WARN("SteamDiagnostics: steam.exe module not loaded; build id unavailable");
            return "(unavailable)";
        }

        const auto getBootstrapperVersion =
            reinterpret_cast<GetBootstrapperVersion_t>(
                GetProcAddress(steam, "GetBootstrapperVersion"));
        if (!getBootstrapperVersion) {
            LOG_WARN("SteamDiagnostics: steam.exe!GetBootstrapperVersion not exported");
            return "(unavailable)";
        }

        return std::to_string(getBootstrapperVersion());
    }

    static std::string HashOrUnavailable(const std::string& path)
    {
        std::string sha256 = Sha256OfFile(path);
        return sha256.empty() ? "(unavailable)" : std::move(sha256);
    }

    static std::string AppendSnapshot(std::string message)
    {
        message +=
            "\n\nSteam diagnostics:\n"
            "  Build ID:              " + g_snapshot.buildID + "\n"
            "  steamclient64.dll SHA: " + g_snapshot.steamclientSha256 + "\n"
            "  steamui.dll SHA:       " + g_snapshot.steamUISha256;
        return message;
    }

} // namespace

void Initialize(const std::string& steamclientPath,
                const std::string& steamUIPath)
{
    g_snapshot.buildID = DetectSteamBuildID();
    g_snapshot.steamclientPath = steamclientPath;
    g_snapshot.steamclientSha256 = HashOrUnavailable(steamclientPath);
    g_snapshot.steamUIPath = steamUIPath;
    g_snapshot.steamUISha256 = HashOrUnavailable(steamUIPath);

    LOG_INFO("SteamDiagnostics: build={} steamclient64.sha256={} steamui.sha256={}",
             g_snapshot.buildID,
             g_snapshot.steamclientSha256,
             g_snapshot.steamUISha256);
}

std::string Sha256Of(const std::string& path)
{
    if (path == g_snapshot.steamclientPath)
        return g_snapshot.steamclientSha256 == "(unavailable)"
            ? std::string{}
            : g_snapshot.steamclientSha256;

    if (path == g_snapshot.steamUIPath)
        return g_snapshot.steamUISha256 == "(unavailable)"
            ? std::string{}
            : g_snapshot.steamUISha256;

    return Sha256OfFile(path);
}

void ShowWarning(std::string title, std::string message)
{
    std::thread([title = std::move(title),
                 message = AppendSnapshot(std::move(message))]() {
        MessageBoxA(nullptr, message.c_str(), title.c_str(),
                    MB_OK | MB_ICONWARNING | MB_TOPMOST);
    }).detach();
}

} // namespace SteamDiagnostics
