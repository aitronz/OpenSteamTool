#pragma once

// ── ISteamUser callbacks (base = 100) ───────────────────────────────

constexpr int k_iSteamUserCallbacks = 100;
constexpr int k_iSteamUserStatsCallbacks = 1100;
constexpr int k_cchStatNameMax = 128;

//-----------------------------------------------------------------------------
// Purpose: Result from RequestEncryptedAppTicket (async)
//-----------------------------------------------------------------------------
struct EncryptedAppTicketResponse_t
{
	enum { k_iCallback = k_iSteamUserCallbacks + 54 };

	EResult m_eResult;
};

//-----------------------------------------------------------------------------
// Purpose: User stats callbacks carry the app identity back to the game.
//-----------------------------------------------------------------------------
struct UserStatsReceived_t
{
	enum { k_iCallback = k_iSteamUserStatsCallbacks + 1 };

	uint64  m_nGameID;
	EResult m_eResult;
	uint32  m_unPadding;
	uint64  m_steamIDUser;
};
static_assert(sizeof(UserStatsReceived_t) == 0x18,
              "UserStatsReceived_t must be 0x18 bytes");

struct UserStatsStored_t
{
	enum { k_iCallback = k_iSteamUserStatsCallbacks + 2 };

	uint64  m_nGameID;
	EResult m_eResult;
};

struct UserAchievementStored_t
{
	enum { k_iCallback = k_iSteamUserStatsCallbacks + 3 };

	uint64 m_nGameID;
	bool   m_bGroupAchievement;
	char   m_rgchAchievementName[k_cchStatNameMax];
	uint32 m_nCurProgress;
	uint32 m_nMaxProgress;
};

struct UserAchievementIconFetched_t
{
	enum { k_iCallback = k_iSteamUserStatsCallbacks + 9 };

	uint64 m_nGameID;
	char   m_rgchAchievementName[k_cchStatNameMax];
	bool   m_bAchieved;
	int32  m_nIconHandle;
};

struct GlobalAchievementPercentagesReady_t
{
	enum { k_iCallback = k_iSteamUserStatsCallbacks + 10 };

	uint64  m_nGameID;
	EResult m_eResult;
};

struct GlobalStatsReceived_t
{
	enum { k_iCallback = k_iSteamUserStatsCallbacks + 12 };

	uint64  m_nGameID;
	EResult m_eResult;
};

//-----------------------------------------------------------------------------
// Purpose: Broadcast when app licenses change (additions / removals / reload).
//          Sent by CClientAppManager after ProcessPendingLicenseUpdates.
//          Size: 0x118 (280 bytes).
//-----------------------------------------------------------------------------
struct AppLicensesChanged_t
{
	enum { k_iCallback = 1020094 };

	bool      m_bReloadAll;                // 0x00  — true = full library refresh
	bool      m_bIsFirstLoad;              // 0x01
	uint32    m_unRemainingPackets;         // 0x04
	uint32    m_unCount;                    // 0x08  — number of entries in m_rgAppsUpdated
	AppId_t   m_rgAppsUpdated[64];         // 0x0C  — batch of updated AppIds
	uint64    m_unAppsAdded;               // 0x110 — bitmask: bit N = m_rgAppsUpdated[N] was added
};
static_assert(sizeof(AppLicensesChanged_t) == 0x118,
              "AppLicensesChanged_t must be 0x118 bytes");
