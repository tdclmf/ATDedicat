#include "pch_script.h"
#include "game_cl_single.h"
#include "actor.h"
#include "gametask.h"
#include "gametaskmanager.h"
#include "alife_registry_wrappers.h"
#include "alife_object_registry.h"
#include "UIGameSP.h"
#include "clsid_game.h"
#include "ai_space.h"
#include "alife_simulator.h"
#include "alife_time_manager.h"

ESingleGameDifficulty g_SingleGameDifficulty = egdStalker;

xr_token difficulty_type_token[] = {
	{ "gd_novice", egdNovice },
	{ "gd_stalker", egdStalker },
	{ "gd_veteran", egdVeteran },
	{ "gd_master", egdMaster },
	{ 0, 0 }
};

game_cl_Single::game_cl_Single()
{
}

CUIGameCustom* game_cl_Single::createGameUI()
{
	CUIGameSP* pUIGame = smart_cast<CUIGameSP*>(NEW_INSTANCE(CLSID_GAME_UI_SINGLE));
	R_ASSERT(pUIGame);
	pUIGame->Load();
	pUIGame->SetClGame(this);
	pUIGame->Init(0);
	pUIGame->Init(1);
	pUIGame->Init(2);
	return pUIGame;
}

char* game_cl_Single::getTeamSection(int Team)
{
	return NULL;
}

void game_cl_Single::OnDifficultyChanged()
{
	if (Actor())
		Actor()->OnDifficultyChanged();
}

ALife::_TIME_ID game_cl_Single::GetGameTime()
{
	if (ai().get_alife() && ai().alife().initialized())
		return (ai().alife().time_manager().game_time());
	else
		return (inherited::GetGameTime());
}

ALife::_TIME_ID game_cl_Single::GetStartGameTime()
{
	if (ai().get_alife() && ai().alife().initialized())
		return (ai().alife().time_manager().start_game_time());
	else
		return (inherited::GetStartGameTime());
}

float game_cl_Single::GetGameTimeFactor()
{
	if (ai().get_alife() && ai().alife().initialized())
		return (ai().alife().time_manager().time_factor());
	else
		return (inherited::GetGameTimeFactor());
}

void game_cl_Single::SetGameTimeFactor(const float fTimeFactor)
{
	Level().Server->game->SetGameTimeFactor(fTimeFactor);
}

ALife::_TIME_ID game_cl_Single::GetEnvironmentGameTime()
{
	if (ai().get_alife() && ai().alife().initialized())
		return (ai().alife().time_manager().game_time());
	else
		return (inherited::GetEnvironmentGameTime());
}

float game_cl_Single::GetEnvironmentGameTimeFactor()
{
	if (ai().get_alife() && ai().alife().initialized())
		return (ai().alife().time_manager().time_factor());
	else
		return (inherited::GetEnvironmentGameTimeFactor());
}

void game_cl_Single::SetEnvironmentGameTimeFactor(const float fTimeFactor)
{
	if (ai().get_alife() && ai().alife().initialized())
		Level().Server->game->SetGameTimeFactor(fTimeFactor);
	else
		inherited::SetEnvironmentGameTimeFactor(fTimeFactor);
}

// ADJUST
void game_cl_Single::OnPortsDataSync(NET_Packet* packet)
{
	u32 portsCount = packet->r_u32();
	Msg("[Client] Receiving [%u] portions from server", portsCount);

	xr_vector<u32> NETportions;
	xr_vector<shared_str> INVportions;

	// READDING ALL
	for (u32 i = 0; i < portsCount; i++)
	{
		shared_str taskId = "";
		packet->r_stringZ(taskId);
		Actor()->TransferInfo(taskId, true);
		NETportions.push_back(taskId._get()->dwCRC);
	}

	// CROPPING MISSING
	KNOWN_INFO_VECTOR& AllPortions = Actor()->m_known_info_registry->registry().objects();
	for (const auto& kv : AllPortions)
		if (std::find(NETportions.begin(), NETportions.end(), kv._get()->dwCRC) == NETportions.end())
			INVportions.push_back(kv);

	// DISABLING PORTIONS THAT NOT EXIST ON HOST SIDE
	for (const auto& kv : INVportions)
		Actor()->TransferInfo(kv, false);
}

void game_cl_Single::OnTasksDataSync(NET_Packet* packet)
{
	// Читаем количество задач
	u32 taskCount = packet->r_u32();
	Msg("[Client] Receiving [%u] tasks from server", taskCount);

	// Читаем каждую задачу
	auto& tasks = Level().GameTaskManager().GetGameTasks();
	for (u32 i = 0; i < taskCount; i++)
	{
		shared_str taskId = "";
		packet->r_stringZ(taskId);
		u32 SerCount = packet->r_u32();

		if (taskId.size() <= 1 || SerCount <= 1) {
			Msg("! ERROR: Not enough tasks data corrupted! %u - %u", taskId.size(), SerCount);
			continue;
		}

		if (SerCount > packet->r_elapsed())
		{
			Msg("! ERROR: Not enough data in packet for task %s", taskId.c_str());
			return;
		}

		SGameTaskKey* task = NULL;
		auto it = std::find_if(tasks.begin(), tasks.end(), [&taskId](const SGameTaskKey& key) {
			return taskId == key.task_id;
		});

		if (it != tasks.end())
			task = &(*it);
		else
		{
			tasks.push_back(SGameTaskKey(taskId));
			task = &tasks.back();
		}

		if (!task) {
			Msg("! ERROR: Failed to create [%s] task!", taskId.c_str());
			return;
		}

		IReader reader = IReader(&packet->B.data[packet->r_pos], SerCount);
		task->load(reader);
	}
}

bool game_cl_Single::OnKeyboardPress(int key)
{
	if (inherited::OnKeyboardPress(key)) return true;
	if (kJUMP == key || kWPN_FIRE == key)
	{
		CGameObject* curr = smart_cast<CGameObject*>(Level().CurrentControlEntity());
		if (!curr) return (false);

		bool is_actor = smart_cast<CActor*>(curr);
		bool is_spectator = smart_cast<CSpectator*>(curr);

		if (is_spectator && !is_actor) {
			Msg("---I'm ready (ID = %d) sending player ready packet !!!", curr->ID());
			NET_Packet P;
			curr->u_EventGen(P, GE_GAME_EVENT, curr->ID());
			P.w_u16(GAME_EVENT_PLAYER_READY);
			curr->u_EventSend(P);
			return true;
		}
	}
	return false;
}

using namespace luabind;
#pragma optimize("s",on)
void CScriptGameDifficulty::script_register(lua_State* L)
{
	module(L)
		[
			class_<enum_exporter<ESingleGameDifficulty>>("game_difficulty")
				.enum_("game_difficulty")
				[
					value("novice", int(egdNovice)),
					value("stalker", int(egdStalker)),
					value("veteran", int(egdVeteran)),
					value("master", int(egdMaster))
				]
		];
}