//----------------------------------------------------
// file: TempObject.cpp
//----------------------------------------------------
#include "stdafx.h"
#pragma hdrstop

#include "ps_instance.h"
#include "IGame_Persistent.h"

CPS_Instance::CPS_Instance(bool destroy_on_game_load) :
	ISpatial(g_SpatialSpace),
	m_destroy_on_game_load(destroy_on_game_load)
{
	g_pGamePersistent->ps_active_cs.Enter();
	g_pGamePersistent->ps_active.insert(this);
	g_pGamePersistent->ps_active_cs.Leave();
	renderable.pROS_Allowed = FALSE;

	m_iLifeTime = int_max;
	m_bAutoRemove = TRUE;
	m_bDead = FALSE;
	dwTimeOfLastExecute = Device.dwTimeGlobal;
}

extern ENGINE_API BOOL g_bRendering;

//----------------------------------------------------
CPS_Instance::~CPS_Instance()
{
	{
		g_pGamePersistent->ps_active_cs.Enter();
		g_pGamePersistent->ps_active.erase(this);
		g_pGamePersistent->ps_active_cs.Leave();
	}

#ifdef DEBUG
	xr_vector<CPS_Instance*>::iterator it2 = std::find(g_pGamePersistent->ps_destroy.begin(),
	                                                   g_pGamePersistent->ps_destroy.end(), this);

	VERIFY(it2 == g_pGamePersistent->ps_destroy.end());
#endif // DEBUG

	spatial_unregister();
}

//----------------------------------------------------
void CPS_Instance::shedule_Update(u32 dt)
{
	if (renderable.pROS) ::Render->ros_destroy(renderable.pROS); //. particles doesn't need ROS

	m_iLifeTime -= dt;

	// remove???
	if (m_bDead) return;
	if (m_bAutoRemove && m_iLifeTime <= 0)
		PSI_destroy();
}

//----------------------------------------------------
void CPS_Instance::PSI_destroy()
{
	if (m_bDead) return;
	m_bDead = TRUE;
	m_iLifeTime = 0;

	g_pGamePersistent->ps_destroy_cs.Enter();
	g_pGamePersistent->ps_destroy.push_back(this);
	g_pGamePersistent->ps_destroy_cs.Leave();
}

//----------------------------------------------------
void CPS_Instance::PSI_internal_delete()
{
	CPS_Instance* self = this;
	xr_delete(self);
}
