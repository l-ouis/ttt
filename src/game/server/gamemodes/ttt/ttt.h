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
	void SendDeathInfoMessage(CCharacter *pVictim, int Killer, int Weapon, int ModeSpecial) override;
	bool SkipDamage(int Dmg, int From, int Weapon, const CCharacter *pCharacter, bool &ApplyForce) override;
	bool OnEntity(int Index, int x, int y, int Layer, int Flags, bool Initial, int Number = 0) override;
	int OnCharacterDeath(class CCharacter *pVictim, CPlayer *pKiller, int Weapon) override;
	bool IsGracePeriodActive() const;
	bool CanPickUpWeaponDuringGrace(int ClientId) const;
	void CountGraceWeaponPickup(int ClientId);

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
	int m_DetectiveLaserCooldownEndTick = -1;
	int m_PostWinGraceEndTick = -1;
	int m_TraitorTesterClientId = -1;
	int m_TraitorTesterStartTick = -1;
	bool m_TraitorTesterResolved = false;
	bool m_PostWinGraceActive = false;
	ERole m_PostWinRole = ERole::NONE;
	bool m_RolesAssigned = false;
	std::array<ERole, MAX_CLIENTS> m_aRoles{};
	std::array<bool, MAX_CLIENTS> m_aGraceAutoJoinOptOut{};
	std::array<bool, MAX_CLIENTS> m_aForcedToSpectatorsDuringRound{};
	std::array<int, MAX_CLIENTS> m_aGraceWeaponPickupCount{};
	std::array<int, MAX_CLIENTS> m_aPlayerEnergy{};
	std::array<int, MAX_CLIENTS> m_aLastBroadcastedEnergy{};
	std::array<std::deque<vec2>, MAX_CLIENTS> m_aPlayerEnergyTrail{};
	std::vector<vec2> m_vTraitorTesterRevealPositions;
	std::vector<vec2> m_vLighthousePositions;
	std::vector<std::string> m_vRoundStartInnocentNames;
	std::vector<std::string> m_vRoundStartTerroristNames;
	int m_LastEnergySpawnSecond = -1;
	bool m_DebugEnergySpawnAnnounced = false;
	bool m_DebugShieldSpawnAnnounced = false;
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

	struct SShieldSpawnTile
	{
		int m_MapIndex = -1;
		vec2 m_Pos{};
		bool m_HasShield = false;
	};
	std::vector<SShieldSpawnTile> m_vShieldSpawnTiles;
	std::unordered_map<int, int> m_ShieldSpawnTileByMapIndex;

	struct SBeacon
	{
		int m_MapIndex = -1;
		vec2 m_Pos{};
		int m_Energy = 0;
		bool m_Broken = false;
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
	std::array<int, MAX_CLIENTS> m_aLastTraitorTesterDepositSecond{};

	struct STravelingTraitorTesterEnergy
	{
		vec2 m_StartPos{};
		vec2 m_TargetPos{};
		int m_TraitorTesterIndex = -1;
		int m_StartTick = -1;
		int m_EndTick = -1;
	};
	std::vector<STravelingTraitorTesterEnergy> m_vTravelingTraitorTesterEnergies;

	struct STraitorTester
	{
		vec2 m_Pos{};
		vec2 m_EnergyDisplayTopLeft{};
		int m_Energy = 0;
	};
	std::vector<STraitorTester> m_vTraitorTesters;
	bool m_LighthouseLowEnergyAnnounced = false;
	bool m_DreadfulMode = false;
	int m_LastDreadDamageSecond = -1;

	bool IsOnWaitingMap() const;
	int NumPlayersReadyForStart() const;
	const char *PickRandomRoundMap() const;
	void ResetRoles();
	void AssignRoles();
	int CountAliveInRole(ERole Role) const;
	int CountAliveInInnocentSide() const;
	void AnnounceWinningRole(ERole WinningRole);
	void RefreshTraitorTesterRevealPositions();
	void RefreshLighthousePositions();
	void TickTraitorTester();
	void TickTraitorTesterEnergy();
	void TickLighthouse();
	bool IsCharacterInsideTraitorTester(const CCharacter *pChr) const;
	void TriggerTraitorTesterReveal(int ClientId);
	bool GiveUniqueLaserTo(int ClientId);
	void TickUniqueLaser();
	void DropUniqueLaser(vec2 Pos);
	void ResetEnergyState();
	void TickEnergySpawns();
	void TickEnergyPickups();
	void TickShieldSpawns();
	void TickShieldPickups();
	void TickEnergyTrails();
	void SnapEnergy(int SnappingClient);
	void RebuildEnergySpawnTilesFromMap();
	void RebuildShieldSpawnTilesFromMap();
	void RefreshBeaconPositions();
	void TickBeacons();
	void TickBrokenBeaconEffects();
	void SnapBeacons(int SnappingClient);
	void SnapTraitorTesterEnergy(int SnappingClient);
	void SnapLighthouse(int SnappingClient);
};
#endif
