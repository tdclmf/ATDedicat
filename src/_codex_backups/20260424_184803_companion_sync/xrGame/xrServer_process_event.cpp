#include "stdafx.h"
#include "xrServer.h"
#include "game_sv_single.h"
#include "alife_simulator.h"
#include "xrserver_objects.h"
#include "game_base.h"
#include "game_cl_base.h"
#include "level.h"
#include "ai_space.h"
#include "alife_object_registry.h"
#include "xrServer_Objects_ALife_Items.h"
#include "xrServer_Objects_ALife_Monsters.h"
#include "weapon_trace.h"
#include <algorithm>

namespace
{
IC bool is_companion_info_id(LPCSTR info_id)
{
	if (!info_id || !*info_id)
		return false;

	return
		(0 == strncmp(info_id, "npcx_", 5)) ||
		(0 == xr_strcmp(info_id, "npcx_is_companion"));
}
}

void xrServer::Process_event(NET_Packet& P, ClientID sender)
{
#	ifdef SLOW_VERIFY_ENTITIES
			VERIFY					(verify_entities());
#	endif

	u32 timestamp;
	u16 type;
	u16 destination;
	u32 MODE = net_flags(TRUE,TRUE);

	// correct timestamp with server-unique-time (note: direct message correction)
	P.r_u32(timestamp);

	// read generic info
	P.r_u16(type);
	P.r_u16(destination);
	const u32 payload_pos = P.r_tell();

	CSE_Abstract* receiver = game->get_entity_from_eid(destination);
	if (receiver)
	{
		R_ASSERT(receiver->owner);
		receiver->OnEvent(P, type, timestamp, sender);
	};

	switch (type)
	{
	case GE_GAME_EVENT:
		{
			u16 game_event_type;
			P.r_u16(game_event_type);
			game->AddDelayedEvent(P, game_event_type, timestamp, sender);
		}
		break;
	case GE_INFO_TRANSFER:
		{
			P.r_seek(payload_pos);

			u16 source_id = u16(-1);
			shared_str info_id;
			u8 add_info = 0;
			P.r_u16(source_id);
			P.r_stringZ(info_id);
			P.r_u8(add_info);

			if (is_companion_info_id(*info_id))
			{
				CALifeSimulator* sim = ai().get_alife();
				if (sim)
				{
					auto* known_info = sim->registry(info_portions).object(destination, true);
					if (known_info)
					{
						auto it = std::find(known_info->begin(), known_info->end(), info_id);
						if (add_info)
						{
							if (it == known_info->end())
								known_info->push_back(info_id);
						}
						else
						{
							if (it != known_info->end())
								known_info->erase(it);
						}

						Msg("* [COMP_SV_SYNC] dst=%u src=%u info=%s add=%u size=%u",
							destination, source_id, *info_id, u32(add_info), u32(known_info->size()));
					}
					else
					{
						Msg("! [COMP_SV_SYNC] no info registry for dst=%u info=%s add=%u",
							destination, *info_id, u32(add_info));
					}
				}
				else
				{
					Msg("! [COMP_SV_SYNC] no ALife for dst=%u info=%s add=%u",
						destination, *info_id, u32(add_info));
				}
			}

			// Keep packet read cursor consistent for downstream handling/broadcast.
			P.r_seek(payload_pos);
			SendBroadcast(BroadcastCID, P, MODE);
		}
		break;
	case GE_WPN_STATE_CHANGE:
	case GE_ZONE_STATE_CHANGE:
	case GE_ACTOR_JUMPING:
	case GEG_PLAYER_PLAY_HEADSHOT_PARTICLE:
	case GEG_PLAYER_ATTACH_HOLDER:
	case GEG_PLAYER_DETACH_HOLDER:
	case GEG_PLAYER_ITEM2SLOT:
	case GEG_PLAYER_ITEM2BELT:
	case GEG_PLAYER_ITEM2RUCK:
		{
			SendBroadcast(BroadcastCID, P, MODE);
		}
		break;
	case GE_GRENADE_EXPLODE:
		{
			xrClientData* sender_client = ID_to_client(sender);
			WPN_TRACE("xrServer::Process_event GE_GRENADE_EXPLODE relay sender=0x%08x dest=%u sender_client=%s sv_client=%s",
				sender.value(),
				destination,
				sender_client ? sender_client->name.c_str() : "<none>",
				SV_Client ? SV_Client->name.c_str() : "<none>");
			SendBroadcast(BroadcastCID, P, MODE);
		}
		break;
	case GEG_PLAYER_ACTIVATEARTEFACT:
		{
			Process_event_activate(P, sender, timestamp, destination, P.r_u16(), true);
			break;
		};
	case GE_INV_ACTION:
		{
			xrClientData* CL = ID_to_client(sender);
			if (CL) CL->net_Ready = TRUE;
			WPN_TRACE("xrServer::Process_event GE_INV_ACTION sender=0x%08x dest=%u sv_client=%u sender_client=%s receiver=%s",
				sender.value(),
				destination,
				SV_Client ? SV_Client->ID.value() : 0,
				CL ? CL->name.c_str() : "<none>",
				receiver ? receiver->name_replace() : "<none>");
			if (SV_Client) SendTo(SV_Client->ID, P, net_flags(TRUE, TRUE));
			else WPN_TRACE("xrServer::Process_event GE_INV_ACTION: SV_Client is null, packet dropped sender=0x%08x", sender.value());
		}
		break;
	case GE_WPN_AMMO_SYNC:
		{
			xrClientData* CL = ID_to_client(sender);
			if (CL) CL->net_Ready = TRUE;
			WPN_TRACE("xrServer::Process_event GE_WPN_AMMO_SYNC sender=0x%08x dest=%u sv_client=%u sender_client=%s receiver=%s",
				sender.value(),
				destination,
				SV_Client ? SV_Client->ID.value() : 0,
				CL ? CL->name.c_str() : "<none>",
				receiver ? receiver->name_replace() : "<none>");
			if (SV_Client) SendTo(SV_Client->ID, P, net_flags(TRUE, TRUE));
			else WPN_TRACE("xrServer::Process_event GE_WPN_AMMO_SYNC: SV_Client is null, packet dropped sender=0x%08x", sender.value());
		}
		break;
	case GE_RESPAWN:
		{
			CSE_Abstract* E = receiver;
			if (E)
			{
				R_ASSERT(E->s_flags.is(M_SPAWN_OBJECT_PHANTOM));

				svs_respawn R;
				R.timestamp = timestamp + E->RespawnTime * 1000;
				R.phantom = destination;
				q_respawn.insert(R);
			}
		}
		break;
	case GE_TRADE_BUY:
	case GE_OWNERSHIP_TAKE:
		{
			Process_event_ownership(P, sender, timestamp, destination);
#ifdef DEBUG
			VERIFY(verify_entities());
#endif
		}
		break;
	case GE_OWNERSHIP_TAKE_MP_FORCED:
		{
			Process_event_ownership(P, sender, timestamp, destination,TRUE);
#ifdef DEBUG
			VERIFY(verify_entities());
#endif
		}
		break;
	case GE_TRADE_SELL:
	case GE_OWNERSHIP_REJECT:
		{
			Process_event_reject(P, sender, timestamp, destination, P.r_u16());
#ifdef DEBUG
			VERIFY(verify_entities());
#endif
		}
		break;
	case GE_LAUNCH_ROCKET:
		{
			Process_event_reject(P, sender, timestamp, destination, P.r_u16(), true, true);
#ifdef DEBUG
			VERIFY(verify_entities());
#endif
		}
		break;
	case GE_DESTROY:
		{
			Process_event_destroy(P, sender, timestamp, destination, NULL);
#ifdef DEBUG
			VERIFY(verify_entities());
#endif
		}
		break;
	case GE_TRANSFER_AMMO:
		{
			u16 id_entity;
			P.r_u16(id_entity);
			CSE_Abstract* e_parent = receiver; // кто забирает (для своих нужд)
			CSE_Abstract* e_entity = game->get_entity_from_eid(id_entity); // кто отдает
			if (!e_entity) break;
			if (0xffff != e_entity->ID_Parent) break; // this item already taken
			xrClientData* c_parent = e_parent->owner;
			xrClientData* c_from = ID_to_client(sender);
			R_ASSERT(c_from == c_parent); // assure client ownership of event

			// Signal to everyone (including sender)
			SendBroadcast(BroadcastCID, P, MODE);

			// Perfrom real destroy
			entity_Destroy(e_entity);
#ifdef DEBUG
			VERIFY(verify_entities());
#endif
		}
		break;
	case GE_HIT:
	case GE_HIT_STATISTIC:
		{
			P.r_pos -= 2;
			if (type == GE_HIT_STATISTIC)
			{
				P.B.count -= 4;
				P.w_u32(sender.value());
			};
			game->AddDelayedEvent(P, GAME_EVENT_ON_HIT, 0, ClientID());
		}
		break;
	case GE_ASSIGN_KILLER:
		{
			u16 id_src;
			P.r_u16(id_src);

			CSE_Abstract* e_dest = receiver; // кто умер
			// this is possible when hit event is sent before destroy event
			if (!e_dest)
				break;

			CSE_ALifeCreatureAbstract* creature = smart_cast<CSE_ALifeCreatureAbstract*>(e_dest);
			if (creature)
				creature->set_killer_id(id_src);

			//		Msg							("[%d][%s] killed [%d][%s]",id_src,id_src==u16(-1) ? "UNKNOWN" : game->get_entity_from_eid(id_src)->name_replace(),id_dest,e_dest->name_replace());

			break;
		}
	case GE_CHANGE_VISUAL:
		{
			CSE_Visual* visual = smart_cast<CSE_Visual*>(receiver);
			VERIFY(visual);
			string256 tmp;
			P.r_stringZ(tmp);

			xrClientData* sender_client = ID_to_client(sender);
			const bool sender_is_server_client = sender_client && (sender_client == GetServerClient());
			const bool sender_owns_receiver = sender_client && sender_client->owner && sender_client->owner->ID == destination;
			if (!sender_is_server_client && !sender_owns_receiver)
			{
				Msg("! [SV] GE_CHANGE_VISUAL rejected: sender 0x%08x does not own object [%u].", sender.value(), destination);
				break;
			}

			if (tmp[0])
				visual->set_visual(tmp);

			if (sender_owns_receiver && tmp[0])
			{
				sender_client->m_requested_player_visual = tmp;
				Msg("--- [SV] Dedicated single bridge: cached player visual [%s] for [%s] (id=%u).",
					tmp, sender_client->name.c_str(), destination);
			}

			NET_Packet visual_sync;
			visual_sync.w_begin(M_EVENT);
			visual_sync.w_u32(timestamp);
			visual_sync.w_u16(GE_CHANGE_VISUAL);
			visual_sync.w_u16(destination);
			visual_sync.w_stringZ(tmp);
			SendBroadcast(BroadcastCID, visual_sync, MODE);
		}
		break;
	case GE_DIE:
		{
			// Parse message
			u16 id_dest = destination, id_src;
			P.r_u16(id_src);


			xrClientData* l_pC = ID_to_client(sender);
			VERIFY(game && l_pC);

			if (l_pC && l_pC->owner)
				Msg("* [%2d] killed by [%2d] - sended by [0x%08x]", id_dest, id_src, l_pC->ID.value());

			CSE_Abstract* e_dest = receiver; // кто умер
			// this is possible when hit event is sent before destroy event
			if (!e_dest)
				break;

			Msg("* [%2d] is [%s:%s]", id_dest, *e_dest->s_name, e_dest->name_replace());

			CSE_Abstract* e_src = game->get_entity_from_eid(id_src); // кто убил
			if (!e_src)
			{
				xrClientData* C = (xrClientData*)game->get_client(id_src);
				if (C) e_src = C->owner;
			};
			VERIFY(e_src);
			if (!e_src)
			{
				Msg("! ERROR: SV: src killer not exist.");
				return;
			}
			
			Msg("* [%2d] is [%s:%s]", id_src, *e_src->s_name, e_src->name_replace());

			game->on_death(e_dest, e_src);

			// Killer source may be an ALife entity without a live client owner.
			xrClientData* c_src = (xrClientData*)game->get_client(id_src);

			if (c_src && c_src->owner && c_src->owner->ID == id_src)
			{
				// Main unit
				P.w_begin(M_EVENT);
				P.w_u32(timestamp);
				P.w_u16(type);
				P.w_u16(destination);
				P.w_u16(id_src);
				P.w_clientID(c_src->ID);
			}

			SendBroadcast(BroadcastCID, P, MODE);

			//////////////////////////////////////////////////////////////////////////
			// 
			if (OnServer() && c_src) // werasik2aa not sure
			{
				P.w_begin(M_EVENT);
				P.w_u32(timestamp);
				P.w_u16(GE_KILL_SOMEONE);
				P.w_u16(id_src);
				P.w_u16(destination);
				SendTo(c_src->ID, P, net_flags(TRUE, TRUE));
			}
			//////////////////////////////////////////////////////////////////////////
#ifdef DEBUG
			VERIFY(verify_entities());
#endif
		}
		break;
	case GE_ADDON_ATTACH:
	case GE_ADDON_DETACH:
		{
			SendBroadcast(BroadcastCID, P, net_flags(TRUE, TRUE));
		}
		break;
	case GE_CHANGE_POS:
		{
			SendTo(SV_Client->ID, P, net_flags(TRUE, TRUE));
		}
		break;
	case GE_INSTALL_UPGRADE:
		{
			shared_str upgrade_id;
			P.r_stringZ(upgrade_id);
			CSE_ALifeInventoryItem* iitem = smart_cast<CSE_ALifeInventoryItem*>(receiver);
			if (!iitem)
			{
				break;
			}
			iitem->add_upgrade(upgrade_id);
		}
		break;
	case GE_INV_BOX_STATUS:
		{
			u8 can_take, closed;
			P.r_u8(can_take);
			P.r_u8(closed);
			shared_str tip_text;
			P.r_stringZ(tip_text);

			CSE_ALifeInventoryBox* box = smart_cast<CSE_ALifeInventoryBox*>(receiver);
			if (!box)
			{
				break;
			}
			box->m_can_take = (can_take == 1);
			box->m_closed = (closed == 1);
			box->m_tip_text._set(tip_text);
		}
		break;
	case GE_INV_OWNER_STATUS:
		{
			u8 can_take, closed;
			P.r_u8(can_take);
			P.r_u8(closed);

			CSE_ALifeTraderAbstract* iowner = smart_cast<CSE_ALifeTraderAbstract*>(receiver);
			if (!iowner)
			{
				break;
			}
			iowner->m_deadbody_can_take = (can_take == 1);
			iowner->m_deadbody_closed = (closed == 1);
		}
		break;

	case GEG_PLAYER_DISABLE_SPRINT:
	case GEG_PLAYER_WEAPON_HIDE_STATE:
		{
			WPN_TRACE("xrServer::Process_event route event type=%u to SV_Client=%u destination=%u",
				type, SV_Client ? SV_Client->ID.value() : 0, destination);
			SendTo(SV_Client->ID, P, net_flags(TRUE, TRUE));

#	ifdef SLOW_VERIFY_ENTITIES
			VERIFY					(verify_entities());
#	endif
		}
		break;
	case GEG_PLAYER_ACTIVATE_SLOT:
	case GEG_PLAYER_ITEM_EAT:
		{
			WPN_TRACE("xrServer::Process_event route event type=%u to SV_Client=%u destination=%u",
				type, SV_Client ? SV_Client->ID.value() : 0, destination);
			SendTo(SV_Client->ID, P, net_flags(TRUE, TRUE));
#	ifdef SLOW_VERIFY_ENTITIES
			VERIFY					(verify_entities());
#	endif
		}
		break;
	case GEG_PLAYER_USE_BOOSTER:
		{
			if (receiver && receiver->owner && (receiver->owner != SV_Client))
			{
				NET_Packet tmp_packet;
				CGameObject::u_EventGen(tmp_packet, GEG_PLAYER_USE_BOOSTER, receiver->ID);
				SendTo(receiver->owner->ID, P, net_flags(TRUE, TRUE));
			}
		}
		break;
	case GEG_PLAYER_ITEM_SELL:
		{
			game->OnPlayer_Sell_Item(sender, P);
		}
		break;
	case GE_TELEPORT_OBJECT:
		{
			game->teleport_object(P, destination);
		}
		break;
	case GE_ADD_RESTRICTION:
		{
			game->add_restriction(P, destination);
		}
		break;
	case GE_REMOVE_RESTRICTION:
		{
			game->remove_restriction(P, destination);
		}
		break;
	case GE_REMOVE_ALL_RESTRICTIONS:
		{
			game->remove_all_restrictions(P, destination);
		}
		break;
	case GE_MONEY:
		{
			CSE_Abstract* e_dest = receiver;
			CSE_ALifeTraderAbstract* pTa = smart_cast<CSE_ALifeTraderAbstract*>(e_dest);
			pTa->m_dwMoney = P.r_u32();
		}
		break;
	case GE_TRADER_FLAGS:
		{
			CSE_ALifeTraderAbstract* pTa = smart_cast<CSE_ALifeTraderAbstract*>(receiver);
			if (pTa)
			{
				pTa->m_trader_flags.assign(P.r_u32());
			}
		}
		break;
	case GE_FREEZE_OBJECT:
		break;
	case GE_REQUEST_PLAYERS_INFO:
		{
			SendPlayersInfo(sender);
		}
		break;
	default:
		R_ASSERT2(0, "Game Event not implemented!!!");
		break;
	}
}

