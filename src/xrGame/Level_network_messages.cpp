#include "stdafx.h"
#include "entity.h"
#include "xrserver_objects.h"
#include "level.h"
#include "xrmessages.h"
#include "game_cl_base.h"
#include "net_queue.h"
//#include "Physics.h"
#include "xrServer.h"
#include "Actor.h"
#include "actor_mp_client.h"
#include "Artefact.h"
#include "ai_space.h"
#include "saved_game_wrapper.h"
#include "level_graph.h"
#include "file_transfer.h"
#include "message_filter.h"
#include "weapon_trace.h"
#include "../xrphysics/iphworld.h"
#include <unordered_map>

extern LPCSTR map_ver_string;
extern ENGINE_API bool g_dedicated_server;
// True only while processing relayed M_CL_UPDATE for actor import.
// Dedicated-single bridge uses this to prevent M_UPDATE_OBJECTS from overwriting remote actor orientation.
bool g_dedicated_single_import_from_cl_update = false;

namespace
{
struct SCrowClientSyncState
{
	std::unordered_map<u16, u32> last_touch_time;
	std::unordered_map<u16, u32> last_log_time;
	std::unordered_map<u16, Fvector> last_touch_position;
};

SCrowClientSyncState g_crow_client_sync_state;

void apply_client_crow_touch(CLevel& level, u16 actor_id, const Fvector& actor_pos)
{
	if (!level.IsServer() || Game().Type() != eGameIDSingle)
		return;
	if (actor_id == 0)
		return;

	const u32 now = Device.dwTimeGlobal;
	u32& last_touch = g_crow_client_sync_state.last_touch_time[actor_id];
	if (last_touch && (now - last_touch) < 200)
		return;

	auto pos_it = g_crow_client_sync_state.last_touch_position.find(actor_id);
	if (pos_it != g_crow_client_sync_state.last_touch_position.end())
	{
		// If actor barely moved since previous touch, lower update pressure.
		const float move_sqr = pos_it->second.distance_to_sqr(actor_pos);
		if (move_sqr < (1.0f * 1.0f) && last_touch && (now - last_touch) < 400)
			return;
		pos_it->second = actor_pos;
	}
	else
	{
		g_crow_client_sync_state.last_touch_position.emplace(actor_id, actor_pos);
	}

	last_touch = now;

	const float touch_radius_sqr = CROW_RADIUS2 * CROW_RADIUS2;
	u32 touched_count = 0;

	const u32 object_count = level.Objects.o_count();
	for (u32 i = 0; i < object_count; ++i)
	{
		CObject* object = level.Objects.o_get_by_iterator(i);
		if (!object || object->ID() == actor_id || object->getDestroy() || !object->processing_enabled())
			continue;

		if (object->Position().distance_to_sqr(actor_pos) > touch_radius_sqr)
			continue;

		object->MakeMeCrow();
		++touched_count;
		if (touched_count >= 512)
			break;
	}

	u32& last_log = g_crow_client_sync_state.last_log_time[actor_id];
	if (!last_log || (now - last_log) >= 5000)
	{
		Msg("* [CROW_SYNC] actor=%u touched=%u pos=(%.2f %.2f %.2f)",
		    actor_id, touched_count, actor_pos.x, actor_pos.y, actor_pos.z);
		last_log = now;
	}
}
} // namespace

LPSTR remove_version_option(LPCSTR opt_str, LPSTR new_opt_str, u32 new_opt_str_size)
{
	LPCSTR temp_substr = strstr(opt_str, map_ver_string);
	if (!temp_substr)
	{
		xr_strcpy(new_opt_str, new_opt_str_size, opt_str);
		return new_opt_str;
	}
	strncpy_s(new_opt_str, new_opt_str_size, opt_str, static_cast<size_t>(temp_substr - opt_str - 1));
	temp_substr = strchr(temp_substr, '/');
	if (!temp_substr)
		return new_opt_str;

	xr_strcat(new_opt_str, new_opt_str_size, temp_substr);
	return new_opt_str;
}

#ifdef DEBUG
s32 lag_simmulator_min_ping	= 0;
s32 lag_simmulator_max_ping	= 0;
static bool SimmulateNetworkLag()
{
	static u32 max_lag_time	= 0;

	if (!lag_simmulator_max_ping && !lag_simmulator_min_ping)
		return false;
	
	if (!max_lag_time || (max_lag_time <= Device.dwTimeGlobal))
	{
		CRandom				tmp_random(Device.dwTimeGlobal);
		max_lag_time		= Device.dwTimeGlobal + tmp_random.randI(lag_simmulator_min_ping, lag_simmulator_max_ping);
		return false;
	}
	return true;
}
#endif

void CLevel::ClientReceive()
{
	PROF_EVENT("CLevel::ClientReceive");
	m_dwRPC = 0;
	m_dwRPS = 0;

	if (IsDemoPlayStarted())
	{
		SimulateServerUpdate();
	}
#ifdef DEBUG
	if (SimmulateNetworkLag())
		return;
#endif
	StartProcessQueue();
	ReceivePackets();
	for (NET_Packet* P = net_msg_Retrieve(); P; P = net_msg_Retrieve())
	{
		if (IsDemoSaveStarted())
		{
			SavePacket(*P);
		}
		//-----------------------------------------------------
		m_dwRPC++;
		m_dwRPS += P->B.count;
		//-----------------------------------------------------
		u16 m_type;
		u16 ID;
		P->r_begin(m_type);
		switch (m_type)
		{
		case M_SPAWN:
			{
				if (!bReady) //!m_bGameConfigStarted || 
				{
					Msg("! Unconventional M_SPAWN received : map_data[%s] | bReady[%s] | deny_m_spawn[%s]",
					    (map_data.m_map_sync_received) ? "true" : "false",
					    (bReady) ? "true" : "false",
					    deny_m_spawn ? "true" : "false");
					break;
				}
				game_events->insert(*P);
				if (g_bDebugEvents) ProcessGameEvents();
			}
			break;
		case M_EVENT:
			game_events->insert(*P);
			if (g_bDebugEvents) ProcessGameEvents();
			break;
		case M_EVENT_PACK:
			{
				NET_Packet tmpP;
				while (!P->r_eof())
				{
					tmpP.B.count = P->r_u8();
					P->r(&tmpP.B.data, tmpP.B.count);
					tmpP.timeReceive = P->timeReceive;

					game_events->insert(tmpP);
					if (g_bDebugEvents) ProcessGameEvents();
				};
			}
			break;
		case M_UPDATE:
			{
				game->net_import_update(*P);
			}
			break;
		case M_UPDATE_OBJECTS:
			{
				u32 imported_objects = 0;
				bool malformed_packet = false;
				if (OnClient() && !OnServer() && (Game().Type() == eGameIDSingle))
				{
					NET_Packet stats_packet = *P;
					while (!stats_packet.r_eof())
					{
						if (stats_packet.r_elapsed() < (sizeof(u16) + sizeof(u8)))
						{
							malformed_packet = true;
							break;
						}

						u16 object_id = 0;
						u8 object_size = 0;
						stats_packet.r_u16(object_id);
						stats_packet.r_u8(object_size);
						(void)object_id;

						if (object_size > stats_packet.r_elapsed())
						{
							malformed_packet = true;
							break;
						}

						stats_packet.r_advance(object_size);
						++imported_objects;
					}

					static u32 s_cl_update_objects_log_time = 0;
					if (Device.dwTimeGlobal >= s_cl_update_objects_log_time)
					{
						Msg("* [CL_NET_UPD] M_UPDATE_OBJECTS bytes=%u objects=%u malformed=%d",
						    P->B.count, imported_objects, malformed_packet ? 1 : 0);
						s_cl_update_objects_log_time = Device.dwTimeGlobal + 5000;
					}
				}

				Objects.net_Import(P);

				if (OnClient()) UpdateDeltaUpd(timeServer());
				IClientStatistic pStat = Level().GetStatistic();
				u32 dTime = 0;

				if ((Level().timeServer() + pStat.getPing()) < P->timeReceive)
				{
					dTime = pStat.getPing();
				}
				else
					dTime = Level().timeServer() - P->timeReceive + pStat.getPing();

				u32 NumSteps = physics_world()->CalcNumSteps(dTime);
				SetNumCrSteps(NumSteps);
			}
			break;
		case M_COMPRESSED_UPDATE_OBJECTS:
			{
				u8 compression_type = P->r_u8();
				ProcessCompressedUpdate(*P, compression_type);
			}
			break;
		case M_CL_UPDATE:
			{
				NET_Packet relay_packet = *P;
				P->r_u16(ID);
				u32 Ping = P->r_u32();
				CGameObject* O = smart_cast<CGameObject*>(Objects.net_Find(ID));
				if (0 == O) break;

				const bool is_actor_update = smart_cast<CActor*>(O) != nullptr;
				Fvector packet_actor_pos;
				packet_actor_pos.set(0.f, 0.f, 0.f);
				float packet_actor_model_yaw = 0.f;
				bool packet_actor_pos_valid = false;
				if (is_actor_update)
				{
					NET_Packet dbg_packet = *P;
					const u32 left_bytes = (dbg_packet.B.count > dbg_packet.r_tell()) ? (dbg_packet.B.count - dbg_packet.r_tell()) : 0;
					const u32 actor_export_min_size = sizeof(float) + sizeof(u32) + sizeof(u8) + sizeof(float) * 4;
					if (left_bytes >= actor_export_min_size)
					{
						float dbg_health = 0.f;
						u32 dbg_server_time = 0;
						u8 dbg_flags = 0;
						dbg_packet.r_float(dbg_health);
						dbg_packet.r_u32(dbg_server_time);
						dbg_packet.r_u8(dbg_flags);
						dbg_packet.r_vec3(packet_actor_pos);
						dbg_packet.r_float(packet_actor_model_yaw);
						packet_actor_pos_valid = true;
					}
				}

				// Dedicated-single bridge:
				// relayed client updates can arrive back to clients as M_CL_UPDATE.
				// Never re-import update for local controlled entity.
				if (OnClient() && O->Local())
				{
					if (is_actor_update)
						//WPN_TRACE("Level::ClientReceive M_CL_UPDATE skip local actor import actor=%u", O->ID());
					break;
				}

				if (is_actor_update)
				{
					//WPN_TRACE("Level::ClientReceive M_CL_UPDATE actor=%u local=%d remote=%d ping=%u from_cl_update=%d",
						//O->ID(), O->Local() ? 1 : 0, O->Remote() ? 1 : 0, Ping,
						//g_dedicated_single_import_from_cl_update ? 1 : 0);
				}

				const bool old_cl_update_import = g_dedicated_single_import_from_cl_update;
				g_dedicated_single_import_from_cl_update = true;
				O->net_Import(*P);
				g_dedicated_single_import_from_cl_update = old_cl_update_import;

				if (OnServer())
				{
					//---------------------------------------------------
					UpdateDeltaUpd(timeServer());

					if (packet_actor_pos_valid && smart_cast<CActor*>(O))
						apply_client_crow_touch(*this, O->ID(), packet_actor_pos);

					if (!pObjects4CrPr.empty() || !pActors4CrPr.empty())
					{
						if (smart_cast<CActor*>(O))
						{
							(void)packet_actor_pos_valid;
							(void)packet_actor_pos;
							(void)packet_actor_model_yaw;

							u32 dTime = 0;
							if ((Level().timeServer() + Ping) < P->timeReceive)
							{
#ifdef DEBUG
								//					Msg("! TimeServer[%d] < TimeReceive[%d]", Level().timeServer(), P->timeReceive);
#endif
								dTime = Ping;
							}
							else
								dTime = Level().timeServer() - P->timeReceive + Ping;
							u32 NumSteps = physics_world()->CalcNumSteps(dTime);
							SetNumCrSteps(NumSteps);

							O->CrPr_SetActivationStep(u32(physics_world()->StepsNum()) - NumSteps);
							AddActor_To_Actors4CrPr(O);
						}
					}

					// Dedicated-single bridge:
					// forward actor M_CL_UPDATE to every real client except sender.
					// This allows peers to receive direct movement/import data.
					if (g_dedicated_server && Game().Type() == eGameIDSingle && Server)
					{
						ClientID source_client;
						source_client.set(u32(-1));
						CSE_Abstract* se_actor = Server->ID_to_entity(ID);
						if (se_actor && se_actor->owner)
							source_client = se_actor->owner->ID;
						//WPN_TRACE("Level::ClientReceive relay M_CL_UPDATE actor=%u source_client=0x%08x", ID, source_client.value());

						struct relay_client_update
						{
							xrServer* server;
							NET_Packet* packet;
							ClientID source;

							relay_client_update(xrServer* sv, NET_Packet* p, ClientID src)
								: server(sv), packet(p), source(src)
							{
							}

							void operator()(IClient* client)
							{
								if (!client || client == server->GetServerClient())
									return;
								if (client->ID == source)
									return;

								xrClientData* xr_client = static_cast<xrClientData*>(client);
								if (!xr_client || !xr_client->net_Ready || !xr_client->flags.bConnected)
									return;

								server->SendTo(client->ID, *packet, net_flags(FALSE, TRUE));
							}
						};

						relay_client_update relayer(Server, &relay_packet, source_client);
						Server->ForEachClientDo(relayer);
					}
				}
			}
			break;
		case M_MOVE_PLAYERS:
			{
				game_events->insert(*P);
				if (g_bDebugEvents) ProcessGameEvents();
			}
			break;
			// [08.11.07] Alexander Maniluk: added new message handler for moving artefacts.
		case M_MOVE_ARTEFACTS:
			{
				/*if (!game_configured)
				{
					Msg("! WARNING: ignoring game event [%d] - game not configured...", m_type);
					break;
				}*/
				u8 Count = P->r_u8();
				for (u8 i = 0; i < Count; ++i)
				{
					u16 ID = P->r_u16();
					Fvector NewPos;
					P->r_vec3(NewPos);
					CArtefact* OArtefact = smart_cast<CArtefact*>(Objects.net_Find(ID));
					if (!OArtefact) break;
					OArtefact->MoveTo(NewPos);
					//destroy_physics_shell(OArtefact->PPhysicsShell());
				};
				/*NET_Packet PRespond;
				PRespond.w_begin(M_MOVE_ARTEFACTS_RESPOND);
				Send(PRespond, net_flags(TRUE, TRUE));*/
			}
			break;
			//------------------------------------------------
		case M_CL_INPUT:
			{
				/*if (!game_configured)
				{
					Msg("! WARNING: ignoring game event [%d] - game not configured...", m_type);
					break;
				}*/
				P->r_u16(ID);
				CObject* O = Objects.net_Find(ID);
				if (0 == O) break;
				O->net_ImportInput(*P);
			}
			break;
			//---------------------------------------------------
		case M_SV_CONFIG_NEW_CLIENT:
			InitializeClientGame(*P);
			break;
		case M_SV_CONFIG_GAME:
			game->net_import_state(*P);
			break;
		case M_SV_CONFIG_FINISHED:
			{
				game_configured = TRUE;
#ifdef DEBUG
				Msg("- Game configuring : Finished ");
#endif // #ifdef DEBUG
				if (IsDemoPlayStarted() && !m_current_spectator)
				{
					SpawnDemoSpectator();
				}
			}
			break;
		case M_MIGRATE_DEACTIVATE: // TO:   Changing server, just deactivate
			{
				NODEFAULT;
			}
			break;
		case M_MIGRATE_ACTIVATE: // TO:   Changing server, full state
			{
				NODEFAULT;
			}
			break;
		case M_CHAT:
			{
				/*if (!game_configured)
				{
					Msg("! WARNING: ignoring game event [%d] - game not configured...", m_type);
					break;
				}*/
				char buffer[256];
				P->r_stringZ(buffer);
				Msg("- %s", buffer);
			}
			break;
		case M_GAMEMESSAGE:
			{
				/*if (!game_configured)
				{
					Msg("! WARNING: ignoring game event [%d] - game not configured...", m_type);
					break;
				}*/
				if (!game) break;
				game_events->insert(*P);
				if (g_bDebugEvents) ProcessGameEvents();
			}
			break;
		case M_RELOAD_GAME:
		case M_LOAD_GAME:
		case M_CHANGE_LEVEL:
			{
#ifdef DEBUG
				Msg("--- Changing level message received...");
#endif // #ifdef DEBUG
				if (m_type == M_LOAD_GAME)
				{
					string256 saved_name;
					P->r_stringZ_s(saved_name);
					if (xr_strlen(saved_name) && ai().get_alife())
					{
						CSavedGameWrapper wrapper(saved_name);
						if (wrapper.level_id() == ai().level_graph().level_id())
						{
							Engine.Event.Defer("Game:QuickLoad", size_t(xr_strdup(saved_name)), 0);

							break;
						}
					}
				}
				MakeReconnect();
			}
			break;
		case M_SAVE_GAME:
			{
				ClientSave();
			}
			break;
		case M_AUTH_CHALLENGE:
			{
				ClientSendProfileData();
				OnBuildVersionChallenge();
			}
			break;
		case M_CLIENT_CONNECT_RESULT:
			{
				OnConnectResult(P);
			}
			break;
		case M_CHAT_MESSAGE:
			{
				/*if (!game_configured)
				{
					Msg("! WARNING: ignoring game event [%d] - game not configured...", m_type);
					break;
				}*/
				if (!game) break;
				Game().OnChatMessage(P);
			}
			break;
		case M_CLIENT_WARN:
			{
				if (!game) break;
				Game().OnWarnMessage(P);
			}
			break;
		case M_REMOTE_CONTROL_AUTH:
		case M_REMOTE_CONTROL_CMD:
			{
				Game().OnRadminMessage(m_type, P);
			}
			break;
		case M_SV_MAP_NAME:
			{
				map_data.ReceiveServerMapSync(*P);
			}
			break;
		case M_SV_DIGEST:
			{
				SendClientDigestToServer();
			}
			break;
		case M_CHANGE_LEVEL_GAME:
			{
				Msg("- M_CHANGE_LEVEL_GAME Received");

				if (OnClient())
				{
					MakeReconnect();
				}
				else
				{
					const char* m_SO = m_caServerOptions.c_str();
					//					const char* m_CO = m_caClientOptions.c_str();

					m_SO = strchr(m_SO, '/');
					if (m_SO) m_SO++;
					m_SO = strchr(m_SO, '/');

					shared_str LevelName;
					shared_str LevelVersion;
					shared_str GameType;

					P->r_stringZ(LevelName);
					P->r_stringZ(LevelVersion);
					P->r_stringZ(GameType);

					string4096 NewServerOptions = "";
					xr_sprintf(NewServerOptions, "%s/%s/%s%s",
					           LevelName.c_str(),
					           GameType.c_str(),
					           map_ver_string,
					           LevelVersion.c_str()
					);

					if (m_SO)
					{
						string4096 additional_options;
						xr_strcat(NewServerOptions, sizeof(NewServerOptions),
						          remove_version_option(m_SO, additional_options, sizeof(additional_options))
						);
					}
					m_caServerOptions = NewServerOptions;
					MakeReconnect();
				};
			}
			break;
		case M_CHANGE_SELF_NAME:
			{
				net_OnChangeSelfName(P);
			}
			break;
		case M_STATISTIC_UPDATE:
			{
				Msg("--- CL: On Update Request");
				if (!game) break;
				game_events->insert(*P);
				if (g_bDebugEvents) ProcessGameEvents();
			}
			break;
		case M_FILE_TRANSFER:
			{
				game_events->insert(*P);
				if (g_bDebugEvents) ProcessGameEvents();
			}
			break;
		case M_SECURE_KEY_SYNC:
			{
				OnSecureKeySync(*P);
			}
			break;
		case M_SECURE_MESSAGE:
			{
				OnSecureMessage(*P);
			}
			break;
		case M_TASKS_DATA:
			if (game)
				Game().OnTasksDataSync(P);
			break;

		case M_PORTIONS_DATA:
			if (game)
				Game().OnPortsDataSync(P);
			break;
		}

		net_msg_Release();
	}
	EndProcessQueue();

	if (g_bDebugEvents) ProcessGameSpawns();
}
