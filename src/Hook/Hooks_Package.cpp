#include "Hooks_Package.h"
#include "HookMacros.h"
#include "Hooks_Misc.h"
#include "dllmain.h"

namespace {
    using CUtlMemoryGrow_t = void* (*)(CUtlVector<AppId_t>* pVec, int grow_size);
    CUtlMemoryGrow_t oCUtlMemoryGrow = nullptr;

    void RewriteOnlineFixStatsCallback(HSteamPipe hSteamPipe, const char* name,
                                       uint64& gameId) {
        const uint64 previous = gameId;
        if (Hooks_Misc::RewriteOnlineFixUserStatsCallback(hSteamPipe, gameId)) {
            LOG_ACHIEVEMENT_TRACE("SendCallbackToPipe: OnlineFix {} CGameID {:#x} -> {:#x}",
                                  name, previous, gameId);
        }
    }

    HOOK_FUNC(LoadPackage, bool, PackageInfo* pInfo, uint8* sha1, int32 cn, void* p4) {
        bool result = oLoadPackage(pInfo, sha1, cn, p4);

        if (pInfo->PackageId == 0) {
            std::vector<AppId_t> appIds = LuaConfig::GetAllDepotIds();
            if (!appIds.empty()) {
                uint32 oldSize = pInfo->AppIdVec.m_Size;
                uint32 numToAdd = static_cast<uint32>(appIds.size());
                LOG_PACKAGE_INFO("LoadPackage(PackageId=0): adding {} apps, oldSize={}", numToAdd, oldSize);
                oCUtlMemoryGrow(&pInfo->AppIdVec, numToAdd);
                for (uint32 i = 0; i < numToAdd; i++)
                    pInfo->AppIdVec.m_Memory.m_pMemory[oldSize + i] = appIds[i];
            }
        }

        return result;
    }

    HOOK_FUNC(CheckAppOwnership, bool, void* pObj, AppId_t appId, AppOwnership* pOwn) {
        bool result = oCheckAppOwnership(pObj, appId, pOwn);
        // LOG_PACKAGE_TRACE("CheckAppOwnership: AppId={} result={} {}", appId, result, pOwn->DebugString());
        if (LuaConfig::HasDepot(appId)) {
            if (result && pOwn->ExistInPackageNums > 1) {
                // Actually owned — record so HasDepot excludes it going forward
                LuaConfig::MarkOwned(appId);
            } else {
                pOwn->PackageId    = 0;
                pOwn->ReleaseState = EAppReleaseState::Released;
                // Setting this free flag to false will hide it from the library UI.
                pOwn->bFreeLicense = false;
                return true;
            }
        }
        return result;
    }

    // Steamclient routes ordinary callbacks only to registrations for appId.
    // OnlineFix games register under public app 480 while their stats execute
    // under the real app ID, so route the result through their registration.
    HOOK_FUNC(DispatchCallbackByAppId, bool, void* pBaseUser, AppId_t appId,
              int iCallback, void* pCallbackData, int cubCallbackData) {
        constexpr uint64 appIdMask = 0xFFFFFF;
        const auto* stats = static_cast<const UserStatsReceived_t*>(pCallbackData);
        const bool routeOnlineFixStats =
            iCallback == UserStatsReceived_t::k_iCallback
            && pCallbackData
            && cubCallbackData >= sizeof(UserStatsReceived_t)
            && Hooks_Misc::ShouldRouteOnlineFixUserStatsCallback(appId)
            && static_cast<AppId_t>(stats->m_nGameID & appIdMask) == appId;

        UserStatsReceived_t publicResult{};
        if (routeOnlineFixStats)
            publicResult = *stats;

        const bool result = oDispatchCallbackByAppId(
            pBaseUser, appId, iCallback, pCallbackData, cubCallbackData);
        if (!routeOnlineFixStats)
            return result;

        const bool publicResultSent = oDispatchCallbackByAppId(
            pBaseUser, kOnlineFixAppId, iCallback,
            &publicResult, sizeof(publicResult));
        LOG_ACHIEVEMENT_TRACE(
            "DispatchCallbackByAppId: OnlineFix UserStatsReceived route AppId {} -> {} result={} delivered={}",
            appId, kOnlineFixAppId, static_cast<int>(publicResult.m_eResult), publicResultSent);
        return result || publicResultSent;
    }

    HOOK_FUNC(SendCallbackToPipe, bool, void* pSteamEngine, HSteamPipe hSteamPipe,
              HSteamUser iClientUser, int iCallback, void* pCallbackData, int cubCallbackData) {
        // ── Callback modifier dispatch ─────────────────────────────────────────
        // Intercept callbacks before they reach the pipe and modify data in-place.
        // To add a new callback: add an else-if branch here.
        if (iCallback == AppLicensesChanged_t::k_iCallback) {
            auto* p = static_cast<AppLicensesChanged_t*>(pCallbackData);
            LOG_PACKAGE_DEBUG("SendCallbackToPipe: AppLicensesChanged m_bReloadAll={} -> true",
                           p->m_bReloadAll);
            p->m_bReloadAll = true;
        } else if (iCallback == UserStatsReceived_t::k_iCallback
                   && cubCallbackData >= sizeof(uint64)) {
            auto* p = static_cast<UserStatsReceived_t*>(pCallbackData);
            RewriteOnlineFixStatsCallback(hSteamPipe, "UserStatsReceived", p->m_nGameID);
        } else if (iCallback == UserStatsStored_t::k_iCallback
                   && cubCallbackData >= sizeof(uint64)) {
            auto* p = static_cast<UserStatsStored_t*>(pCallbackData);
            RewriteOnlineFixStatsCallback(hSteamPipe, "UserStatsStored", p->m_nGameID);
        } else if (iCallback == UserAchievementStored_t::k_iCallback
                   && cubCallbackData >= sizeof(uint64)) {
            auto* p = static_cast<UserAchievementStored_t*>(pCallbackData);
            RewriteOnlineFixStatsCallback(hSteamPipe, "UserAchievementStored", p->m_nGameID);
        } else if (iCallback == UserAchievementIconFetched_t::k_iCallback
                   && cubCallbackData >= sizeof(uint64)) {
            auto* p = static_cast<UserAchievementIconFetched_t*>(pCallbackData);
            RewriteOnlineFixStatsCallback(hSteamPipe, "UserAchievementIconFetched", p->m_nGameID);
        }

        return oSendCallbackToPipe(pSteamEngine, hSteamPipe, iClientUser,
                                   iCallback, pCallbackData, cubCallbackData);
    }
}

namespace Hooks_Package {
    void Install() {
        RESOLVE_D(CUtlMemoryGrow);

        HOOK_BEGIN();
        INSTALL_HOOK_D(LoadPackage);
        INSTALL_HOOK_D(CheckAppOwnership);
        INSTALL_HOOK_D(DispatchCallbackByAppId);
        INSTALL_HOOK_D(SendCallbackToPipe);
        HOOK_END();
    }

    void Uninstall() {
        UNHOOK_BEGIN();
        UNINSTALL_HOOK(LoadPackage);
        UNINSTALL_HOOK(CheckAppOwnership);
        UNINSTALL_HOOK(DispatchCallbackByAppId);
        UNINSTALL_HOOK(SendCallbackToPipe);
        UNHOOK_END();
        oCUtlMemoryGrow = nullptr;
    }
}
