////////////////////////////////////////////////////////////////////////////
//	Module 		: dynamic_obstacles_avoider.cpp
//	Created 	: 16.05.2007
//  Modified 	: 16.05.2007
//	Author		: Dmitriy Iassenev
//	Description : dynamic obstacles avoider
////////////////////////////////////////////////////////////////////////////

#include "stdafx.h"
#include "dynamic_obstacles_avoider.h"
#include "ai_space.h"
#include "moving_objects.h"
#include "stalker_movement_manager_smart_cover.h"
#include "ai/stalker/ai_stalker.h"
#include "moving_object.h"

extern int g_sp_diag_net_updates;

void dynamic_obstacles_avoider::query()
{
	moving_object* object_to_query = object().get_moving_object();
	if (!object_to_query)
	{
		m_current_iteration.clear();
		return;
	}

	ai().moving_objects().query_action_dynamic(object_to_query);
	m_current_iteration.swap(object_to_query->dynamic_query());

	static u32 s_sp_dyn_obs_next = 0;
	static u32 s_sp_dyn_obs_count = 0;
	if (g_sp_diag_net_updates)
	{
		const u32 now = Device.dwTimeGlobal;
		const bool allow = (s_sp_dyn_obs_count < 256) || (now >= s_sp_dyn_obs_next);
		if (allow)
		{
			if (s_sp_dyn_obs_count >= 256)
			{
				s_sp_dyn_obs_next = now + 1000;
				s_sp_dyn_obs_count = 0;
			}

			++s_sp_dyn_obs_count;
			Msg("* [SP_DYN_OBS] id=%hu name=[%s] action=%s frame=%u query_objects=%u pos=(%.2f %.2f %.2f) predicted=(%.2f %.2f %.2f) target=(%.2f %.2f %.2f)",
				object().ID(), object().cName().c_str(),
				object_to_query->action() == moving_object::action_wait ? "wait" : "move",
				object_to_query->action_frame(),
				(u32)m_current_iteration.obstacles().size(),
				VPUSH(object().Position()),
				VPUSH(object_to_query->predict_position(1.f)),
				VPUSH(object_to_query->target_position()));
		}
	}
}

bool dynamic_obstacles_avoider::movement_enabled() const
{
	moving_object* object_to_query = object().get_moving_object();
	if (!object_to_query)
		return true;

	switch (object_to_query->action())
	{
	case moving_object::action_wait:
		{
			return (false);
		}
	case moving_object::action_move:
		{
			return (true);
		}
	default: NODEFAULT;
	}
#ifdef DEBUG
	return			(false);
#endif //DEBUG
}

bool dynamic_obstacles_avoider::process_query(const bool& change_path_state)
{
	if (!movement_enabled())
		return (true);

	m_inactive_query.set_intersection(m_current_iteration);
	m_active_query.set_intersection(m_current_iteration);

	return (inherited::process_query(change_path_state));
}
