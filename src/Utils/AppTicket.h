#pragma once

#include "dllmain.h"

namespace AppTicket {
    inline constexpr uint32 kAppTicketSteamIdOffset = 8;
    inline constexpr uint32 kAppTicketAppIdOffset = 16;
    inline constexpr uint32 kAppTicketSignatureSize = 128;

    struct AppOwnershipTicket {
        std::vector<uint8_t> data;
        uint32 totalSize = 0;
        uint32 appIdOffset = kAppTicketAppIdOffset;
        uint32 steamIdOffset = kAppTicketSteamIdOffset;
        uint32 signatureOffset = 0;
        uint32 signatureSize = kAppTicketSignatureSize;
    };

    // Reads the app ownership ticket cached by Steam under
    //   HKCU\Software\Valve\Steam\Apps\<AppId>\AppTicket  (REG_BINARY)
    // Returns an empty vector when no ticket is available.
    std::vector<uint8_t> GetAppOwnershipTicketFromRegistry(AppId_t appId);

    bool GetAppOwnershipTicket(AppId_t appId, AppOwnershipTicket& ticket);

    // Reads the encrypted app ticket cached by Steam under
    //   HKCU\Software\Valve\Steam\Apps\<AppId>\ETicket  (REG_BINARY)
    // Returns an empty vector when no ticket is available.
    std::vector<uint8_t> GetEncryptedTicketFromRegistry(AppId_t appId);

    //Get spoof steamID From the cached AppOwnershipTicket for the given AppId.
    uint64_t GetSpoofSteamID(AppId_t appId);

    // Write AppTicket binary data to registry.
    bool WriteAppOwnershipTicket(AppId_t appId, const std::vector<uint8_t>& data);

    // Write ETicket binary data to registry.
    bool WriteEncryptedTicket(AppId_t appId, const std::vector<uint8_t>& data);
}
