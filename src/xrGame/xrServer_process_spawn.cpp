#include "stdafx.h"
#include "xrServer.h"
#include "xrserver_objects.h"
#include "xrServer_Objects_ALife_All.h"

#ifdef DEBUG
#	include "xrserver_objects_alife_items.h"
#endif

CSE_Abstract* xrServer::Process_spawn(NET_Packet& P, ClientID sender, BOOL bSpawnWithClientsMainEntityAsParent,
                                      CSE_Abstract* tpExistedEntity)
{
	// create server entity
	xrClientData* CL = ID_to_client(sender);
	CSE_Abstract* E = tpExistedEntity;
	if (!E)
	{
		// read spawn information
		string64 s_name;
		P.r_stringZ(s_name);
		// create entity
		E = entity_Create(s_name);
		R_ASSERT3(E, "Can't create entity.", s_name);
		E->Spawn_Read(P);
		if (
				//.				!( (game->Type()==E->s_gameid) || (GAME_ANY==E->s_gameid) ) ||

			!E->m_gameType.MatchType((u16)game->Type()) ||
			!E->match_configuration() ||
			!game->OnPreCreate(E)
		)
		{
#ifndef MASTER_GOLD
			Msg			("- SERVER: Entity [%s] incompatible with current game type.",*E->s_name);
#endif // #ifndef MASTER_GOLD
			F_entity_Destroy(E);
			return NULL;
		}

		//		E->m_bALifeControl = false;
	}
	else
	{
		VERIFY(E->m_bALifeControl);
		//		E->owner			= CL;
		//		if (CL != NULL)
		//		{
		//			int x=0;
		//			x=x;
		//		};
		//		E->m_bALifeControl = true;
	}

	CSE_Abstract* e_parent = 0;
	if (E->ID_Parent != 0xffff)
	{
		e_parent = ID_to_entity(E->ID_Parent);
		if (!e_parent)
		{
			R_ASSERT(!tpExistedEntity);
			//			VERIFY3			(smart_cast<CSE_ALifeItemBolt*>(E) || smart_cast<CSE_ALifeItemGrenade*>(E),*E->s_name,E->name_replace());
			F_entity_Destroy(E);
			return NULL;
		}
	}

	// Dedicated-single bridge stability:
	// if the same ALife root object is requested twice, drop the duplicate spawn early.
	// This prevents visible "double NPC" cases caused by repeated spawn requests.
	if (!tpExistedEntity && !E->s_flags.is(M_SPAWN_OBJECT_PHANTOM) && E->ID_Parent == u16(-1))
	{
		CSE_ALifeObject* incoming_alife = smart_cast<CSE_ALifeObject*>(E);
		const bool has_story_id = incoming_alife && incoming_alife->m_story_id != ALife::_STORY_ID(-1);
		const bool has_spawn_story_id = incoming_alife && incoming_alife->m_spawn_story_id != ALife::_SPAWN_STORY_ID(-1);
		const bool has_spawn_id = E->m_tSpawnID != ALife::_SPAWN_ID(-1);

		if (has_story_id || has_spawn_story_id || has_spawn_id)
		{
			for (xrS_entities::const_iterator it = entities.begin(); it != entities.end(); ++it)
			{
				CSE_Abstract* existing = it->second;
				if (!existing || existing == E)
					continue;
				if (existing->s_flags.is(M_SPAWN_OBJECT_PHANTOM))
					continue;
				if (existing->ID_Parent != u16(-1))
					continue;

				const bool same_section = (0 == xr_strcmp(*existing->s_name, *E->s_name));
				if (!same_section)
					continue;

				CSE_ALifeObject* existing_alife = smart_cast<CSE_ALifeObject*>(existing);
				const bool same_story_id =
					has_story_id &&
					existing_alife &&
					(existing_alife->m_story_id == incoming_alife->m_story_id);
				const bool same_spawn_story_id =
					has_spawn_story_id &&
					existing_alife &&
					(existing_alife->m_spawn_story_id == incoming_alife->m_spawn_story_id);
				const bool same_spawn_id = has_spawn_id && (existing->m_tSpawnID == E->m_tSpawnID);

				if (!(same_story_id || same_spawn_story_id || same_spawn_id))
					continue;

				LPCSTR reason = same_story_id ? "story_id" : (same_spawn_story_id ? "spawn_story_id" : "spawn_id");
				Msg("! [SV_DUP_SPAWN] Skip duplicate spawn by %s: sec=[%s] new_name=[%s] old_name=[%s] new_story=%u old_story=%u new_spawn_story=%u old_spawn_story=%u new_spawn_id=%u old_spawn_id=%u",
					reason,
					*E->s_name,
					E->name_replace() ? E->name_replace() : "",
					existing->name_replace() ? existing->name_replace() : "",
					incoming_alife ? incoming_alife->m_story_id : u32(-1),
					existing_alife ? existing_alife->m_story_id : u32(-1),
					incoming_alife ? incoming_alife->m_spawn_story_id : u32(-1),
					existing_alife ? existing_alife->m_spawn_story_id : u32(-1),
					E->m_tSpawnID,
					existing->m_tSpawnID);

				F_entity_Destroy(E);
				return existing;
			}
		}
	}

	// Secondary duplicate guard for named creatures:
	// some broken sync paths can spawn the same creature twice with different server IDs
	// while story/spawn IDs are invalid or lost. Guard by section + name_replace.
	if (!tpExistedEntity &&
		!E->s_flags.is(M_SPAWN_OBJECT_PHANTOM) &&
		!E->s_flags.is(M_SPAWN_OBJECT_ASPLAYER) &&
		E->ID_Parent == u16(-1))
	{
		CSE_ALifeCreatureAbstract* incoming_creature = smart_cast<CSE_ALifeCreatureAbstract*>(E);
		LPCSTR incoming_name_replace = E->name_replace();
		const bool has_name_replace = incoming_name_replace && incoming_name_replace[0] != 0;
		if (incoming_creature && has_name_replace)
		{
			for (xrS_entities::const_iterator it = entities.begin(); it != entities.end(); ++it)
			{
				CSE_Abstract* existing = it->second;
				if (!existing || existing == E)
					continue;
				if (existing->s_flags.is(M_SPAWN_OBJECT_PHANTOM))
					continue;
				if (existing->s_flags.is(M_SPAWN_OBJECT_ASPLAYER))
					continue;
				if (existing->ID_Parent != u16(-1))
					continue;

				CSE_ALifeCreatureAbstract* existing_creature = smart_cast<CSE_ALifeCreatureAbstract*>(existing);
				if (!existing_creature)
					continue;

				LPCSTR existing_name_replace = existing->name_replace();
				if (0 != xr_strcmp(*existing->s_name, *E->s_name))
					continue;

				const bool same_name_replace =
					existing_name_replace && existing_name_replace[0] &&
					(0 == xr_strcmp(existing_name_replace, incoming_name_replace));

				if (!same_name_replace)
					continue;

				LPCSTR reason = "name_replace";
				Msg("! [SV_DUP_SPAWN] Skip duplicate creature spawn by %s: sec=[%s] name=[%s] old_id=%u new_id=%u sender=%u old_spawn_id=%u new_spawn_id=%u old_story=%u new_story=%u",
					reason,
					*E->s_name,
					incoming_name_replace,
					existing->ID,
					E->ID,
					sender.value(),
					existing->m_tSpawnID,
					E->m_tSpawnID,
					existing_creature->m_story_id,
					incoming_creature->m_story_id);

				F_entity_Destroy(E);
				return existing;
			}
		}
	}

	// check if we can assign entity to some client
	if (0 == CL)
	{
		CL = SelectBestClientToMigrateTo(E);
	}

	// check for respawn-capability and create phantom as needed
	if (E->RespawnTime && (0xffff == E->ID_Phantom))
	{
		// Create phantom
		CSE_Abstract* Phantom = entity_Create(*E->s_name);
		R_ASSERT(Phantom);
		Phantom->Spawn_Read(P);
		Phantom->ID = PerformIDgen(0xffff);
		Phantom->ID_Phantom = Phantom->ID; // Self-linked to avoid phantom-breeding
		Phantom->owner = NULL;
		entities.insert(mk_pair(Phantom->ID, Phantom));

		Phantom->s_flags.set(M_SPAWN_OBJECT_PHANTOM,TRUE);

		// Spawn entity
		E->ID = PerformIDgen(E->ID);
		E->ID_Phantom = Phantom->ID;
		E->owner = CL;
		entities.insert(mk_pair(E->ID, E));
	}
	else
	{
		if (E->s_flags.is(M_SPAWN_OBJECT_PHANTOM))
		{
			// Clone from Phantom
			E->ID = PerformIDgen(0xffff);
			E->owner = CL; //		= SelectBestClientToMigrateTo	(E);
			E->s_flags.set(M_SPAWN_OBJECT_PHANTOM,FALSE);
			entities.insert(mk_pair(E->ID, E));
		}
		else
		{
			// Simple spawn
			if (bSpawnWithClientsMainEntityAsParent)
			{
				R_ASSERT(CL);
				CSE_Abstract* P = CL->owner;
				R_ASSERT(P);
				E->ID_Parent = P->ID;
			}
			E->ID = PerformIDgen(E->ID);
			E->owner = CL;
			entities.insert(mk_pair(E->ID, E));
		}
	}

	// PROCESS NAME; Name this entity
	if (CL && (E->s_flags.is(M_SPAWN_OBJECT_ASPLAYER)))
	{
		CL->owner = E;
		//		E->set_name_replace	(CL->Name);
	}

	// PROCESS RP;	 3D position/orientation
	PerformRP(E);
	E->s_RP = 0xFE; // Use supplied

	// Parent-Connect
	if (!tpExistedEntity)
	{
		game->OnCreate(E->ID);

		if (0xffff != E->ID_Parent)
		{
			R_ASSERT(e_parent);

			game->OnTouch(E->ID_Parent, E->ID);

			e_parent->children.push_back(E->ID);
		}
	}

	// create packet and broadcast packet to everybody
	NET_Packet Packet;
	if (CL)
	{
		// For local ONLY
		E->Spawn_Write(Packet,TRUE);
		if (E->s_flags.is(M_SPAWN_UPDATE))
			E->UPDATE_Write(Packet);
		SendTo(CL->ID, Packet, net_flags(TRUE,TRUE));

		// For everybody, except client, which contains authorative copy
		E->Spawn_Write(Packet,FALSE);
		if (E->s_flags.is(M_SPAWN_UPDATE))
			E->UPDATE_Write(Packet);
		SendBroadcast(CL->ID, Packet, net_flags(TRUE,TRUE));
	}
	else
	{
		E->Spawn_Write(Packet,FALSE);
		if (E->s_flags.is(M_SPAWN_UPDATE))
			E->UPDATE_Write(Packet);
		ClientID clientID;
		clientID.set(0);
		SendBroadcast(clientID, Packet, net_flags(TRUE,TRUE));
	}
	if (!tpExistedEntity)
	{
		game->OnPostCreate(E->ID);
	};

	// log
	//Msg		("- SERVER: Spawning '%s'(%d,%d,%d) as #%d, on '%s'", E->s_name_replace, E->g_team(), E->g_squad(), E->g_group(), E->ID, CL?CL->Name:"*SERVER*");
	return E;
}

/*
void spawn_WithPhantom
void spawn_FromPhantom
void spawn_Simple
*/
