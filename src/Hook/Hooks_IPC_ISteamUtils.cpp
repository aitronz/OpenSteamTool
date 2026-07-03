#include "Hooks_IPC.h"
#include "Hooks_IPC_ISteamUtils.h"
#include "Hooks_Misc.h"
#include "PendingAPICalls.h"
#include "Steam/Callback.h"
#include "Utils/Logging/Log.h"

#include <type_traits>

namespace {
    using namespace IPCMessages::IClientUtils;

    template <class CallbackT>
    bool WriteAPICallResult(CUtlBuffer* pWrite,uint32 callbackCapacity,const CallbackT& callback)
    {
        static_assert(std::is_trivially_copyable_v<CallbackT>);
        if (callbackCapacity < sizeof(CallbackT)) return false;

        GetAPICallResultResp resp{pWrite, callbackCapacity};
        if (!resp.ok()) return false;
        if (!resp.set_pCallback(IPCMessages::asBytes(callback))) return false;
        resp.set_returnValue(true);
        resp.set_pbFailed(false);
        return true;
    }

    // [Post-Handler]: IClientUtils::GetAppID
    //  SpawnProcess rewrites pGameID to 480 for OnlineFix games,
    //  so steamclient returns 480.  Restore the real app_id.
    //  GetAppID reads and updates the response steamclient pre-filled.
    void HandlerPost_IClientUtils_GetAppID(CPipeClient* pipe, CUtlBuffer* pRead, CUtlBuffer* pWrite)
    {
        AppId_t realAppId = Hooks_Misc::ResolveAppId();
        if (!realAppId) return;

        GetAppIDResp resp{pWrite};
        if (!resp.ok()) return;

        // Read what steamclient just wrote, decide whether to spoof.
        const AppId_t current = resp.returnValue();
        if (current == realAppId) return;
        resp.set_returnValue(realAppId);
        LOG_IPC_INFO("GetAppID: spoof response {} -> {}", current, realAppId);
    }

    // ════════════════════════════════════════════════════════════════
    //  GetAPICallResult per-callback handlers
    // ════════════════════════════════════════════════════════════════

    bool HandleCallback_EncryptedAppTicketResponse(CPipeClient* /*pipe*/, CUtlBuffer* pWrite, uint64 hAsyncCall, uint32 cubCallback)
    {
        const auto appId = PendingAPICalls::TakeEncryptedTicket(hAsyncCall);
        if (!appId) return false;

        EncryptedAppTicketResponse_t callback{};
        callback.m_eResult = k_EResultOK;
        if (!WriteAPICallResult(pWrite, cubCallback, callback)) {
            PendingAPICalls::RecordEncryptedTicket(hAsyncCall, *appId);
            LOG_IPC_WARN("Failed to write EncryptedAppTicketResponse for AppId={} hAsyncCall=0x{:X}", 
                            *appId, hAsyncCall);
            return false;
        }
        LOG_IPC_DEBUG("Set K_EResultOK for EncryptedAppTicketResponse callback, AppId={} hAsyncCall=0x{:X}",
                        *appId, hAsyncCall);
        return true;
    }

    template<typename CallbackT>
    bool RewriteOnlineFixStatsResult(
        CPipeClient* pipe, CUtlBuffer* pWrite, const char* name, uint32 cubCallback)
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
        CPipeClient* pipe, CUtlBuffer* pWrite, uint64, uint32 cubCallback)
    {
        return RewriteOnlineFixStatsResult<UserStatsReceived_t>(
            pipe, pWrite, "UserStatsReceived", cubCallback);
    }

    bool HandleCallback_GlobalAchievementPercentagesReady(
        CPipeClient* pipe, CUtlBuffer* pWrite, uint64, uint32 cubCallback)
    {
        return RewriteOnlineFixStatsResult<GlobalAchievementPercentagesReady_t>(
            pipe, pWrite, "GlobalAchievementPercentagesReady", cubCallback);
    }

    bool HandleCallback_GlobalStatsReceived(
        CPipeClient* pipe, CUtlBuffer* pWrite, uint64, uint32 cubCallback)
    {
        return RewriteOnlineFixStatsResult<GlobalStatsReceived_t>(
            pipe, pWrite, "GlobalStatsReceived", cubCallback);
    }

    struct APICallResultHandlerEntry {
        uint32 callbackId;
        bool (*handler)(CPipeClient* pipe, CUtlBuffer* pWrite, uint64 hAsyncCall, uint32 cubCallback);
    };

    constexpr APICallResultHandlerEntry kAPICallResultHandlers[] = {
        { EncryptedAppTicketResponse_t::k_iCallback, HandleCallback_EncryptedAppTicketResponse },
        { UserStatsReceived_t::k_iCallback, HandleCallback_UserStatsReceived },
        { GlobalAchievementPercentagesReady_t::k_iCallback,
          HandleCallback_GlobalAchievementPercentagesReady },
        { GlobalStatsReceived_t::k_iCallback, HandleCallback_GlobalStatsReceived },
    };

    // [Post-Handler]: IClientUtils::GetAPICallResult
    void HandlerPost_IClientUtils_GetAPICallResult(CPipeClient* pipe, CUtlBuffer* pRead, CUtlBuffer* pWrite)
    {
        GetAPICallResultReq req{pRead};
        if (!req.ok()) return;

        AppId_t appId = Hooks_Misc::ResolveAppId();
        LOG_IPC_DEBUG("{}, AppId={}", req.DebugString(),appId);
        for (const auto& entry : kAPICallResultHandlers) {
            if (entry.callbackId == req.iCallbackExpected()) {
                entry.handler(pipe, pWrite, req.hSteamAPICall(), req.cubCallback());
                return;
            }
        }
    }

} // namespace

namespace Hooks_IPC_ISteamUtils {
    void Register() {
        IPCHandlerEntry UtilsEntries[] = {
            ADD_IPC_POST_HANDLER(IClientUtils, GetAppID),
            ADD_IPC_POST_HANDLER(IClientUtils, GetAPICallResult),
        };
        Hooks_IPC::RegisterHandlers(UtilsEntries);
    }
}
