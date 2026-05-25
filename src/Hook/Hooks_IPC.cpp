#include "Hooks_IPC.h"
#include "Hooks_IPC_ISteamUser.h"
#include "Hooks_IPC_ISteamUtils.h"
#include "HookMacros.h"
#include "dllmain.h"
#include "Utils/Hash.h"
#include "Hooks_Misc.h"

namespace {

    using GetPipeClient_t = CSteamPipeClient*(*)(void* pEngine, HSteamPipe hSteamPipe);
    GetPipeClient_t oGetPipeClient = nullptr;

    static CSteamPipeClient* GetPipe(void* pServer, HSteamPipe hSteamPipe) {
        return oGetPipeClient ? oGetPipeClient(pServer, hSteamPipe) : nullptr;
    }

    // ════════════════════════════════════════════════════════════════
    //  Handler registry
    // ════════════════════════════════════════════════════════════════
    using namespace Hooks_IPC;

    std::vector<IpcHandlerEntry> g_Handlers;

    static const IpcHandlerEntry* FindHandler(EIPCInterface iface, uint32 funcHash) {
        for (auto& e : g_Handlers) {
            if (e.interfaceID == iface && e.funcHash == funcHash) return &e;
        }
        return nullptr;
    }

    // ════════════════════════════════════════════════════════════════
    //  Main hook
    // ════════════════════════════════════════════════════════════════
    HOOK_FUNC(IPCProcessMessage, bool,
              void* pServer, HSteamPipe hSteamPipe,
              CUtlBuffer* pRead, CUtlBuffer* pWrite)
    {
        auto* pipe = GetPipe(pServer, hSteamPipe);

        // ── Parse header, find handler ──────────────────────────
        const IpcHandlerEntry* entry = nullptr;
        bool userStatsCall = false;

        if (pRead->TellPut() >= IPC_HEADER_SIZE) {
            const uint8* data = pRead->Base();
            const auto cmd = static_cast<EIPCCommand>(data[OFFSET_CMD]);

            if (cmd == EIPCCommand::Handshake) {
                LOG_IPC_INFO("[Handshake]: {}", pipe->DebugString());
            } else if (cmd == EIPCCommand::InterfaceCall) {
                // exclude InterfaceCall from steam
                if ((pipe->m_hSteamPipe & 0xFFFF) <= 2) {
                    LOG_IPC_TRACE("[InterfaceCall] from steam, pipe=0x{:08X} skip handler", pipe->m_hSteamPipe);
                    return oIPCProcessMessage(pServer, hSteamPipe, pRead, pWrite);
                }
                const auto iface = static_cast<EIPCInterface>(data[OFFSET_INTERFACE_ID]);
                const uint32 funcHash = *reinterpret_cast<const uint32*>(data + OFFSET_FUNC_HASH);
                userStatsCall = iface == EIPCInterface::IClientUserStats;
                entry = FindHandler(iface, funcHash);
                if (entry) {
                    LOG_IPC_DEBUG("[InterfaceCall] {} {} realAppId={},AppId={}",
                                  entry->name, pipe->DebugString(),
                                  Hooks_Misc::ResolveAppId(),
                                  Hooks_Misc::GetAppIDForCurrentPipe()
                                );
                } else {
                    LOG_IPC_TRACE("[InterfaceCall(unhandled)]{}::0x{:08X} {} realAppId={},AppId={}",
                                  EIPCInterfaceName(iface), funcHash,
                                  pipe->DebugString(),
                                  Hooks_Misc::ResolveAppId(),
                                  Hooks_Misc::GetAppIDForCurrentPipe()
                                );
                }
            } else {
                LOG_IPC_TRACE("[{}] {}", EIPCCommandName(cmd), pipe->DebugString());
            }
        }

        // ── Run original ────────────────────────────────────────
        if (userStatsCall)
            Hooks_Misc::SetUserStatsContext(hSteamPipe, true);
        const bool result = oIPCProcessMessage(pServer, hSteamPipe, pRead, pWrite);
        if (userStatsCall)
            Hooks_Misc::SetUserStatsContext(hSteamPipe, false);
        if (!result || !entry) return result;

        // Only run handlers for apps with configured depots.
        AppId_t appId = Hooks_Misc::ResolveAppId();
        if (!LuaConfig::HasDepot(appId)) {
            LOG_IPC_TRACE("{}: appId={} has no configured depot, skip handler {}",
                entry->name, appId, pipe->DebugString());
            return result;
        }

        entry->handler(pipe, pRead, pWrite);
        return result;
    }

} // namespace


namespace Hooks_IPC {

    void RegisterHandlers(const IpcHandlerEntry* entries, size_t count) {
        g_Handlers.insert(g_Handlers.end(), entries, entries + count);
    }

    void Install() {
        RESOLVE_D(GetPipeClient);

        // Interface modules register their handlers here.
        Hooks_IPC_ISteamUser::Register();
        Hooks_IPC_ISteamUtils::Register();

        HOOK_BEGIN();
        INSTALL_HOOK_D(IPCProcessMessage);
        HOOK_END();
    }

    void Uninstall() {
        UNHOOK_BEGIN();
        UNINSTALL_HOOK(IPCProcessMessage);
        UNHOOK_END();
        oGetPipeClient = nullptr;
        g_Handlers.clear();
    }

}
