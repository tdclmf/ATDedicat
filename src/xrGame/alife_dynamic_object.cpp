////////////////////////////////////////////////////////////////////////////
//	Module 		: alife_dynamic_object.cpp
//	Created 	: 27.10.2005
//  Modified 	: 27.10.2005
//	Author		: Dmitriy Iassenev
//	Description : ALife dynamic object class
////////////////////////////////////////////////////////////////////////////

#include "stdafx.h"
#include "xrServer_Objects_ALife.h"
#include "alife_simulator.h"
#include "alife_schedule_registry.h"
#include "alife_graph_registry.h"
#include "alife_object_registry.h"
#include "level_graph.h"
#include "game_level_cross_table.h"
#include "game_graph.h"
#include "xrServer.h"
#include "player_actor_context.h"

extern ENGINE_API bool g_dedicated_server;

namespace
{
	static u32 g_alife_switch_diag_budget = 2000;

	IC bool alife_switch_diag_allow_log()
	{
		if (!g_alife_switch_diag_budget)
			return false;

		--g_alife_switch_diag_budget;
		return true;
	}

	IC bool alife_switch_has_valid_actor(const CSE_ALifeCreatureActor* actor)
	{
		if (!actor)
			return false;

		// Dedicated-single keeps a technical actor with id 0. It is not a real
		// spatial player anchor for ALife online/offline decisions.
		if (g_dedicated_server && actor->ID == 0)
			return false;

		return true;
	}

	bool alife_switch_find_actor_anchor(const CSE_ALifeCreatureActor* graph_actor, const Fvector& object_position, Fvector& actor_position, u16& actor_id, LPCSTR& actor_name)
	{
		player_actor_context::SActorAnchor runtime_anchor;
		if (player_actor_context::FindNearestRuntimePlayerAnchor(object_position, runtime_anchor))
		{
			actor_position.set(runtime_anchor.position);
			actor_id = runtime_anchor.id;
			actor_name = runtime_anchor.name;
			return true;
		}

		if (g_dedicated_server)
			return false;

		if (!alife_switch_has_valid_actor(graph_actor))
			return false;

		actor_position.set(graph_actor->o_Position);
		actor_id = graph_actor->ID;
		actor_name = graph_actor->name_replace();
		return true;
	}
}

void CSE_ALifeDynamicObject::on_spawn()
{
#ifdef DEBUG
	//	Msg			("[LSS] spawning object [%d][%d][%s][%s]",ID,ID_Parent,name(),name_replace());
#endif
}

void CSE_ALifeDynamicObject::on_register()
{
	CSE_ALifeObject* object = this;
	while (object->ID_Parent != ALife::_OBJECT_ID(-1))
	{
		object = ai().alife().objects().object(object->ID_Parent);
		VERIFY(object);
	}

	if (!alife().graph().level().object(object->ID, true))
		clear_client_data();
}

void CSE_ALifeDynamicObject::on_before_register()
{
}

#include "level.h"
#include "map_manager.h"

void CSE_ALifeDynamicObject::on_unregister()
{
	extern luabind::functor<void>* CSE_ALifeDynamicObject_on_unregister;
	if (CSE_ALifeDynamicObject_on_unregister)
		(*CSE_ALifeDynamicObject_on_unregister)((u16)ID);
	Level().MapManager().OnObjectDestroyNotify(ID);
}

void CSE_ALifeDynamicObject::switch_online()
{
	R_ASSERT(!m_bOnline);
	m_bOnline = true;
	alife().add_online(this);
}

void CSE_ALifeDynamicObject::switch_offline()
{
	R_ASSERT(m_bOnline);
	m_bOnline = false;
	alife().remove_online(this);

	clear_client_data();
}

void CSE_ALifeDynamicObject::add_online(const bool& update_registries)
{
	if (!update_registries)
		return;

	alife().scheduled().remove(this);
	alife().graph().remove(this, m_tGraphID, false);
}

void CSE_ALifeDynamicObject::add_offline(const xr_vector<ALife::_OBJECT_ID>& saved_children,
                                         const bool& update_registries)
{
	if (!update_registries)
		return;

	alife().scheduled().add(this);
	alife().graph().add(this, m_tGraphID, false);
}

bool CSE_ALifeDynamicObject::synchronize_location()
{
	if (!ai().level_graph().valid_vertex_id(m_tNodeID)) return false;

	if (!ai().level_graph().valid_vertex_position(o_Position) || ai().level_graph().inside(ai().level_graph().vertex(m_tNodeID),
		o_Position))
		return (true);

	u32 const new_vertex_id = ai().level_graph().vertex(m_tNodeID, o_Position);
	if (!m_bOnline && !ai().level_graph().inside(new_vertex_id, o_Position))
		return (true);

	m_tNodeID = new_vertex_id;
	GameGraph::_GRAPH_ID tGraphID = ai().cross_table().vertex(m_tNodeID).game_vertex_id();
	if (tGraphID != m_tGraphID)
	{
		if (!m_bOnline)
		{
			Fvector position = o_Position;
			u32 level_vertex_id = m_tNodeID;
			alife().graph().change(this, m_tGraphID, tGraphID);
			if (ai().level_graph().inside(ai().level_graph().vertex(level_vertex_id), position))
			{
				level_vertex_id = m_tNodeID;
				o_Position = position;
			}
		}
		else
		{
			VERIFY(ai().game_graph().vertex(tGraphID)->level_id() == alife().graph().level().level_id());
			m_tGraphID = tGraphID;
		}
	}

	m_fDistance = ai().cross_table().vertex(m_tNodeID).distance();

	return (true);
}

void CSE_ALifeDynamicObject::try_switch_online()
{
	CSE_ALifeSchedulable* schedulable = smart_cast<CSE_ALifeSchedulable*>(this);
	// checking if the abstract monster has just died
	if (schedulable)
	{
		if (!schedulable->need_update(this))
		{
			if (alife().scheduled().object(ID, true))
				alife().scheduled().remove(this);
		}
		else if (!alife().scheduled().object(ID, true))
			alife().scheduled().add(this);
	}

	if (!can_switch_online())
	{
		on_failed_switch_online();
		return;
	}

	if (!can_switch_offline())
	{
		if (alife_switch_diag_allow_log())
		{
			Msg("* [ALIFE_SW][ONLINE_REQ] dedicated=%d id=%u name=[%s] reason=cannot_offline parent=%u graph=%u node=%u pos=(%.2f %.2f %.2f)",
				g_dedicated_server ? 1 : 0,
				ID,
				name_replace(),
				ID_Parent,
				m_tGraphID,
				m_tNodeID,
				VPUSH(o_Position));
		}
		alife().switch_online(this);
		return;
	}

	CSE_ALifeCreatureActor* actor = alife().graph().actor();
	Fvector actor_position;
	u16 actor_id = u16(-1);
	LPCSTR actor_name = "";
	if (!alife_switch_find_actor_anchor(actor, o_Position, actor_position, actor_id, actor_name))
	{
		if (alife_switch_diag_allow_log())
		{
			Msg("* [ALIFE_SW][ONLINE_SKIP] dedicated=%d id=%u name=[%s] reason=no_valid_actor actor_id=%u actor=[%s] parent=%u graph=%u node=%u pos=(%.2f %.2f %.2f)",
				g_dedicated_server ? 1 : 0,
				ID,
				name_replace(),
				(!g_dedicated_server && actor) ? actor->ID : u16(-1),
				(!g_dedicated_server && actor) ? actor->name_replace() : "",
				ID_Parent,
				m_tGraphID,
				m_tNodeID,
				VPUSH(o_Position));
		}
		return;
	}

	const float distance_to_actor = actor_position.distance_to(o_Position);
	const float online_distance = alife().online_distance();
	if (distance_to_actor > online_distance)
	{
		if (alife_switch_diag_allow_log())
		{
			Msg("* [ALIFE_SW][ONLINE_SKIP] dedicated=%d id=%u name=[%s] reason=distance dist=%.2f on=%.2f actor_id=%u actor=[%s] parent=%u graph=%u node=%u pos=(%.2f %.2f %.2f) actor_pos=(%.2f %.2f %.2f)",
				g_dedicated_server ? 1 : 0,
				ID,
				name_replace(),
				distance_to_actor,
				online_distance,
				actor_id,
				actor_name,
				ID_Parent,
				m_tGraphID,
				m_tNodeID,
				VPUSH(o_Position),
				VPUSH(actor_position));
		}
		on_failed_switch_online();
		return;
	}

	if (alife_switch_diag_allow_log())
	{
		Msg("* [ALIFE_SW][ONLINE_REQ] dedicated=%d id=%u name=[%s] reason=distance dist=%.2f on=%.2f actor_id=%u actor=[%s] parent=%u graph=%u node=%u pos=(%.2f %.2f %.2f) actor_pos=(%.2f %.2f %.2f)",
			g_dedicated_server ? 1 : 0,
			ID,
			name_replace(),
			distance_to_actor,
			online_distance,
			actor_id,
			actor_name,
			ID_Parent,
			m_tGraphID,
			m_tNodeID,
			VPUSH(o_Position),
			VPUSH(actor_position));
	}
	alife().switch_online(this);
}

void CSE_ALifeDynamicObject::try_switch_offline()
{
	if (!can_switch_offline())
		return;

	if (g_dedicated_server && ID == 0)
	{
		if (alife_switch_diag_allow_log())
		{
			Msg("* [ALIFE_SW][OFFLINE_SKIP] id=%u name=[%s] reason=system_actor_id0 parent=%u graph=%u node=%u",
				ID,
				name_replace(),
				ID_Parent,
				m_tGraphID,
				m_tNodeID);
		}
		return;
	}

	CSE_ALifeCreatureActor* actor = alife().graph().actor();
	Fvector actor_position;
	u16 actor_id = u16(-1);
	LPCSTR actor_name = "";
	if (!alife_switch_find_actor_anchor(actor, o_Position, actor_position, actor_id, actor_name))
	{
		if (alife_switch_diag_allow_log())
		{
			Msg("* [ALIFE_SW][OFFLINE_SKIP] id=%u name=[%s] reason=no_valid_actor actor_id=%u actor=[%s] parent=%u graph=%u node=%u",
				ID,
				name_replace(),
				(!g_dedicated_server && actor) ? actor->ID : u16(-1),
				(!g_dedicated_server && actor) ? actor->name_replace() : "",
				ID_Parent,
				m_tGraphID,
				m_tNodeID);
		}
		return;
	}
	const float distance_to_actor = actor_position.distance_to(o_Position);
	const float offline_distance = alife().offline_distance();

	if (!can_switch_online())
	{
		if (alife_switch_diag_allow_log())
		{
			Msg("* [ALIFE_SW][OFFLINE_REQ] id=%u name=[%s] reason=cannot_online dist=%.2f off=%.2f actor_id=%u actor=[%s] parent=%u graph=%u node=%u",
				ID,
				name_replace(),
				distance_to_actor,
				offline_distance,
				actor_id,
				actor_name,
				ID_Parent,
				m_tGraphID,
				m_tNodeID);
		}
		alife().switch_offline(this);
		return;
	}

	if (distance_to_actor <= offline_distance)
		return;

	if (alife_switch_diag_allow_log())
	{
		Msg("* [ALIFE_SW][OFFLINE_REQ] id=%u name=[%s] reason=distance dist=%.2f off=%.2f actor_id=%u actor=[%s] parent=%u graph=%u node=%u",
			ID,
			name_replace(),
			distance_to_actor,
			offline_distance,
			actor_id,
			actor_name,
			ID_Parent,
			m_tGraphID,
			m_tNodeID);
	}
	alife().switch_offline(this);
}

bool CSE_ALifeDynamicObject::redundant() const
{
	return (false);
}

/// ---------------------------- CSE_ALifeInventoryBox ---------------------------------------------

void CSE_ALifeInventoryBox::add_online(const bool& update_registries)
{
	CSE_ALifeDynamicObjectVisual* object = (this);

	NET_Packet tNetPacket;
	ClientID clientID;
	clientID.set(
		object->alife().server().GetServerClient() ? object->alife().server().GetServerClient()->ID.value() : 0);

	ALife::OBJECT_IT I = object->children.begin();
	ALife::OBJECT_IT E = object->children.end();
	for (; I != E; ++I)
	{
		CSE_ALifeDynamicObject* l_tpALifeDynamicObject = ai().alife().objects().object(*I);
		CSE_ALifeInventoryItem* l_tpALifeInventoryItem = smart_cast<CSE_ALifeInventoryItem*>(l_tpALifeDynamicObject);
		R_ASSERT2(l_tpALifeInventoryItem, "Non inventory item object has parent?!");
		l_tpALifeInventoryItem->base()->s_flags.or(M_SPAWN_UPDATE);
		CSE_Abstract* l_tpAbstract = smart_cast<CSE_Abstract*>(l_tpALifeInventoryItem);
		object->alife().server().entity_Destroy(l_tpAbstract);

#ifdef DEBUG
		//		if (psAI_Flags.test(aiALife))
//			Msg					("[LSS] Spawning item [%s][%s][%d]",l_tpALifeInventoryItem->base()->name_replace(),*l_tpALifeInventoryItem->base()->s_name,l_tpALifeDynamicObject->ID);
		Msg						(
			"[LSS][%d] Going online [%d][%s][%d] with parent [%d][%s] on '%s'",
			Device.dwFrame,
			Device.dwTimeGlobal,
			l_tpALifeInventoryItem->base()->name_replace(),
			l_tpALifeInventoryItem->base()->ID,
			ID,
			name_replace(),
			"*SERVER*"
		);
#endif

		l_tpALifeDynamicObject->o_Position = object->o_Position;
		l_tpALifeDynamicObject->m_tNodeID = object->m_tNodeID;
		object->alife().server().Process_spawn(tNetPacket, clientID,FALSE, l_tpALifeInventoryItem->base());
		l_tpALifeDynamicObject->s_flags.and(u16(-1) ^ M_SPAWN_UPDATE);
		l_tpALifeDynamicObject->m_bOnline = true;
	}

	CSE_ALifeDynamicObjectVisual::add_online(update_registries);
}

void CSE_ALifeInventoryBox::add_offline(const xr_vector<ALife::_OBJECT_ID>& saved_children,
                                        const bool& update_registries)
{
	CSE_ALifeDynamicObjectVisual* object = (this);

	for (u32 i = 0, n = saved_children.size(); i < n; ++i)
	{
		CSE_ALifeDynamicObject* child = smart_cast<CSE_ALifeDynamicObject*>(
			ai().alife().objects().object(saved_children[i], true));
		// R_ASSERT(child);
		if (!child)
		{
			Msg("[DO] can't switch child [%d] offline, it's null", saved_children[i]);
			continue;
		}
		child->m_bOnline = false;

		CSE_ALifeInventoryItem* inventory_item = smart_cast<CSE_ALifeInventoryItem*>(child);
		VERIFY2(inventory_item, "Non inventory item object has parent?!");
#ifdef DEBUG
		//		if (psAI_Flags.test(aiALife))
//			Msg					("[LSS] Destroying item [%s][%s][%d]",inventory_item->base()->name_replace(),*inventory_item->base()->s_name,inventory_item->base()->ID);
		Msg						(
			"[LSS][%d] Going offline [%d][%s][%d] with parent [%d][%s] on '%s'",
			Device.dwFrame,
			Device.dwTimeGlobal,
			inventory_item->base()->name_replace(),
			inventory_item->base()->ID,
			ID,
			name_replace(),
			"*SERVER*"
		);
#endif

		ALife::_OBJECT_ID item_id = inventory_item->base()->ID;
		inventory_item->base()->ID = object->alife().server().PerformIDgen(item_id);

		if (!child->can_save())
		{
			object->alife().release(child);
			--i;
			--n;
			continue;
		}
		child->clear_client_data();
		object->alife().graph().add(child, child->m_tGraphID, false);
		//		object->alife().graph().attach	(*object,inventory_item,child->m_tGraphID,true);
		alife().graph().remove(child, child->m_tGraphID);
		children.push_back(child->ID);
		child->ID_Parent = ID;
	}


	CSE_ALifeDynamicObjectVisual::add_offline(saved_children, update_registries);
}

void CSE_ALifeDynamicObject::clear_client_data()
{
#ifdef DEBUG
	if (!client_data.empty())
		Msg						("CSE_ALifeDynamicObject::switch_offline: client_data is cleared for [%d][%s]",ID,name_replace());
#endif // DEBUG
	if (!keep_saved_data_anyway())
		client_data.clear();
}

void CSE_ALifeDynamicObject::on_failed_switch_online()
{
	clear_client_data();
}
