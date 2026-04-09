#ifndef GAME_SERVER_GAMEMODES_OUTLIER_OUTLIER_H
#define GAME_SERVER_GAMEMODES_OUTLIER_OUTLIER_H

#include <insta/server/gamemodes/base_pvp/base_pvp.h>

#include <array>
#include <string>

class CGameControllerOutlier : public CGameControllerBasePvp
{
public:
	CGameControllerOutlier(CGameContext *pGameServer);
	~CGameControllerOutlier() override;

	void Tick() override;
	void OnRoundStart() override;
	void OnRoundEnd() override;
	void OnCharacterSpawn(class CCharacter *pChr) override;
	void OnPlayerConnect(class CPlayer *pPlayer) override;
	bool OnFireWeapon(CCharacter &Character, int &Weapon, vec2 &Direction, vec2 &MouseTarget, vec2 &ProjStartPos) override;
	void DoTeamChange(CPlayer *pPlayer, int Team, bool DoChatMsg = true) override;
	bool CanJoinTeam(int Team, int NotThisId, char *pErrorReason, int ErrorReasonSize) override;
	bool OnCharacterTakeDamage(vec2 &Force, int &Dmg, int &From, int &Weapon, CCharacter &Character) override;
	bool OnChatMessage(const CNetMsg_Cl_Say *pMsg, int Length, int &Team, CPlayer *pPlayer) override;
	bool DoWincheckRound() override;
	int OnCharacterDeath(class CCharacter *pVictim, class CPlayer *pKiller, int Weapon) override;
	bool OnChangeInfoNetMessage(const CNetMsg_Cl_ChangeInfo *pMsg, int ClientId) override;
	bool OnSkinChange7(protocol7::CNetMsg_Cl_SkinChange *pMsg, int ClientId) override;
	int SnapPlayerScore(int SnappingClient, CPlayer *pPlayer) override;
	int SnapPlayerLatency(int SnappingClient, CPlayer *pPlayer, int DefaultLatency) override;

private:
	enum class ERole
	{
		NONE,
		TAGGER,
		HIDER_REAL,
		HIDER_FAKE,
	};

	struct SBotBehavior
	{
		int m_NextSegmentTick = 0;
		int m_Direction = 0;
		int m_HookTicksLeft = 0;
		int m_JumpTicksLeft = 0;
		int m_SecondJumpDelayTicks = 0;
		float m_CurrentAngle = 0.0f;
		float m_TargetAngle = 0.0f;
	};

	int m_RoundInitTick = -1;
	int m_HidePhaseEndTick = -1;
	int m_ActivePhaseEndTick = -1;
	int m_RestartRoundTick = -1;
	int m_LastCountdownSecond = -1;
	int m_LastSkinEnforceSecond = -1;
	int m_PrevRandomClientSlots = 0;
	bool m_RolesAssigned = false;
	bool m_RoundResolved = false;
	std::array<ERole, MAX_CLIENTS> m_aRoles{};
	std::array<bool, MAX_CLIENTS> m_aEliminated{};
	std::array<bool, MAX_CLIENTS> m_aHammerUsedThisRound{};
	std::array<bool, MAX_CLIENTS> m_aIdentityAppliedThisRound{};
	std::array<bool, MAX_CLIENTS> m_aHasOriginalName{};
	std::array<std::string, MAX_CLIENTS> m_aOriginalNames;
	std::array<SBotBehavior, MAX_CLIENTS> m_aBotBehavior{};
	std::array<bool, MAX_CLIENTS> m_aBotBehaviorInit{};

	void ResetRoundData();
	void InitializeRound();
	void AssignRoles();
	void BroadcastCountdown();
	void ReconnectClientsForRoundShuffle();
	void EnforceDefaultSkins();
	void UpdateBotPopulation();
	void TickBotBehavior();
	void EnsureIdentityApplied();
	bool IsDebugDummyClient(int ClientId) const;
	void ApplyRoleAppearance(int ClientId);
	void ApplyDefaultAppearance(int ClientId);
	void ApplyTaggerAppearance(int ClientId);
	void SpawnHammerSmoke(const vec2 &Pos);
	void RandomizeName(int ClientId, bool IsBot);
	const char *ChatNameForClient(int ClientId) const;
	std::string GenerateRandomName(bool IsBot) const;
	int CountAlive(ERole Role) const;
};

#endif
