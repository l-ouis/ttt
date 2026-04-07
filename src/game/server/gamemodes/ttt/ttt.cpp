#include "ttt.h"

#include <base/math.h>
#include <base/system.h>

#include <engine/server/server.h>
#include <engine/shared/config.h>

#include <generated/protocol.h>
#include <generated/protocol7.h>

#include <game/server/entities/character.h>
#include <game/server/entity.h>
#include <game/server/gamecontext.h>
#include <game/server/player.h>

#include <game/mapitems.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace
{
constexpr int PICKUP_PHYSICS_RADIUS = 14;
constexpr int ROLE_FLAG_ID_BASE = 2000;
constexpr int ENERGY_TILE_SNAP_ID_BASE = 6000;
constexpr int ENERGY_TRAIL_SNAP_ID_BASE = 9000;
constexpr int ENERGY_MAX_PER_PLAYER = 5;
constexpr int ENERGY_TRAIL_SPACING_TICKS = 6;
constexpr int ENERGY_TRAIL_MAX_HISTORY = 128;
constexpr int BEACON_RADIUS = 10 * 32;
constexpr int BEACON_MAX_ENERGY = 10;
constexpr int BEACON_RING_RADIUS = 3 * 32;
constexpr int ENERGY_BEACON_RING_SNAP_ID_BASE = 12000;
constexpr int ENERGY_BEACON_TRAVEL_SNAP_ID_BASE = 20000;
bool g_TttWaitingBootstrapHandled = false;

std::string TttNormalizeMapName(const char *pMapName)
{
	if(!pMapName)
		return "";

	std::string Name(pMapName);
	if(Name.size() >= 4 && str_comp_nocase(Name.c_str() + Name.size() - 4, ".map") == 0)
		Name.resize(Name.size() - 4);
	return Name;
}

int TttPickupAmmoForWeapon(int Weapon)
{
	switch(Weapon)
	{
	case WEAPON_GUN:
		return 5;
	case WEAPON_SHOTGUN:
		return 4;
	case WEAPON_GRENADE:
		return 3;
	default:
		return 0;
	}
}

int TttRandomPickupWeapon()
{
	static const int s_aTttWeapons[] = {
		WEAPON_GUN,
		WEAPON_SHOTGUN,
		WEAPON_GRENADE,
	};
	return s_aTttWeapons[secure_rand_below(std::size(s_aTttWeapons))];
}

class CTttWeaponPickup : public CEntity
{
public:
	static const int ms_CollisionExtraSize = 6;

	CTttWeaponPickup(CGameWorld *pGameWorld, vec2 Pos, int Weapon, int Ammo, int Layer, int Number, int Flags) :
		CEntity(pGameWorld, CGameWorld::ENTTYPE_PICKUP, Pos, PICKUP_PHYSICS_RADIUS)
	{
		m_Weapon = Weapon;
		m_Ammo = Ammo;
		m_Flags = Flags;
		m_Core = vec2(0.0f, 0.0f);
		m_Layer = Layer;
		m_Number = Number;
		m_InfiniteAmmo = false;
		GameWorld()->InsertEntity(this);
	}

	CTttWeaponPickup(CGameWorld *pGameWorld, vec2 Pos, int Weapon, int Layer, int Number, int Flags, bool InfiniteAmmo) :
		CEntity(pGameWorld, CGameWorld::ENTTYPE_PICKUP, Pos, PICKUP_PHYSICS_RADIUS)
	{
		m_Weapon = Weapon;
		m_Ammo = 0;
		m_Flags = Flags;
		m_Core = vec2(0.0f, 0.0f);
		m_Layer = Layer;
		m_Number = Number;
		m_InfiniteAmmo = InfiniteAmmo;
		GameWorld()->InsertEntity(this);
	}

	void Tick() override
	{
		Move();

		CEntity *apEnts[MAX_CLIENTS];
		const int Num = GameWorld()->FindEntities(m_Pos, GetProximityRadius() + ms_CollisionExtraSize, apEnts, MAX_CLIENTS, CGameWorld::ENTTYPE_CHARACTER);
		for(int i = 0; i < Num; ++i)
		{
			auto *pChr = static_cast<CCharacter *>(apEnts[i]);
			if(!pChr || !pChr->IsAlive())
				continue;

			if(m_Layer == LAYER_SWITCH && m_Number > 0 && !Switchers()[m_Number].m_aStatus[pChr->Team()])
				continue;

			const bool HadWeapon = pChr->GetWeaponGot(m_Weapon);
			if(m_InfiniteAmmo)
			{
				if(!HadWeapon)
					pChr->GiveWeapon(m_Weapon);
				pChr->SetWeaponAmmo(m_Weapon, -1);
			}
			else
			{
				int Ammo = pChr->GetWeaponAmmo(m_Weapon);
				if(!HadWeapon)
				{
					pChr->GiveWeapon(m_Weapon);
					Ammo = 0;
				}
				if(Ammo < 0)
					Ammo = 0;

				pChr->SetWeaponAmmo(m_Weapon, Ammo + m_Ammo);
			}

			if(m_Weapon == WEAPON_GRENADE)
				GameServer()->CreateSound(m_Pos, SOUND_PICKUP_GRENADE, pChr->TeamMask());
			else
				GameServer()->CreateSound(m_Pos, SOUND_PICKUP_SHOTGUN, pChr->TeamMask());

			if(pChr->GetPlayer())
				GameServer()->SendWeaponPickup(pChr->GetPlayer()->GetCid(), m_Weapon);

			m_MarkedForDestroy = true;
			return;
		}
	}

	void Snap(int SnappingClient) override
	{
		if(NetworkClipped(SnappingClient))
			return;

		const int SnappingClientVersion = GameServer()->GetClientVersion(SnappingClient);
		const bool Sixup = false;
		GameServer()->SnapPickup(CSnapContext(SnappingClientVersion, Sixup, SnappingClient), GetId(), m_Pos, POWERUP_WEAPON, m_Weapon, m_Number, m_Flags);
	}

private:
	void Move()
	{
		if(Server()->Tick() % (int)(Server()->TickSpeed() * 0.15f) == 0)
		{
			GameServer()->Collision()->MoverSpeed(m_Pos.x, m_Pos.y, &m_Core);
			m_Pos += m_Core;
		}
	}

	int m_Weapon{};
	int m_Ammo{};
	int m_Flags{};
	vec2 m_Core;
	bool m_InfiniteAmmo{};
};
} // namespace

bool CGameControllerTtt::GiveUniqueLaserTo(int ClientId)
{
	if(ClientId < 0 || ClientId >= MAX_CLIENTS)
		return false;

	for(CPlayer *pPlayer : GameServer()->m_apPlayers)
	{
		if(!pPlayer)
			continue;

		CCharacter *pChr = pPlayer->GetCharacter();
		if(!pChr || !pChr->IsAlive())
			continue;

		pChr->SetWeaponGot(WEAPON_LASER, false);
		pChr->SetWeaponAmmo(WEAPON_LASER, 0);
		if(pChr->GetActiveWeapon() == WEAPON_LASER)
		{
			pChr->SetActiveWeapon(WEAPON_HAMMER);
			pChr->SetLastWeapon(WEAPON_HAMMER);
		}
	}

	CPlayer *pPlayer = GameServer()->m_apPlayers[ClientId];
	if(!pPlayer)
		return false;
	CCharacter *pChr = pPlayer->GetCharacter();
	if(!pChr || !pChr->IsAlive())
		return false;

	pChr->SetWeaponGot(WEAPON_LASER, true);
	pChr->SetWeaponAmmo(WEAPON_LASER, -1);
	m_UniqueLaserOwnerCid = ClientId;
	return true;
}

void CGameControllerTtt::DropUniqueLaser(vec2 Pos)
{
	new CTttWeaponPickup(&GameServer()->m_World, Pos, WEAPON_LASER, LAYER_GAME, 0, 0, true);
	m_UniqueLaserOwnerCid = -1;
}

void CGameControllerTtt::TickUniqueLaser()
{
	if(IsOnWaitingMap() || !m_RolesAssigned)
	{
		m_UniqueLaserOwnerCid = -1;
		return;
	}

	if(m_UniqueLaserOwnerCid >= 0 && m_UniqueLaserOwnerCid < MAX_CLIENTS)
	{
		CPlayer *pOwner = GameServer()->m_apPlayers[m_UniqueLaserOwnerCid];
		if(pOwner)
		{
			CCharacter *pOwnerChr = pOwner->GetCharacter();
			if(pOwnerChr && pOwnerChr->IsAlive() && pOwnerChr->GetWeaponGot(WEAPON_LASER))
			{
				pOwnerChr->SetWeaponAmmo(WEAPON_LASER, -1);
				return;
			}
		}
	}

	int NewOwnerCid = -1;
	for(CPlayer *pPlayer : GameServer()->m_apPlayers)
	{
		if(!pPlayer)
			continue;
		if(!Server()->ClientIngame(pPlayer->GetCid()))
			continue;

		CCharacter *pChr = pPlayer->GetCharacter();
		if(!pChr || !pChr->IsAlive())
			continue;
		if(!pChr->GetWeaponGot(WEAPON_LASER))
			continue;

		if(NewOwnerCid == -1)
		{
			NewOwnerCid = pPlayer->GetCid();
			pChr->SetWeaponAmmo(WEAPON_LASER, -1);
		}
		else
		{
			pChr->SetWeaponGot(WEAPON_LASER, false);
			pChr->SetWeaponAmmo(WEAPON_LASER, 0);
			if(pChr->GetActiveWeapon() == WEAPON_LASER)
			{
				pChr->SetActiveWeapon(WEAPON_HAMMER);
				pChr->SetLastWeapon(WEAPON_HAMMER);
			}
		}
	}

	m_UniqueLaserOwnerCid = NewOwnerCid;
}

CGameControllerTtt::CGameControllerTtt(CGameContext *pGameServer) :
	CGameControllerBasePvp(pGameServer)
{
	// TTT must be FFA so roles remain hidden from scoreboard/team UI.
	m_GameFlags = 0;
	m_pGameType = "ttt";
	m_DefaultWeapon = WEAPON_HAMMER;
	m_IsVanillaGameType = true;
	
	m_pStatsTable = "ttt";
	m_pExtraColumns = nullptr; // new CTttColumns();
	m_pSqlStats->SetExtraColumns(m_pExtraColumns);
	m_pSqlStats->CreateTable(m_pStatsTable);
	ResetEnergyState();
}

CGameControllerTtt::~CGameControllerTtt() = default;

void CGameControllerTtt::RefreshTraitorTesterRevealPositions()
{
	m_vTraitorTesterRevealPositions.clear();

	CCollision *pCollision = GameServer()->Collision();
	if(!pCollision)
		return;

	const int Width = pCollision->GetWidth();
	const int Height = pCollision->GetHeight();
	if(Width <= 0 || Height <= 0)
		return;

	for(int y = 0; y < Height; ++y)
	{
		for(int x = 0; x < Width; ++x)
		{
			const int MapIndex = y * Width + x;
			if(pCollision->IsTune(MapIndex) != 2)
				continue;

			m_vTraitorTesterRevealPositions.emplace_back(x * 32.0f + 16.0f, y * 32.0f + 16.0f);
		}
	}
}

bool CGameControllerTtt::IsCharacterInsideTraitorTester(const CCharacter *pChr) const
{
	if(!pChr || !pChr->IsAlive())
		return false;

	const int MapIndex = GameServer()->Collision()->GetMapIndex(pChr->GetPos());
	return GameServer()->Collision()->IsTune(MapIndex) == 1;
}

void CGameControllerTtt::TriggerTraitorTesterReveal(int ClientId)
{
	if(!Server()->ClientIngame(ClientId))
		return;

	const ERole Role = m_aRoles[ClientId];
	const bool IsTraitor = Role == ERole::TERRORIST;

	for(const vec2 &Pos : m_vTraitorTesterRevealPositions)
	{
		if(IsTraitor)
			GameServer()->CreatePlayerSpawn(Pos);
		else
			GameServer()->CreateFinishEffect(Pos);
	}
}

void CGameControllerTtt::TickTraitorTester()
{
	if(IsOnWaitingMap() || !m_RolesAssigned)
	{
		m_TraitorTesterClientId = -1;
		m_TraitorTesterStartTick = -1;
		m_TraitorTesterResolved = false;
		return;
	}

	int CandidateClientId = -1;
	for(const CPlayer *pPlayer : GameServer()->m_apPlayers)
	{
		if(!pPlayer)
			continue;
		if(!Server()->ClientIngame(pPlayer->GetCid()))
			continue;
		if(pPlayer->GetTeam() == TEAM_SPECTATORS)
			continue;

		const CCharacter *pChr = pPlayer->GetCharacter();
		if(!IsCharacterInsideTraitorTester(pChr))
			continue;

		if(CandidateClientId != -1)
		{
			// Multiple tees in tester: reset progress.
			m_TraitorTesterClientId = -1;
			m_TraitorTesterStartTick = -1;
			m_TraitorTesterResolved = false;
			return;
		}

		CandidateClientId = pPlayer->GetCid();
	}

	if(CandidateClientId == -1)
	{
		m_TraitorTesterClientId = -1;
		m_TraitorTesterStartTick = -1;
		m_TraitorTesterResolved = false;
		return;
	}

	if(CandidateClientId != m_TraitorTesterClientId)
	{
		m_TraitorTesterClientId = CandidateClientId;
		m_TraitorTesterStartTick = Server()->Tick();
		m_TraitorTesterResolved = false;
		return;
	}

	if(m_TraitorTesterResolved)
		return;

	if(m_TraitorTesterStartTick == -1)
	{
		m_TraitorTesterStartTick = Server()->Tick();
		return;
	}

	const int NeededTicks = 3 * Server()->TickSpeed();
	if(Server()->Tick() - m_TraitorTesterStartTick < NeededTicks)
		return;

	TriggerTraitorTesterReveal(CandidateClientId);
	m_TraitorTesterResolved = true;
}

void CGameControllerTtt::ResetRoles()
{
	m_RolesAssigned = false;
	m_aRoles.fill(ERole::NONE);
	m_vRoundStartInnocentNames.clear();
	m_vRoundStartTerroristNames.clear();
	m_PostWinGraceActive = false;
	m_PostWinRole = ERole::NONE;
	m_PostWinGraceEndTick = -1;
}

void CGameControllerTtt::ResetEnergyState()
{
	m_LastEnergySpawnSecond = -1;
	m_DebugEnergySpawnAnnounced = false;
	m_DebugEnergyNodeSummarySent = false;
	m_DebugEnergyNodesGameLayer = 0;
	m_DebugEnergyNodesFrontLayer = 0;
	m_DebugEnergyNodesSwitchLayer = 0;
	m_aPlayerEnergy.fill(0);
	m_aLastBeaconDepositSecond.fill(-1);
	m_LastBeaconDecaySecond = -1;
	m_vTravelingBeaconEnergies.clear();
	for(auto &Beacon : m_vBeacons)
		Beacon.m_Energy = 0;
	for(auto &Trail : m_aPlayerEnergyTrail)
		Trail.clear();
	for(auto &Tile : m_vEnergySpawnTiles)
		Tile.m_HasEnergy = false;
}

void CGameControllerTtt::RefreshBeaconPositions()
{
	m_vBeacons.clear();

	CCollision *pCollision = GameServer()->Collision();
	if(!pCollision)
		return;

	const int Width = pCollision->GetWidth();
	const int Height = pCollision->GetHeight();
	if(Width <= 0 || Height <= 0)
		return;

	for(int y = 0; y < Height; y++)
	{
		for(int x = 0; x < Width; x++)
		{
			const int MapIndex = y * Width + x;
			if(pCollision->IsTune(MapIndex) != 3)
				continue;

			SBeacon Beacon;
			Beacon.m_MapIndex = MapIndex;
			Beacon.m_Pos = vec2(x * 32.0f + 16.0f, y * 32.0f + 16.0f);
			Beacon.m_Energy = 0;
			m_vBeacons.push_back(Beacon);
		}
	}
}

void CGameControllerTtt::TickBeacons()
{
	if(IsOnWaitingMap() || !m_RolesAssigned || m_PostWinGraceActive)
		return;
	if(m_vBeacons.empty())
		return;

	for(size_t i = 0; i < m_vTravelingBeaconEnergies.size();)
	{
		const STravelingBeaconEnergy &Travel = m_vTravelingBeaconEnergies[i];
		if(Server()->Tick() < Travel.m_EndTick)
		{
			i++;
			continue;
		}

		if(Travel.m_BeaconIndex >= 0 && Travel.m_BeaconIndex < (int)m_vBeacons.size())
		{
			SBeacon &Beacon = m_vBeacons[Travel.m_BeaconIndex];
			if(Beacon.m_Energy < BEACON_MAX_ENERGY)
				Beacon.m_Energy++;
		}

		m_vTravelingBeaconEnergies.erase(m_vTravelingBeaconEnergies.begin() + i);
	}

	if(Server()->Tick() % Server()->TickSpeed() != 0)
		return;

	const int CurrentSecond = Server()->Tick() / Server()->TickSpeed();
	if(CurrentSecond % 30 == 0 && CurrentSecond != m_LastBeaconDecaySecond)
	{
		for(SBeacon &Beacon : m_vBeacons)
			Beacon.m_Energy = maximum(0, Beacon.m_Energy - 1);
		m_LastBeaconDecaySecond = CurrentSecond;
	}

	for(CPlayer *pPlayer : GameServer()->m_apPlayers)
	{
		if(!pPlayer)
			continue;

		const int ClientId = pPlayer->GetCid();
		if(!Server()->ClientIngame(ClientId))
			continue;
		if(pPlayer->GetTeam() == TEAM_SPECTATORS)
			continue;
		if(m_aPlayerEnergy[ClientId] <= 0)
			continue;
		if(m_aLastBeaconDepositSecond[ClientId] == CurrentSecond)
			continue;

		CCharacter *pChr = pPlayer->GetCharacter();
		if(!pChr || !pChr->IsAlive())
			continue;

		const vec2 PlayerPos = pChr->GetPos();
		int ClosestBeaconIndex = -1;
		float ClosestDistance = 0.0f;
		for(int BeaconIndex = 0; BeaconIndex < (int)m_vBeacons.size(); BeaconIndex++)
		{
			const SBeacon &Beacon = m_vBeacons[BeaconIndex];
			if(Beacon.m_Energy >= BEACON_MAX_ENERGY)
				continue;

			const float Distance = distance(PlayerPos, Beacon.m_Pos);
			if(Distance > (float)BEACON_RADIUS)
				continue;
			if(ClosestBeaconIndex == -1 || Distance < ClosestDistance)
			{
				ClosestBeaconIndex = BeaconIndex;
				ClosestDistance = Distance;
			}
		}

		if(ClosestBeaconIndex == -1)
			continue;

		m_aPlayerEnergy[ClientId]--;
		m_aLastBeaconDepositSecond[ClientId] = CurrentSecond;

		STravelingBeaconEnergy Travel;
		Travel.m_StartPos = PlayerPos;
		Travel.m_TargetPos = m_vBeacons[ClosestBeaconIndex].m_Pos;
		Travel.m_BeaconIndex = ClosestBeaconIndex;
		Travel.m_StartTick = Server()->Tick();
		Travel.m_EndTick = Server()->Tick() + Server()->TickSpeed();
		m_vTravelingBeaconEnergies.push_back(Travel);
	}
}

void CGameControllerTtt::RebuildEnergySpawnTilesFromMap()
{
	m_vEnergySpawnTiles.clear();
	m_EnergySpawnTileByMapIndex.clear();
	m_DebugEnergyNodesGameLayer = 0;
	m_DebugEnergyNodesFrontLayer = 0;
	m_DebugEnergyNodesSwitchLayer = 0;

	CCollision *pCollision = GameServer()->Collision();
	if(!pCollision)
		return;

	const int Width = pCollision->GetWidth();
	const int Height = pCollision->GetHeight();
	if(Width <= 0 || Height <= 0)
		return;

	const CTile *pGame = pCollision->GameLayer();
	const CTile *pFront = pCollision->FrontLayer();
	const CSwitchTile *pSwitch = pCollision->SwitchLayer();

	const unsigned char LaserShieldIndex = ENTITY_OFFSET + ENTITY_ARMOR_LASER;
	for(int y = 0; y < Height; y++)
	{
		for(int x = 0; x < Width; x++)
		{
			const int MapIndex = y * Width + x;
			bool IsEnergyNode = false;

			if(pGame && pGame[MapIndex].m_Index == LaserShieldIndex)
			{
				m_DebugEnergyNodesGameLayer++;
				IsEnergyNode = true;
			}
			if(pFront && pFront[MapIndex].m_Index == LaserShieldIndex)
			{
				m_DebugEnergyNodesFrontLayer++;
				IsEnergyNode = true;
			}
			if(pSwitch && pSwitch[MapIndex].m_Type == LaserShieldIndex)
			{
				m_DebugEnergyNodesSwitchLayer++;
				IsEnergyNode = true;
			}

			if(!IsEnergyNode)
				continue;

			if(m_EnergySpawnTileByMapIndex.find(MapIndex) != m_EnergySpawnTileByMapIndex.end())
				continue;

			SEnergySpawnTile Tile;
			Tile.m_MapIndex = MapIndex;
			Tile.m_Pos = vec2(x * 32.0f + 16.0f, y * 32.0f + 16.0f);
			Tile.m_HasEnergy = false;
			m_EnergySpawnTileByMapIndex[MapIndex] = (int)m_vEnergySpawnTiles.size();
			m_vEnergySpawnTiles.push_back(Tile);
		}
	}
}

void CGameControllerTtt::TickEnergySpawns()
{
	if(IsOnWaitingMap() || !m_RolesAssigned || m_PostWinGraceActive)
		return;
	if(Server()->Tick() % Server()->TickSpeed() != 0)
		return;

	const int CurrentSecond = Server()->Tick() / Server()->TickSpeed();
	if(CurrentSecond == m_LastEnergySpawnSecond)
		return;
	m_LastEnergySpawnSecond = CurrentSecond;

	const int SpawnChance = g_Config.m_SvEnergySpawnChance;
	for(auto &Tile : m_vEnergySpawnTiles)
	{
		if(Tile.m_HasEnergy)
			continue;

		if((int)secure_rand_below(100u) < SpawnChance)
		{
			Tile.m_HasEnergy = true;
			if(!m_DebugEnergySpawnAnnounced)
			{
				GameServer()->SendChat(-1, TEAM_ALL, "TTT energy spawned");
				m_DebugEnergySpawnAnnounced = true;
			}
		}
	}
}

void CGameControllerTtt::TickEnergyPickups()
{
	if(IsOnWaitingMap() || !m_RolesAssigned || m_PostWinGraceActive)
		return;

	for(CPlayer *pPlayer : GameServer()->m_apPlayers)
	{
		if(!pPlayer)
			continue;
		const int ClientId = pPlayer->GetCid();
		if(!Server()->ClientIngame(ClientId))
			continue;
		if(pPlayer->GetTeam() == TEAM_SPECTATORS)
			continue;
		if(m_aPlayerEnergy[ClientId] >= ENERGY_MAX_PER_PLAYER)
			continue;

		CCharacter *pChr = pPlayer->GetCharacter();
		if(!pChr || !pChr->IsAlive())
			continue;

		const int MapIndex = GameServer()->Collision()->GetPureMapIndex(pChr->GetPos());
		auto It = m_EnergySpawnTileByMapIndex.find(MapIndex);
		if(It == m_EnergySpawnTileByMapIndex.end())
			continue;

		SEnergySpawnTile &Tile = m_vEnergySpawnTiles[It->second];
		if(!Tile.m_HasEnergy)
			continue;

		Tile.m_HasEnergy = false;
		m_aPlayerEnergy[ClientId] = minimum(ENERGY_MAX_PER_PLAYER, m_aPlayerEnergy[ClientId] + 1);
		GameServer()->CreateSound(pChr->GetPos(), SOUND_PICKUP_ARMOR, pChr->TeamMask());
		GameServer()->SendChatTarget(ClientId, "+1 energy");
	}
}

void CGameControllerTtt::TickEnergyTrails()
{
	for(int ClientId = 0; ClientId < MAX_CLIENTS; ClientId++)
	{
		if(m_aPlayerEnergy[ClientId] <= 0)
		{
			m_aPlayerEnergyTrail[ClientId].clear();
			continue;
		}

		CPlayer *pPlayer = GameServer()->m_apPlayers[ClientId];
		if(!pPlayer || !Server()->ClientIngame(ClientId) || pPlayer->GetTeam() == TEAM_SPECTATORS)
		{
			m_aPlayerEnergyTrail[ClientId].clear();
			continue;
		}

		CCharacter *pChr = pPlayer->GetCharacter();
		if(!pChr || !pChr->IsAlive())
		{
			m_aPlayerEnergyTrail[ClientId].clear();
			continue;
		}

		auto &Trail = m_aPlayerEnergyTrail[ClientId];
		Trail.push_front(pChr->GetPos());
		if((int)Trail.size() > ENERGY_TRAIL_MAX_HISTORY)
			Trail.pop_back();
	}
}

void CGameControllerTtt::SnapEnergy(int SnappingClient)
{
	const CSnapContext Context(GameServer()->GetClientVersion(SnappingClient), Server()->IsSixup(SnappingClient), SnappingClient);
	const auto SnapSquare = [&](int BaseSnapId, const vec2 &Center) {
		const float HalfSize = 4.0f;
		GameServer()->SnapLaserObject(Context, BaseSnapId + 0, Center + vec2(-HalfSize, -HalfSize), Center + vec2(HalfSize, -HalfSize), -1, -1, LASERTYPE_RIFLE, 0, 0);
		GameServer()->SnapLaserObject(Context, BaseSnapId + 1, Center + vec2(HalfSize, -HalfSize), Center + vec2(HalfSize, HalfSize), -1, -1, LASERTYPE_RIFLE, 0, 0);
		GameServer()->SnapLaserObject(Context, BaseSnapId + 2, Center + vec2(HalfSize, HalfSize), Center + vec2(-HalfSize, HalfSize), -1, -1, LASERTYPE_RIFLE, 0, 0);
		GameServer()->SnapLaserObject(Context, BaseSnapId + 3, Center + vec2(-HalfSize, HalfSize), Center + vec2(-HalfSize, -HalfSize), -1, -1, LASERTYPE_RIFLE, 0, 0);
	};

	for(size_t i = 0; i < m_vEnergySpawnTiles.size(); i++)
	{
		const SEnergySpawnTile &Tile = m_vEnergySpawnTiles[i];
		if(!Tile.m_HasEnergy)
			continue;

		SnapSquare(ENERGY_TILE_SNAP_ID_BASE + (int)i * 4, Tile.m_Pos);
	}

	for(int ClientId = 0; ClientId < MAX_CLIENTS; ClientId++)
	{
		const int EnergyCount = minimum(ENERGY_MAX_PER_PLAYER, m_aPlayerEnergy[ClientId]);
		if(EnergyCount <= 0)
			continue;

		const auto &Trail = m_aPlayerEnergyTrail[ClientId];
		if(Trail.empty())
			continue;

		for(int Segment = 0; Segment < EnergyCount; Segment++)
		{
			const int TrailIndex = minimum((int)Trail.size() - 1, (Segment + 1) * ENERGY_TRAIL_SPACING_TICKS);
			const vec2 Pos = Trail[TrailIndex];
			const int SnapId = ENERGY_TRAIL_SNAP_ID_BASE + (ClientId * ENERGY_MAX_PER_PLAYER + Segment) * 4;
			SnapSquare(SnapId, Pos);
		}
	}
}

void CGameControllerTtt::SnapBeacons(int SnappingClient)
{
	const CSnapContext Context(GameServer()->GetClientVersion(SnappingClient), Server()->IsSixup(SnappingClient), SnappingClient);
	const auto SnapSquare = [&](int BaseSnapId, const vec2 &Center) {
		const float HalfSize = 4.0f;
		GameServer()->SnapLaserObject(Context, BaseSnapId + 0, Center + vec2(-HalfSize, -HalfSize), Center + vec2(HalfSize, -HalfSize), -1, -1, LASERTYPE_RIFLE, 0, 0);
		GameServer()->SnapLaserObject(Context, BaseSnapId + 1, Center + vec2(HalfSize, -HalfSize), Center + vec2(HalfSize, HalfSize), -1, -1, LASERTYPE_RIFLE, 0, 0);
		GameServer()->SnapLaserObject(Context, BaseSnapId + 2, Center + vec2(HalfSize, HalfSize), Center + vec2(-HalfSize, HalfSize), -1, -1, LASERTYPE_RIFLE, 0, 0);
		GameServer()->SnapLaserObject(Context, BaseSnapId + 3, Center + vec2(-HalfSize, HalfSize), Center + vec2(-HalfSize, -HalfSize), -1, -1, LASERTYPE_RIFLE, 0, 0);
	};

	for(size_t BeaconIndex = 0; BeaconIndex < m_vBeacons.size(); BeaconIndex++)
	{
		const SBeacon &Beacon = m_vBeacons[BeaconIndex];
		const int BeaconEnergy = minimum(BEACON_MAX_ENERGY, Beacon.m_Energy);
		for(int i = 0; i < BeaconEnergy; i++)
		{
			const float Angle = (2.0f * pi * (float)i) / (float)BeaconEnergy;
			const vec2 Pos = Beacon.m_Pos + vec2(std::cos(Angle), std::sin(Angle)) * (float)BEACON_RING_RADIUS;
			const int SnapId = ENERGY_BEACON_RING_SNAP_ID_BASE + ((int)BeaconIndex * BEACON_MAX_ENERGY + i) * 4;
			SnapSquare(SnapId, Pos);
		}
	}

	for(size_t i = 0; i < m_vTravelingBeaconEnergies.size(); i++)
	{
		const STravelingBeaconEnergy &Travel = m_vTravelingBeaconEnergies[i];
		if(Travel.m_EndTick <= Travel.m_StartTick)
			continue;

		const float t = std::clamp((float)(Server()->Tick() - Travel.m_StartTick) / (float)(Travel.m_EndTick - Travel.m_StartTick), 0.0f, 1.0f);
		const vec2 Pos = mix(Travel.m_StartPos, Travel.m_TargetPos, t);
		const int SnapId = ENERGY_BEACON_TRAVEL_SNAP_ID_BASE + (int)i * 4;
		SnapSquare(SnapId, Pos);
	}
}

int CGameControllerTtt::CountAliveInRole(ERole Role) const
{
	int Alive = 0;
	for(const CPlayer *pPlayer : GameServer()->m_apPlayers)
	{
		if(!pPlayer)
			continue;
		if(!Server()->ClientIngame(pPlayer->GetCid()))
			continue;
		if(m_aRoles[pPlayer->GetCid()] != Role)
			continue;

		const CCharacter *pChr = pPlayer->GetCharacter();
		if(pChr && pChr->IsAlive())
			Alive++;
	}
	return Alive;
}

int CGameControllerTtt::CountAliveInInnocentSide() const
{
	int Alive = 0;
	for(const CPlayer *pPlayer : GameServer()->m_apPlayers)
	{
		if(!pPlayer)
			continue;
		if(!Server()->ClientIngame(pPlayer->GetCid()))
			continue;

		const ERole Role = m_aRoles[pPlayer->GetCid()];
		if(Role != ERole::INNOCENT && Role != ERole::DETECTIVE)
			continue;

		const CCharacter *pChr = pPlayer->GetCharacter();
		if(pChr && pChr->IsAlive())
			Alive++;
	}
	return Alive;
}

void CGameControllerTtt::AssignRoles()
{
	std::vector<int> vPlayers;
	for(const CPlayer *pPlayer : GameServer()->m_apPlayers)
	{
		if(!pPlayer)
			continue;
		if(!Server()->ClientIngame(pPlayer->GetCid()))
			continue;
		if(pPlayer->GetTeam() == TEAM_SPECTATORS)
			continue;
		vPlayers.push_back(pPlayer->GetCid());
	}

	ResetRoles();
	if(vPlayers.empty())
		return;

	for(int ClientId : vPlayers)
		m_aRoles[ClientId] = ERole::INNOCENT;

	const int NumTerrorists = std::max(1, (int)vPlayers.size() / 4);
	std::vector<int> vCandidates = vPlayers;
	for(int i = 0; i < NumTerrorists && !vCandidates.empty(); i++)
	{
		const int PickIndex = secure_rand_below(vCandidates.size());
		const int ClientId = vCandidates[PickIndex];
		m_aRoles[ClientId] = ERole::TERRORIST;
		std::swap(vCandidates[PickIndex], vCandidates.back());
		vCandidates.pop_back();
	}

	int DetectiveId = -1;
	if(!vCandidates.empty())
	{
		DetectiveId = vCandidates[secure_rand_below(vCandidates.size())];
		m_aRoles[DetectiveId] = ERole::DETECTIVE;
	}

	m_vRoundStartInnocentNames.clear();
	m_vRoundStartTerroristNames.clear();
	for(int ClientId : vPlayers)
	{
		const ERole Role = m_aRoles[ClientId];
		if(Role == ERole::TERRORIST)
			m_vRoundStartTerroristNames.emplace_back(Server()->ClientName(ClientId));
		else if(Role == ERole::INNOCENT || Role == ERole::DETECTIVE)
			m_vRoundStartInnocentNames.emplace_back(Server()->ClientName(ClientId));
	}

	for(int ClientId : vPlayers)
	{
		switch(m_aRoles[ClientId])
		{
		case ERole::TERRORIST:
			SendChatTarget(ClientId, "You are a Terrorist.");
			break;
		case ERole::DETECTIVE:
			SendChatTarget(ClientId, "You are the Detective.");
			break;
		case ERole::INNOCENT:
			SendChatTarget(ClientId, "You are Innocent.");
			break;
		case ERole::NONE:
			break;
		}
	}

	SendChat(-1, TEAM_ALL, "TTT roles assigned. PvP is now enabled.");
	if(DetectiveId != -1)
		GiveUniqueLaserTo(DetectiveId);
	m_RolesAssigned = true;
}

void CGameControllerTtt::AnnounceWinningRole(ERole WinningRole)
{
	const char *pWinningTeamName = WinningRole == ERole::TERRORIST ? "Terrorists" : "Innocents";

	char aBroadcast[128];
	str_format(aBroadcast, sizeof(aBroadcast), "%s win the round!", pWinningTeamName);
	GameServer()->SendBroadcast(aBroadcast, -1);

	char aChatTeam[128];
	str_format(aChatTeam, sizeof(aChatTeam), "Winning team: %s", pWinningTeamName);
	SendChat(-1, TEAM_ALL, aChatTeam);

	const auto &vOriginalWinners = WinningRole == ERole::TERRORIST ? m_vRoundStartTerroristNames : m_vRoundStartInnocentNames;
	std::string Winners;
	for(const std::string &Name : vOriginalWinners)
	{
		if(!Winners.empty())
			Winners += ", ";
		Winners += Name;
	}

	if(!Winners.empty())
	{
		char aWinners[1024];
		str_format(aWinners, sizeof(aWinners), "Winning players: %s", Winners.c_str());
		SendChat(-1, TEAM_ALL, aWinners);
	}
}
bool CGameControllerTtt::IsOnWaitingMap() const
{
	if(g_Config.m_SvWaitingMap[0] == '\0')
		return false;

	const std::string CurrentMap = TttNormalizeMapName(GameServer()->Map()->BaseName());
	const std::string WaitingMap = TttNormalizeMapName(g_Config.m_SvWaitingMap);
	return str_comp_nocase(CurrentMap.c_str(), WaitingMap.c_str()) == 0;
}

int CGameControllerTtt::NumPlayersReadyForStart() const
{
	int NumReadyPlayers = 0;
	for(const CPlayer *pPlayer : GameServer()->m_apPlayers)
	{
		if(!pPlayer)
			continue;
		if(!Server()->ClientIngame(pPlayer->GetCid()))
			continue;
		if(pPlayer->GetTeam() == TEAM_SPECTATORS)
			continue;
		NumReadyPlayers++;
	}
	return NumReadyPlayers;
}

const char *CGameControllerTtt::PickRandomRoundMap() const
{
	std::vector<const char *> vMapPool;
	vMapPool.reserve(12);

	const char *apMapCvars[] = {
		g_Config.m_SvMap1,
		g_Config.m_SvMap2,
		g_Config.m_SvMap3,
		g_Config.m_SvMap4,
		g_Config.m_SvMap5,
		g_Config.m_SvMap6,
		g_Config.m_SvMap7,
		g_Config.m_SvMap8,
		g_Config.m_SvMap9,
		g_Config.m_SvMap10,
		g_Config.m_SvMap11,
		g_Config.m_SvMap12,
	};

	for(const char *pMap : apMapCvars)
	{
		if(pMap[0] == '\0')
			continue;
		vMapPool.push_back(pMap);
	}

	if(vMapPool.empty())
		return nullptr;

	return vMapPool[secure_rand_below(vMapPool.size())];
}

void CGameControllerTtt::OnInit()
{
	CGameControllerBasePvp::OnInit();
	m_WinType = WIN_BY_SURVIVAL;
	m_WaitingCountdownStartTick = -1;
	m_LastWaitingCountdownSecond = -1;
	m_LastGraceCountdownSecond = -1;
	m_ReturnToWaitingMapTick = -1;
	m_ActiveMapStartTick = -1;
	m_GracePeriodEndTick = -1;
	m_PostWinGraceEndTick = -1;
	m_PostWinGraceActive = false;
	m_PostWinRole = ERole::NONE;
	m_TraitorTesterClientId = -1;
	m_TraitorTesterStartTick = -1;
	m_TraitorTesterResolved = false;
	m_UniqueLaserOwnerCid = -1;
	m_vEnergySpawnTiles.clear();
	m_EnergySpawnTileByMapIndex.clear();
	ResetRoles();
	m_aGraceAutoJoinOptOut.fill(false);
	ResetEnergyState();
	RefreshTraitorTesterRevealPositions();
	RefreshBeaconPositions();

	if(!g_TttWaitingBootstrapHandled && g_Config.m_SvWaitingMap[0] != '\0' && !IsOnWaitingMap())
	{
		g_TttWaitingBootstrapHandled = true;
		ChangeMap(g_Config.m_SvWaitingMap);
		return;
	}

	g_TttWaitingBootstrapHandled = true;
}

void CGameControllerTtt::Snap(int SnappingClient)
{
	CGameControllerBasePvp::Snap(SnappingClient);

	if(SnappingClient == SERVER_DEMO_CLIENT)
		return;

	if(IsOnWaitingMap() || !m_RolesAssigned)
		return;

	const CPlayer *pViewer = GameServer()->m_apPlayers[SnappingClient];
	if(!pViewer || !Server()->ClientIngame(SnappingClient))
		return;

	SnapEnergy(SnappingClient);
	SnapBeacons(SnappingClient);

	const bool ViewerIsTraitor = m_aRoles[SnappingClient] == ERole::TERRORIST;

	for(const CPlayer *pPlayer : GameServer()->m_apPlayers)
	{
		if(!pPlayer)
			continue;

		const int ClientId = pPlayer->GetCid();
		if(!Server()->ClientIngame(ClientId))
			continue;

		const CCharacter *pChr = pPlayer->GetCharacter();
		if(!pChr || !pChr->IsAlive())
			continue;

		const ERole Role = m_aRoles[ClientId];
		const bool ShowRedTraitorFlag = Role == ERole::TERRORIST && ViewerIsTraitor;
		const bool ShowBlueDetectiveFlag = Role == ERole::DETECTIVE;
		if(!ShowRedTraitorFlag && !ShowBlueDetectiveFlag)
			continue;

		const int TeamColor = ShowBlueDetectiveFlag ? TEAM_BLUE : TEAM_RED;
		const int SnapId = ROLE_FLAG_ID_BASE + ClientId * 2 + (TeamColor == TEAM_BLUE ? 1 : 0);
		const vec2 FlagPos = pChr->GetPos();

		if(Server()->IsSixup(SnappingClient))
		{
			auto *pFlag = Server()->SnapNewItem<protocol7::CNetObj_Flag>(SnapId);
			if(!pFlag)
				continue;
			pFlag->m_X = round_to_int(FlagPos.x);
			pFlag->m_Y = round_to_int(FlagPos.y);
			pFlag->m_Team = TeamColor;
		}
		else
		{
			auto *pFlag = Server()->SnapNewItem<CNetObj_Flag>(SnapId);
			if(!pFlag)
				continue;
			pFlag->m_X = round_to_int(FlagPos.x);
			pFlag->m_Y = round_to_int(FlagPos.y);
			pFlag->m_Team = TeamColor;
		}
	}
}

void CGameControllerTtt::OnCharacterSpawn(CCharacter *pChr)
{
	CGameControllerBasePvp::OnCharacterSpawn(pChr);

	// Spawn loadout for TTT is hammer-only. Weapons come from map pickups.
	pChr->SetWeaponGot(WEAPON_HAMMER, true);
	pChr->SetWeaponAmmo(WEAPON_HAMMER, -1);
	pChr->SetWeaponGot(WEAPON_GUN, false);
	pChr->SetWeaponAmmo(WEAPON_GUN, 0);
	pChr->SetWeaponGot(WEAPON_SHOTGUN, false);
	pChr->SetWeaponAmmo(WEAPON_SHOTGUN, 0);
	pChr->SetWeaponGot(WEAPON_GRENADE, false);
	pChr->SetWeaponAmmo(WEAPON_GRENADE, 0);
	pChr->SetWeaponGot(WEAPON_LASER, false);
	pChr->SetWeaponAmmo(WEAPON_LASER, 0);
	pChr->SetWeaponGot(WEAPON_NINJA, false);
	pChr->SetWeaponAmmo(WEAPON_NINJA, 0);
	pChr->SetActiveWeapon(WEAPON_HAMMER);
	pChr->SetLastWeapon(WEAPON_HAMMER);
}

void CGameControllerTtt::AmmoRegen(CCharacter *pChr)
{
	if(pChr && pChr->GetActiveWeapon() == WEAPON_GUN)
	{
		// TTT pistol ammo must be finite and not regenerate over time.
		return;
	}

	CGameControllerBasePvp::AmmoRegen(pChr);
}

void CGameControllerTtt::OnPlayerConnect(CPlayer *pPlayer)
{
	CGameControllerBasePvp::OnPlayerConnect(pPlayer);

	const bool GraceActive = !IsOnWaitingMap() && m_GracePeriodEndTick != -1 && Server()->Tick() < m_GracePeriodEndTick;
	if(GraceActive && pPlayer->GetTeam() == TEAM_SPECTATORS)
	{
		DoTeamChange(pPlayer, TEAM_GAME, false);
		pPlayer->m_RespawnTick = 0;
		pPlayer->TryRespawn();
		return;
	}

	if(!IsOnWaitingMap() && !GraceActive && pPlayer->GetTeam() != TEAM_SPECTATORS)
	{
		DoTeamChange(pPlayer, TEAM_SPECTATORS, false);
	}
}

void CGameControllerTtt::DoTeamChange(CPlayer *pPlayer, int Team, bool DoChatMsg)
{
	if(!pPlayer)
		return;

	const int OldTeam = pPlayer->GetTeam();
	const bool GraceActive = !IsOnWaitingMap() && m_GracePeriodEndTick != -1 && Server()->Tick() < m_GracePeriodEndTick;

	if(GraceActive)
	{
		if(OldTeam != TEAM_SPECTATORS && Team == TEAM_SPECTATORS)
			m_aGraceAutoJoinOptOut[pPlayer->GetCid()] = true;
		else if(Team != TEAM_SPECTATORS)
			m_aGraceAutoJoinOptOut[pPlayer->GetCid()] = false;
	}

	CGameControllerBasePvp::DoTeamChange(pPlayer, Team, DoChatMsg);
}

bool CGameControllerTtt::CanJoinTeam(int Team, int NotThisId, char *pErrorReason, int ErrorReasonSize)
{
	const bool GraceActive = !IsOnWaitingMap() && m_GracePeriodEndTick != -1 && Server()->Tick() < m_GracePeriodEndTick;
	if(!IsOnWaitingMap() && !GraceActive && Team != TEAM_SPECTATORS)
	{
		if(pErrorReason)
			str_copy(pErrorReason, "Round in progress. You can join in waiting map.", ErrorReasonSize);
		return false;
	}

	return CGameControllerBasePvp::CanJoinTeam(Team, NotThisId, pErrorReason, ErrorReasonSize);
}

bool CGameControllerTtt::OnCharacterTakeDamage(vec2 &Force, int &Dmg, int &From, int &Weapon, CCharacter &Character)
{
	const bool GraceActive = !IsOnWaitingMap() && m_GracePeriodEndTick != -1 && Server()->Tick() < m_GracePeriodEndTick;

	if(Weapon == WEAPON_GUN || Weapon == WEAPON_SHOTGUN)
		Dmg = 1;
	if(Weapon == WEAPON_LASER)
		Dmg = 5;
	if(Weapon == WEAPON_HAMMER)
		Dmg = std::max(Dmg, 3);

	if(IsOnWaitingMap() && Weapon == WEAPON_HAMMER && Dmg <= 0)
		Dmg = 3;

	OnAnyDamage(Force, Dmg, From, Weapon, &Character);

	if(GraceActive)
	{
		if(Weapon == WEAPON_GRENADE)
		{
			// In grace, grenade push is still active server-side while damage stays at 0.
			if(length(Force) <= 0.001f)
			{
				vec2 Dir(0.f, -1.f);
				const CPlayer *pFrom = (From >= 0 && From < MAX_CLIENTS) ? GameServer()->m_apPlayers[From] : nullptr;
				const CCharacter *pFromChr = pFrom ? pFrom->GetCharacter() : nullptr;
				if(pFromChr)
				{
					const vec2 Diff = Character.GetPos() - pFromChr->GetPos();
					if(length(Diff) > 0.001f)
						Dir = normalize(Diff);
				}
				Force = Dir * 12.0f;
			}

			Dmg = 0;
			return false;
		}

		Dmg = 0;
		return true;
	}

	bool ApplyForce = true;
	if(SkipDamage(Dmg, From, Weapon, &Character, ApplyForce))
	{
		Dmg = 0;
		return !ApplyForce;
	}
	OnAppliedDamage(Dmg, From, Weapon, &Character);
	ApplyVanillaDamage(Dmg, From, Weapon, &Character);
	DecreaseHealthAndKill(Dmg, From, Weapon, &Character);

	return false;
}

void CGameControllerTtt::Tick()
{
	CGameControllerBasePvp::Tick();

	if(!IsOnWaitingMap())
	{
		TickTraitorTester();
		TickUniqueLaser();
		TickEnergyPickups();
		TickEnergySpawns();
		TickEnergyTrails();
		TickBeacons();

		if(m_PostWinGraceActive && m_PostWinGraceEndTick != -1 && Server()->Tick() >= m_PostWinGraceEndTick)
		{
			m_PostWinGraceActive = false;
			m_PostWinRole = ERole::NONE;
			m_PostWinGraceEndTick = -1;
			if(g_Config.m_SvWaitingMap[0] != '\0')
				ChangeMap(g_Config.m_SvWaitingMap);
			return;
		}

		m_WaitingCountdownStartTick = -1;
		m_LastWaitingCountdownSecond = -1;
		if(m_ActiveMapStartTick == -1)
		{
			m_ActiveMapStartTick = Server()->Tick();
			RebuildEnergySpawnTilesFromMap();
			const int GraceDurationSeconds = g_Config.m_SvGraceDuration;
			m_GracePeriodEndTick = m_ActiveMapStartTick + GraceDurationSeconds * Server()->TickSpeed();
			m_LastGraceCountdownSecond = -1;
			ResetRoles();
			m_aGraceAutoJoinOptOut.fill(false);
			ResetEnergyState();

			if(!m_DebugEnergyNodeSummarySent)
			{
				char aEnergyNodesBuf[196];
				str_format(aEnergyNodesBuf, sizeof(aEnergyNodesBuf), "TTT energy nodes found: %d (game=%d front=%d switch=%d)",
					(int)m_vEnergySpawnTiles.size(),
					m_DebugEnergyNodesGameLayer,
					m_DebugEnergyNodesFrontLayer,
					m_DebugEnergyNodesSwitchLayer);
				SendChat(-1, TEAM_ALL, aEnergyNodesBuf);
				m_DebugEnergyNodeSummarySent = true;
			}

			char aGraceStartBuf[128];
			str_format(aGraceStartBuf, sizeof(aGraceStartBuf), "TTT grace period: PvP disabled for %d seconds.", GraceDurationSeconds);
			SendChat(-1, TEAM_ALL, aGraceStartBuf);
		}

		// Always allow the full grace period to pass before any waiting map transition.
		if(m_ReturnToWaitingMapTick != -1 && Server()->Tick() >= m_ReturnToWaitingMapTick)
		{
			if(Server()->Tick() >= m_GracePeriodEndTick)
			{
				m_ReturnToWaitingMapTick = -1;
				if(g_Config.m_SvWaitingMap[0] != '\0')
					ChangeMap(g_Config.m_SvWaitingMap);
				return;
			}
			m_ReturnToWaitingMapTick = m_GracePeriodEndTick;
		}

		if(!m_RolesAssigned)
		{
			const int TicksLeft = std::max(0, m_GracePeriodEndTick - Server()->Tick());
			const int SecondsLeft = (TicksLeft + Server()->TickSpeed() - 1) / Server()->TickSpeed();
			if(SecondsLeft != m_LastGraceCountdownSecond)
			{
				char aGraceBuf[96];
				str_format(aGraceBuf, sizeof(aGraceBuf), "Grace period ends in %d", SecondsLeft);
				GameServer()->SendBroadcast(aGraceBuf, -1);
				m_LastGraceCountdownSecond = SecondsLeft;
			}

			// Players that finish loading during grace should auto-join the game.
			for(CPlayer *pPlayer : GameServer()->m_apPlayers)
			{
				if(!pPlayer)
					continue;
				if(!Server()->ClientIngame(pPlayer->GetCid()))
					continue;
				if(pPlayer->GetTeam() != TEAM_SPECTATORS)
					continue;
				if(m_aGraceAutoJoinOptOut[pPlayer->GetCid()])
					continue;

				DoTeamChange(pPlayer, TEAM_GAME, false);
				pPlayer->m_RespawnTick = 0;
				pPlayer->TryRespawn();
			}

			if(Server()->Tick() >= m_GracePeriodEndTick)
			{
				GameServer()->SendBroadcast("", -1);
				AssignRoles();
			}
		}
		return;
	}

	m_ActiveMapStartTick = -1;
	m_GracePeriodEndTick = -1;
	m_LastGraceCountdownSecond = -1;
	m_PostWinGraceEndTick = -1;
	m_PostWinGraceActive = false;
	m_PostWinRole = ERole::NONE;
	m_UniqueLaserOwnerCid = -1;
	ResetRoles();
	m_aGraceAutoJoinOptOut.fill(false);
	ResetEnergyState();

	constexpr int MIN_READY_PLAYERS = 4;
	const int StartDelaySeconds = g_Config.m_SvWaitingDuration;

	const int NumReadyPlayers = NumPlayersReadyForStart();
	if(NumReadyPlayers < MIN_READY_PLAYERS)
	{
		if(m_WaitingCountdownStartTick != -1)
		{
			SendChat(-1, TEAM_ALL, "TTT start countdown cancelled. Need at least 4 players in game.");
			GameServer()->SendBroadcast("", -1);
			m_WaitingCountdownStartTick = -1;
			m_LastWaitingCountdownSecond = -1;
		}

		if(Server()->Tick() % Server()->TickSpeed() == 0)
		{
			const int DotState = (Server()->Tick() / Server()->TickSpeed()) % 3;
			char aBuf[128];
			if(DotState == 0)
				str_format(aBuf, sizeof(aBuf), "Waiting for players. (%d/%d)", NumReadyPlayers, MIN_READY_PLAYERS);
			else if(DotState == 1)
				str_format(aBuf, sizeof(aBuf), "Waiting for players.. (%d/%d)", NumReadyPlayers, MIN_READY_PLAYERS);
			else
				str_format(aBuf, sizeof(aBuf), "Waiting for players... (%d/%d)", NumReadyPlayers, MIN_READY_PLAYERS);
			GameServer()->SendBroadcast(aBuf, -1);
		}
		return;
	}

	if(m_WaitingCountdownStartTick == -1)
	{
		m_WaitingCountdownStartTick = Server()->Tick();
		m_LastWaitingCountdownSecond = -1;

		char aStartBuf[128];
		str_format(aStartBuf, sizeof(aStartBuf), "Enough players are ready. Round map starts in %d seconds.", StartDelaySeconds);
		SendChat(-1, TEAM_ALL, aStartBuf);
		return;
	}

	const int CountdownTicks = StartDelaySeconds * Server()->TickSpeed();
	const int ElapsedTicks = Server()->Tick() - m_WaitingCountdownStartTick;
	if(ElapsedTicks < CountdownTicks)
	{
		const int TicksLeft = CountdownTicks - ElapsedTicks;
		const int SecondsLeft = std::max(0, (TicksLeft + Server()->TickSpeed() - 1) / Server()->TickSpeed());
		if(SecondsLeft != m_LastWaitingCountdownSecond)
		{
			char aBuf[96];
			str_format(aBuf, sizeof(aBuf), "Game starts in %d", SecondsLeft);
			GameServer()->SendBroadcast(aBuf, -1);
			m_LastWaitingCountdownSecond = SecondsLeft;
		}
		return;
	}

	if(m_LastWaitingCountdownSecond != 0)
	{
		GameServer()->SendBroadcast("Game starts in 0", -1);
		m_LastWaitingCountdownSecond = 0;
	}

	const char *pRoundMap = PickRandomRoundMap();
	if(!pRoundMap)
	{
		SendChat(-1, TEAM_ALL, "TTT map pool is empty. Configure sv_map_1 to sv_map_12.");
		GameServer()->SendBroadcast("", -1);
		m_WaitingCountdownStartTick = -1;
		m_LastWaitingCountdownSecond = -1;
		return;
	}

	ChangeMap(pRoundMap);
}

bool CGameControllerTtt::DoWincheckRound()
{
	if(IsOnWaitingMap())
		return false;
	if(!m_RolesAssigned)
		return false;
	if(m_PostWinGraceActive)
		return false;

	const int AliveTerrorists = CountAliveInRole(ERole::TERRORIST);
	const int AliveInnocents = CountAliveInInnocentSide();

	if(AliveTerrorists > 0 && AliveInnocents > 0)
		return false;
	if(AliveTerrorists == 0 && AliveInnocents == 0)
		return false;
	const ERole WinningRole = AliveTerrorists > 0 ? ERole::TERRORIST : ERole::INNOCENT;
	m_PostWinGraceActive = true;
	m_PostWinRole = WinningRole;
	m_PostWinGraceEndTick = Server()->Tick() + 5 * Server()->TickSpeed();
	m_ReturnToWaitingMapTick = m_PostWinGraceEndTick;
	AnnounceWinningRole(WinningRole);
	return false;
}

void CGameControllerTtt::OnRoundEnd()
{
	CGameControllerBasePvp::OnRoundEnd();

	if(IsOnWaitingMap())
		return;
	if(g_Config.m_SvWaitingMap[0] == '\0')
		return;

	// Give players time to read winner broadcasts before map transition.
	m_ReturnToWaitingMapTick = Server()->Tick() + 3 * Server()->TickSpeed();
}

bool CGameControllerTtt::SkipDamage(int Dmg, int From, int Weapon, const CCharacter *pCharacter, bool &ApplyForce)
{
	if(m_PostWinGraceActive)
	{
		ApplyForce = false;
		return true;
	}

	if(IsOnWaitingMap() && Weapon == WEAPON_HAMMER)
	{
		ApplyForce = true;
		return false;
	}

	if(!IsOnWaitingMap() && m_GracePeriodEndTick != -1 && Server()->Tick() < m_GracePeriodEndTick)
	{
		// During grace, grenades may still knock players around, but no damage is dealt.
		ApplyForce = Weapon == WEAPON_GRENADE;
		return true;
	}

	return CGameControllerBasePvp::SkipDamage(Dmg, From, Weapon, pCharacter, ApplyForce);
}

bool CGameControllerTtt::OnEntity(int Index, int x, int y, int Layer, int Flags, bool Initial, int Number)
{
	if(Initial && Index == ENTITY_ARMOR_LASER)
	{
		const int MapIndex = y * GameServer()->Collision()->GetWidth() + x;
		if(m_EnergySpawnTileByMapIndex.find(MapIndex) == m_EnergySpawnTileByMapIndex.end())
		{
			SEnergySpawnTile Tile;
			Tile.m_MapIndex = MapIndex;
			Tile.m_Pos = vec2(x * 32.0f + 16.0f, y * 32.0f + 16.0f);
			Tile.m_HasEnergy = false;
			m_EnergySpawnTileByMapIndex[MapIndex] = (int)m_vEnergySpawnTiles.size();
			m_vEnergySpawnTiles.push_back(Tile);
		}

		// Laser shield tile (index 229 / ENTITY_ARMOR_LASER) is reserved as TTT energy node.
		return true;
	}

	if(Initial && Index == ENTITY_HEALTH_1)
	{
		const int Weapon = TttRandomPickupWeapon();
		const int Ammo = TttPickupAmmoForWeapon(Weapon);
		const vec2 Pos(x * 32.0f + 16.0f, y * 32.0f + 16.0f);
		new CTttWeaponPickup(&GameServer()->m_World, Pos, Weapon, Ammo, Layer, Number, TileFlagsToPickupFlags(Flags));
		return true;
	}

	return CGameControllerBasePvp::OnEntity(Index, x, y, Layer, Flags, Initial, Number);
}

int CGameControllerTtt::OnCharacterDeath(CCharacter *pVictim, class CPlayer *pKiller, int Weapon)
{
	const bool HadLaser = pVictim && pVictim->GetWeaponGot(WEAPON_LASER);
	const int VictimCid = pVictim && pVictim->GetPlayer() ? pVictim->GetPlayer()->GetCid() : -1;
	const vec2 VictimPos = pVictim ? pVictim->GetPos() : vec2(0.0f, 0.0f);

	const int Result = CGameControllerBasePvp::OnCharacterDeath(pVictim, pKiller, Weapon);

	if((HadLaser || (VictimCid != -1 && VictimCid == m_UniqueLaserOwnerCid)) && !IsOnWaitingMap())
		DropUniqueLaser(VictimPos);

	if(!IsOnWaitingMap() && m_RolesAssigned && pVictim && pKiller && pVictim->GetPlayer() && pKiller->GetCid() != pVictim->GetPlayer()->GetCid())
	{
		const int KillerCid = pKiller->GetCid();
		const int DeadCid = pVictim->GetPlayer()->GetCid();
		if(KillerCid >= 0 && KillerCid < MAX_CLIENTS && DeadCid >= 0 && DeadCid < MAX_CLIENTS)
		{
			const ERole KillerRole = m_aRoles[KillerCid];
			const ERole VictimRole = m_aRoles[DeadCid];
			const bool InnocentOnInnocent = (KillerRole == ERole::INNOCENT || KillerRole == ERole::DETECTIVE) && VictimRole == ERole::INNOCENT;
			const bool TraitorOnTraitor = KillerRole == ERole::TERRORIST && VictimRole == ERole::TERRORIST;
			const bool KillerIsInnocentSide = KillerRole == ERole::INNOCENT || KillerRole == ERole::DETECTIVE;
			const bool VictimIsInnocentSide = VictimRole == ERole::INNOCENT || VictimRole == ERole::DETECTIVE;
			const bool OppositeSides = (KillerRole == ERole::TERRORIST && VictimIsInnocentSide) ||
				(KillerIsInnocentSide && VictimRole == ERole::TERRORIST);
			if(InnocentOnInnocent || TraitorOnTraitor)
			{
				CCharacter *pKillerChr = pKiller->GetCharacter();
				if(pKillerChr && pKillerChr->IsAlive())
					GameServer()->CreatePlayerSpawn(pKillerChr->GetPos(), CClientMask().set(KillerCid));
			}
			else if(OppositeSides)
			{
				CCharacter *pKillerChr = pKiller->GetCharacter();
				if(pKillerChr && pKillerChr->IsAlive())
					GameServer()->CreateFinishEffect(pKillerChr->GetPos(), CClientMask().set(KillerCid));
			}

			if(m_aRoles[KillerCid] == ERole::DETECTIVE && m_aRoles[DeadCid] == ERole::INNOCENT)
			{
				CCharacter *pKillerChr = pKiller->GetCharacter();
				if(pKillerChr && pKillerChr->IsAlive())
				{
					pKillerChr->AddHealth(-5);
					if(pKillerChr->Health() <= 0)
						pKillerChr->Die(DeadCid, WEAPON_GAME);
				}
			}
		}
	}

	if(IsOnWaitingMap() && pVictim && pVictim->GetPlayer())
	{
		pVictim->GetPlayer()->m_RespawnTick = Server()->Tick() + Server()->TickSpeed() / 2;
	}
	else if(pVictim && pVictim->GetPlayer())
	{
		CPlayer *pVictimPlayer = pVictim->GetPlayer();
		pVictimPlayer->m_ForceTeam.m_Team = TEAM_SPECTATORS;
		pVictimPlayer->m_ForceTeam.m_Tick = Server()->Tick() + 1;
		pVictimPlayer->m_RespawnTick = std::max(pVictimPlayer->m_RespawnTick, Server()->Tick() + 2);
	}

	return Result;
}

REGISTER_GAMEMODE(ttt, CGameControllerTtt(pGameServer));
