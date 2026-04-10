#include "outlier.h"

#include <base/math.h>
#include <base/system.h>

#include <engine/server.h>
#include <engine/shared/config.h>
#include <engine/shared/network.h>

#include <generated/protocol.h>

#include <game/server/entities/character.h>
#include <game/server/gamecontext.h>
#include <game/server/player.h>
#include <game/server/teeinfo.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace
{
constexpr int HIDE_PHASE_SECONDS = 10;
constexpr int ACTIVE_PHASE_SECONDS = 60;
constexpr int WIN_BROADCAST_SECONDS = 5;
constexpr float PI_F = 3.14159265358979323846f;
constexpr int s_aBotRandomEmoticons[] = {
	EMOTICON_OOP,
	EMOTICON_EXCLAMATION,
	EMOTICON_HEARTS,
	EMOTICON_DROP,
	EMOTICON_DOTDOT,
	EMOTICON_MUSIC,
	EMOTICON_SORRY,
	EMOTICON_GHOST,
	EMOTICON_SUSHI,
	EMOTICON_SPLATTEE,
	EMOTICON_DEVILTEE,
	EMOTICON_ZOMG,
	EMOTICON_ZZZ,
	EMOTICON_WTF,
	EMOTICON_EYES,
	EMOTICON_QUESTION};
constexpr const char *const s_apRoundSkins[] = {
	"bluekitty",
	"bluestripe",
	"brownbear",
	"cammo",
	"cammostripes",
	"coala",
	"default",
	"limekitty",
	"pinky",
	"redbopp",
	"redstripe",
	"saddo",
	"toptri",
	"twinbop",
	"twintri",
	"warpaint"};

float NormalizeAngle(float Angle)
{
	while(Angle > PI_F)
		Angle -= 2.0f * PI_F;
	while(Angle < -PI_F)
		Angle += 2.0f * PI_F;
	return Angle;
}

float RandomAngle()
{
	const int Milli = secure_rand_below(6284);
	return -PI_F + Milli / 1000.0f;
}

const char *RandomRoundSkinName()
{
	return s_apRoundSkins[secure_rand_below((int)(sizeof(s_apRoundSkins) / sizeof(s_apRoundSkins[0])))];
}

CTeeInfo MakeRoundSkinInfo(const char *pSkinName, bool UseCustomColor, int ColorBody, int ColorFeet)
{
	CTeeInfo Info(pSkinName, UseCustomColor, ColorBody, ColorFeet);
	Info.ToSixup();
	return Info;
}
} // namespace

CGameControllerOutlier::CGameControllerOutlier(CGameContext *pGameServer) :
	CGameControllerBasePvp(pGameServer)
{
	m_PrevRandomClientSlots = g_Config.m_SvRandomClientSlots;
	g_Config.m_SvRandomClientSlots = 1;

	m_GameFlags = 0;
	m_pGameType = "outlier";
	m_DefaultWeapon = WEAPON_HAMMER;
	m_IsVanillaGameType = true;
	m_pStatsTable = "outlier";
	m_pExtraColumns = nullptr;
	m_pSqlStats->SetExtraColumns(m_pExtraColumns);
	m_pSqlStats->CreateTable(m_pStatsTable);
	ResetRoundData();
}

CGameControllerOutlier::~CGameControllerOutlier()
{
	for(int ClientId = 0; ClientId < MAX_CLIENTS; ClientId++)
	{
		if(!m_aHasOriginalName[ClientId])
			continue;
		if(!Server()->ClientIngame(ClientId))
			continue;
		if(IsDebugDummyClient(ClientId))
			continue;
		Server()->SetClientName(ClientId, m_aOriginalNames[ClientId].c_str());
	}
	g_Config.m_DbgDummies = 0;
	g_Config.m_SvRandomClientSlots = m_PrevRandomClientSlots;
}

void CGameControllerOutlier::ResetRoundData()
{
	m_RoundInitTick = -1;
	m_HidePhaseEndTick = -1;
	m_ActivePhaseEndTick = -1;
	m_RestartRoundTick = -1;
	m_LastCountdownSecond = -1;
	m_LastSkinEnforceSecond = -1;
	m_RolesAssigned = false;
	m_RoundResolved = false;
	m_aRoles.fill(ERole::NONE);
	m_aEliminated.fill(false);
	m_aHammerUsedThisRound.fill(false);
	m_aIdentityAppliedThisRound.fill(false);
	m_aPendingBotPenaltyTick.fill(-1);
	m_aPendingBotPenaltyVictim.fill(-1);
	m_aLastRealTagHitTick.fill(-1);
	m_aRoundSkinNames.fill(std::string());
	m_aBotBehaviorInit.fill(false);
}

void CGameControllerOutlier::OnRoundStart()
{
	CGameControllerBasePvp::OnRoundStart();
	ResetRoundData();
}

void CGameControllerOutlier::OnRoundEnd()
{
	CGameControllerBasePvp::OnRoundEnd();
	GameServer()->SendBroadcast("", -1);
	ReconnectClientsForRoundShuffle();
	ResetRoundData();
}

bool CGameControllerOutlier::OnEntity(int Index, int x, int y, int Layer, int Flags, bool Initial, int Number)
{
	if(Initial && Index >= 1 && Index <= 3)
		m_vSpawnTiles.emplace_back(x * 32.0f + 16.0f, y * 32.0f + 16.0f);

	return CGameControllerBasePvp::OnEntity(Index, x, y, Layer, Flags, Initial, Number);
}

bool CGameControllerOutlier::CanSpawn(int Team, vec2 *pOutPos, int ClientId)
{
	if(Team == TEAM_SPECTATORS)
		return false;

	if(!m_vSpawnTiles.empty())
	{
		*pOutPos = m_vSpawnTiles[secure_rand_below((int)m_vSpawnTiles.size())];
		return true;
	}

	return CGameControllerBasePvp::CanSpawn(Team, pOutPos, ClientId);
}

void CGameControllerOutlier::ReconnectClientsForRoundShuffle()
{
	for(int ClientId = 0; ClientId < MAX_CLIENTS; ClientId++)
	{
		if(!Server()->ClientIngame(ClientId))
			continue;
		if(IsDebugDummyClient(ClientId))
			continue;
		if(Server()->GetClientVersion(ClientId) < VERSION_DDNET_RECONNECT)
			continue;

		Server()->ReconnectClient(ClientId);
	}
}

bool CGameControllerOutlier::IsDebugDummyClient(int ClientId) const
{
	if(ClientId < 0 || ClientId >= MAX_CLIENTS)
		return false;
	const NETADDR *pAddr = Server()->ClientAddr(ClientId);
	if(!pAddr)
		return false;
	return pAddr->type == NETTYPE_IPV6 && pAddr->ip[0] == 0xfd && pAddr->ip[6] == 0xc0 && pAddr->ip[7] == 0xde;
}

std::string CGameControllerOutlier::GenerateRandomName(bool IsBot) const
{
	static const char *const s_apAdj[] = {
		"Calm", "Quiet", "Dusty", "Silver", "Nimble", "Hidden", "Steady", "Swift", "Mellow", "Bright", "Echo", "Misty"};
	static const char *const s_apNoun[] = {
		"Pebble", "Comet", "Harbor", "Drift", "Willow", "Signal", "Glider", "Nimbus", "Tide", "Orbit", "Breeze", "Beacon"};

	char aBuf[64];
	const char *pAdj = s_apAdj[secure_rand_below(std::size(s_apAdj))];
	const char *pNoun = s_apNoun[secure_rand_below(std::size(s_apNoun))];
	const int Number = secure_rand_below(900) + 100;
	if(IsBot)
		str_format(aBuf, sizeof(aBuf), "%s%s%03d", pAdj, pNoun, Number);
	else
		str_format(aBuf, sizeof(aBuf), "%s%s%03d", pNoun, pAdj, Number);
	return aBuf;
}

void CGameControllerOutlier::RandomizeName(int ClientId, bool IsBot)
{
	if(ClientId < 0 || ClientId >= MAX_CLIENTS)
		return;
	if(!Server()->ClientIngame(ClientId))
		return;

	if(!IsBot && !m_aHasOriginalName[ClientId])
	{
		m_aOriginalNames[ClientId] = Server()->ClientName(ClientId);
		m_aHasOriginalName[ClientId] = true;
	}

	for(int Attempt = 0; Attempt < 8; Attempt++)
	{
		const std::string Name = GenerateRandomName(IsBot);
		Server()->SetClientName(ClientId, Name.c_str());
		if(str_comp(Server()->ClientName(ClientId), Name.c_str()) == 0)
			return;
	}
}

const char *CGameControllerOutlier::ChatNameForClient(int ClientId) const
{
	if(ClientId < 0 || ClientId >= MAX_CLIENTS)
		return "unknown";
	if(m_aHasOriginalName[ClientId] && !m_aOriginalNames[ClientId].empty())
		return m_aOriginalNames[ClientId].c_str();
	return Server()->ClientName(ClientId);
}

void CGameControllerOutlier::ApplyDefaultAppearance(int ClientId)
{
	if(ClientId < 0 || ClientId >= MAX_CLIENTS)
		return;
	CPlayer *pPlayer = GameServer()->m_apPlayers[ClientId];
	if(!pPlayer)
		return;

	CTeeInfo ForcedInfo = MakeRoundSkinInfo(RoundSkinName(ClientId), false, 0, 0);

	// Clamp both the current tee info and the managed user/override state,
	// otherwise later refresh paths can reapply client-selected custom colors.
	pPlayer->m_SkinInfoManager.SetUserChoice(ForcedInfo);
	pPlayer->m_SkinInfoManager.SetUseCustomColor(ESkinPrio::HIGH, false);
	pPlayer->m_TeeInfos = pPlayer->m_SkinInfoManager.TeeInfo();
	pPlayer->m_TeeInfos.m_UseCustomColor = false;
	for(bool &UseCustom : pPlayer->m_TeeInfos.m_aUseCustomColors)
		UseCustom = false;
	GameServer()->SendSkinChange7(ClientId);
}

void CGameControllerOutlier::ApplyTaggerAppearance(int ClientId)
{
	if(ClientId < 0 || ClientId >= MAX_CLIENTS)
		return;
	CPlayer *pPlayer = GameServer()->m_apPlayers[ClientId];
	if(!pPlayer)
		return;

	CTeeInfo TaggerInfo = MakeRoundSkinInfo(RoundSkinName(ClientId), true, 65387, 65387);

	pPlayer->m_SkinInfoManager.SetUserChoice(TaggerInfo);
	pPlayer->m_SkinInfoManager.SetUseCustomColor(ESkinPrio::HIGH, true);
	pPlayer->m_TeeInfos = pPlayer->m_SkinInfoManager.TeeInfo();
	pPlayer->m_TeeInfos.m_UseCustomColor = true;
	for(bool &UseCustom : pPlayer->m_TeeInfos.m_aUseCustomColors)
		UseCustom = true;
	for(int &PartColor : pPlayer->m_TeeInfos.m_aSkinPartColors)
		PartColor = 65280;
	pPlayer->m_TeeInfos.m_ColorBody = 65280;
	pPlayer->m_TeeInfos.m_ColorFeet = 65280;
	GameServer()->SendSkinChange7(ClientId);
}

void CGameControllerOutlier::ApplyTaggerLoadout(int ClientId)
{
	if(ClientId < 0 || ClientId >= MAX_CLIENTS)
		return;
	CPlayer *pPlayer = GameServer()->m_apPlayers[ClientId];
	if(!pPlayer)
		return;
	CCharacter *pChr = pPlayer->GetCharacter();
	if(!pChr || !pChr->IsAlive())
		return;

	pChr->SetWeaponGot(WEAPON_LASER, true);
	pChr->SetWeaponAmmo(WEAPON_LASER, -1);
	pChr->SetActiveWeapon(WEAPON_LASER);
	pChr->SetLastWeapon(WEAPON_LASER);
}

void CGameControllerOutlier::ApplyRoleAppearance(int ClientId)
{
	if(ClientId < 0 || ClientId >= MAX_CLIENTS)
		return;

	if(m_RolesAssigned && m_aRoles[ClientId] == ERole::TAGGER)
		ApplyTaggerAppearance(ClientId);
	else
		ApplyDefaultAppearance(ClientId);
}

void CGameControllerOutlier::SpawnHammerSmoke(const vec2 &Pos)
{
	constexpr int NumRingParticles = 24;
	constexpr int NumInnerParticles = 32;
	constexpr int NumWaves = 2;
	constexpr float Radius = 6.0f * 32.0f;
	for(int Wave = 0; Wave < NumWaves; Wave++)
	{
		const float WaveRadius = Radius * (Wave == 0 ? 0.75f : 1.0f);
		for(int i = 0; i < NumRingParticles; i++)
		{
			const float Angle = (2.0f * PI_F * i) / NumRingParticles + (Wave * PI_F) / NumRingParticles;
			const vec2 Offset(cosf(Angle) * WaveRadius, sinf(Angle) * WaveRadius);
			GameServer()->CreatePlayerSpawn(Pos + Offset);
		}

		for(int i = 0; i < NumInnerParticles; i++)
		{
			const float Angle = -PI_F + (2.0f * PI_F * secure_rand_below(10000)) / 10000.0f;
			const float Distance = WaveRadius * sqrtf(secure_rand_below(10000) / 10000.0f);
			const vec2 Offset(cosf(Angle) * Distance, sinf(Angle) * Distance);
			GameServer()->CreatePlayerSpawn(Pos + Offset);
		}
	}

	const float RadiusSquared = Radius * Radius;
	for(int ClientId = 0; ClientId < MAX_CLIENTS; ClientId++)
	{
		if(!Server()->ClientIngame(ClientId))
			continue;
		CPlayer *pPlayer = GameServer()->m_apPlayers[ClientId];
		if(!pPlayer || pPlayer->GetTeam() == TEAM_SPECTATORS)
			continue;
		if(m_RolesAssigned && m_aRoles[ClientId] == ERole::TAGGER)
			continue;

		CCharacter *pChr = pPlayer->GetCharacter();
		if(!pChr || !pChr->IsAlive())
			continue;

		const vec2 Delta = pChr->GetPos() - Pos;
		const float DistanceSquared = Delta.x * Delta.x + Delta.y * Delta.y;
		if(DistanceSquared > RadiusSquared)
			continue;

		m_aRoundSkinNames[ClientId] = RandomRoundSkinName();
		ApplyRoleAppearance(ClientId);
	}
}

void CGameControllerOutlier::EnsureIdentityApplied()
{
	for(int ClientId = 0; ClientId < MAX_CLIENTS; ClientId++)
	{
		if(!Server()->ClientIngame(ClientId))
			continue;
		if(!GameServer()->m_apPlayers[ClientId])
			continue;

		ApplyRoleAppearance(ClientId);
		if(m_aIdentityAppliedThisRound[ClientId])
			continue;

		const bool IsBot = IsDebugDummyClient(ClientId);
		RandomizeName(ClientId, IsBot);
		m_aIdentityAppliedThisRound[ClientId] = true;
	}
}

void CGameControllerOutlier::EnforceDefaultSkins()
{
	const int SecondNow = Server()->Tick() / Server()->TickSpeed();
	if(SecondNow == m_LastSkinEnforceSecond)
		return;
	m_LastSkinEnforceSecond = SecondNow;

	for(int ClientId = 0; ClientId < MAX_CLIENTS; ClientId++)
	{
		if(!Server()->ClientIngame(ClientId))
			continue;
		if(!GameServer()->m_apPlayers[ClientId])
			continue;
		Server()->SetClientClan(ClientId, "");
		ApplyRoleAppearance(ClientId);
	}
}

void CGameControllerOutlier::InitializeRound()
{
	m_RoundInitTick = Server()->Tick();
	m_HidePhaseEndTick = m_RoundInitTick + HIDE_PHASE_SECONDS * Server()->TickSpeed();
	m_ActivePhaseEndTick = -1;
	m_RestartRoundTick = -1;
	m_LastCountdownSecond = -1;
	m_RolesAssigned = false;
	m_RoundResolved = false;
	m_aRoles.fill(ERole::NONE);
	m_aEliminated.fill(false);
	m_aIdentityAppliedThisRound.fill(false);
	m_aBotBehaviorInit.fill(false);

	AssignRoundSkins();

	for(CPlayer *pPlayer : GameServer()->m_apPlayers)
	{
		if(!pPlayer)
			continue;
		if(!Server()->ClientIngame(pPlayer->GetCid()))
			continue;
		if(pPlayer->GetTeam() == TEAM_SPECTATORS)
		{
			DoTeamChange(pPlayer, TEAM_GAME, false);
			pPlayer->m_RespawnTick = 0;
			pPlayer->TryRespawn();
		}
	}

	for(CPlayer *pPlayer : GameServer()->m_apPlayers)
	{
		if(!pPlayer)
			continue;
		if(!Server()->ClientIngame(pPlayer->GetCid()))
			continue;
		ApplyRoleAppearance(pPlayer->GetCid());
	}

}

void CGameControllerOutlier::AssignRoundSkins()
{
	for(int ClientId = 0; ClientId < MAX_CLIENTS; ClientId++)
	{
		if(!Server()->ClientIngame(ClientId))
		{
			m_aRoundSkinNames[ClientId].clear();
			continue;
		}

		m_aRoundSkinNames[ClientId] = RandomRoundSkinName();
	}
}

const char *CGameControllerOutlier::RoundSkinName(int ClientId)
{
	if(ClientId < 0 || ClientId >= MAX_CLIENTS)
		return "default";
	if(m_aRoundSkinNames[ClientId].empty())
		m_aRoundSkinNames[ClientId] = RandomRoundSkinName();
	return m_aRoundSkinNames[ClientId].c_str();
}

void CGameControllerOutlier::AssignRoles()
{
	if(m_RolesAssigned)
		return;

	std::vector<int> vRealPlayers;
	for(CPlayer *pPlayer : GameServer()->m_apPlayers)
	{
		if(!pPlayer)
			continue;
		const int ClientId = pPlayer->GetCid();
		if(!Server()->ClientIngame(ClientId))
			continue;
		if(pPlayer->GetTeam() == TEAM_SPECTATORS)
			continue;
		if(IsDebugDummyClient(ClientId))
			continue;

		CCharacter *pChr = pPlayer->GetCharacter();
		if(!pChr || !pChr->IsAlive())
		{
			pPlayer->m_RespawnTick = 0;
			pPlayer->TryRespawn();
		}
		vRealPlayers.push_back(ClientId);
	}

	if(vRealPlayers.empty())
		return;

	for(int ClientId : vRealPlayers)
		m_aRoles[ClientId] = ERole::HIDER_REAL;

	for(CPlayer *pPlayer : GameServer()->m_apPlayers)
	{
		if(!pPlayer)
			continue;
		const int ClientId = pPlayer->GetCid();
		if(!Server()->ClientIngame(ClientId))
			continue;
		if(!IsDebugDummyClient(ClientId))
			continue;
		if(pPlayer->GetTeam() == TEAM_SPECTATORS)
			continue;
		m_aRoles[ClientId] = ERole::HIDER_FAKE;
	}

	for(int i = (int)vRealPlayers.size() - 1; i > 0; --i)
	{
		const int j = secure_rand_below(i + 1);
		std::swap(vRealPlayers[i], vRealPlayers[j]);
	}

	const int NumTaggers = maximum(1, (int)vRealPlayers.size() / 3);
	for(int i = 0; i < NumTaggers; i++)
		m_aRoles[vRealPlayers[i]] = ERole::TAGGER;

	for(int ClientId : vRealPlayers)
	{
		ApplyRoleAppearance(ClientId);
		if(m_aRoles[ClientId] == ERole::TAGGER)
			ApplyTaggerLoadout(ClientId);
	}

	m_RolesAssigned = true;
	m_ActivePhaseEndTick = Server()->Tick() + ACTIVE_PHASE_SECONDS * Server()->TickSpeed();
	m_LastCountdownSecond = -1;

	char aBuf[128];
	str_format(aBuf, sizeof(aBuf), "Taggers: %d/%d", NumTaggers, (int)vRealPlayers.size());
	SendChat(-1, TEAM_ALL, aBuf);

	for(int ClientId : vRealPlayers)
	{
		if(m_aRoles[ClientId] == ERole::TAGGER)
			SendChatTarget(ClientId, "You are a TAGGER.");
		else
			SendChatTarget(ClientId, "You are a HIDER.");
	}
}

void CGameControllerOutlier::BroadcastCountdown()
{
	const int TickNow = Server()->Tick();

	if(m_RoundResolved)
	{
		if(m_RestartRoundTick <= 0)
			return;
		const int TicksLeft = maximum(0, m_RestartRoundTick - TickNow);
		const int SecondsLeft = (TicksLeft + Server()->TickSpeed() - 1) / Server()->TickSpeed();
		if(SecondsLeft != m_LastCountdownSecond)
		{
			char aBuf[96];
			str_format(aBuf, sizeof(aBuf), "Next round in: %d", SecondsLeft);
			GameServer()->SendBroadcast(aBuf, -1);
			m_LastCountdownSecond = SecondsLeft;
		}
		return;
	}

	if(!m_RolesAssigned)
	{
		const int TicksLeft = maximum(0, m_HidePhaseEndTick - TickNow);
		const int SecondsLeft = (TicksLeft + Server()->TickSpeed() - 1) / Server()->TickSpeed();
		if(SecondsLeft != m_LastCountdownSecond)
		{
			char aBuf[96];
			str_format(aBuf, sizeof(aBuf), "Hide phase: %d", SecondsLeft);
			GameServer()->SendBroadcast(aBuf, -1);
			m_LastCountdownSecond = SecondsLeft;
		}
	}
	else
	{
		const int TicksLeft = maximum(0, m_ActivePhaseEndTick - TickNow);
		const int SecondsLeft = (TicksLeft + Server()->TickSpeed() - 1) / Server()->TickSpeed();
		if(SecondsLeft != m_LastCountdownSecond)
		{
			char aBuf[96];
			str_format(aBuf, sizeof(aBuf), "Tagging time left: %d", SecondsLeft);
			GameServer()->SendBroadcast(aBuf, -1);
			m_LastCountdownSecond = SecondsLeft;
		}
	}
}

void CGameControllerOutlier::UpdateBotPopulation()
{
	int RealPlayers = 0;
	for(CPlayer *pPlayer : GameServer()->m_apPlayers)
	{
		if(!pPlayer)
			continue;
		if(!Server()->ClientIngame(pPlayer->GetCid()))
			continue;
		if(IsDebugDummyClient(pPlayer->GetCid()))
			continue;
		RealPlayers++;
	}

	const int MaxBots = maximum(0, Server()->MaxClients() - RealPlayers - 1);
	const int WantedBots = std::clamp(g_Config.m_SvOutlierBots, 0, MaxBots);
	g_Config.m_DbgDummies = WantedBots;

	for(CPlayer *pPlayer : GameServer()->m_apPlayers)
	{
		if(!pPlayer)
			continue;
		const int ClientId = pPlayer->GetCid();
		if(!Server()->ClientIngame(ClientId))
			continue;
		if(!IsDebugDummyClient(ClientId))
			continue;

		if(pPlayer->GetTeam() == TEAM_SPECTATORS)
		{
			DoTeamChange(pPlayer, TEAM_GAME, false);
			pPlayer->m_RespawnTick = 0;
			pPlayer->TryRespawn();
		}
		m_aRoles[ClientId] = ERole::HIDER_FAKE;
	}
}

void CGameControllerOutlier::TickBotBehavior()
{
	for(int ClientId = 0; ClientId < MAX_CLIENTS; ClientId++)
	{
		if(!Server()->ClientIngame(ClientId))
			continue;
		if(!IsDebugDummyClient(ClientId))
			continue;

		CPlayer *pPlayer = GameServer()->m_apPlayers[ClientId];
		if(!pPlayer || pPlayer->GetTeam() == TEAM_SPECTATORS)
			continue;
		CCharacter *pChr = pPlayer->GetCharacter();
		if(!pChr || !pChr->IsAlive())
			continue;

		SBotBehavior &Bot = m_aBotBehavior[ClientId];
		if(!m_aBotBehaviorInit[ClientId])
		{
			Bot.m_CurrentAngle = RandomAngle();
			Bot.m_TargetAngle = Bot.m_CurrentAngle;
			Bot.m_NextSegmentTick = 0;
			m_aBotBehaviorInit[ClientId] = true;
		}

		if(Server()->Tick() >= Bot.m_NextSegmentTick)
		{
			Bot.m_Direction = (int)secure_rand_below(3) - 1;
			Bot.m_TargetAngle = RandomAngle();
			if(secure_rand_below(100) < 10)
			{
				const int Emoticon = s_aBotRandomEmoticons[secure_rand_below((int)std::size(s_aBotRandomEmoticons))];
				GameServer()->SendEmoticon(ClientId, Emoticon, -1);
			}
			if(secure_rand_below(100) < 18)
			{
				const int MaxHoldTicks = 5 * Server()->TickSpeed();
				Bot.m_HookTicksLeft = 8 + secure_rand_below(MaxHoldTicks - 7);
			}
			else
			{
				Bot.m_HookTicksLeft = 0;
			}
			if(secure_rand_below(100) < 6)
			{
				Bot.m_JumpTicksLeft = 2;
				Bot.m_SecondJumpDelayTicks = 6;
			}
			else
			{
				Bot.m_JumpTicksLeft = 0;
				Bot.m_SecondJumpDelayTicks = 0;
			}
			Bot.m_NextSegmentTick = Server()->Tick() + 18 + secure_rand_below(42);
		}

		float Diff = NormalizeAngle(Bot.m_TargetAngle - Bot.m_CurrentAngle);
		Bot.m_CurrentAngle = NormalizeAngle(Bot.m_CurrentAngle + Diff * 0.08f);

		CNetObj_PlayerInput Input = {0};
		Input.m_Direction = Bot.m_Direction;
		Input.m_TargetX = (int)round_to_int(cosf(Bot.m_CurrentAngle) * 220.0f);
		Input.m_TargetY = (int)round_to_int(sinf(Bot.m_CurrentAngle) * 220.0f);
		if(Input.m_TargetX == 0 && Input.m_TargetY == 0)
			Input.m_TargetY = -1;

		if(Bot.m_JumpTicksLeft > 0)
		{
			Input.m_Jump = 1;
			Bot.m_JumpTicksLeft--;
		}
		else if(Bot.m_SecondJumpDelayTicks > 0)
		{
			Bot.m_SecondJumpDelayTicks--;
			if(Bot.m_SecondJumpDelayTicks == 0)
				Bot.m_JumpTicksLeft = 2;
		}

		if(Bot.m_HookTicksLeft > 0)
		{
			Input.m_Hook = 1;
			Bot.m_HookTicksLeft--;
		}

		GameServer()->OnClientPredictedEarlyInput(ClientId, &Input);
		GameServer()->OnClientPredictedInput(ClientId, &Input);
		GameServer()->OnClientDirectInput(ClientId, &Input);
	}
}

void CGameControllerOutlier::ResolvePendingBotPenalties()
{
	const int TickNow = Server()->Tick();
	for(int ClientId = 0; ClientId < MAX_CLIENTS; ClientId++)
	{
		const int PenaltyTick = m_aPendingBotPenaltyTick[ClientId];
		if(PenaltyTick < 0)
			continue;
		if(PenaltyTick >= TickNow)
			continue;

		const int VictimId = m_aPendingBotPenaltyVictim[ClientId];
		const bool HitRealOnSameTick = m_aLastRealTagHitTick[ClientId] == PenaltyTick;
		m_aPendingBotPenaltyTick[ClientId] = -1;
		m_aPendingBotPenaltyVictim[ClientId] = -1;
		if(HitRealOnSameTick)
			continue;

		if(!Server()->ClientIngame(ClientId))
			continue;
		if(m_aRoles[ClientId] != ERole::TAGGER)
			continue;

		CPlayer *pAttacker = GameServer()->m_apPlayers[ClientId];
		CCharacter *pAttackerChr = pAttacker ? pAttacker->GetCharacter() : nullptr;
		if(!pAttackerChr || !pAttackerChr->IsAlive())
			continue;

		pAttackerChr->Die(VictimId, WEAPON_GAME);
		SendChatTarget(ClientId, "You tagged a bot.");
	}
}

void CGameControllerOutlier::Tick()
{
	CGameControllerBasePvp::Tick();

	if(GameState() != IGS_GAME_RUNNING)
		return;

	if(m_RoundInitTick == -1)
		InitializeRound();

	if(m_RoundResolved && m_RestartRoundTick > 0 && Server()->Tick() >= m_RestartRoundTick)
	{
		ReconnectClientsForRoundShuffle();
		StartRound();
		return;
	}

	UpdateBotPopulation();
	EnsureIdentityApplied();
	ResolvePendingBotPenalties();
	EnforceDefaultSkins();
	BroadcastCountdown();

	if(!m_RolesAssigned && Server()->Tick() >= m_HidePhaseEndTick)
		AssignRoles();

	TickBotBehavior();
}

void CGameControllerOutlier::OnCharacterSpawn(CCharacter *pChr)
{
	CGameControllerBasePvp::OnCharacterSpawn(pChr);

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

	if(!pChr->GetPlayer())
		return;

	const int ClientId = pChr->GetPlayer()->GetCid();
	ApplyDefaultAppearance(ClientId);
	if(m_RolesAssigned && m_aRoles[ClientId] == ERole::TAGGER)
		ApplyTaggerLoadout(ClientId);
}

void CGameControllerOutlier::OnPlayerConnect(CPlayer *pPlayer)
{
	CGameControllerBasePvp::OnPlayerConnect(pPlayer);
	if(!pPlayer)
		return;

	const int ClientId = pPlayer->GetCid();
	m_aIdentityAppliedThisRound[ClientId] = false;
	m_aPendingBotPenaltyTick[ClientId] = -1;
	m_aPendingBotPenaltyVictim[ClientId] = -1;
	m_aLastRealTagHitTick[ClientId] = -1;
	m_aRoundSkinNames[ClientId].clear();
	m_aHasOriginalName[ClientId] = false;
	m_aOriginalNames[ClientId].clear();
	m_aHammerUsedThisRound[ClientId] = false;
	pPlayer->SetInitialAfk(false);

	Server()->SetClientClan(ClientId, "");
	pPlayer->m_EyeEmoteEnabled = false;
	pPlayer->OverrideDefaultEmote(EMOTE_NORMAL, Server()->Tick() + 60 * Server()->TickSpeed());
	if(CCharacter *pChr = pPlayer->GetCharacter())
		pChr->SetEmote(EMOTE_NORMAL, -1);
	ApplyRoleAppearance(ClientId);
	if(m_RolesAssigned && m_aRoles[ClientId] == ERole::TAGGER)
		ApplyTaggerLoadout(ClientId);
	RandomizeName(ClientId, IsDebugDummyClient(ClientId));
	m_aIdentityAppliedThisRound[ClientId] = true;

	if(GameState() != IGS_GAME_RUNNING || !m_RolesAssigned)
	{
		if(pPlayer->GetTeam() == TEAM_SPECTATORS)
		{
			DoTeamChange(pPlayer, TEAM_GAME, false);
			pPlayer->m_RespawnTick = 0;
			pPlayer->TryRespawn();
		}
	}
	else
	{
		if(!IsDebugDummyClient(pPlayer->GetCid()) && pPlayer->GetTeam() != TEAM_SPECTATORS)
			DoTeamChange(pPlayer, TEAM_SPECTATORS, false);
	}
}

bool CGameControllerOutlier::OnFireWeapon(CCharacter &Character, int &Weapon, vec2 &Direction, vec2 &MouseTarget, vec2 &ProjStartPos)
{
	if(CGameControllerBasePvp::OnFireWeapon(Character, Weapon, Direction, MouseTarget, ProjStartPos))
		return true;

	if(Weapon != WEAPON_HAMMER)
		return false;

	CPlayer *pPlayer = Character.GetPlayer();
	if(!pPlayer)
		return false;

	const int ClientId = pPlayer->GetCid();
	if(ClientId < 0 || ClientId >= MAX_CLIENTS)
		return false;
	if(m_RolesAssigned && m_aRoles[ClientId] == ERole::TAGGER)
		return false;

	// Real players only get one hammer use per round; consuming happens on fire attempt, even on misses.
	if(IsDebugDummyClient(ClientId))
		return false;

	if(m_aHammerUsedThisRound[ClientId])
		return true;

	m_aHammerUsedThisRound[ClientId] = true;
	SpawnHammerSmoke(Character.GetPos());
	return false;
}

bool CGameControllerOutlier::OnChangeInfoNetMessage(const CNetMsg_Cl_ChangeInfo *pMsg, int ClientId)
{
	if(ClientId < 0 || ClientId >= MAX_CLIENTS)
		return true;
	if(!Server()->ClientIngame(ClientId))
		return true;
	ApplyDefaultAppearance(ClientId);
	return true;
}

bool CGameControllerOutlier::OnSkinChange7(protocol7::CNetMsg_Cl_SkinChange *pMsg, int ClientId)
{
	if(ClientId < 0 || ClientId >= MAX_CLIENTS)
		return true;
	if(!Server()->ClientIngame(ClientId))
		return true;
	ApplyDefaultAppearance(ClientId);
	return true;
}

void CGameControllerOutlier::DoTeamChange(CPlayer *pPlayer, int Team, bool DoChatMsg)
{
	if(!pPlayer)
		return;

	if(m_RolesAssigned && Team != TEAM_SPECTATORS && !IsDebugDummyClient(pPlayer->GetCid()))
		return;

	CGameControllerBasePvp::DoTeamChange(pPlayer, Team, DoChatMsg);
}

bool CGameControllerOutlier::CanJoinTeam(int Team, int NotThisId, char *pErrorReason, int ErrorReasonSize)
{
	if(m_RolesAssigned && Team != TEAM_SPECTATORS)
	{
		if(pErrorReason)
			str_copy(pErrorReason, "Round in progress. Wait for next round.", ErrorReasonSize);
		return false;
	}
	return CGameControllerBasePvp::CanJoinTeam(Team, NotThisId, pErrorReason, ErrorReasonSize);
}

bool CGameControllerOutlier::OnCharacterTakeDamage(vec2 &Force, int &Dmg, int &From, int &Weapon, CCharacter &Character)
{
	if(Weapon == WEAPON_WORLD || From < 0)
		return CGameControllerBasePvp::OnCharacterTakeDamage(Force, Dmg, From, Weapon, Character);

	Dmg = 0;
	if(Weapon != WEAPON_HAMMER && Weapon != WEAPON_LASER)
		return true;
	if(!m_RolesAssigned)
		return true;

	CPlayer *pAttacker = GetPlayerOrNullptr(From);
	CPlayer *pVictim = Character.GetPlayer();
	if(!pAttacker || !pVictim)
		return true;
	if(!pAttacker->GetCharacter() || !pAttacker->GetCharacter()->IsAlive())
		return true;

	if(m_RoundResolved)
	{
		Character.Die(From, Weapon);
		return true;
	}

	const int VictimId = pVictim->GetCid();
	if(m_aRoles[From] != ERole::TAGGER)
		return true;

	if(m_aRoles[VictimId] == ERole::HIDER_FAKE)
	{
		m_aPendingBotPenaltyTick[From] = Server()->Tick();
		m_aPendingBotPenaltyVictim[From] = VictimId;
		return true;
	}

	if(m_aRoles[VictimId] == ERole::HIDER_REAL)
	{
		m_aLastRealTagHitTick[From] = Server()->Tick();
		Character.Die(From, Weapon);
		return true;
	}

	return true;
}

bool CGameControllerOutlier::OnChatMessage(const CNetMsg_Cl_Say *pMsg, int Length, int &Team, CPlayer *pPlayer)
{
	if(CGameControllerBasePvp::OnChatMessage(pMsg, Length, Team, pPlayer))
		return true;

	if(!pMsg || !pPlayer)
		return false;

	if(str_startswith_nocase(pMsg->m_pMessage, "/emote") || str_startswith_nocase(pMsg->m_pMessage, "/eyeemote"))
	{
		SendChatTarget(pPlayer->GetCid(), "Eye emotes are fixed in outlier.");
		return true;
	}

	// Keep command/whisper behavior untouched.
	if(pMsg->m_pMessage[0] == '/')
		return false;

	char aBuf[512];
	str_format(aBuf, sizeof(aBuf), "%s: %s", ChatNameForClient(pPlayer->GetCid()), pMsg->m_pMessage);

	if(Team == TEAM_ALL)
	{
		GameServer()->SendChatTarget(-1, aBuf);
	}
	else if(Team == TEAM_SPECTATORS)
	{
		for(int i = 0; i < MAX_CLIENTS; i++)
		{
			CPlayer *pTarget = GameServer()->m_apPlayers[i];
			if(!pTarget)
				continue;
			if(pTarget->GetTeam() != TEAM_SPECTATORS)
				continue;
			GameServer()->SendChatTarget(i, aBuf);
		}
	}
	else
	{
		GameServer()->SendChatTeam(Team, aBuf);
	}

	return true;
}

int CGameControllerOutlier::OnCharacterDeath(CCharacter *pVictim, CPlayer *pKiller, int Weapon)
{
	const int Result = CGameControllerBasePvp::OnCharacterDeath(pVictim, pKiller, Weapon);

	if(!pVictim || !pVictim->GetPlayer())
		return Result;

	CPlayer *pVictimPlayer = pVictim->GetPlayer();
	const int VictimId = pVictimPlayer->GetCid();

	if(IsDebugDummyClient(VictimId))
	{
		m_aRoles[VictimId] = ERole::HIDER_FAKE;
		pVictimPlayer->m_RespawnTick = Server()->Tick() + Server()->TickSpeed() / 2;
		return Result;
	}

	m_aEliminated[VictimId] = true;
	if(GameState() == IGS_GAME_RUNNING && m_RolesAssigned)
	{
		pVictimPlayer->m_ForceTeam.m_Team = TEAM_SPECTATORS;
		pVictimPlayer->m_ForceTeam.m_Tick = Server()->Tick() + 1;
	}

	return Result;
}

int CGameControllerOutlier::CountAlive(ERole Role) const
{
	int Alive = 0;
	for(int ClientId = 0; ClientId < MAX_CLIENTS; ClientId++)
	{
		if(m_aRoles[ClientId] != Role)
			continue;
		if(!Server()->ClientIngame(ClientId))
			continue;
		const CPlayer *pPlayer = GameServer()->m_apPlayers[ClientId];
		if(!pPlayer)
			continue;
		const CCharacter *pChr = pPlayer->GetCharacter();
		if(!pChr || !pChr->IsAlive())
			continue;
		Alive++;
	}
	return Alive;
}

bool CGameControllerOutlier::DoWincheckRound()
{
	if(m_RoundResolved)
		return false;
	if(GameState() != IGS_GAME_RUNNING)
		return false;
	if(!m_RolesAssigned)
		return false;

	const int AliveTaggers = CountAlive(ERole::TAGGER);
	const int AliveRealHiders = CountAlive(ERole::HIDER_REAL);

	auto SendWinners = [this](ERole WinnerRole, const char *pPrefix)
	{
		char aNames[512] = "";
		bool First = true;
		for(int ClientId = 0; ClientId < MAX_CLIENTS; ClientId++)
		{
			if(m_aRoles[ClientId] != WinnerRole)
				continue;
			if(!Server()->ClientIngame(ClientId))
				continue;
			if(IsDebugDummyClient(ClientId))
				continue;

			if(!First)
				str_append(aNames, ", ", sizeof(aNames));
			str_append(aNames, ChatNameForClient(ClientId), sizeof(aNames));
			First = false;
		}

		char aBuf[768];
		if(aNames[0] == '\0')
			str_format(aBuf, sizeof(aBuf), "%s", pPrefix);
		else
			str_format(aBuf, sizeof(aBuf), "%s Winners: %s", pPrefix, aNames);
		GameServer()->SendChat(-1, TEAM_ALL, aBuf);
	};

	if(AliveRealHiders <= 0)
	{
		m_RoundResolved = true;
		m_RestartRoundTick = Server()->Tick() + WIN_BROADCAST_SECONDS * Server()->TickSpeed();
		m_LastCountdownSecond = -1;
		SendWinners(ERole::TAGGER, "Taggers win.");
		return true;
	}

	if(AliveTaggers <= 0)
	{
		m_RoundResolved = true;
		m_RestartRoundTick = Server()->Tick() + WIN_BROADCAST_SECONDS * Server()->TickSpeed();
		m_LastCountdownSecond = -1;
		SendWinners(ERole::HIDER_REAL, "Hiders win.");
		return true;
	}

	if(Server()->Tick() >= m_ActivePhaseEndTick)
	{
		m_RoundResolved = true;
		m_RestartRoundTick = Server()->Tick() + WIN_BROADCAST_SECONDS * Server()->TickSpeed();
		m_LastCountdownSecond = -1;
		SendWinners(ERole::HIDER_REAL, "Hiders win.");
		return true;
	}

	return false;
}

int CGameControllerOutlier::SnapPlayerScore(int SnappingClient, CPlayer *pPlayer)
{
	return 0;
}

int CGameControllerOutlier::SnapPlayerLatency(int SnappingClient, CPlayer *pPlayer, int DefaultLatency)
{
	// Everyone shows 50ms ping for anonymity
	return 50;
}

REGISTER_GAMEMODE(outlier, CGameControllerOutlier(pGameServer));
