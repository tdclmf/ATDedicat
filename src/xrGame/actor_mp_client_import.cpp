#include "stdafx.h"
#include "actor_mp_client.h"
#include "inventory.h"
#include "level.h"
#include "GamePersistent.h"
#include "game_cl_base.h"
#include "../xrEngine/camerabase.h"
//#include "Physics.h"
#include "../xrphysics/phvalide.h"
#include "weapon_trace.h"

extern bool g_dedicated_single_import_from_cl_update;

void CActorMP::net_Import(NET_Packet& P)
{
	net_update N;

	m_state_holder.read(P);
	R_ASSERT2(valid_pos(m_state_holder.state().position), "imported bad position");
	const u16 imported_active_slot = m_state_holder.state().inventory_active_slot;
	if (imported_active_slot != inventory().GetActiveSlot())
	{
		WPN_TRACE("ActorMP::net_Import actor=%u local=%d remote=%d on_client=%d on_server=%d active_slot=%u->%u",
			ID(), Local() ? 1 : 0, Remote() ? 1 : 0, OnClient() ? 1 : 0, OnServer() ? 1 : 0,
			inventory().GetActiveSlot(), imported_active_slot);
	}

	const bool ignore_remote_from_non_cl_update =
		!g_dedicated_server &&
		OnClient() &&
		Game().Type() == eGameIDSingle &&
		!Local() &&
		!g_dedicated_single_import_from_cl_update;
	if (ignore_remote_from_non_cl_update)
	{
		WPN_TRACE("ActorMP::net_Import ignored remote non-M_CL_UPDATE actor=%u slot=%u", ID(), imported_active_slot);
		return;
	}

	/*if (m_i_am_dead)
		return;*/

	if (OnClient())
	{
		/*#ifdef DEBUG
				if (GetfHealth() != m_state_holder.state().health)
					Msg("net_Import: [%d][%s], is going to set health to %2.04f", this->ID(), Name(), m_state_holder.state().health);
		#endif*/

		game_PlayerState* ps = Game().GetPlayerByGameID(this->object_id());
		float new_health = m_state_holder.state().health;
		if (GetfHealth() < new_health)
		{
			SetfHealth(new_health);
		}
		else
		{
			if (!ps || !ps->testFlag(GAME_PLAYER_FLAG_INVINCIBLE))
			{
				SetfHealth(new_health);
			}
		}
	}

	if (PPhysicsShell() != NULL)
	{
		return;
	}


	if (OnClient())
		SetfRadiation(m_state_holder.state().radiation * 100.0f);

	u16 ActiveSlot = m_state_holder.state().inventory_active_slot;
	const bool dedicated_single_local_actor_import =
		!g_dedicated_server &&
		OnClient() &&
		Game().Type() == eGameIDSingle &&
		Local();

	if (OnClient() && !dedicated_single_local_actor_import && (inventory().GetActiveSlot() != ActiveSlot))
	{
#ifdef DEBUG
		Msg("Client-SetActiveSlot[%d][%d]",ActiveSlot, Device.dwFrame);
#endif // #ifdef DEBUG
		WPN_TRACE("ActorMP::net_Import apply active slot actor=%u slot=%u old_slot=%u",
			ID(), ActiveSlot, inventory().GetActiveSlot());
		inventory().SetActiveSlot(ActiveSlot);
	}
	else if (OnClient() && dedicated_single_local_actor_import && (inventory().GetActiveSlot() != ActiveSlot))
	{
		WPN_TRACE("ActorMP::net_Import skip active slot apply for local dedicated-single actor=%u incoming_slot=%u current_slot=%u",
			ID(), ActiveSlot, inventory().GetActiveSlot());
	}

	N.mstate = m_state_holder.state().body_state_flags;

	N.dwTimeStamp = m_state_holder.state().time;
	N.p_pos = m_state_holder.state().position;

	N.o_model = m_state_holder.state().model_yaw;
	N.o_torso.yaw = m_state_holder.state().camera_yaw;
	N.o_torso.pitch = m_state_holder.state().camera_pitch;
	N.o_torso.roll = m_state_holder.state().camera_roll;

	if (N.o_torso.roll > PI)
		N.o_torso.roll -= PI_MUL_2;

	{
		if (Level().IsDemoPlay() || OnServer() || Remote())
		{
			unaffected_r_torso.yaw = N.o_torso.yaw;
			unaffected_r_torso.pitch = N.o_torso.pitch;
			unaffected_r_torso.roll = N.o_torso.roll;

			cam_Active()->yaw = -N.o_torso.yaw;
			cam_Active()->pitch = N.o_torso.pitch;
		};
	};

	//CSE_ALifeCreatureActor
	N.p_accel = m_state_holder.state().logic_acceleration;

	process_packet(N);

	net_update_A N_A;
	m_States.clear();

	N_A.State.enabled = m_state_holder.state().physics_state_enabled;
	N_A.State.angular_vel = m_state_holder.state().physics_angular_velocity;
	N_A.State.linear_vel = m_state_holder.state().physics_linear_velocity;
	N_A.State.force = m_state_holder.state().physics_force;
	N_A.State.torque = m_state_holder.state().physics_torque;
	N_A.State.position = m_state_holder.state().physics_position;
	N_A.State.quaternion = m_state_holder.state().physics_quaternion;

	// interpolcation
	postprocess_packet(N_A);
}

void CActorMP::postprocess_packet(net_update_A& N_A)
{
	if (!NET.empty())
		N_A.dwTimeStamp = NET.back().dwTimeStamp;
	else
		N_A.dwTimeStamp = Level().timeServer();

	N_A.State.previous_position = N_A.State.position;
	N_A.State.previous_quaternion = N_A.State.quaternion;

	if (Local() && OnClient() || !g_Alive()) return;

	{
		//-----------------------------------------------
		if (!NET_A.empty() && N_A.dwTimeStamp < NET_A.back().dwTimeStamp) return;
		if (!NET_A.empty() && N_A.dwTimeStamp == NET_A.back().dwTimeStamp)
		{
			NET_A.back() = N_A;
		}
		else
		{
			VERIFY(valid_pos(N_A.State.position));
			NET_A.push_back(N_A);
			if (NET_A.size() > 5) NET_A.pop_front();
		};

		if (!NET_A.empty()) m_bInterpolate = true;
	};

	Level().AddObject_To_Objects4CrPr(this);
	CrPr_SetActivated(false);
	CrPr_SetActivationStep(0);
}

void CActorMP::process_packet(net_update&N)
{
	if (Local() && OnClient())
		return;

	if (!NET.empty() && (N.dwTimeStamp < NET.back().dwTimeStamp))
		return;

	if (g_Alive())
	{
		setVisible((BOOL)!HUDview());
		setEnabled(TRUE);
	};

	if (!NET.empty() && (N.dwTimeStamp == NET.back().dwTimeStamp))
	{
		NET.back() = N;
		return;
	}

	NET.push_back(N);

	if (NET.size() > 5)
		NET.pop_front();
}
