#include "Hooks_IPC.h"
#include "Hooks_IPC_ISteamUtils.h"
#include "Hooks_IPC_ISteamUser.h"
#include "Hooks_Misc.h"
#include "Steam/Callback.h"
#include "Utils/Log.h"

namespace {

    // ── IClientUtils::GetAPICallResult request args ──────────────
    struct GetAPICallResultRequest {
        uint64  hSteamAPICall;     // +0
        uint32  cubCallback;       // +8
        uint32  iCallbackExpected; // +12
    };

    // ── Helper: write the GetAPICallResult response boilerplate ───
    template<typename CallbackT, typename F>
    bool WriteCallbackResponse(CUtlBuffer* pWrite, F&& fill)
    {
        constexpr int32 total = 1 + 1 + sizeof(CallbackT) + 1;
        if (pWrite->m_Put < total) return false;

        uint8* base = pWrite->m_Memory.m_pMemory;
        base[0] = RESPONSE_PREFIX;
        base[1] = 1;
        base[2 + sizeof(CallbackT)] = 0;

        auto* cb = reinterpret_cast<CallbackT*>(base + 2);
        fill(*cb);
        return true;
    }

    // ════════════════════════════════════════════════════════════════
    //  GetAPICallResult per-callback handlers
    // ════════════════════════════════════════════════════════════════

    bool HandleCallback_EncryptedAppTicketResponse(
        CSteamPipeClient*, CUtlBuffer* pWrite, uint64 hAsyncCall, uint32 cubCallback)
    {
        AppId_t appId = Hooks_IPC_ISteamUser::LookupEticketAsyncCall(hAsyncCall);
        if (!appId) return false;

        LOG_IPC_DEBUG("GetAPICallResult: EncryptedAppTicketResponse hAsyncCall=0x{:016X} "
                  "AppId={} - injecting k_EResultOK", hAsyncCall, appId);

        if (!WriteCallbackResponse<EncryptedAppTicketResponse_t>(pWrite, [](auto& cb) {
            cb.m_eResult = k_EResultOK;
        })) return false;

        Hooks_IPC_ISteamUser::EraseEticketAsyncCall(hAsyncCall);
        return true;
    }

    template<typename CallbackT>
    bool RewriteOnlineFixStatsResult(
        CSteamPipeClient* pipe, CUtlBuffer* pWrite, const char* name, uint32 cubCallback)
    {
        constexpr int32 total = 1 + 1 + sizeof(CallbackT) + 1;
        if (!pipe || cubCallback < sizeof(CallbackT) || pWrite->m_Put < total
            || pWrite->Base()[1] == 0) {
            return false;
        }

        auto* cb = reinterpret_cast<CallbackT*>(pWrite->Base() + 2);
        const uint64 previous = cb->m_nGameID;
        if (!Hooks_Misc::RewriteOnlineFixUserStatsCallback(pipe->m_hSteamPipe, cb->m_nGameID))
            return false;

        LOG_IPC_TRACE("GetAPICallResult: OnlineFix {} CGameID {:#x} -> {:#x}",
                      name, previous, cb->m_nGameID);
        return true;
    }

    bool HandleCallback_UserStatsReceived(
        CSteamPipeClient* pipe, CUtlBuffer* pWrite, uint64, uint32 cubCallback)
    {
        return RewriteOnlineFixStatsResult<UserStatsReceived_t>(
            pipe, pWrite, "UserStatsReceived", cubCallback);
    }

    bool HandleCallback_GlobalAchievementPercentagesReady(
        CSteamPipeClient* pipe, CUtlBuffer* pWrite, uint64, uint32 cubCallback)
    {
        return RewriteOnlineFixStatsResult<GlobalAchievementPercentagesReady_t>(
            pipe, pWrite, "GlobalAchievementPercentagesReady", cubCallback);
    }

    bool HandleCallback_GlobalStatsReceived(
        CSteamPipeClient* pipe, CUtlBuffer* pWrite, uint64, uint32 cubCallback)
    {
        return RewriteOnlineFixStatsResult<GlobalStatsReceived_t>(
            pipe, pWrite, "GlobalStatsReceived", cubCallback);
    }

    struct GacrDispatchEntry {
        uint32  callbackId;
        bool  (*handler)(CSteamPipeClient* pipe, CUtlBuffer* pWrite,
                         uint64 hAsyncCall, uint32 cubCallback);
    };

    constexpr GacrDispatchEntry g_GacrDispatch[] = {
        { EncryptedAppTicketResponse_t::k_iCallback, HandleCallback_EncryptedAppTicketResponse },
        { UserStatsReceived_t::k_iCallback, HandleCallback_UserStatsReceived },
        { GlobalAchievementPercentagesReady_t::k_iCallback,
          HandleCallback_GlobalAchievementPercentagesReady },
        { GlobalStatsReceived_t::k_iCallback, HandleCallback_GlobalStatsReceived },
    };

    // ── Handler: IClientUtils::GetAPICallResult ──────────────────
    void Handler_IClientUtils_GetAPICallResult(
        CSteamPipeClient* pipe, CUtlBuffer* pRead, CUtlBuffer* pWrite)
    {
        if (pRead->m_Put < OFFSET_ARGS + sizeof(GetAPICallResultRequest)) return;

        const auto* req = reinterpret_cast<const GetAPICallResultRequest*>(
            pRead->Base() + OFFSET_ARGS);

        AppId_t appId = Hooks_Misc::GetAppIDForCurrentPipe();
        LOG_IPC_DEBUG("GetAPICallResult: hAsyncCall=0x{:016X} AppId={} iCallback={} cubCallback={}",
                  req->hSteamAPICall, appId, req->iCallbackExpected, req->cubCallback);
        for (auto& entry : g_GacrDispatch) {
            if (entry.callbackId == req->iCallbackExpected) {
                entry.handler(pipe, pWrite, req->hSteamAPICall, req->cubCallback);
                return;
            }
        }
    }

    const Hooks_IPC::IpcHandlerEntry g_Entries[] = {
        ADD_IPC_HANDLER(IClientUtils, GetAPICallResult),
    };

} // namespace

namespace Hooks_IPC_ISteamUtils {
    void Register() {
        Hooks_IPC::RegisterHandlers(g_Entries, std::size(g_Entries));
    }
}
