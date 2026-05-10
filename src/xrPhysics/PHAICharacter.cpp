#include "stdafx.h"


#include "PHDynamicData.h"
#include "Physics.h"
#include "ExtendedGeom.h"
#include "../xrEngine/cl_intersect.h"
#include "tri-colliderKNoOPC\__aabb_tri.h"

#include "phaicharacter.h"
#include "../xrengine/device.h"
#include "../xrCore/xrCore.h"
#include "IPhysicsShellHolder.h"

#ifdef DEBUG
//#	include "../xrEngine/StatGraph.h"
#	include "debug_output.h"
//#	include "level.h"
//#	include "debug_renderer.h"
#endif

CPHAICharacter::CPHAICharacter()
{
	m_forced_physics_control = false;
}

namespace
{
bool sp_trypos_diag_enabled()
{
	return Core.Params && strstr(Core.Params, "-sp_trypos_diag");
}

bool sp_trypos_diag_allow()
{
	static u32 next_time = 0;
	static u32 count = 0;
	const u32 now = Device.dwTimeGlobal;
	if (count < 512)
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

void sp_trypos_diag_log(CPHAICharacter* character, LPCSTR stage, LPCSTR reason, const Fvector& current_pos,
	const Fvector& requested_pos, const Fvector& result_pos, float disp_mag, float foot_radius, u32 steps_num,
	float rest, u32 fail_step_idx, bool exact_state, bool ret)
{
	if (!sp_trypos_diag_enabled() || !sp_trypos_diag_allow())
		return;

	IPhysicsShellHolder* ref_object = character ? character->PhysicsRefObject() : nullptr;
	Fvector velocity;
	velocity.set(0.f, 0.f, 0.f);
	if (character && character->b_exist)
		character->GetVelocity(velocity);

	Msg("* [SP_TRYPOS] id=%hu name=[%s] sect=[%s] stage=%s reason=%s ret=%d exact=%d exist=%d forced=%d jump=%d collide_now=%d enabled=%d disp=%.4f foot=%.4f steps=%u rest=%.4f fail_idx=%u fixed=%.4f dt=%.4f cur=(%.3f %.3f %.3f) req=(%.3f %.3f %.3f) res=(%.3f %.3f %.3f) move=%.4f vel=(%.3f %.3f %.3f)",
		ref_object ? ref_object->ObjectID() : u16(-1),
		ref_object ? ref_object->ObjectName() : "",
		ref_object ? ref_object->ObjectNameSect() : "",
		stage ? stage : "",
		reason ? reason : "",
		ret ? 1 : 0,
		exact_state ? 1 : 0,
		character && character->b_exist ? 1 : 0,
		character && character->ForcedPhysicsControl() ? 1 : 0,
		character && character->JumpState() ? 1 : 0,
		character && character->Island().IsObjGroun() ? 1 : 0,
		character && character->IsEnabled() ? 1 : 0,
		disp_mag,
		foot_radius,
		steps_num,
		rest,
		fail_step_idx,
		fixed_step,
		inl_ph_world().Device().fTimeDelta,
		VPUSH(current_pos),
		VPUSH(requested_pos),
		VPUSH(result_pos),
		current_pos.distance_to(result_pos),
		VPUSH(velocity));
}
}

void CPHAICharacter::Create(dVector3 sizes)
{
	inherited::Create(sizes);

	m_forced_physics_control = false; //.
}

bool CPHAICharacter::TryPosition(Fvector pos, bool exact_state)
{
	Fvector current_pos;
	current_pos.set(0.f, 0.f, 0.f);
	if (b_exist)
		GetPosition(current_pos);

	if (!b_exist)
	{
		sp_trypos_diag_log(this, "precheck", "no_exist", current_pos, pos, current_pos, 0.f, 0.f, 0, 0.f, u32(-1), exact_state, false);
		return false;
	}
	if (m_forced_physics_control)
	{
		sp_trypos_diag_log(this, "precheck", "forced_control", current_pos, pos, current_pos, 0.f, 0.f, 0, 0.f, u32(-1), exact_state, false);
		return false;
	}
	if (JumpState())
	{
		sp_trypos_diag_log(this, "precheck", "jump", current_pos, pos, current_pos, 0.f, 0.f, 0, 0.f, u32(-1), exact_state, false);
		return false;
	}
	if (DoCollideObj())
	{
		Fvector collide_pos;
		GetPosition(collide_pos);
		sp_trypos_diag_log(this, "precheck", "initial_collide", current_pos, pos, collide_pos, current_pos.distance_to(pos), FootRadius(), 0, 0.f, u32(-1), exact_state, false);
		return false;
	}
	Fvector cur_vel;
	GetVelocity(cur_vel);

	Fvector displace;
	displace.sub(pos, current_pos);
	float disp_mag = displace.magnitude();

	if (fis_zero(disp_mag))
	{
		sp_trypos_diag_log(this, "precheck", "zero_displace", current_pos, pos, current_pos, disp_mag, FootRadius(), 0, 0.f, u32(-1), exact_state, true);
		return true;
	}
	if (fis_zero(inl_ph_world().Device().fTimeDelta))
	{
		sp_trypos_diag_log(this, "precheck", "zero_dt", current_pos, pos, current_pos, disp_mag, FootRadius(), 0, 0.f, u32(-1), exact_state, true);
		return true;
	}
	const u32 max_steps = 15;
	const float fmax_steps = float(max_steps);
	float fsteps_num = 1.f;
	u32 steps_num = 1;
	float disp_pstep = FootRadius();
	float rest = 0.f;

	float parts = disp_mag / disp_pstep;
	fsteps_num = floor(parts);
	steps_num = iFloor(parts);
	if (steps_num > max_steps)
	{
		steps_num = max_steps;
		fsteps_num = fmax_steps;
		disp_pstep = disp_mag / fsteps_num;
	}
	rest = disp_mag - fsteps_num * disp_pstep;

	Fvector vel;
	vel.mul(displace, disp_pstep / fixed_step / disp_mag);
	bool ret = true;
	int save_gm = dBodyGetGravityMode(m_body);
	dBodySetGravityMode(m_body, 0);
	for (u32 i = 0; steps_num > i; ++i)
	{
		SetVelocity(vel);
		Enable();
		if (!step_single(fixed_step))
		{
			SetVelocity(cur_vel);
			Fvector pos_failed;
			GetPosition(pos_failed);
			sp_trypos_diag_log(this, "step_loop", "step_single_false", current_pos, pos, pos_failed, disp_mag, FootRadius(), steps_num, rest, i, exact_state, false);
			ret = false;
			break;
		}
	}

	vel.mul(displace, rest / fixed_step / disp_mag);
	SetVelocity(vel);
	Enable();
	ret = step_single(fixed_step);


	dBodySetGravityMode(m_body, save_gm);
	SetVelocity(cur_vel);
	Fvector pos_new;
	GetPosition(pos_new);
	if (!ret)
		sp_trypos_diag_log(this, "final_step", "step_single_false", current_pos, pos, pos_new, disp_mag, FootRadius(), steps_num, rest, steps_num, exact_state, false);

#if 0
	Fvector	dif;dif .sub( pos, pos_new );
	float	dif_m = dif.magnitude();
	if(ret&&dif_m>EPS_L)
	{
		Msg("dif vec %f,%f,%f \n",dif.x,dif.y,dif.z);
		Msg("dif mag %f \n",dif_m);
	}
#endif

	SetPosition(pos_new);
	m_last_move.sub(pos_new, current_pos).mul(1.f / inl_ph_world().Device().fTimeDelta);
	m_body_interpolation.UpdatePositions();
	m_body_interpolation.UpdatePositions();
	if (ret)
		Disable();
	m_collision_damage_info.m_contact_velocity = 0.f;
	if (ret && sp_trypos_diag_enabled())
		sp_trypos_diag_log(this, "ok", "accepted", current_pos, pos, pos_new, disp_mag, FootRadius(), steps_num, rest, u32(-1), exact_state, true);
	return ret;
}

/*
void CPHAICharacter::		SetPosition	(const Fvector &pos)	
{
	//m_vDesiredPosition.set(pos);
	inherited::SetPosition(pos);

}
*/
/*
void CPHAICharacter::BringToDesired(float time,float velocity,float force)
{
	Fvector pos,move;
	GetPosition(pos);

	move.sub(m_vDesiredPosition,pos);
	move.y=0.f;
	float dist=move.magnitude();

	float vel;
	if(dist>EPS_L*100.f)
	{
		vel=dist/time;
		move.mul(1.f/dist);
	}
	else if(dist>EPS_L*10.f)
	{
		vel=dist*dist*dist;
		move.mul(1.f/dist);
	}
	else
	{
		vel=0.f;
		move.set(0,0,0);
	}

	if(vel>velocity)//&&velocity>EPS_L
		vel=velocity;

	if(velocity<EPS_L/fixed_step) 
	{
		vel=0.f;
		move.set(0,0,0);
	}

	SetMaximumVelocity(vel);

	SetAcceleration(move);
}

*/

void CPHAICharacter::Jump(const Fvector& jump_velocity)
{
	b_jump = true;
	m_jump_accel.set(jump_velocity);
}

void CPHAICharacter::ValidateWalkOn()
{
	//if(b_on_object)
	//	ValidateWalkOnObject();
	// else 
	//	 b_clamb_jump=true;
	inherited::ValidateWalkOn();
}

void CPHAICharacter::InitContact(dContact* c, bool& do_collide, u16 material_idx_1, u16 material_idx_2)
{
	SGameMtl* material_1 = GMLibrary().GetMaterialByIdx(material_idx_1);
	SGameMtl* material_2 = GMLibrary().GetMaterialByIdx(material_idx_2);
	if ((material_1 && material_1->Flags.test(SGameMtl::flActorObstacle)) || (material_2 && material_2
	                                                                                        ->Flags.test(
		                                                                                        SGameMtl::
		                                                                                        flActorObstacle)))
		do_collide = true;
	inherited::InitContact(c, do_collide, material_idx_1, material_idx_2);
	if (is_control || b_lose_control || b_jumping)
		c->surface.mu = 0.00f;
	dxGeomUserData* D1 = retrieveGeomUserData(c->geom.g1);
	dxGeomUserData* D2 = retrieveGeomUserData(c->geom.g2);
	if (D1 && D2 && D1->ph_object && D2->ph_object && D1->ph_object->CastType() == tpCharacter && D2
	                                                                                              ->ph_object->
	                                                                                              CastType() ==
		tpCharacter)
	{
		b_on_object = true;
		b_valide_wall_contact = false;
	}
#ifdef DEBUG
	if(debug_output().ph_dbg_draw_mask().test(phDbgNeverUseAiPhMove))do_collide=false;
#endif
}

/*
EEnvironment CPHAICharacter::CheckInvironment()
{

	return inherited::CheckInvironment();
}
*/
#ifdef DEBUG
void	CPHAICharacter::OnRender()	
{
	inherited::OnRender();
#if	0
	if(!b_exist) return;

	Fvector pos;
	GetDesiredPosition(pos);
	pos.y+=m_radius;


	Fvector scale;
	scale.set(0.35f,0.35f,0.35f);
	Fmatrix M;
	M.identity();
	M.scale(scale);
	M.c.set(pos);


	Level().debug_renderer().draw_ellipse(M, 0xffffffff);
#endif
}
#endif
