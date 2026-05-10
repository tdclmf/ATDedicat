////////////////////////////////////////////////////////////////////////////
//	Module 		: movement_manager_physic.cpp
//	Created 	: 03.12.2003
//  Modified 	: 03.12.2003
//	Author		: Dmitriy Iassenev
//	Description : Movement manager : physic character movement
////////////////////////////////////////////////////////////////////////////

#include "stdafx.h"
#include "movement_manager.h"
#include "PHMovementControl.h"
#include "detail_path_manager.h"
#include "level.h"
#include "custommonster.h"
#include "actor.h"
#include "physicsshellholder.h"
#include "../xrphysics/physicsshell.h"
#include "../xrEngine/xr_object_list.h"
#include "../xrphysics/IColisiondamageInfo.h"

#include "profiler.h"

// Lain: added 
#include "steering_behaviour.h"

extern int g_sp_diag_net_updates;
extern ENGINE_API bool g_dedicated_server;

namespace
{
constexpr LPCSTR kSpMphysSource = "src_20260507_sp_compare_no_preavoid";

bool sp_mphys_diag_allow(u32& next_time, u32& count, u32 interval_ms, u32 max_count)
{
	if (!g_sp_diag_net_updates)
		return false;

	if (count < max_count)
	{
		++count;
		return true;
	}

	const u32 now = Device.dwTimeGlobal;
	if (now < next_time)
		return false;

	next_time = now + interval_ms;
	count = 0;
	return true;
}

bool sp_mphys_has_param(LPCSTR param)
{
    return param && param[0] && Core.Params && strstr(Core.Params, param);
}

struct SSpMphysActorDiag
{
    u16 global_id;
    LPCSTR global_name;
    float global_dist;
    int global_alive;
    u16 nearest_id;
    LPCSTR nearest_name;
    float nearest_dist;
    u32 runtime_count;
};

void sp_mphys_fill_actor_diag(const CCustomMonster& object, SSpMphysActorDiag& diag)
{
    diag.global_id = u16(-1);
    diag.global_name = "";
    diag.global_dist = -1.f;
    diag.global_alive = 0;
    diag.nearest_id = u16(-1);
    diag.nearest_name = "";
    diag.nearest_dist = flt_max;
    diag.runtime_count = 0;

    CActor* global_actor = Actor();
    if (global_actor)
    {
        diag.global_id = global_actor->ID();
        diag.global_name = global_actor->cName().c_str();
        diag.global_dist = object.Position().distance_to(global_actor->Position());
        diag.global_alive = global_actor->g_Alive() ? 1 : 0;
    }

    const u32 object_count = Level().Objects.o_count();
    for (u32 i = 0; i < object_count; ++i)
    {
        CObject* level_object = Level().Objects.o_get_by_iterator(i);
        if (!level_object)
            continue;

        CActor* actor = smart_cast<CActor*>(level_object);
        if (!actor || (actor->ID() == 0) || !actor->g_Alive())
            continue;

        ++diag.runtime_count;
        const float distance = object.Position().distance_to(actor->Position());
        if (distance >= diag.nearest_dist)
            continue;

        diag.nearest_dist = distance;
        diag.nearest_id = actor->ID();
        diag.nearest_name = actor->cName().c_str();
    }

    if (diag.nearest_dist == flt_max)
        diag.nearest_dist = -1.f;
}

void sp_mphys_diag_log(
	LPCSTR branch,
	const CCustomMonster& object,
	CPHMovementControl* movement_control,
	const Fvector& start_position,
	const Fvector& requested_position,
	const Fvector& result_position,
	float desirable_speed,
	float real_speed,
	float time_delta,
	u32 nearest_count,
	const CObject* nearest_object,
	u32 travel_point,
	float dist,
	float dist_to_target)
{
	static u32 s_sp_mphys_next = 0;
	static u32 s_sp_mphys_count = 0;

	if (!sp_mphys_diag_allow(s_sp_mphys_next, s_sp_mphys_count, 1000, 512))
		return;

	if ((desirable_speed <= 0.05f) && (nearest_count == 0))
		return;

	Fvector control_position = result_position;
	Fvector character_position = result_position;
	Fvector velocity = {0.f, 0.f, 0.f};
	float actual_velocity = -1.f;
	int character_enabled = -1;
	if (movement_control)
	{
		movement_control->GetPosition(control_position);
		movement_control->GetCharacterPosition(character_position);
		velocity = movement_control->GetVelocity();
		actual_velocity = movement_control->GetVelocityActual();
		character_enabled = movement_control->IsCharacterEnabled() ? 1 : 0;
	}

    SSpMphysActorDiag actor_diag;
    sp_mphys_fill_actor_diag(object, actor_diag);
    const float camera_distance = Device.vCameraPosition.distance_to(object.Position());
    const int physics_only = movement_control && movement_control->PhysicsOnlyMode() ? 1 : 0;

    Msg("* [SP_MPHYS] src=%s dedicated=%d server=%d id=%hu name=[%s] branch=%s enabled=%d physics_only=%d nearest=%u nearest0=%hu/%s withactor=%d actorproxy=%d cam_dist=%.3f global_actor=%hu/%s/%.3f/%d runtime_actors=%u nearest_actor=%hu/%s/%.3f dt=%.4f start=(%.2f %.2f %.2f) req=(%.2f %.2f %.2f) result=(%.2f %.2f %.2f) ctrl=(%.2f %.2f %.2f) char=(%.2f %.2f %.2f) req_step=%.4f res_step=%.4f desired=%.3f real=%.3f actual=%.3f vel=(%.3f %.3f %.3f) tp=%u dist=%.3f dist_target=%.3f",
        kSpMphysSource,
        g_dedicated_server ? 1 : 0, IIsServer() ? 1 : 0,
        object.ID(), object.cName().c_str(), branch, character_enabled, physics_only,
        nearest_count, nearest_object ? nearest_object->ID() : u16(-1), nearest_object ? nearest_object->cName().c_str() : "",
        sp_mphys_has_param("-withactor") ? 1 : 0, 0, camera_distance,
        actor_diag.global_id, actor_diag.global_name, actor_diag.global_dist, actor_diag.global_alive,
        actor_diag.runtime_count, actor_diag.nearest_id, actor_diag.nearest_name, actor_diag.nearest_dist,
        time_delta, VPUSH(start_position), VPUSH(requested_position), VPUSH(result_position), VPUSH(control_position), VPUSH(character_position),
        start_position.distance_to(requested_position), start_position.distance_to(result_position),
        desirable_speed, real_speed, actual_velocity, VPUSH(velocity), travel_point, dist, dist_to_target);
}
void sp_mphys_diag_log_blockers(
	const CCustomMonster& object,
	const xr_vector<CObject*>& nearest_objects,
	const Fvector& start_position,
	const Fvector& requested_position,
	const Fvector& result_position)
{
	static u32 s_sp_mphys_block_next = 0;
	static u32 s_sp_mphys_block_count = 0;

	if (!sp_mphys_diag_allow(s_sp_mphys_block_next, s_sp_mphys_block_count, 1000, 256))
		return;

	Msg("* [SP_MPHYS_BLOCK] src=%s dedicated=%d server=%d id=%hu name=[%s] blockers=%u req_step=%.4f res_step=%.4f start=(%.2f %.2f %.2f) req=(%.2f %.2f %.2f) result=(%.2f %.2f %.2f)",
		kSpMphysSource,
		g_dedicated_server ? 1 : 0, IIsServer() ? 1 : 0,
		object.ID(),
		object.cName().c_str(),
		(u32)nearest_objects.size(),
		start_position.distance_to(requested_position),
		start_position.distance_to(result_position),
		VPUSH(start_position),
		VPUSH(requested_position),
		VPUSH(result_position));

	const u32 max_to_log = nearest_objects.size() < 6 ? (u32)nearest_objects.size() : 6u;
	for (u32 i = 0; i < max_to_log; ++i)
	{
		const CObject* nearest = nearest_objects[i];
		if (!nearest)
			continue;

		const CCustomMonster* nearest_monster = smart_cast<const CCustomMonster*>(nearest);
		const CActor* nearest_actor = smart_cast<const CActor*>(nearest);
		CPhysicsShellHolder* shell_holder = smart_cast<CPhysicsShellHolder*>(const_cast<CObject*>(nearest));
		CPhysicsShell* physics_shell = shell_holder ? shell_holder->PPhysicsShell() : nullptr;

		Msg("* [SP_MPHYS_BLOCK][OBJ] src=%s owner=%hu idx=%u id=%hu name=[%s] sect=[%s] enabled=%d destroy=%d spatial=%u collidable=%d alive=%d actor=%d monster=%d shell=%d shell_active=%d dist_req=%.3f dist_self=%.3f pos=(%.2f %.2f %.2f)",
			kSpMphysSource,
			object.ID(),
			i,
			nearest->ID(),
			nearest->cName().c_str(),
			nearest->cNameSect().c_str(),
			nearest->getEnabled() ? 1 : 0,
			nearest->getDestroy() ? 1 : 0,
			nearest->spatial.type,
			nearest->collidable.model ? 1 : 0,
			nearest_monster && nearest_monster->g_Alive() ? 1 : 0,
			nearest_actor ? 1 : 0,
			nearest_monster ? 1 : 0,
			physics_shell ? 1 : 0,
			physics_shell && physics_shell->isActive() ? 1 : 0,
			nearest->Position().distance_to(requested_position),
			nearest->Position().distance_to(object.Position()),
			VPUSH(nearest->Position()));
	}
}
}
#ifdef DEBUG
#	include "PHDebug.h"

#	define	DBG_PH_MOVE_CONDITIONS(c)				c
#else // DEBUG
#	define	DBG_PH_MOVE_CONDITIONS(c)					
#endif // DEBUG

#define DISTANCE_PHISICS_ENABLE_CHARACTERS 2.f

float CMovementManager::speed(CPHMovementControl* movement_control) const
{
	VERIFY(movement_control);
	if (fis_zero(m_speed))
		return (0.f);

	if (movement_control->IsCharacterEnabled())
		return (movement_control->GetXZActVelInGoingDir());

	return (m_speed);
}
#ifdef	DEBUG
BOOL dbg_dump_collision_hit =FALSE;
void dump_collision_hit(CPHMovementControl *movement_control)
{
	if( !dbg_dump_collision_hit )
		return;
	VERIFY( movement_control );
	//CPHCharacter * phch = movement_control->PHCharacter();
	//VERIFY( phch );

	IPhysicsShellHolder  *iobj = movement_control->PhysicsRefObject();
	VERIFY( iobj );
	VERIFY( smart_cast<CPhysicsShellHolder*>(iobj) );
	CPhysicsShellHolder	*obj = static_cast<CPhysicsShellHolder	*>(iobj);
	Msg( "ai unit: %s hited by collision; power: %f, spawn frame %d, current frame %d ", obj->cName().c_str(), movement_control->gcontact_HealthLost, obj->spawn_time(), Device.dwFrame ); 
	//CPhysicsShellHolder* object =static_cast<CPhysicsShellHolder*>(Level().Objects.net_Find(m_collision_damage_info.m_obj_id));
//const ICollisionDamageInfo * di=movement_control->CollisionDamageInfo();
//VERIFY( di );
	//di->
}
#endif
void CMovementManager::apply_collision_hit(CPHMovementControl* movement_control)
{
	VERIFY(movement_control);
	if (object().g_Alive() && !fsimilar(0.f, movement_control->gcontact_HealthLost))
	{
#ifdef	DEBUG
		dump_collision_hit( movement_control );
#endif
		const ICollisionDamageInfo* di = movement_control->CollisionDamageInfo();
		VERIFY(di);
		Fvector dir;
		di->HitDir(dir);

		SHit HDS = SHit(movement_control->gcontact_HealthLost,
		                dir,
		                di->DamageInitiator(),
		                movement_control->ContactBone(),
		                di->HitPos(),
		                0.f,
		                di->HitType(),
		                0.0f,
		                false);
		object().Hit(&HDS);
	}
}

bool CMovementManager::move_along_path() const
{
	if (!enabled())
		return (false);

	if (!actual())
		return (false);

	//	if (path_completed())
	//		return			(true);

	if (detail().path().empty())
		return (false);

	if (detail().completed(object().Position(), true))
		return (false);

	if (detail().curr_travel_point_index() >= detail().path().size() - 1)
		return (false);

	if (fis_zero(old_desirable_speed()))
		return (false);

	return (true);
}

Fvector CMovementManager::path_position(const float& velocity, const Fvector& position, const float& time_delta,
                                        u32& current_travel_point, float& dist, float& dist_to_target,
                                        Fvector& dir_to_target)
{
	VERIFY(current_travel_point < (detail().path().size() - 1));

	Fvector dest_position = position;

	// Вычислить пройденную дистанцию, определить целевую позицию на маршруте, 
	//			 изменить detail().m_current_travel_point

	float desirable_speed = velocity; // желаемая скорость объекта
	dist = desirable_speed * time_delta; // пройденное расстояние в соостветствие с желаемой скоростью 

	// определить целевую точку
	Fvector target;

	// обновить detail().m_current_travel_point в соответствие с текущей позицией
	while (current_travel_point < detail().path().size() - 2)
	{
		float pos_dist_to_cur_point = dest_position.distance_to(detail().path()[current_travel_point].position);
		float pos_dist_to_next_point = dest_position.distance_to(detail().path()[current_travel_point + 1].position);
		float cur_point_dist_to_next_point = detail().path()[current_travel_point].position.distance_to(
			detail().path()[
				current_travel_point + 1].
			position);

		if ((pos_dist_to_cur_point > cur_point_dist_to_next_point) && (pos_dist_to_cur_point > pos_dist_to_next_point))
		{
			++current_travel_point;
		}
		else break;
	}

	target.set(detail().path()[current_travel_point + 1].position);
	// определить направление к целевой точке
	dir_to_target.sub(target, dest_position);

	// дистанция до целевой точки
	dist_to_target = dir_to_target.magnitude();

	while (dist > dist_to_target)
	{
		dest_position.set(target);
		dist -= dist_to_target;

		if (current_travel_point + 1 >= detail().path().size())
		{
			//			VERIFY				(dist <= dist_to_target);
			return (dest_position);
		}

		++current_travel_point;
		if ((current_travel_point + 1) >= detail().path().size())
		{
			//			VERIFY				(dist <= dist_to_target);
			dist = 0.f;
			return (dest_position);
		}

		target.set(detail().path()[current_travel_point + 1].position);
		dir_to_target.sub(target, dest_position);
		dist_to_target = dir_to_target.magnitude();
	}

	VERIFY(dist <= dist_to_target);
	return (dest_position);
}

Fvector CMovementManager::path_position(const float& time_to_check)
{
	if (path_completed())
		return (object().Position());

	if (detail().path().empty())
		return (object().Position());

	if (detail().completed(object().Position(), true))
		return (object().Position());

	Fvector dir_to_target;
	float dist_to_target;
	float dist;
	u32 current_travel_point = detail().m_current_travel_point;
	return (
		path_position(
			old_desirable_speed(),
			object().Position(),
			time_to_check,
			current_travel_point,
			dist,
			dist_to_target,
			dir_to_target
		)
	);
}

void CMovementManager::move_along_path(CPHMovementControl* movement_control, Fvector& dest_position, float time_delta)
{
	START_PROFILE("Build Path/Move Along Path")
		VERIFY(movement_control);

		Fvector motion;
		dest_position = object().Position();

		float precision = 0.5f;

		// Если нет движения по пути
		if (!move_along_path())
		{
			m_speed = 0.f;


			DBG_PH_MOVE_CONDITIONS(
				if(ph_dbg_draw_mask.test(phDbgNeverUseAiPhMove)){movement_control->SetPosition(dest_position);if(
					movement_control->CharacterExist( ) )movement_control->DisableCharacter();})
			if (movement_control->IsCharacterEnabled())
			{
				movement_control->Calculate(detail().path(), 0.f, detail().m_current_travel_point, precision);
				movement_control->GetPosition(dest_position);
			}

			// проверка на хит
			apply_collision_hit(movement_control);
			//		Msg				("[%6d][%s] no move, curr_tp=%d",Device.dwFrame,*object().cName(),detail().m_current_travel_point);
			return;
		}

		//. 	VERIFY2(movement_control->CharacterExist() || object().animation_movement_controlled() , "! Can not move - physics movement shell does not exist. Try to move in wonded state?" );
		if (!movement_control->CharacterExist())
			return;

		if (time_delta < EPS) return;

		float desirable_speed = old_desirable_speed(); // желаемая скорость объекта
		float desirable_dist = desirable_speed * time_delta;
		float dist;

		// position_computation
		Fvector dir_to_target;
		float dist_to_target;
		u32 current_travel_point = detail().m_current_travel_point;
		dest_position = path_position(old_desirable_speed(), object().Position(), time_delta, current_travel_point,
		                              dist, dist_to_target, dir_to_target);

		// Lain: added steering behaviour
		// 	Fvector target; 
		// 	target.add(dest_position, dir_to_target);
		// 	Fvector steer_offs = m_steer_manager->calc_acceleration();
		// 	steer_offs.mul(time_delta*10.f);
		// 	target.add(steer_offs);
		// 	dir_to_target.sub(target, dest_position);
		// 	dist_to_target = dir_to_target.magnitude();
		// 	Fvector steer_offs = m_steer_manager->calc_acceleration();
		// 	steer_offs.mul(time_delta*1000.f);
		// 	movement_control->AddControlVel(steer_offs);

		if (detail().m_current_travel_point != current_travel_point)
			on_travel_point_change(detail().m_current_travel_point);
		detail().m_current_travel_point = current_travel_point;

		if (dist_to_target < EPS_L)
		{
#pragma todo("Dima to ? : is this correct?")
			if (current_travel_point + 1 < detail().path().size())
				detail().m_current_travel_point = current_travel_point + 1;
			else
				detail().m_current_travel_point = detail().path().size() - 1;
			m_speed = 0.f;
			//Msg				("[%6d][%s] strange exit, curr_tp=%d",Device.dwFrame,*object().cName(),detail().m_current_travel_point);
			return;
		}
		//	Msg					("[%6d][%s] curr_tp=%d",Device.dwFrame,*object().cName(),detail().m_current_travel_point);

		// Физика устанавливает новую позицию
		Device.Statistic->Physics.Begin();

		// получить физ. объекты в радиусе
		m_nearest_objects.clear_not_free();
		Level().ObjectSpace.GetNearest(m_nearest_objects, dest_position,
		                               DISTANCE_PHISICS_ENABLE_CHARACTERS + (movement_control->IsCharacterEnabled()
			                                                                     ? 0.5f
			                                                                     : 0.f), &object());

		const u32 sp_mphys_nearest_count = (u32)m_nearest_objects.size();
		const CObject* sp_mphys_nearest0 = sp_mphys_nearest_count ? m_nearest_objects.front() : nullptr;
		const Fvector sp_mphys_start_position = object().Position();

		// установить позицию
		VERIFY(dist >= 0.f);
		VERIFY(dist_to_target >= 0.f);
		//	VERIFY				(dist <= dist_to_target);
		motion.mul(dir_to_target, dist / dist_to_target);
		dest_position.add(motion);
		Fvector sp_mphys_requested_position = dest_position;
		const float sp_mphys_real_motion = sp_mphys_start_position.distance_to(sp_mphys_requested_position);
		const float sp_mphys_real_speed = time_delta > EPS_L ? sp_mphys_real_motion / time_delta : 0.f;

		Fvector velocity = dir_to_target;
		velocity.normalize_safe();
		if (velocity.y > 0.9f)
			velocity.y = 0.8f;
		if (velocity.y < -0.9f)
			velocity.y = -0.8f;
		velocity.normalize_safe(); //как не странно, mdir - не нормирован
		velocity.mul(desirable_speed); //*1.25f

		if (!movement_control->PhysicsOnlyMode())
			movement_control->SetCharacterVelocity(velocity);

		if (DBG_PH_MOVE_CONDITIONS(
			ph_dbg_draw_mask.test(phDbgNeverUseAiPhMove)||!ph_dbg_draw_mask.test(phDbgAlwaysUseAiPhMove)&&)!(
			m_nearest_objects.empty()))
		{
			//  физ. объект

			if (DBG_PH_MOVE_CONDITIONS(!ph_dbg_draw_mask.test(phDbgNeverUseAiPhMove)&&) !movement_control->TryPosition(
				dest_position))
			{
				LPCSTR sp_mphys_branch = "try_fail";
				movement_control->GetPosition(dest_position);
				movement_control->Calculate(detail().path(), desirable_speed, detail().m_current_travel_point,
				                            precision);

				// Collision hit check.
				apply_collision_hit(movement_control);

				Fvector sp_mphys_result_position = dest_position;
				movement_control->GetPosition(sp_mphys_result_position);
				sp_mphys_diag_log(sp_mphys_branch, object(), movement_control, sp_mphys_start_position, sp_mphys_requested_position,
				                  sp_mphys_result_position, desirable_speed, sp_mphys_real_speed, time_delta,
				                  sp_mphys_nearest_count, sp_mphys_nearest0, detail().m_current_travel_point, dist, dist_to_target);
				sp_mphys_diag_log_blockers(object(), m_nearest_objects, sp_mphys_start_position, sp_mphys_requested_position, sp_mphys_result_position);
			}
			else
			{
				DBG_PH_MOVE_CONDITIONS(
					if(ph_dbg_draw_mask.test(phDbgNeverUseAiPhMove)){movement_control->SetPosition(dest_position);
					movement_control->DisableCharacter();})
				movement_control->b_exect_position = true;
				Fvector sp_mphys_result_position = dest_position;
				movement_control->GetPosition(sp_mphys_result_position);
                sp_mphys_diag_log("try_ok", object(), movement_control, sp_mphys_start_position, sp_mphys_requested_position,
				                   sp_mphys_result_position, desirable_speed, sp_mphys_real_speed, time_delta,
				                   sp_mphys_nearest_count, sp_mphys_nearest0, detail().m_current_travel_point, dist, dist_to_target);
				sp_mphys_diag_log_blockers(object(), m_nearest_objects, sp_mphys_start_position, sp_mphys_requested_position, sp_mphys_result_position);
			}
			movement_control->GetPosition(dest_position);
		}
		else
		{
			//DBG_PH_MOVE_CONDITIONS( if(ph_dbg_draw_mask.test(phDbgNeverUseAiPhMove)){movement_control->SetPosition(dest_position);movement_control->DisableCharacter();})
			movement_control->SetPosition(dest_position);
			movement_control->DisableCharacter();
			movement_control->b_exect_position = true;
            sp_mphys_diag_log("direct_set", object(), movement_control, sp_mphys_start_position, sp_mphys_requested_position,
			                   dest_position, desirable_speed, sp_mphys_real_speed, time_delta,
			                   sp_mphys_nearest_count, sp_mphys_nearest0, detail().m_current_travel_point, dist, dist_to_target);
		}
		/*
		} else { // есть физ. объекты
	
			movement_control->Calculate				(detail().path(), desirable_speed, detail().m_current_travel_point, precision);
			movement_control->GetPosition			(dest_position);
			
			// проверка на хит
			apply_collision_hit						(movement_control);
		}
			*/

		// установить скорость
		float real_motion = motion.magnitude() + desirable_dist - dist;
		float real_speed = real_motion / time_delta;

		m_speed = 0.5f * desirable_speed + 0.5f * real_speed;


		// Физика устанавливает позицию в соответствии с нулевой скоростью 
		if (detail().completed(dest_position, true))
		{
			if (!movement_control->PhysicsOnlyMode())
			{
				Fvector velocity = {0.f, 0.f, 0.f};
				movement_control->SetVelocity(velocity);
				m_speed = 0.f;
			}
		}

		Device.Statistic->Physics.End();

	STOP_PROFILE
}
