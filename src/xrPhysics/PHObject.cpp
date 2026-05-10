#include "stdafx.h"
#include "Physics.h"
#include "PHObject.h"
#include "PHWorld.h"
#include "PHMoveStorage.h"
#include "dRayMotions.h"
#include "PHCollideValidator.h"
#include "console_vars.h"
#include "PHAICharacter.h"
#include "IPhysicsShellHolder.h"
#include "../xrCore/xrCore.h"
#include "../xrEngine/device.h"
#ifdef DEBUG
#include "debug_output.h"
#endif
extern CPHWorld* ph_world;

CPHObject::CPHObject() : ISpatial(g_SpatialSpacePhysic)
{
	m_flags.flags = 0;
	spatial.type |= STYPE_PHYSIC;
	m_island.Init();
	m_check_count = 0;
	CPHCollideValidator::InitObject(*this);
}

namespace
{
bool sp_trypos_diag_enabled()
{
	return Core.Params && strstr(Core.Params, "-sp_trypos_diag");
}

bool sp_ph_step_diag_allow()
{
	static u32 next_time = 0;
	static u32 count = 0;
	const u32 now = Device.dwTimeGlobal;
	if (count < 256)
	{
		++count;
		return true;
	}

	if (now < next_time)
		return false;

	next_time = now + 1000;
	count = 0;
	return true;
}

CPHAICharacter* sp_ai_character(CPHObject* object)
{
	if (!object || object->CastType() != CPHObject::tpCharacter)
		return nullptr;

	return static_cast<CPHCharacter*>(object)->CastAICharacter();
}

void sp_ph_step_log(CPHObject* object, LPCSTR phase, bool ret, bool grouped, u32 spatial_count, dReal step)
{
	CPHAICharacter* ai_character = sp_ai_character(object);
	if (!ai_character || !sp_trypos_diag_enabled() || !sp_ph_step_diag_allow())
		return;

	IPhysicsShellHolder* ref_object = ai_character->PhysicsRefObject();
	Fvector position;
	position.set(0.f, 0.f, 0.f);
	ai_character->GetPosition(position);

	Msg("* [SP_PH_STEP] id=%hu name=[%s] sect=[%s] phase=%s ret=%d grouped=%d island_grouped=%d spatial=%u active=%d enabled=%d step=%.4f pos=(%.3f %.3f %.3f)",
		ref_object ? ref_object->ObjectID() : u16(-1),
		ref_object ? ref_object->ObjectName() : "",
		ref_object ? ref_object->ObjectNameSect() : "",
		phase ? phase : "",
		ret ? 1 : 0,
		grouped ? 1 : 0,
		object->Island().IsObjGroun() ? 1 : 0,
		spatial_count,
		object->is_active() ? 1 : 0,
		ai_character->IsEnabled() ? 1 : 0,
		float(step),
		VPUSH(position));
}
}

void CPHObject::activate()
{
	R_ASSERT2(dSpacedGeom(), "trying to activate destroyed or not created object!");
	if (m_flags.test(st_activated))return;
	if (m_flags.test(st_freezed))
	{
		UnFreeze();
		return;
	}
	if (m_flags.test(st_recently_deactivated))remove_from_recently_deactivated();
	ph_world->AddObject(this);
	vis_update_activate();
	m_flags.set(st_activated,TRUE);
}

void CPHObject::EnableObject(CPHObject* obj)
{
	activate();
}

void CPHObject::deactivate()
{
	if (!m_flags.test(st_activated))return;
	VERIFY2(m_island.IsActive(), "can not do it during processing");
	ph_world->RemoveObject(PH_OBJECT_I(this));
	vis_update_deactivate();
	m_flags.set(st_activated,FALSE);
}

void CPHObject::put_in_recently_deactivated()
{
	VERIFY(!m_flags.test(st_activated)&&!m_flags.test(st_freezed));
	if (m_flags.test(st_recently_deactivated))return;
	m_check_count = u8(ph_console::ph_tri_clear_disable_count);
	m_flags.set(st_recently_deactivated,TRUE);
	ph_world->AddRecentlyDisabled(this);
}

void CPHObject::remove_from_recently_deactivated()
{
	if (!m_flags.test(st_recently_deactivated))return;
	m_check_count = 0;
	m_flags.set(st_recently_deactivated,FALSE);
	ph_world->RemoveFromRecentlyDisabled(PH_OBJECT_I(this));
}

void CPHObject::check_recently_deactivated()
{
	if (m_check_count == 0)
	{
		ClearRecentlyDeactivated();
		remove_from_recently_deactivated();
	}
	else m_check_count--;
}

void CPHObject::spatial_move()
{
	get_spatial_params();
	ISpatial::spatial_move();
	m_flags.set(st_dirty,TRUE);
}

void CPHObject::Collide()
{
	TRY
	if (m_flags.test(fl_ray_motions))
	{
		CPHMoveStorage* tracers = MoveStorage();
		CPHMoveStorage::iterator I = tracers->begin(), E = tracers->end();
		for (; E != I; I++)
		{
			const Fvector *from = 0, *to = 0;
			Fvector dir;
			I.Positions(from, to);
			if (from->x == -dInfinity) continue;
			dir.sub(*to, *from);
			float magnitude = dir.magnitude();
			if (magnitude < EPS) continue;
			dir.mul(1.f / magnitude);
			g_SpatialSpacePhysic->q_ray(ph_world->r_spatial, 0, STYPE_PHYSIC, *from, dir, magnitude);
			//|ISpatial_DB::O_ONLYFIRST
#ifdef DEBUG
				if(debug_output().ph_dbg_draw_mask().test(phDbgDrawRayMotions))
				{
					debug_output().DBG_OpenCashedDraw();
					debug_output().DBG_DrawLine(*from,Fvector().add(*from,Fvector().mul(dir,magnitude)),D3DCOLOR_XRGB(0,255,0));
					debug_output().DBG_ClosedCashedDraw(30000);
				}

#endif
			qResultVec& result = ph_world->r_spatial;
			qResultIt i = result.begin(), e = result.end();
			for (; i != e; ++i)
			{
				CPHObject* obj2 = static_cast<CPHObject*>(*i);
				if (!obj2 || obj2 == this || !obj2->m_flags.test(st_dirty)) continue;
				dGeomID motion_ray = ph_world->GetMotionRayGeom();
				dGeomRayMotionSetGeom(motion_ray, I.dGeom());
				dGeomRayMotionsSet(motion_ray, (const dReal*)from, (const dReal*)&dir, magnitude);
				NearCallback(this, obj2, motion_ray, obj2->dSpacedGeom());
			}
		}
	}
	CollideDynamics();
	///////////////////////////////
	if (CPHCollideValidator::DoCollideStatic(*this)) CollideStatic(dSpacedGeom(), this);
	m_flags.set(st_dirty,FALSE);
	CATCH
}

#include "PHCharacter.h"
extern u32 g_dead_body_collision;

void CPHObject::CollideDynamics()
{
	g_SpatialSpacePhysic->q_box(ph_world->r_spatial, 0, STYPE_PHYSIC, spatial.sphere.P, AABB);
	qResultVec& result = ph_world->r_spatial;
	qResultIt i = result.begin(), e = result.end();
	for (; i != e; ++i)
	{
		CPHObject* obj2 = static_cast<CPHObject*>(*i);

		if (!obj2 || obj2 == this || !obj2->m_flags.test(st_dirty)) continue;

		// Dead Body Collision
		if (obj2->m_flags.test(is_deadbody))
		{
			CPHCharacter* achar = smart_cast<CPHCharacter*>(this);

			if (g_dead_body_collision == 2 || (g_dead_body_collision == 1 && achar && achar->CastActorCharacter()))
			{
				if (obj2->collide_class_bits().test(CPHCollideValidator::cbNCClassCharacter))
					obj2->collide_class_bits().set(CPHCollideValidator::cbNCClassCharacter, FALSE);
			}
			else
			{
				if (!obj2->collide_class_bits().test(CPHCollideValidator::cbNCClassCharacter))
					obj2->collide_class_bits().set(CPHCollideValidator::cbNCClassCharacter, TRUE);
			}
		}

		if (CPHCollideValidator::DoCollide(*this, *obj2)) NearCallback(this, obj2, dSpacedGeom(), obj2->dSpacedGeom());
	}
}

void CPHObject::reinit_single()
{
	IslandReinit();
	qResultVec& result = ph_world->r_spatial;
	qResultIt i = result.begin(), e = result.end();
	for (; i != e; ++i)
	{
		CPHObject* obj = static_cast<CPHObject*>(*i);
		obj->IslandReinit();
	}
	result.clear_not_free();
	dJointGroupEmpty(ContactGroup);
	ContactFeedBacks.empty();
	ContactEffectors.empty();
}

void CPHObject::step_prediction(float time)
{
	//general idea:
	//perform normal step by time as local as possible for this object then return world to 
	//the pervious state
}

bool CPHObject::step_single(dReal step)
{
	CollideDynamics();
	sp_ph_step_log(this, "pre_collide", !m_island.IsObjGroun(), m_island.IsObjGroun(),
		static_cast<u32>(ph_world->r_spatial.size()), step);
	bool ret = !m_island.IsObjGroun();
	if (ret)
	{
		//PhTune							(step);
		IslandStep(step);
		sp_ph_step_log(this, "post_island_step", true, m_island.IsObjGroun(),
			static_cast<u32>(ph_world->r_spatial.size()), step);
		reinit_single();
		//PhDataUpdate					(step);
		spatial_move();
		CollideDynamics();
		ret = !m_island.IsObjGroun();
		sp_ph_step_log(this, "post_collide", ret, m_island.IsObjGroun(),
			static_cast<u32>(ph_world->r_spatial.size()), step);
	}
	reinit_single();
	return ret;
}

void CPHObject::step(float time)
//it is still not a true step for object because it collide the object only not subsequent collision is doing
{
	ph_world->r_spatial.clear_not_free();
	reinit_single();
	Collide();
	IslandStep(time);
	reinit_single();
}

bool CPHObject::DoCollideObj()
{
	CollideDynamics();
	bool ret = m_island.IsObjGroun();
	reinit_single();
	return ret;
}

void CPHObject::FreezeContent()
{
	R_ASSERT(!m_flags.test(st_freezed));
	m_flags.set(st_freezed,TRUE);
	m_flags.set(st_activated,FALSE);
	vis_update_deactivate();
}

void CPHObject::UnFreezeContent()
{
	R_ASSERT(m_flags.test(st_freezed));
	m_flags.set(st_freezed,FALSE);
	m_flags.set(st_activated,TRUE);
	vis_update_activate();
}

void CPHObject::spatial_register()
{
	get_spatial_params();
	ISpatial::spatial_register();
	m_flags.set(st_dirty,TRUE);
}

void CPHObject::collision_disable()
{
	ISpatial::spatial_unregister();
}

void CPHObject::collision_enable()
{
	ISpatial::spatial_register();
}

void CPHObject::Freeze()
{
	if (!m_flags.test(st_activated))return;
	ph_world->RemoveObject(this);
	ph_world->AddFreezedObject(this);
	FreezeContent();
}

void CPHObject::UnFreeze()
{
	if (!m_flags.test(st_freezed)) return;
	UnFreezeContent();
	ph_world->RemoveFreezedObject(this);
	ph_world->AddObject(this);
}


CPHUpdateObject::CPHUpdateObject()
{
	b_activated = false;
}

void CPHUpdateObject::Activate()
{
	if (b_activated)return;
	ph_world->AddUpdateObject(this);
	b_activated = true;
}

void CPHUpdateObject::Deactivate()
{
	if (!b_activated)return;
	ph_world->RemoveUpdateObject(this);
	b_activated = false;
}
