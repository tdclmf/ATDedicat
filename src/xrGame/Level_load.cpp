#include "stdafx.h"
#include "LevelGameDef.h"
#include "ai_space.h"
#include "ParticlesObject.h"
#include "script_process.h"
#include "script_engine.h"
#include "script_engine_space.h"
#include "level.h"
#include "game_cl_base.h"
#include "../xrEngine/x_ray.h"
#include "../xrEngine/gamemtllib.h"
#include "../xrphysics/PhysicsCommon.h"
#include "level_sounds.h"
#include "GamePersistent.h"
#include "../xrEngine/Rain.h"
#include "character_community.h"
#include "character_rank.h"
#include "character_reputation.h"
#include "monster_community.h"
#include "HudManager.h"
#include "game_graph.h"

extern ENGINE_API bool g_dedicated_server;

static bool run_remote_client_start_game_callback(LPCSTR phase)
{
	if (!IIsClient())
		return false;

	ai().script_engine().process_file_if_exists("mp_single_bridge_patches", false);

	luabind::functor<bool> init;
	if (!ai().script_engine().functor("mp_single_bridge_patches.init", init, false))
	{
		Msg("! [MP_BRIDGE] early callback skipped at [%s]: mp_single_bridge_patches.init not found", phase);
		return false;
	}

	const bool initialized = init();
	Msg("* [MP_BRIDGE] early callback at [%s] result=%d", phase, initialized ? 1 : 0);
	return initialized;
}

bool CLevel::net_proxy_bootstrap_game_graph()
{
	if (g_dedicated_server || !OnClient() || OnServer() || ai().get_alife())
		return false;

	if (m_proxy_game_graph && ai().get_game_graph() == m_proxy_game_graph)
		return true;

	if (ai().get_game_graph())
		return true;

	net_proxy_release_game_graph();

	string_path spawn_file;
	LPCSTR requested_spawn = nullptr;
	if (g_pGamePersistent)
		requested_spawn = g_pGamePersistent->m_game_params.m_game_or_spawn;

	LPCSTR selected_spawn = nullptr;
	if (requested_spawn && xr_strlen(requested_spawn) &&
		FS.exist(spawn_file, "$game_spawn$", requested_spawn, ".spawn"))
	{
		selected_spawn = requested_spawn;
	}
	else if (FS.exist(spawn_file, "$game_spawn$", "all", ".spawn"))
	{
		selected_spawn = "all";
	}

	if (!selected_spawn)
	{
		Msg("! [NET_PROXY][AI] Can't bootstrap game_graph: spawn file not found (requested=[%s])",
		    requested_spawn ? requested_spawn : "<empty>");
		return false;
	}

	m_proxy_spawn_file = FS.r_open(spawn_file);
	if (!m_proxy_spawn_file)
	{
		Msg("! [NET_PROXY][AI] Can't open spawn file for game_graph bootstrap: [%s]", spawn_file);
		return false;
	}

	m_proxy_spawn_chunk = m_proxy_spawn_file->open_chunk(4);
	if (!m_proxy_spawn_chunk)
	{
		Msg("! [NET_PROXY][AI] Spawn [%s] has no chunk 4 (game graph).", spawn_file);
		net_proxy_release_game_graph();
		return false;
	}

	m_proxy_game_graph = xr_new<CGameGraph>(*m_proxy_spawn_chunk);
	ai().m_game_graph = m_proxy_game_graph;

	Msg("* [NET_PROXY][AI] Client game_graph loaded from [%s] (spawn=%s).", spawn_file, selected_spawn);
	return true;
}

void CLevel::net_proxy_release_game_graph()
{
	if (m_proxy_game_graph && ai().m_game_graph == m_proxy_game_graph)
	{
		ai().unload(true);
		ai().m_game_graph = nullptr;
	}

	xr_delete(m_proxy_game_graph);

	if (m_proxy_spawn_chunk)
	{
		m_proxy_spawn_chunk->close();
		m_proxy_spawn_chunk = nullptr;
	}

	if (m_proxy_spawn_file)
	{
		FS.r_close(m_proxy_spawn_file);
		m_proxy_spawn_file = nullptr;
	}
}

bool CLevel::Load_GameSpecific_Before()
{
	// AI space
	//	g_pGamePersistent->LoadTitle		("st_loading_ai_objects");
	g_pGamePersistent->LoadTitle();
	string_path fn_game;

	const bool client_proxy_world =
		!g_dedicated_server &&
		OnClient() &&
		!OnServer() &&
		!ai().get_alife();

	if (client_proxy_world)
	{
		run_remote_client_start_game_callback("Load_GameSpecific_Before");

		if (net_proxy_bootstrap_game_graph())
		{
			Msg("* [NET_PROXY][AI] Proxy client runtime: game_graph bootstrap ready.");
		}
		else
		{
			Msg("! [NET_PROXY][AI] Proxy client runtime: game_graph bootstrap failed.");
		}
	}

	if (!ai().get_alife() && ai().get_game_graph() && FS.exist(fn_game, "$level$", "level.ai"))
	{
		LPCSTR ai_level_name = *name();
		if (!ai_level_name || !xr_strlen(ai_level_name))
			ai_level_name = has_SessionName() ? net_SessionName() : nullptr;

		if (!ai_level_name || !xr_strlen(ai_level_name))
		{
			Msg("! [NET_PROXY][AI] Can't load ai().load: level name is empty.");
		}
		else
		{
			const GameGraph::SLevel* level_desc = ai().game_graph().header().level(ai_level_name, true);
			if (!level_desc)
			{
				Msg("! [NET_PROXY][AI] Can't load ai().load: level [%s] not found in game_graph.", ai_level_name);
			}
			else
			{
				ai().load(ai_level_name);

				if (ai().get_level_graph() && ai().get_cross_table())
				{
					Msg("* [NET_PROXY][AI] Loaded level.ai for [%s].", ai_level_name);
				}
				else
				{
					Msg("! [NET_PROXY][AI] ai().load finished for [%s], but level_graph/cross_table are still missing.",
					    ai_level_name);
				}
			}
		}
	}

	if (!ai().get_alife() && ai().get_game_graph() && FS.exist(fn_game, "$level$", "level.game"))
	{
		if (ai().get_level_graph() && ai().get_cross_table())
		{
			IReader* stream = FS.r_open(fn_game);
			ai().patrol_path_storage_raw(*stream);
			FS.r_close(stream);
		}
		else
		{
			Msg("* [NET_PROXY][AI] Skipping patrol_path_storage_raw: no level_graph/cross_table for current client runtime.");
		}
	}

	CHARACTER_COMMUNITY::Reset();
	CHARACTER_RANK::Reset();
	CHARACTER_REPUTATION::Reset();
	MONSTER_COMMUNITY::Reset();

	return (TRUE);
}

bool CLevel::Load_GameSpecific_After()
{
	R_ASSERT(m_StaticParticles.empty());
	// loading static particles
	string_path fn_game;
	if (FS.exist(fn_game, "$level$", "level.ps_static"))
	{
		IReader* F = FS.r_open(fn_game);
		CParticlesObject* pStaticParticles;
		u32 chunk = 0;
		string256 ref_name;
		Fmatrix transform;
		Fvector zero_vel = {0.f, 0.f, 0.f};
		u32 ver = 0;
		for (IReader* OBJ = F->open_chunk_iterator(chunk); OBJ; OBJ = F->open_chunk_iterator(chunk, OBJ))
		{
			if (chunk == 0)
			{
				if (OBJ->length() == sizeof(u32))
				{
					ver = OBJ->r_u32();
#ifndef MASTER_GOLD
					Msg		("PS new version, %d", ver);
#endif // #ifndef MASTER_GOLD
					continue;
				}
			}
			u16 gametype_usage = 0;
			if (ver > 0)
			{
				gametype_usage = OBJ->r_u16();
			}
			OBJ->r_stringZ(ref_name, sizeof(ref_name));
			OBJ->r(&transform, sizeof(Fmatrix));
			transform.c.y += 0.01f;


			if ((g_pGamePersistent->m_game_params.m_e_game_type & EGameIDs(gametype_usage)) || (ver == 0))
			{
				pStaticParticles = CParticlesObject::Create(ref_name,FALSE, false);
				pStaticParticles->UpdateParent(transform, zero_vel);
				pStaticParticles->Play(false);
				m_StaticParticles.push_back(pStaticParticles);
			}
		}
		FS.r_close(F);
	}

	if (!g_dedicated_server)
	{
		// loading static sounds
		VERIFY(m_level_sound_manager);
		m_level_sound_manager->Load();

		// loading sound environment
		if (FS.exist(fn_game, "$level$", "level.snd_env"))
		{
			IReader* F = FS.r_open(fn_game);
			::Sound->set_geometry_env(F);
			FS.r_close(F);
		}
		// loading SOM
		if (FS.exist(fn_game, "$level$", "level.som"))
		{
			IReader* F = FS.r_open(fn_game);
			::Sound->set_geometry_som(F);
			FS.r_close(F);
		}

		// loading random (around player) sounds
		if (pSettings->section_exist("sounds_random"))
		{
			CInifile::Sect& S = pSettings->r_section("sounds_random");
			Sounds_Random.reserve(S.Data.size());
			for (CInifile::SectCIt I = S.Data.begin(); S.Data.end() != I; ++I)
			{
				Sounds_Random.push_back(ref_sound());
				Sound->create(Sounds_Random.back(), *I->first, st_Effect, sg_SourceType);
			}
			Sounds_Random_dwNextTime = Device.TimerAsync() + 50000;
			Sounds_Random_Enabled = FALSE;
		}

		if (g_pGamePersistent->pEnvironment)
		{
			if (CEffect_Rain* rain = g_pGamePersistent->pEnvironment->eff_Rain)
			{
				rain->InvalidateState();
			}
		}

		if (FS.exist(fn_game, "$level$", "level.fog_vol"))
		{
			IReader* F = FS.r_open(fn_game);
			u16 version = F->r_u16();
			if (version == 2)
			{
				u32 cnt = F->r_u32();

				Fmatrix volume_matrix;
				for (u32 i = 0; i < cnt; ++i)
				{
					F->r(&volume_matrix, sizeof(volume_matrix));
					u32 sub_cnt = F->r_u32();
					for (u32 is = 0; is < sub_cnt; ++is)
					{
						F->r(&volume_matrix, sizeof(volume_matrix));
					}
				}
			}
			FS.r_close(F);
		}
	}

	// loading scripts
	ai().script_engine().remove_script_process(ScriptEngine::eScriptProcessorLevel);

	if (pLevel->section_exist("level_scripts") && pLevel->line_exist("level_scripts", "script"))
		ai().script_engine().add_script_process(ScriptEngine::eScriptProcessorLevel,
			xr_new<CScriptProcess>(
				"level", pLevel->r_string("level_scripts", "script")));
	else
		ai().script_engine().add_script_process(ScriptEngine::eScriptProcessorLevel,
			xr_new<CScriptProcess>("level", ""));

	BlockCheatLoad();

	g_pGamePersistent->Environment().SetGameTime(GetEnvironmentGameDayTimeSec(), game->GetEnvironmentGameTimeFactor());

	HUD().SetRenderable(true);

	return TRUE;
}

struct translation_pair
{
	u32 m_id;
	u16 m_index;

	IC translation_pair(u32 id, u16 index)
	{
		m_id = id;
		m_index = index;
	}

	IC bool operator==(const u16& id) const
	{
		return (m_id == id);
	}

	IC bool operator<(const translation_pair& pair) const
	{
		return (m_id < pair.m_id);
	}

	IC bool operator<(const u16& id) const
	{
		return (m_id < id);
	}
};

void CLevel::Load_GameSpecific_CFORM(CDB::TRI* tris, u32 count)
{
	static thread_local xr_vector<translation_pair> translator;
	translator.clear();
	translator.reserve(GMLib.CountMaterial());
	u16 default_id = (u16)GMLib.GetMaterialIdx("default");
	translator.push_back(translation_pair(u32(-1), default_id));

	u16 index = 0, static_mtl_count = 1;
	int max_ID = 0;
	int max_static_ID = 0;
	for (GameMtlIt I = GMLib.FirstMaterial(); GMLib.LastMaterial() != I; ++I, ++index)
	{
		if (!(*I).Flags.test(SGameMtl::flDynamic))
		{
			++static_mtl_count;
			translator.push_back(translation_pair((*I).GetID(), index));
			if ((*I).GetID() > max_static_ID) max_static_ID = (*I).GetID();
		}
		if ((*I).GetID() > max_ID) max_ID = (*I).GetID();
	}
	// Msg("* Material remapping ID: [Max:%d, StaticMax:%d]",max_ID,max_static_ID);
	VERIFY(max_static_ID<0xFFFF);

	if (static_mtl_count < 128)
	{
		CDB::TRI* I = tris;
		CDB::TRI* E = tris + count;
		for (; I != E; ++I)
		{
			auto iter = std::find(translator.begin(), translator.end(), (u16)(*I).material);
			if (iter != translator.end())
			{
				(*I).material = (*iter).m_index;
				SGameMtl* mtl = GMLib.GetMaterialByIdx((*iter).m_index);
				(*I).suppress_shadows = mtl->Flags.is(SGameMtl::flSuppressShadows);
				(*I).suppress_wm = mtl->Flags.is(SGameMtl::flSuppressWallmarks);
				continue;
			}

			Debug.fatal(DEBUG_INFO, "Game material '%d' not found", (*I).material);
		}
		return;
	}

	std::sort(translator.begin(), translator.end());
	{
		CDB::TRI* I = tris;
		CDB::TRI* E = tris + count;
		for (; I != E; ++I)
		{
			auto iter = std::lower_bound(translator.begin(), translator.end(), (u16)(*I).material);
			if ((iter != translator.end()) && ((*iter).m_id == (*I).material))
			{
				(*I).material = (*iter).m_index;
				SGameMtl* mtl = GMLib.GetMaterialByIdx((*iter).m_index);
				(*I).suppress_shadows = mtl->Flags.is(SGameMtl::flSuppressShadows);
				(*I).suppress_wm = mtl->Flags.is(SGameMtl::flSuppressWallmarks);
				continue;
			}

			Debug.fatal(DEBUG_INFO, "Game material '%d' not found", (*I).material);
		}
	}
}

void CLevel::BlockCheatLoad()
{
	if (game && OnClient() && phTimefactor != 1)
		phTimefactor = 1;
}
