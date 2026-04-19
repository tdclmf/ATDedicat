////////////////////////////////////////////////////////////////////////////
//	Module 		: script_binder.cpp
//	Created 	: 26.03.2004
//  Modified 	: 26.03.2004
//	Author		: Dmitriy Iassenev
//	Description : Script objects binder
////////////////////////////////////////////////////////////////////////////

#include "pch_script.h"
#include "ai_space.h"
#include "script_engine.h"
#include "script_binder.h"
#include "xrServer_Objects_ALife.h"
#include "script_binder_object.h"
#include "script_game_object.h"
#include "gameobject.h"
#include "level.h"

namespace
{
	static u32 g_binder_diag_budget = 512;

	IC bool binder_diag_allow_log()
	{
		if (!g_binder_diag_budget)
			return false;

		--g_binder_diag_budget;
		return true;
	}

	IC LPCSTR binder_section_script_binding(LPCSTR section)
	{
		if (!section || !*section || !pSettings || !pSettings->section_exist(section))
			return "";

		if (!pSettings->line_exist(section, "script_binding"))
			return "";

		return pSettings->r_string(section, "script_binding");
	}
}

// comment next string when commiting
//#define DBG_DISABLE_SCRIPTS

CScriptBinder::CScriptBinder()
{
	init();
}

CScriptBinder::~CScriptBinder()
{
	VERIFY(!m_object);
}

void CScriptBinder::init()
{
	m_object = 0;
}

void CScriptBinder::clear()
{
	try
	{
		xr_delete(m_object);
	}
	catch (...)
	{
		m_object = 0;
	}
	init();
}

void CScriptBinder::reinit()
{
#ifdef DEBUG_MEMORY_MANAGER
	size_t									start = 0;
	if (g_bMEMO)
		start							= Memory.mem_usage();
#endif // DEBUG_MEMORY_MANAGER
	if (m_object)
	{
		try
		{
			m_object->reinit();
		}
		catch (...)
		{
			clear();
		}
	}
#ifdef DEBUG_MEMORY_MANAGER
	if (g_bMEMO) {
//		lua_gc				(ai().script_engine().lua(),LUA_GCCOLLECT,0);
//		lua_gc				(ai().script_engine().lua(),LUA_GCCOLLECT,0);
		Msg					("CScriptBinder::reinit() : %lld",Memory.mem_usage() - start);
	}
#endif // DEBUG_MEMORY_MANAGER
}

void CScriptBinder::Load(LPCSTR section)
{
}

void CScriptBinder::reload(LPCSTR section)
{
#ifdef DEBUG_MEMORY_MANAGER
	size_t									start = 0;
	if (g_bMEMO)
		start							= Memory.mem_usage();
#endif // DEBUG_MEMORY_MANAGER
#ifndef DBG_DISABLE_SCRIPTS
	VERIFY(!m_object);
	if (!pSettings->line_exist(section, "script_binding"))
		return;

	luabind::functor<void> lua_function;
	if (!ai().script_engine().functor(pSettings->r_string(section, "script_binding"), lua_function))
	{
		ai().script_engine().script_log(ScriptStorage::eLuaMessageTypeError, "function %s is not loaded!",
		                                pSettings->r_string(section, "script_binding"));
		return;
	}

	CGameObject* game_object = smart_cast<CGameObject*>(this);

	try
	{
		lua_function(game_object ? game_object->lua_game_object() : 0);
	}
	catch (...)
	{
		clear();
		return;
	}

	if (!m_object && game_object && binder_diag_allow_log())
	{
		Msg("! [BINDER] reload produced no binder object for [%s][%u] section=[%s] binding=[%s].",
			game_object->cName().c_str(),
			game_object->ID(),
			*game_object->cNameSect(),
			pSettings->r_string(section, "script_binding"));
	}

	if (m_object)
	{
		try
		{
			m_object->reload(section);
		}
		catch (...)
		{
			clear();
		}
	}
#endif
#ifdef DEBUG_MEMORY_MANAGER
	if (g_bMEMO) {
//		lua_gc				(ai().script_engine().lua(),LUA_GCCOLLECT,0);
//		lua_gc				(ai().script_engine().lua(),LUA_GCCOLLECT,0);
		Msg					("CScriptBinder::reload() : %lld",Memory.mem_usage() - start);
	}
#endif // DEBUG_MEMORY_MANAGER
}

BOOL CScriptBinder::net_Spawn(CSE_Abstract* DC)
{
#ifdef DEBUG_MEMORY_MANAGER
	size_t									start = 0;
	if (g_bMEMO)
		start							= Memory.mem_usage();
#endif // DEBUG_MEMORY_MANAGER
	CSE_Abstract* abstract = (CSE_Abstract*)DC;
	CSE_ALifeObject* object = smart_cast<CSE_ALifeObject*>(abstract);
	if (object && m_object)
	{
		try
		{
			const bool spawn_ok = m_object->net_Spawn(object);
			if (!spawn_ok && OnClient() && !OnServer())
			{
				CGameObject* game_object = smart_cast<CGameObject*>(this);
				const LPCSTR section = game_object ? *game_object->cNameSect() : "";
				const LPCSTR binding = binder_section_script_binding(section);
				if (game_object && binder_diag_allow_log())
				{
					Msg("! [CL] Binder net_spawn rejected [%s][%u] section=[%s] binding=[%s] on_client=%d on_server=%d.",
						game_object->cName().c_str(),
						game_object->ID(),
						section,
						binding && *binding ? binding : "<none>",
						(int)OnClient(),
						(int)OnServer());
				}
			}
			return (spawn_ok ? TRUE : FALSE);
		}
		catch (...)
		{
			clear();
		}
	}

#ifdef DEBUG_MEMORY_MANAGER
	if (g_bMEMO) {
//		lua_gc				(ai().script_engine().lua(),LUA_GCCOLLECT,0);
//		lua_gc				(ai().script_engine().lua(),LUA_GCCOLLECT,0);
		Msg					("CScriptBinder::net_Spawn() : %lld",Memory.mem_usage() - start);
	}
#endif // DEBUG_MEMORY_MANAGER

	return (TRUE);
}

void CScriptBinder::net_Destroy()
{
	if (m_object)
	{
#ifdef _DEBUG
		Msg						("* Core object %s is UNbinded from the script object",smart_cast<CGameObject*>(this) ? *smart_cast<CGameObject*>(this)->cName() : "");
#endif // _DEBUG
		try
		{
			m_object->net_Destroy();
		}
		catch (...)
		{
			clear();
		}
	}
	xr_delete(m_object);
}

void CScriptBinder::set_object(CScriptBinderObject* object)
{
	if (g_pGameLevel)
	{
		VERIFY2(!m_object, "Cannot bind to the object twice!");
#ifdef _DEBUG
		Msg					("* Core object %s is binded with the script object",smart_cast<CGameObject*>(this) ? *smart_cast<CGameObject*>(this)->cName() : "");
#endif // _DEBUG
		m_object = object;
	}
	else
		xr_delete(object);
}

void CScriptBinder::shedule_Update(u32 time_delta)
{
	PROF_EVENT("CScriptBinder::shedule_Update");
	if (m_object)
	{
		try
		{
			m_object->shedule_Update(time_delta);
		}
		catch (...)
		{
			clear();
		}
	}
}

void CScriptBinder::save(NET_Packet& output_packet)
{
	if (m_object)
	{
		try
		{
			m_object->save(&output_packet);
		}
		catch (...)
		{
			clear();
		}
	}
}

void CScriptBinder::load(IReader& input_packet)
{
	if (m_object)
	{
		try
		{
			m_object->load(&input_packet);
		}
		catch (...)
		{
			clear();
		}
	}
}

BOOL CScriptBinder::net_SaveRelevant()
{
	if (m_object)
	{
		try
		{
			return (m_object->net_SaveRelevant());
		}
		catch (...)
		{
			clear();
		}
	}
	return (FALSE);
}

void CScriptBinder::net_Relcase(CObject* object)
{
	CGameObject* game_object = smart_cast<CGameObject*>(object);
	if (m_object && game_object)
	{
		try
		{
			m_object->net_Relcase(game_object->lua_game_object());
		}
		catch (...)
		{
			clear();
		}
	}
}
