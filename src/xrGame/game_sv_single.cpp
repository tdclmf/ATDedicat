#include "stdafx.h"
#include "game_sv_single.h"
#include "xrserver_objects_alife_monsters.h"
#include "alife_simulator.h"
#include "alife_object_registry.h"
#include "alife_graph_registry.h"
#include "alife_time_manager.h"
#include "object_broker.h"
#include "gamepersistent.h"
#include "xrServer.h"
#include "actor.h"
#include "level.h"
#include "gametaskmanager.h"
#include "alife_registry_wrappers.h"
#include "../xrEngine/x_ray.h"
#include "../xrEngine/dedicated_server_only.h"
#include "../xrEngine/no_single.h"

game_sv_Single::game_sv_Single()
{
	m_alife_simulator = NULL;
	m_type = eGameIDSingle;
	m_phase = GAME_PHASE_PENDING;
}

game_sv_Single::~game_sv_Single()
{
	delete_data(m_alife_simulator);
}

void game_sv_Single::Update()
{
	inherited::Update();
}

void game_sv_Single::Create(shared_str& options)
{
	inherited::Create(options);

	if (strstr(*options, "/alife"))
		m_alife_simulator = xr_new<CALifeSimulator>(&server(), &options);
	switch_Phase(GAME_PHASE_INPROGRESS);

}


void game_sv_Single::RespawnPlayer(ClientID id_who, bool bReady)
{
	xrClientData* xrCData = server().ID_to_client(id_who);
	if (!xrCData)
		return;

	if (g_dedicated_server && xrCData == server().GetServerClient())
	{
		if (xrCData->ps)
			xrCData->ps->setFlag(GAME_PLAYER_FLAG_SKIP);
		Msg("--- [SV] Dedicated server system client respawn request ignored.");
		return;
	}

	inherited::RespawnPlayer(id_who, bReady);

	if (xrCData->owner)
	{
		CSE_Abstract* E = xrCData->owner;
		E->s_flags.set(M_SPAWN_OBJECT_LOCAL, TRUE);
		E->s_flags.set(M_SPAWN_OBJECT_ASPLAYER, TRUE);
		Msg("--- [SV] mp_actor [%u] successfully created and assigned to [%s] with LOCAL and ASPLAYER flags.", E->ID, xrCData->name.c_str());
	}
}

void game_sv_Single::OnCreate(u16 id_who)
{
	if (!ai().get_alife())
		return;

	CSE_Abstract* e_who = get_entity_from_eid(id_who);
	VERIFY(e_who);
	if (!e_who->m_bALifeControl)
		return;

	CSE_ALifeObject* alife_object = smart_cast<CSE_ALifeObject*>(e_who);
	if (!alife_object)
		return;

	alife_object->m_bOnline = true;

	if (alife_object->ID_Parent != 0xffff)
	{
		CSE_ALifeDynamicObject* parent = ai().alife().objects().object(alife_object->ID_Parent, true);
		if (parent)
		{
			CSE_ALifeTraderAbstract* trader = smart_cast<CSE_ALifeTraderAbstract*>(parent);
			if (trader)
				alife().create(alife_object);
			else
			{
				CSE_ALifeInventoryBox* const box = smart_cast<CSE_ALifeInventoryBox*>(parent);
				if (box)
					alife().create(alife_object);
				else
					alife_object->m_bALifeControl = false;
			}
		}
		else
			alife_object->m_bALifeControl = false;
	}
	else
		alife().create(alife_object);
}

BOOL game_sv_Single::OnTouch(u16 eid_who, u16 eid_what, BOOL bForced)
{
	CSE_Abstract* e_who = get_entity_from_eid(eid_who);
	VERIFY(e_who);
	CSE_Abstract* e_what = get_entity_from_eid(eid_what);
	VERIFY(e_what);

	if (ai().get_alife())
	{
		CSE_ALifeInventoryItem* l_tpALifeInventoryItem = smart_cast<CSE_ALifeInventoryItem*>(e_what);
		CSE_ALifeDynamicObject* l_tpDynamicObject = smart_cast<CSE_ALifeDynamicObject*>(e_who);

		if (
			l_tpALifeInventoryItem &&
			l_tpDynamicObject &&
			ai().alife().graph().level().object(l_tpALifeInventoryItem->base()->ID, true) &&
			ai().alife().objects().object(e_who->ID, true) &&
			ai().alife().objects().object(e_what->ID, true)
		)
			alife().graph().attach(*e_who, l_tpALifeInventoryItem, l_tpDynamicObject->m_tGraphID, false, false);
#ifdef DEBUG
		else
			if (psAI_Flags.test(aiALife)) {
				Msg				("Cannot attach object [%s][%s][%d] to object [%s][%s][%d]",e_what->name_replace(),*e_what->s_name,e_what->ID,e_who->name_replace(),*e_who->s_name,e_who->ID);
			}
#endif
	}
	return TRUE;
}

void game_sv_Single::OnDetach(u16 eid_who, u16 eid_what)
{
	if (ai().get_alife())
	{
		CSE_Abstract* e_who = get_entity_from_eid(eid_who);
		VERIFY(e_who);
		CSE_Abstract* e_what = get_entity_from_eid(eid_what);
		VERIFY(e_what);

		CSE_ALifeInventoryItem* l_tpALifeInventoryItem = smart_cast<CSE_ALifeInventoryItem*>(e_what);
		if (!l_tpALifeInventoryItem)
			return;

		CSE_ALifeDynamicObject* l_tpDynamicObject = smart_cast<CSE_ALifeDynamicObject*>(e_who);
		if (!l_tpDynamicObject)
			return;

		if (
			ai().alife().objects().object(e_who->ID, true) &&
			!ai().alife().graph().level().object(l_tpALifeInventoryItem->base()->ID, true) &&
			ai().alife().objects().object(e_what->ID, true)
		)
			alife().graph().detach(*e_who, l_tpALifeInventoryItem, l_tpDynamicObject->m_tGraphID, false, false);
		else
		{
			if (!ai().alife().objects().object(e_what->ID, true))
			{
				u16 id = l_tpALifeInventoryItem->base()->ID_Parent;
				l_tpALifeInventoryItem->base()->ID_Parent = 0xffff;

				CSE_ALifeDynamicObject* dynamic_object = smart_cast<CSE_ALifeDynamicObject*>(e_what);
				VERIFY(dynamic_object);
				dynamic_object->m_tNodeID = l_tpDynamicObject->m_tNodeID;
				dynamic_object->m_tGraphID = l_tpDynamicObject->m_tGraphID;
				dynamic_object->m_bALifeControl = true;
				dynamic_object->m_bOnline = true;
				alife().create(dynamic_object);
				l_tpALifeInventoryItem->base()->ID_Parent = id;
			}
#ifdef DEBUG
			else
				if (psAI_Flags.test(aiALife)) {
					Msg			("Cannot detach object [%s][%s][%d] from object [%s][%s][%d]",l_tpALifeInventoryItem->base()->name_replace(),*l_tpALifeInventoryItem->base()->s_name,l_tpALifeInventoryItem->base()->ID,l_tpDynamicObject->base()->name_replace(),l_tpDynamicObject->base()->s_name,l_tpDynamicObject->ID);
				}
#endif
		}
	}
}

ALife::_TIME_ID game_sv_Single::GetStartGameTime()
{
	if (ai().get_alife() && ai().alife().initialized())
		return (ai().alife().time_manager().start_game_time());
	else
		return (inherited::GetStartGameTime());
}

ALife::_TIME_ID game_sv_Single::GetGameTime()
{
	if (ai().get_alife() && ai().alife().initialized())
		return (ai().alife().time_manager().game_time());
	else
		return (inherited::GetGameTime());
}

float game_sv_Single::GetGameTimeFactor()
{
	if (ai().get_alife() && ai().alife().initialized())
		return (ai().alife().time_manager().time_factor());
	else
		return (inherited::GetGameTimeFactor());
}

void game_sv_Single::SetGameTimeFactor(const float fTimeFactor)
{
	if (ai().get_alife() && ai().alife().initialized())
		return (alife().time_manager().set_time_factor(fTimeFactor));
	else
		return (inherited::SetGameTimeFactor(fTimeFactor));
}

ALife::_TIME_ID game_sv_Single::GetEnvironmentGameTime()
{
	if (ai().get_alife() && ai().alife().initialized())
		return (alife().time_manager().game_time());
	else
		return (inherited::GetGameTime());
}

float game_sv_Single::GetEnvironmentGameTimeFactor()
{
	return (inherited::GetGameTimeFactor());
}

void game_sv_Single::SetEnvironmentGameTimeFactor(const float fTimeFactor)
{
	return (inherited::SetGameTimeFactor(fTimeFactor));
}

bool game_sv_Single::change_level(NET_Packet& net_packet, ClientID sender)
{
	if (ai().get_alife())
		return (alife().change_level(net_packet));
	else
		return (true);
}

void game_sv_Single::save_game(NET_Packet& net_packet, ClientID sender)
{
	if (!ai().get_alife())
		return;

	alife().save(net_packet);
}

bool game_sv_Single::load_game(NET_Packet& net_packet, ClientID sender)
{
	if (!ai().get_alife())
		return (inherited::load_game(net_packet, sender));
	shared_str game_name;
	net_packet.r_stringZ(game_name);
	return (alife().load_game(*game_name, true));
}

void game_sv_Single::reload_game(NET_Packet& net_packet, ClientID sender)
{
}

void game_sv_Single::switch_distance(NET_Packet& net_packet, ClientID sender)
{
	if (!ai().get_alife())
		return;

	alife().set_switch_distance(net_packet.r_float());
}

void game_sv_Single::teleport_object(NET_Packet& net_packet, u16 id)
{
	if (!ai().get_alife())
		return;

	GameGraph::_GRAPH_ID game_vertex_id;
	u32 level_vertex_id;
	Fvector position;

	net_packet.r(&game_vertex_id, sizeof(game_vertex_id));
	net_packet.r(&level_vertex_id, sizeof(level_vertex_id));
	net_packet.r_vec3(position);

	alife().teleport_object(id, game_vertex_id, level_vertex_id, position);
}

void game_sv_Single::add_restriction(NET_Packet& packet, u16 id)
{
	if (!ai().get_alife())
		return;

	ALife::_OBJECT_ID restriction_id;
	packet.r(&restriction_id, sizeof(restriction_id));

	RestrictionSpace::ERestrictorTypes restriction_type;
	packet.r(&restriction_type, sizeof(restriction_type));

	alife().add_restriction(id, restriction_id, restriction_type);
}

void game_sv_Single::remove_restriction(NET_Packet& packet, u16 id)
{
	if (!ai().get_alife())
		return;

	ALife::_OBJECT_ID restriction_id;
	packet.r(&restriction_id, sizeof(restriction_id));

	RestrictionSpace::ERestrictorTypes restriction_type;
	packet.r(&restriction_type, sizeof(restriction_type));

	alife().remove_restriction(id, restriction_id, restriction_type);
}

void game_sv_Single::remove_all_restrictions(NET_Packet& packet, u16 id)
{
	if (!ai().get_alife())
		return;

	RestrictionSpace::ERestrictorTypes restriction_type;
	packet.r(&restriction_type, sizeof(restriction_type));

	alife().remove_all_restrictions(id, restriction_type);
}

void game_sv_Single::sls_default()
{
	alife().update_switch();
}

shared_str game_sv_Single::level_name(const shared_str& server_options) const
{
	if (!ai().get_alife())
		return (inherited::level_name(server_options));
	return (alife().level_name());
}

void game_sv_Single::on_death(CSE_Abstract* e_dest, CSE_Abstract* e_src)
{
	inherited::on_death(e_dest, e_src);

	if (!ai().get_alife())
		return;

	alife().on_death(e_dest, e_src);
}

void game_sv_Single::restart_simulator(LPCSTR saved_game_name)
{
	shared_str& options = *alife().server_command_line();

	delete_data(m_alife_simulator);
	server().clear_ids();

	xr_strcpy(g_pGamePersistent->m_game_params.m_game_or_spawn, saved_game_name);
	xr_strcpy(g_pGamePersistent->m_game_params.m_new_or_load, "load");

	pApp->ls_header[0] = '\0';
	pApp->ls_tip_number[0] = '\0';
	pApp->ls_tip[0] = '\0';
	pApp->LoadBegin();
	m_alife_simulator = xr_new<CALifeSimulator>(&server(), &options);
	g_pGamePersistent->LoadTitle();
	Device.PreCache(60, true, true);
	pApp->LoadEnd();
}


// NEW FUCA

void game_sv_Single::assign_RP(CSE_Abstract* E, game_PlayerState* ps_who)
{
	for (const auto& kv : ai().alife().objects().objects())
	{
		if (!kv.second || !kv.second->m_bOnline)
			continue;
		CSE_ALifeHumanStalker* aga = smart_cast<CSE_ALifeHumanStalker*>(kv.second);
		if (aga)
		{
			E->o_Position.set(kv.second->o_Position);
			E->o_Angle.set(kv.second->o_Angle);
			return;
		}
	}
	// DEFAULT POINT
	Fvector F = Fvector().set(0, 0, 0);
	E->o_Position.set(F);
	E->o_Angle.set(F);
}

void game_sv_Single::OnPlayerConnect(ClientID id_who)
{
	inherited::OnPlayerConnect(id_who);
	xrClientData* xrCData = server().ID_to_client(id_who);
	game_PlayerState* ps_who = xrCData->ps;

	if (!xrCData->flags.bReconnect)
	{
		ps_who->clear();
		ps_who->team = 0;
		ps_who->skin = -1;
	}

	ps_who->setFlag(GAME_PLAYER_FLAG_SPECTATOR);
	ps_who->resetFlag(GAME_PLAYER_FLAG_SKIP);

	if (g_dedicated_server && xrCData == server().GetServerClient())
	{
		ps_who->setFlag(GAME_PLAYER_FLAG_SKIP);
		return;
	}

	SetPlayersDefItems(ps_who);
}

void game_sv_Single::OnPlayerConnectFinished(ClientID id_who)
{
	xrClientData* xrCData = server().ID_to_client(id_who);
	if (!xrCData || !xrCData->ps)
		return;

	// Системный клиент дедика нам не нужен в мире
	if (g_dedicated_server && xrCData == server().GetServerClient())
	{
		Msg("--- [SV] Dedicated Server system client registered. Skipping body spawn.");
		return;
	}

	if (!xrCData->owner)
	{
		Msg("--- [SV] Client [%s] connected. Spawning spectator shell...", xrCData->name.c_str());
		SpawnPlayer(id_who, "spectator");
	}
	else
	{
		SendPortionsData(id_who);
		SendGamTasksData(id_who);
	}

	xrCData->ps->setFlag(GAME_PLAYER_FLAG_SPECTATOR);
	Msg("--- [SV] Client [%s] finished connecting. Spawning mp_actor...", xrCData->name.c_str());
	OnPlayerReady(id_who);

	NET_Packet P;
	P.w_begin(M_GAMEMESSAGE);
	P.w_u32(GAME_EVENT_PLAYER_CONNECTED);
	P.w_stringZ(xrCData->name.c_str());
	server().SendBroadcast(BroadcastCID, P, net_flags(TRUE, TRUE, TRUE));
}

void game_sv_Single::OnPlayerReady(ClientID id)
{
	if (m_phase == GAME_PHASE_INPROGRESS)
	{
		xrClientData* xrCData = server().ID_to_client(id);
		if (!xrCData || !xrCData->ps)
			return;

		game_PlayerState* ps = xrCData->ps;
		if (g_dedicated_server && xrCData == server().GetServerClient())
		{
			ps->setFlag(GAME_PLAYER_FLAG_SKIP);
			return;
		}

		if (!xrCData->owner)
		{
			Msg("--- [SV] No owner for [%s], spawning spectator shell before respawn...", xrCData->name.c_str());
			SpawnPlayer(id, "spectator");
			if (!xrCData->owner)
				return;
		}

		if (ps->IsSkip())
			return;

		if (ps->testFlag(GAME_PLAYER_FLAG_SPECTATOR) || ps->testFlag(GAME_PLAYER_FLAG_VERY_VERY_DEAD))
		{
			Msg("--- [SV] Spawning mp_actor for [%s]...", xrCData->name.c_str());
			RespawnPlayer(id, true);
			ps->resetFlag(GAME_PLAYER_FLAG_SPECTATOR);
		}
	}
}

void game_sv_Single::SetSkin(CSE_Abstract* E, u16 Team, u16 ID)
{
	if (auto visual = smart_cast<CSE_Visual*>(E))
		visual->set_visual("actors\\hero\\stalker_novice.ogf");
}

bool game_sv_Single::AssignOwnershipToConnectingClient(CSE_Abstract* E, xrClientData* CL)
{
	auto act = smart_cast<CSE_ALifeCreatureActor*>(E);

	if (!act || act->owner != server().GetServerClient())
		return false;

	return CL->name == act->name_replace();
}

void game_sv_Single::SendGamTasksData(ClientID target)
{
	if (!Actor()) return;
	auto& tasks = Level().GameTaskManager().GetGameTasks();
	Msg("[Client] Syncing [%u] tasks! with client [%u]", tasks.size(), target.value());

	NET_Packet packet;
	packet.w_begin(M_TASKS_DATA);
	packet.w_u32(tasks.size());

	for (auto& task : tasks)
	{
		CMemoryWriter taskSerialized;
		task.save(taskSerialized);
		packet.w_stringZ(task.task_id);
		packet.w_u32(taskSerialized.size());
		packet.w(taskSerialized.pointer(), taskSerialized.size());
	}

	packet.w_stringZ(g_active_task_id);
	server().SendTo(target, packet);
}

void game_sv_Single::SendPortionsData(ClientID target)
{
	if (!Actor()) return;
	auto& portions = Actor()->m_known_info_registry->registry().objects();
	Msg("[Client] Syncing [%u] tasks! with client [%u]", portions.size(), target.value());

	NET_Packet packet;
	packet.w_begin(M_PORTIONS_DATA);
	packet.w_u32(portions.size());

	for (auto& portion : portions)
		packet.w_stringZ(portion);

	server().SendTo(target, packet);
}
