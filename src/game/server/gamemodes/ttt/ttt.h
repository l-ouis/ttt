#ifndef GAME_SERVER_GAMEMODES_TTT_TTT_H
#define GAME_SERVER_GAMEMODES_TTT_TTT_H

#include <insta/server/gamemodes/base_pvp/base_pvp.h>

#include <array>
#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

class CGameControllerTtt : public CGameControllerBasePvp
{
public:
	CGameControllerTtt(CGameContext *pGameServer);
	~CGameControllerTtt() override;

	void OnInit() override;
	void Tick() override;
	void Snap(int SnappingClient) override;
	void OnCharacterSpawn(class CCharacter *pChr) override;
	void OnPlayerConnect(class CPlayer *pPlayer) override;
	void DoTeamChange(CPlayer *pPlayer, int Team, bool DoChatMsg = true) override;
	bool CanJoinTeam(int Team, int NotThisId, char *pErrorReason, int ErrorReasonSize) override;
	void AmmoRegen(CCharacter *pChr) override;
	bool OnCharacterTakeDamage(vec2 &Force, int &Dmg, int &From, int &Weapon, CCharacter &Character) override;
	bool DoWincheckRound() override;
	void OnRoundEnd() override;
	bool SkipDamage(int Dmg, int From, int Weapon, const CCharacter *pCharacter, bool &ApplyForce) override;
	bool OnEntity(int Index, int x, int y, int Layer, int Flags, bool Initial, int Number = 0) override;
	int OnCharacterDeath(class CCharacter *pVictim, CPlayer *pKiller, int Weapon) override;

private:
	enum class ERole
	{
		NONE,
		INNOCENT,
		DETECTIVE,
		TERRORIST,
	};

	int m_WaitingCountdownStartTick = -1;
	int m_LastWaitingCountdownSecond = -1;
	int m_ReturnToWaitingMapTick = -1;
	int m_ActiveMapStartTick = -1;
	int m_GracePeriodEndTick = -1;
	int m_LastGraceCountdownSecond = -1;
	int m_UniqueLaserOwnerCid = -1;
	int m_PostWinGraceEndTick = -1;
	int m_TraitorTesterClientId = -1;
	int m_TraitorTesterStartTick = -1;
	bool m_TraitorTesterResolved = false;
	bool m_PostWinGraceActive = false;
	ERole m_PostWinRole = ERole::NONE;
	bool m_RolesAssigned = false;
	std::array<ERole, MAX_CLIENTS> m_aRoles{};
	std::array<bool, MAX_CLIENTS> m_aGraceAutoJoinOptOut{};
	std::array<int, MAX_CLIENTS> m_aPlayerEnergy{};
	std::array<std::deque<vec2>, MAX_CLIENTS> m_aPlayerEnergyTrail{};
	std::vector<vec2> m_vTraitorTesterRevealPositions;
	std::vector<std::string> m_vRoundStartInnocentNames;
	std::vector<std::string> m_vRoundStartTerroristNames;
	int m_LastEnergySpawnSecond = -1;
	bool m_DebugEnergySpawnAnnounced = false;
	bool m_DebugEnergyNodeSummarySent = false;
	int m_DebugEnergyNodesGameLayer = 0;
	int m_DebugEnergyNodesFrontLayer = 0;
	int m_DebugEnergyNodesSwitchLayer = 0;

	struct SEnergySpawnTile
	{
		int m_MapIndex = -1;
		vec2 m_Pos{};
		bool m_HasEnergy = false;
	};
	std::vector<SEnergySpawnTile> m_vEnergySpawnTiles;
	std::unordered_map<int, int> m_EnergySpawnTileByMapIndex;

	struct SBeacon
	{
		int m_MapIndex = -1;
		vec2 m_Pos{};
		int m_Energy = 0;
	};
	std::vector<SBeacon> m_vBeacons;

	struct STravelingBeaconEnergy
	{
		vec2 m_StartPos{};
		vec2 m_TargetPos{};
		int m_BeaconIndex = -1;
		int m_StartTick = -1;
		int m_EndTick = -1;
	};
	std::vector<STravelingBeaconEnergy> m_vTravelingBeaconEnergies;
	std::array<int, MAX_CLIENTS> m_aLastBeaconDepositSecond{};
	int m_LastBeaconDecaySecond = -1;

	bool IsOnWaitingMap() const;
	int NumPlayersReadyForStart() const;
	const char *PickRandomRoundMap() const;
	void ResetRoles();
	void AssignRoles();
	int CountAliveInRole(ERole Role) const;
	int CountAliveInInnocentSide() const;
	void AnnounceWinningRole(ERole WinningRole);
	void RefreshTraitorTesterRevealPositions();
	void TickTraitorTester();
	bool IsCharacterInsideTraitorTester(const CCharacter *pChr) const;
	void TriggerTraitorTesterReveal(int ClientId);
	bool GiveUniqueLaserTo(int ClientId);
	void TickUniqueLaser();
	void DropUniqueLaser(vec2 Pos);
	void ResetEnergyState();
	void TickEnergySpawns();
	void TickEnergyPickups();
	void TickEnergyTrails();
	void SnapEnergy(int SnappingClient);
	void RebuildEnergySpawnTilesFromMap();
	void RefreshBeaconPositions();
	void TickBeacons();
	void SnapBeacons(int SnappingClient);
};
#endif
