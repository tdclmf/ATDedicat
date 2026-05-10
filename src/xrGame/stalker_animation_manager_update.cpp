////////////////////////////////////////////////////////////////////////////
//	Module 		: stalker_animation_manager_update.cpp
//	Created 	: 25.02.2003
//  Modified 	: 13.12.2006
//	Author		: Dmitriy Iassenev
//	Description : Stalker animation manager update cycle
////////////////////////////////////////////////////////////////////////////

#include "pch_script.h"
#include "stalker_animation_manager.h"
#include "ai/stalker/ai_stalker.h"
#include "game_object_space.h"
#include "script_callback_ex.h"
#include "profiler.h"
#include "stalker_movement_manager_smart_cover.h"

namespace
{
enum EStalkerNetworkAnimationTrack
{
	eStalkerNetAnimGlobal = 0,
	eStalkerNetAnimHead,
	eStalkerNetAnimTorso,
	eStalkerNetAnimLegs,
	eStalkerNetAnimScript,
	eStalkerNetAnimCount
};

bool network_motion_valid(u32 motion_value, MotionID& motion)
{
	if (motion_value == u32(u16(-1)))
	{
		motion.invalidate();
		return false;
	}

	motion.val = motion_value;
	return motion.valid();
}

void sync_network_animation_pair_time(
	CStalkerAnimationPair& pair,
	u32 motion_value,
	u16 packed_time,
	u8 flags,
	u32& time_applied,
	u32& missing,
	u32& mismatched)
{
	if (!(flags & 1))
		return;

	MotionID server_motion;
	if (!network_motion_valid(motion_value, server_motion))
		return;

	CBlend* blend = pair.blend();
	if (!blend)
	{
		++missing;
		return;
	}

	if (pair.animation() != server_motion)
	{
		++mismatched;
		return;
	}

	if ((flags & 2) && (blend->timeTotal > EPS_L))
	{
		blend->timeCurrent = blend->timeTotal * (float(packed_time) / 65535.f);
		clamp(blend->timeCurrent, 0.f, blend->timeTotal);
		++time_applied;
	}
}
}

void CStalkerAnimationManager::play_delayed_callbacks()
{
	if (m_call_script_callback)
	{
		m_call_script_callback = false;
		object().callback(GameObject::eScriptAnimation)();
		return;
	}

	if (m_call_global_callback)
	{
		m_call_global_callback = false;
		if (m_global_callback)
			m_global_callback();
		return;
	}
}

IC bool CStalkerAnimationManager::script_callback() const
{
	if (script_animations().empty())
		return (false);

	return (object().callback(GameObject::eScriptAnimation));
}

IC bool CStalkerAnimationManager::need_update() const
{
	if (script_callback())
		return (true);

	return (non_script_need_update());
}

IC void CStalkerAnimationManager::update_tracks()
{
	if (!need_update())
		return;

	m_skeleton_animated->UpdateTracks();
}

#ifdef USE_HEAD_BONE_PART_FAKE
IC void CStalkerAnimationManager::play_script_impl()
{
	clear_unsafe_callbacks();
	global().reset();
	torso().reset();
	legs().reset();

	const CStalkerAnimationScript& selected = assign_script_animation();
	script().animation(selected.animation());
	if (selected.use_movement_controller())
	{
		script().target_matrix(selected.transform(object()));
		if (m_start_new_script_animation)
		{
			m_start_new_script_animation = false;
			if (selected.has_transform() && object().animation_movement())
				object().destroy_anim_mov_ctrl();
		}
	}

	script().play(
		m_skeleton_animated,
		script_play_callback,
		selected.use_movement_controller(),
		selected.local_animation(),
		false,
		m_script_bone_part_mask
	);

	head().animation(assign_head_animation());
	head().play(m_skeleton_animated, head_play_callback, false, false);
}
#else // USE_HEAD_BONE_PART_FAKE
IC	void CStalkerAnimationManager::play_script_impl			()
{
    clear_unsafe_callbacks	();
    global().reset			();
    head().reset			();
    torso().reset			();
    legs().reset			();

    const CStalkerAnimationScript	&selected = assign_script_animation();
    script().animation		(selected.animation());
    script().play			(
        m_skeleton_animated,
        script_play_callback,
        selected.use_movement_controller(),
        selected.local_animation(),
        false,
        m_script_bone_part_mask
        );
}
#endif // USE_HEAD_BONE_PART_FAKE

bool CStalkerAnimationManager::play_script()
{
	if (script_animations().empty())
	{
		if (m_network_script_animation_active && m_network_script_animation.valid())
		{
			if ((script().animation() != m_network_script_animation) || !script().blend())
			{
				script().animation(m_network_script_animation);
				script().play(m_skeleton_animated, script_play_callback, false, false);
			}
			return (true);
		}

		m_start_new_script_animation = false;
		script().reset();
		return (false);
	}

	m_network_script_animation_active = false;
	m_network_script_animation.invalidate();
	play_script_impl();

	return (true);
}

#ifdef USE_HEAD_BONE_PART_FAKE
IC void CStalkerAnimationManager::play_global_impl(const MotionID& animation, bool const& animation_movement_controller)
{
	torso().reset();
	legs().reset();

	global().animation(animation);
	global().play(
		m_skeleton_animated,
		global_play_callback,
		animation_movement_controller,
		true,
		false,
		m_script_bone_part_mask,
		true
	);

	if (m_global_modifier)
		m_global_modifier(global().blend());

	head().animation(assign_head_animation());
	head().play(m_skeleton_animated, head_play_callback, false, false);
}
#else // USE_HEAD_BONE_PART_FAKE
IC	void CStalkerAnimationManager::play_global_impl			(const MotionID &animation, bool const &animation_movement_controller)
{
    head().reset			();
    torso().reset			();
    legs().reset			();

    global().animation		(animation);
    global().play			(m_skeleton_animated,global_play_callback,false,false,false);
}
#endif // USE_HEAD_BONE_PART_FAKE

bool CStalkerAnimationManager::play_global()
{
	bool animation_movement_controller = false;
	const MotionID& global_animation = assign_global_animation(animation_movement_controller);
	if (!global_animation)
	{
		clear_unsafe_callbacks();
		global().reset();
		return (false);
	}

	play_global_impl(global_animation, animation_movement_controller);

	return (true);
}

IC void CStalkerAnimationManager::play_head()
{
	head().animation(assign_head_animation());
	head().play(m_skeleton_animated, head_play_callback, false, false);
}

IC void CStalkerAnimationManager::play_torso()
{
	torso().animation(assign_torso_animation());
	torso().play(m_skeleton_animated, torso_play_callback, false, false);
}

void CStalkerAnimationManager::play_legs()
{
	float speed = 0.f;
	bool first_time = !legs().animation();
	bool result = legs().animation(assign_legs_animation());

	if (!first_time && !result && legs().blend())
	{
		float amount = legs().blend()->blendAmount;
		m_previous_speed = (m_target_speed - m_previous_speed) * amount + m_previous_speed;
	}

	legs().play(m_skeleton_animated, legs_play_callback, false, false, !fis_zero(m_target_speed));

	if (result && legs().blend())
	{
		float amount = legs().blend()->blendAmount;
		speed = (m_target_speed - m_previous_speed) * amount + m_previous_speed;
	}

	if (fis_zero(speed))
		return;

	if (!legs().blend())
		return;

	object().movement().setup_speed_from_animation(speed);
}

void CStalkerAnimationManager::apply_network_animation_state(
	const u32* motion,
	const u16* time,
	const u8* flags,
	u32 track_count,
	u32& time_applied,
	u32& missing,
	u32& mismatched,
	u32& forced)
{
	if (!m_skeleton_animated || !motion || !time || !flags || (track_count < eStalkerNetAnimCount))
		return;

	auto force_track = [&](
		CStalkerAnimationPair& pair,
		u32 motion_value,
		u16 packed_time,
		u8 track_flags,
		PlayCallback callback,
		bool continue_interrupted_animation)
	{
		if (!(track_flags & 1))
			return;

		MotionID server_motion;
		if (!network_motion_valid(motion_value, server_motion))
			return;

		const bool same_motion = (pair.animation() == server_motion);
		if (!same_motion)
		{
			++mismatched;
			pair.animation(server_motion);
			pair.play(m_skeleton_animated, callback, false, false, continue_interrupted_animation);
			++forced;
		}
		else if (!pair.blend())
		{
			++missing;
			pair.make_inactual();
			pair.play(m_skeleton_animated, callback, false, false, continue_interrupted_animation);
			++forced;
		}

		CBlend* blend = pair.blend();
		if (!blend)
		{
			++missing;
			return;
		}

		if ((track_flags & 2) && (blend->timeTotal > EPS_L))
		{
			blend->timeCurrent = blend->timeTotal * (float(packed_time) / 65535.f);
			clamp(blend->timeCurrent, 0.f, blend->timeTotal);
			++time_applied;
		}
	};

	auto force_global_track = [&]()
	{
		if (!(flags[eStalkerNetAnimGlobal] & 1))
			return;

		MotionID server_motion;
		if (!network_motion_valid(motion[eStalkerNetAnimGlobal], server_motion))
			return;

		const bool same_motion = (global().animation() == server_motion);
		if (!same_motion)
		{
			++mismatched;
			play_global_impl(server_motion, false);
			++forced;
		}
		else if (!global().blend())
		{
			++missing;
			global().make_inactual();
			play_global_impl(server_motion, false);
			++forced;
		}

		sync_network_animation_pair_time(
			global(),
			motion[eStalkerNetAnimGlobal],
			time[eStalkerNetAnimGlobal],
			flags[eStalkerNetAnimGlobal],
			time_applied,
			missing,
			mismatched
		);
	};

	force_global_track();

	force_track(
		head(),
		motion[eStalkerNetAnimHead],
		time[eStalkerNetAnimHead],
		flags[eStalkerNetAnimHead],
		head_play_callback,
		false
	);
	force_track(
		torso(),
		motion[eStalkerNetAnimTorso],
		time[eStalkerNetAnimTorso],
		flags[eStalkerNetAnimTorso],
		torso_play_callback,
		false
	);
	force_track(
		legs(),
		motion[eStalkerNetAnimLegs],
		time[eStalkerNetAnimLegs],
		flags[eStalkerNetAnimLegs],
		legs_play_callback,
		!fis_zero(m_target_speed)
	);

	MotionID network_script_motion;
	m_network_script_animation_active = (flags[eStalkerNetAnimScript] & 1) && network_motion_valid(motion[eStalkerNetAnimScript], network_script_motion);
	if (m_network_script_animation_active)
		m_network_script_animation = network_script_motion;
	else
		m_network_script_animation.invalidate();

	force_track(
		script(),
		motion[eStalkerNetAnimScript],
		time[eStalkerNetAnimScript],
		flags[eStalkerNetAnimScript],
		script_play_callback,
		false
	);

	torso().synchronize(m_skeleton_animated, legs());
}

void CStalkerAnimationManager::reset_network_animation_state()
{
	clear_unsafe_callbacks();
	global().reset();
	head().reset();
	torso().reset();
	legs().reset();
	script().reset();
	m_network_script_animation_active = false;
	m_network_script_animation.invalidate();

	if (!m_skeleton_animated)
		return;

	for (u16 i = 0; i < MAX_PARTS; ++i)
		m_skeleton_animated->LL_CloseCycle(i);
}

void CStalkerAnimationManager::update_impl()
{
	if (!object().g_Alive())
		return;

	update_tracks();
	play_delayed_callbacks();

	if (play_script())
		return;

	if (play_global())
		return;

	play_head();
	play_torso();
	play_legs();

	torso().synchronize(m_skeleton_animated, m_legs);
}

void CStalkerAnimationManager::update()
{
	START_PROFILE("stalker/client_update/animations")
		try
		{
			update_impl();
		}
		catch (...)
		{
			Msg("! error in stalker with visual %s", *object().cNameVisual());
			/* avo: prevent game from crashing */
			global().reset();
			head().reset();
			torso().reset();
			legs().reset();
			return;
			//throw;
			/* avo: end */
		}
	STOP_PROFILE
}
