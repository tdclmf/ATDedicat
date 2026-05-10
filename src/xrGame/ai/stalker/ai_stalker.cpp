////////////////////////////////////////////////////////////////////////////
//	Module 		: ai_stalker.cpp
//	Created 	: 25.02.2003
//  Modified 	: 25.02.2003
//	Author		: Dmitriy Iassenev
//	Description : AI Behaviour for monster "Stalker"
////////////////////////////////////////////////////////////////////////////

#include "pch_script.h"
#include "ai_stalker.h"
#include "../ai_monsters_misc.h"
#include "../../weapon.h"
#include "../../hit.h"
#include "../../phdestroyable.h"
#include "../../CharacterPhysicsSupport.h"
#include "../../script_entity_action.h"
#include "../../game_level_cross_table.h"
#include "../../game_graph.h"
#include "../../inventory.h"
#include "../../artefact.h"
#include "../../phmovementcontrol.h"
#include "../../../xrServerEntities/xrserver_objects_alife_monsters.h"
#include "../../../xrServerEntities/xrMessages.h"
#include "../../cover_evaluators.h"
#include "../../xrserver.h"
#include "../../xr_level_controller.h"
#include "../../level.h"
#include "../../HudItem.h"
#include "../../player_actor_context.h"
#include "../../../Include/xrRender/Kinematics.h"
#include "../../../Include/xrRender/animation_blend.h"
#include "../../../xrServerEntities/character_info.h"
#include "../../actor.h"
#include "../../relation_registry.h"
#include "../../stalker_animation_manager.h"
#include "../../stalker_planner.h"
#include "../../script_game_object.h"
#include "../../detail_path_manager.h"
#include "../../agent_manager.h"
#include "../../agent_corpse_manager.h"
#include "../../object_handler_planner.h"
#include "../../object_handler_space.h"
#include "../../memory_manager.h"
#include "../../sight_manager.h"
#include "../../ai_object_location.h"
#include "../../stalker_movement_manager_smart_cover.h"
#include "../../entitycondition.h"
#include "../../../xrServerEntities/script_engine.h"
#include "ai_stalker_impl.h"
#include "../../sound_player.h"
#include "../../stalker_sound_data.h"
#include "../../stalker_sound_data_visitor.h"
#include "ai_stalker_space.h"
#include "../../mt_config.h"
#include "../../effectorshot.h"
#include "../../visual_memory_manager.h"
#include "../../enemy_manager.h"
#include "../../../xrServerEntities/alife_human_brain.h"
#include "../../profiler.h"
#include "../../BoneProtections.h"
#include "../../stalker_animation_names.h"
#include "../../stalker_decision_space.h"
#include "../../agent_member_manager.h"
#include "../../location_manager.h"
#include "smart_cover_animation_selector.h"
#include "smart_cover_animation_planner.h"
#include "smart_cover_planner_target_selector.h"
#include "../../../xrEngine/xr_object_list.h"
#include "../../../xrServerEntities/gametype_chooser.h"
#include "../../object_handler_planner_impl.h"

#ifdef DEBUG
#	include "../../alife_simulator.h"
#	include "../../alife_object_registry.h"
#	include "../../level.h"
#	include "../../map_location.h"
#	include "../../map_manager.h"
#endif // DEBUG

using namespace StalkerSpace;

extern int g_AI_inactive_time;
int g_sp_diag_net_updates = 0;
extern ENGINE_API bool g_dedicated_server;

namespace
{
static const u8 kStalkerWorldStateExtMarker = 0xA7;
static const u8 kStalkerWorldStateExtVersion = 5;
static const u32 kStalkerWorldStateExtBaseSize = 2 * sizeof(u8) + 3 * sizeof(u8) + 6 * sizeof(float);
static const u32 kStalkerWorldStateExtV3ExtraSize = 3 * sizeof(u16) + 7 * sizeof(u8);
static const u32 kStalkerWorldStateExtAnimationTrackCount = 5;
static const u32 kStalkerWorldStateExtV4AnimExtraSize = kStalkerWorldStateExtAnimationTrackCount * (sizeof(u32) + sizeof(u16) + sizeof(u8));
static const u32 kStalkerWorldStateExtV5ObjectHandlerExtraSize = 2 * sizeof(u16);
static const u32 kStalkerWorldStateExtSize = kStalkerWorldStateExtBaseSize + kStalkerWorldStateExtV3ExtraSize + kStalkerWorldStateExtV4AnimExtraSize + kStalkerWorldStateExtV5ObjectHandlerExtraSize;

bool sp_net_diag_allow(u32& next_time, u32& count, u32 interval_ms, u32 max_count)
{
	if (!g_sp_diag_net_updates)
		return false;

	const u32 now = Device.dwTimeGlobal;
	if (now >= next_time)
	{
		next_time = now + interval_ms;
		count = 0;
	}

	if (count >= max_count)
		return false;

	++count;
	return true;
}

bool stalker_rate_allow(u32& next_time, u32& count, u32 interval_ms, u32 max_count)
{
	const u32 now = Device.dwTimeGlobal;
	if (now >= next_time)
	{
		next_time = now + interval_ms;
		count = 0;
	}

	if (count >= max_count)
		return false;

	++count;
	return true;
}

bool stalker_net_has_param(LPCSTR param)
{
	if (!param || !param[0] || !Core.Params)
		return false;

	return strstr(Core.Params, param) != nullptr;
}

bool stalker_full_anim_sync_enabled()
{
	return (g_dedicated_server && IIsServer() && (GameID() == eGameIDSingle)) || stalker_net_has_param("-stalker_full_anim_sync");
}
bool stalker_remote_tpose_enabled()
{
	return IIsClient() && stalker_net_has_param("-sp_npc_tpose_remote");
}

bool stalker_has_remote_pose(CAI_Stalker& stalker)
{
	return stalker.animation().global().blend() ||
		stalker.animation().head().blend() ||
		stalker.animation().torso().blend() ||
		stalker.animation().legs().blend() ||
		stalker.animation().script().blend();
}

void stalker_freeze_remote_blend(CStalkerAnimationPair& pair)
{
	CBlend* blend = pair.blend();
	if (!blend)
		return;

	blend->speed = 0.f;
}

bool stalker_destroy_remote_anim_root(CAI_Stalker& stalker)
{
	if (!IIsClient() || !stalker.Remote() || !stalker.animation_movement())
		return false;

	stalker.destroy_anim_mov_ctrl();
	return true;
}

bool stalker_hold_remote_tpose(CAI_Stalker& stalker)
{
	// Closing every cycle leaves some stalker visuals without a valid pose, so keep one
	// local static pose and only disable further animation/time/root-motion changes.
	if (!stalker_has_remote_pose(stalker) && !Device.Paused())
		stalker.animation().update();

	const bool root_killed = stalker_destroy_remote_anim_root(stalker);

	stalker_freeze_remote_blend(stalker.animation().global());
	stalker_freeze_remote_blend(stalker.animation().head());
	stalker_freeze_remote_blend(stalker.animation().torso());
	stalker_freeze_remote_blend(stalker.animation().legs());
	stalker_freeze_remote_blend(stalker.animation().script());
	return root_killed;
}

bool find_nearest_runtime_actor(const CAI_Stalker& stalker, u16& actor_id, LPCSTR& actor_name, float& actor_dist, Fvector* actor_position);

bool stalker_net_param_value_matches(LPCSTR prefix, LPCSTR first, LPCSTR second)
{
	if (!prefix || !prefix[0] || !Core.Params)
		return false;

	LPCSTR param = strstr(Core.Params, prefix);
	if (!param)
		return false;

	param += strlen(prefix);
	if (!param[0])
		return false;

	string128 token;
	u32 i = 0;
	while (param[i] && (param[i] != ' ') && (param[i] != '\t') && (param[i] != '\r') && (param[i] != '\n') && (i + 1 < sizeof(token)))
	{
		token[i] = param[i];
		++i;
	}
	token[i] = 0;

	return (first && first[0] && strstr(first, token)) || (second && second[0] && strstr(second, token));
}

bool stalker_anim_diag_enabled()
{
	return g_sp_diag_net_updates || stalker_net_has_param("-stalker_anim_diag");
}

bool stalker_anim_diag_watch_object(const CAI_Stalker& stalker)
{
	if (stalker_net_has_param("-stalker_anim_diag_all"))
		return true;

	string32 id_text;
	xr_sprintf(id_text, "%hu", stalker.ID());
	if (stalker_net_param_value_matches("-stalker_anim_diag_watch=", stalker.cName().c_str(), stalker.cNameSect().c_str()))
		return true;

	return stalker_net_param_value_matches("-stalker_anim_diag_watch=", id_text, nullptr);
}

bool stalker_anim_diag_near_actor(const CAI_Stalker& stalker, float max_distance)
{
	u16 actor_id = u16(-1);
	LPCSTR actor_name = "";
	float actor_dist = -1.f;
	return find_nearest_runtime_actor(stalker, actor_id, actor_name, actor_dist, nullptr) && (actor_dist <= max_distance);
}

LPCSTR stalker_anim_diag_motion_name(CAI_Stalker& stalker, u32 motion_value, string128& buffer)
{
	buffer[0] = 0;
#ifdef DEBUG
	if (motion_value == u32(u16(-1)) || !stalker.Visual())
		return buffer;

	IKinematicsAnimated* skeleton = stalker.Visual()->dcast_PKinematicsAnimated();
	if (!skeleton)
		return buffer;

	MotionID motion;
	motion.val = motion_value;
	if (!motion.valid())
		return buffer;

	std::pair<LPCSTR, LPCSTR> name = skeleton->LL_MotionDefName_dbg(motion);
	xr_sprintf(buffer, "%s/%s", name.first ? name.first : "", name.second ? name.second : "");
#endif
	return buffer;
}

float stalker_anim_diag_yaw_from_delta(const Fvector& from, const Fvector& to, bool& valid)
{
	Fvector delta;
	delta.sub(to, from);
	delta.y = 0.f;
	const float magnitude = delta.magnitude();
	valid = magnitude > 0.025f;
	if (!valid)
		return 0.f;

	return atan2f(delta.x, delta.z);
}

float stalker_anim_diag_abs_yaw_delta(float a, float b)
{
	return _abs(angle_normalize_signed(a - b));
}

bool actor_scan_name_interesting(CObject* object)
{
	if (!object)
		return false;

	LPCSTR name = object->cName().c_str();
	LPCSTR section = object->cNameSect().c_str();
	return (name && strstr(name, "actor")) ||
		(section && strstr(section, "actor")) ||
		(name && strstr(name, "mp_")) ||
		(section && strstr(section, "mp_"));
}

void log_runtime_actor_scan(const CAI_Stalker& stalker, LPCSTR reason)
{
	if (!IIsServer() || !g_sp_diag_net_updates)
		return;

	static u32 s_actor_scan_next = 0;
	static u32 s_actor_scan_count = 0;
	if (!sp_net_diag_allow(s_actor_scan_next, s_actor_scan_count, 3000, 1))
		return;

	const u32 object_count = Level().Objects.o_count();
	u32 actor_cast_count = 0;
	u32 valid_actor_count = 0;
	u32 interesting_count = 0;

	for (u32 i = 0; i < object_count; ++i)
	{
		CObject* object = Level().Objects.o_get_by_iterator(i);
		if (!object)
			continue;

		CActor* actor = smart_cast<CActor*>(object);
		const bool interesting = actor || actor_scan_name_interesting(object);
		if (!interesting)
			continue;

		++interesting_count;
		if (actor)
		{
			++actor_cast_count;
			if ((actor->ID() != 0) && actor->g_Alive())
				++valid_actor_count;
		}
	}

	CActor* global_actor = Actor();
	Msg("* [SP_ACTOR_SCAN] reason=%s stalker=%hu/%s objects=%u interesting=%u actor_cast=%u valid_alive_nonzero=%u global_actor=%hu/%s global_alive=%d actorproxy=%d withactor=%d",
		reason ? reason : "unknown",
		stalker.ID(), stalker.cName().c_str(),
		object_count, interesting_count, actor_cast_count, valid_actor_count,
		global_actor ? global_actor->ID() : u16(-1),
		global_actor ? global_actor->cName().c_str() : "",
		global_actor ? (global_actor->g_Alive() ? 1 : 0) : 0,
		0,
		stalker_net_has_param("-withactor") ? 1 : 0);

	u32 printed = 0;
	for (u32 i = 0; i < object_count; ++i)
	{
		CObject* object = Level().Objects.o_get_by_iterator(i);
		if (!object)
			continue;

		CActor* actor = smart_cast<CActor*>(object);
		const bool interesting = actor || actor_scan_name_interesting(object);
		if (!interesting)
			continue;

		if (printed >= 24)
			break;

		const CObject* parent = object->H_Parent();
		const float distance = stalker.Position().distance_to(object->Position());
		Msg("* [SP_ACTOR_SCAN][OBJ] idx=%u id=%hu name=[%s] sect=[%s] cast_actor=%d alive=%d hp=%.3f id0=%d enabled=%d destroy=%d ready=%d local=%d svu=%d parent=%hu dist=%.3f pos=(%.2f %.2f %.2f)",
			i,
			object->ID(),
			object->cName().c_str(),
			object->cNameSect().c_str(),
			actor ? 1 : 0,
			actor && actor->g_Alive() ? 1 : 0,
			actor ? actor->GetfHealth() : -1.f,
			object->ID() == 0 ? 1 : 0,
			object->getEnabled() ? 1 : 0,
			object->getDestroy() ? 1 : 0,
			object->getReady() ? 1 : 0,
			object->Local() ? 1 : 0,
			object->getSVU() ? 1 : 0,
			parent ? parent->ID() : u16(-1),
			distance,
			VPUSH(object->Position()));
		++printed;
	}
}
enum EStalkerNetworkAnimationTrack
{
	eStalkerNetAnimGlobal = 0,
	eStalkerNetAnimHead,
	eStalkerNetAnimTorso,
	eStalkerNetAnimLegs,
	eStalkerNetAnimScript,
	eStalkerNetAnimCount
};

void reset_stalker_animation_state(u32* motion, u16* time, u8* flags)
{
	for (u32 i = 0; i < kStalkerWorldStateExtAnimationTrackCount; ++i)
	{
		motion[i] = u32(u16(-1));
		time[i] = 0;
		flags[i] = 0;
	}
}

bool packet_begins_with_message(const NET_Packet& P, u16 message_type)
{
	if (P.B.count < sizeof(u16))
		return false;

	u16 packet_type = u16(-1);
	CopyMemory(&packet_type, P.B.data, sizeof(packet_type));
	return packet_type == message_type;
}

void write_stalker_animation_pair_state(NET_Packet& P, CStalkerAnimationPair& pair, bool write_time = true)
{
	const MotionID& motion = pair.animation();
	CBlend* blend = pair.blend();
	u8 flags = 0;
	u16 packed_time = 0;

	if (motion.valid())
		flags |= 1;

	if (write_time && blend && (blend->timeTotal > EPS_L))
	{
		flags |= 2;
		packed_time = (u16)clampr(iFloor((blend->timeCurrent / blend->timeTotal) * 65535.f + 0.5f), 0, 65535);
	}

	P.w_u32(motion.valid() ? motion.val : u32(u16(-1)));
	P.w_u16(packed_time);
	P.w_u8(flags);
}

void write_stalker_empty_animation_pair_state(NET_Packet& P)
{
	P.w_u32(u32(u16(-1)));
	P.w_u16(0);
	P.w_u8(0);
}

void write_stalker_animation_state(CAI_Stalker& stalker, NET_Packet& P)
{
	const bool full_anim_sync = stalker_full_anim_sync_enabled();
	write_stalker_animation_pair_state(P, stalker.animation().global(), full_anim_sync);
	if (full_anim_sync)
	{
		write_stalker_animation_pair_state(P, stalker.animation().head());
		write_stalker_animation_pair_state(P, stalker.animation().torso());
		write_stalker_animation_pair_state(P, stalker.animation().legs());
		write_stalker_animation_pair_state(P, stalker.animation().script());
		return;
	}

	// Keep locomotion tracks client-side by default, but still replicate script poses
	// such as sitting/lying. Do not sync script time here: packet cadence is lower
	// than animation ticks, and forcing time every update makes remote idles jitter.
	write_stalker_empty_animation_pair_state(P);
	write_stalker_empty_animation_pair_state(P);
	write_stalker_empty_animation_pair_state(P);
	write_stalker_animation_pair_state(P, stalker.animation().script(), false);
}

struct SStalkerMoveDiag
{
	Fvector path_target;
	float path_target_dist;
	u32 path_target_index;
	u16 nearest_stalker_id;
	float nearest_stalker_dist;
	LPCSTR nearest_stalker_name;
	u16 global_actor_id;
	float global_actor_dist;
	LPCSTR global_actor_name;
	u16 nearest_actor_id;
	float nearest_actor_dist;
	LPCSTR nearest_actor_name;
};

bool find_nearest_runtime_actor(const CAI_Stalker& stalker, u16& actor_id, LPCSTR& actor_name, float& actor_dist, Fvector* actor_position = nullptr)
{
	player_actor_context::SActorAnchor anchor;
	if (!player_actor_context::FindNearestRuntimePlayerAnchor(stalker.Position(), anchor))
	{
		actor_id = u16(-1);
		actor_name = "";
		actor_dist = -1.f;
		if (actor_position)
			actor_position->set(0.f, 0.f, 0.f);
		log_runtime_actor_scan(stalker, "find_nearest_runtime_actor_missing");
		return false;
	}

	actor_id = anchor.id;
	actor_name = anchor.name;
	actor_dist = anchor.distance;
	if (actor_position)
		actor_position->set(anchor.position);
	return true;
}
void fill_stalker_move_diag(CAI_Stalker& stalker, SStalkerMoveDiag& diag)
{
	diag.path_target.set(0.f, 0.f, 0.f);
	diag.path_target_dist = -1.f;
	diag.path_target_index = u32(-1);
	diag.nearest_stalker_id = u16(-1);
	diag.nearest_stalker_dist = flt_max;
	diag.nearest_stalker_name = "";
	diag.global_actor_id = u16(-1);
	diag.global_actor_dist = -1.f;
	diag.global_actor_name = "";
	diag.nearest_actor_id = u16(-1);
	diag.nearest_actor_dist = flt_max;
	diag.nearest_actor_name = "";

	CActor* global_actor = Actor();
	if (global_actor)
	{
		diag.global_actor_id = global_actor->ID();
		diag.global_actor_dist = stalker.Position().distance_to(global_actor->Position());
		diag.global_actor_name = global_actor->cName().c_str();
	}

	const auto& path = stalker.movement().detail().path();
	const u32 detail_size = (u32)path.size();
	if (detail_size)
	{
		const u32 detail_index = stalker.movement().detail().curr_travel_point_index();
		const u32 target_index = (detail_index + 1 < detail_size) ? (detail_index + 1) : (detail_size - 1);
		diag.path_target_index = target_index;
		diag.path_target = path[target_index].position;
		diag.path_target_dist = stalker.Position().distance_to(diag.path_target);
	}

	const u32 object_count = Level().Objects.o_count();
	for (u32 i = 0; i < object_count; ++i)
	{
		CObject* object = Level().Objects.o_get_by_iterator(i);
		if (!object || (object == &stalker))
			continue;

		CAI_Stalker* other_stalker = smart_cast<CAI_Stalker*>(object);
		if (!other_stalker || !other_stalker->g_Alive())
			continue;

		const float distance = stalker.Position().distance_to(other_stalker->Position());
		if (distance >= diag.nearest_stalker_dist)
			continue;

		diag.nearest_stalker_dist = distance;
		diag.nearest_stalker_id = other_stalker->ID();
		diag.nearest_stalker_name = other_stalker->cName().c_str();
	}

	if (diag.nearest_stalker_dist == flt_max)
		diag.nearest_stalker_dist = -1.f;

	for (u32 i = 0; i < object_count; ++i)
	{
		CObject* object = Level().Objects.o_get_by_iterator(i);
		if (!object)
			continue;

		CActor* actor = smart_cast<CActor*>(object);
		if (!actor || (actor->ID() == 0) || !actor->g_Alive())
			continue;

		const float distance = stalker.Position().distance_to(actor->Position());
		if (distance >= diag.nearest_actor_dist)
			continue;

		diag.nearest_actor_dist = distance;
		diag.nearest_actor_id = actor->ID();
		diag.nearest_actor_name = actor->cName().c_str();
	}

	if (diag.nearest_actor_dist == flt_max)
	{
		diag.nearest_actor_dist = -1.f;
		log_runtime_actor_scan(stalker, "fill_move_diag_missing");
	}
}

MonsterSpace::EObjectAction stalker_network_object_action(u16 object_handler_state_id)
{
	switch (object_handler_state_id)
	{
	case ObjectHandlerSpace::eWorldOperatorUse:
	case ObjectHandlerSpace::eWorldOperatorPrepare:
		return MonsterSpace::eObjectActionUse;
	case ObjectHandlerSpace::eWorldOperatorStrapped:
	case ObjectHandlerSpace::eWorldOperatorStrapping:
	case ObjectHandlerSpace::eWorldOperatorStrapping2Idle:
		return MonsterSpace::eObjectActionStrapped;
	case ObjectHandlerSpace::eWorldOperatorDrop:
		return MonsterSpace::eObjectActionDrop;
	default:
		return MonsterSpace::eObjectActionIdle;
	}
}

CInventoryItem* find_stalker_inventory_item(CAI_Stalker& stalker, u16 item_id)
{
	if (item_id == u16(-1))
		return nullptr;

	TIItemContainer::iterator I = stalker.inventory().m_all.begin();
	TIItemContainer::iterator E = stalker.inventory().m_all.end();
	for (; I != E; ++I)
	{
		if ((*I) && ((*I)->object().ID() == item_id))
			return *I;
	}

	return nullptr;
}

void apply_stalker_server_inventory_state(
    CAI_Stalker& stalker,
    u16 active_slot,
    u16 active_item_id,
    u8 hud_state,
    u8 hud_next_state,
    u8 weapon_state,
    u8 weapon_next_state,
    u16 weapon_ammo_elapsed,
    u8 weapon_ammo_type,
    u16 object_handler_item_id,
    u16 object_handler_state_id)
{
    if (active_slot != NO_ACTIVE_SLOT)
    {
        if ((active_slot <= stalker.inventory().LastSlot()) && stalker.inventory().ItemFromSlot(active_slot))
        {
            if (stalker.inventory().GetActiveSlot() != active_slot)
            {
                stalker.inventory().SetActiveSlot(active_slot);
                CInventoryItem* selected_item = stalker.inventory().ActiveItem();
                if (selected_item)
                    selected_item->ActivateItem();
            }
        }
    }
    else if (stalker.inventory().GetActiveSlot() != NO_ACTIVE_SLOT)
    {
        CInventoryItem* old_item = stalker.inventory().ActiveItem();
        CHudItem* old_hud_item = old_item ? old_item->cast_hud_item() : nullptr;
        if (old_hud_item)
        {
            old_hud_item->SetState(CHUDState::eHidden);
            old_hud_item->SetNextState(CHUDState::eHidden);
        }
        stalker.inventory().SetActiveSlot(NO_ACTIVE_SLOT);
    }

    CInventoryItem* active_item = stalker.inventory().ActiveItem();
    if (!active_item)
    {
        CInventoryItem* object_handler_item = find_stalker_inventory_item(stalker, object_handler_item_id);
        if (object_handler_item)
            stalker.CObjectHandler::set_goal(stalker_network_object_action(object_handler_state_id), object_handler_item);
        else if (object_handler_item_id == u16(-1))
            stalker.CObjectHandler::set_goal(MonsterSpace::eObjectActionIdle);
        return;
    }

    if ((active_item_id != u16(-1)) && (active_item->object().ID() != active_item_id))
        return;

    CHudItem* hud_item = active_item->cast_hud_item();
    if (hud_item)
    {
        if ((hud_state != u8(-1)) && (hud_item->GetState() != hud_state))
            hud_item->SetState(hud_state);
        if ((hud_next_state != u8(-1)) && (hud_item->GetNextState() != hud_next_state))
            hud_item->SetNextState(hud_next_state);
    }

    CWeapon* weapon = active_item->cast_weapon();
    if (weapon)
    {
        if ((weapon_state != u8(-1)) && (weapon->GetState() != weapon_state))
            weapon->SetState(weapon_state);
        if ((weapon_next_state != u8(-1)) && (weapon->GetNextState() != weapon_next_state))
            weapon->SetNextState(weapon_next_state);
        if (weapon->GetAmmoElapsed() != weapon_ammo_elapsed)
            weapon->SetAmmoElapsed(weapon_ammo_elapsed);
        if ((weapon_ammo_type != u8(-1)) && (weapon->GetAmmoType() != weapon_ammo_type))
            weapon->SetAmmoType(weapon_ammo_type);
    }
}

float stalker_network_actual_speed(CAI_Stalker& stalker)
{
	CPHMovementControl* movement_control = stalker.character_physics_support() ? stalker.character_physics_support()->movement() : nullptr;
	if (movement_control && movement_control->CharacterExist())
		return stalker.movement().speed(movement_control);

	return stalker.movement().speed();
}

void write_stalker_server_state(CAI_Stalker& stalker, NET_Packet& P)
{
	P.w_u8(kStalkerWorldStateExtMarker);
	P.w_u8(kStalkerWorldStateExtVersion);
	P.w_u8((u8)stalker.movement().movement_type());
	P.w_u8((u8)stalker.movement().body_state());
	P.w_u8((u8)stalker.movement().mental_state());
	P.w_float(stalker.movement().m_body.current.yaw);
	P.w_float(stalker.movement().m_body.target.yaw);
	P.w_float(stalker.movement().m_head.current.yaw);
	P.w_float(stalker.movement().m_head.current.pitch);
	P.w_float(stalker.movement().speed());
	P.w_float(stalker_network_actual_speed(stalker));

	u16 active_slot = stalker.inventory().GetActiveSlot();
	u16 active_item_id = u16(-1);
	u8 hud_state = u8(-1);
	u8 hud_next_state = u8(-1);
	u8 weapon_state = u8(-1);
	u8 weapon_next_state = u8(-1);
	u16 weapon_ammo_elapsed = 0;
	u8 weapon_ammo_type = u8(-1);
	u8 weapon_zoomed = 0;
	u8 weapon_working = 0;
	u16 object_handler_item_id = u16(-1);
	u16 object_handler_state_id = u16(-1);
	if (stalker_full_anim_sync_enabled())
	{
		object_handler_item_id = stalker.CObjectHandler::planner().current_action_object_id();
		object_handler_state_id = (u16)clampr(stalker.CObjectHandler::planner().current_action_state_id(), u32(0), u32(65535));
		if (!find_stalker_inventory_item(stalker, object_handler_item_id))
		{
			object_handler_item_id = u16(-1);
			object_handler_state_id = u16(-1);
		}
	}

	CInventoryItem* active_item = stalker.inventory().ActiveItem();
	if (active_item)
	{
		active_item_id = active_item->object().ID();

		CHudItem* hud_item = active_item->cast_hud_item();
		if (hud_item)
		{
			hud_state = (u8)hud_item->GetState();
			hud_next_state = (u8)hud_item->GetNextState();
		}

		CWeapon* weapon = active_item->cast_weapon();
		if (weapon)
		{
			weapon_state = (u8)weapon->GetState();
			weapon_next_state = (u8)weapon->GetNextState();
			weapon_ammo_elapsed = (u16)clampr(weapon->GetAmmoElapsed(), 0, 65534);
			weapon_ammo_type = weapon->GetAmmoType();
			weapon_zoomed = weapon->IsZoomed() ? 1 : 0;
			weapon_working = weapon->IsWorking() ? 1 : 0;
		}
	}

	P.w_u16(active_slot);
	P.w_u16(active_item_id);
	P.w_u8(hud_state);
	P.w_u8(hud_next_state);
	P.w_u8(weapon_state);
	P.w_u8(weapon_next_state);
	P.w_u16(weapon_ammo_elapsed);
	P.w_u8(weapon_ammo_type);
	P.w_u8(weapon_zoomed);
	P.w_u8(weapon_working);
	write_stalker_animation_state(stalker, P);
	P.w_u16(object_handler_item_id);
	P.w_u16(object_handler_state_id);
}
}

CAI_Stalker::CAI_Stalker() :
	m_sniper_update_rate(false),
	m_sniper_fire_mode(false),
	m_net_server_state_valid(false),
	m_net_server_state_time(0),
	m_net_server_movement_type(2),
	m_net_server_body_state(1),
	m_net_server_mental_state(0),
	m_net_server_body_current_yaw(0.f),
	m_net_server_body_target_yaw(0.f),
	m_net_server_head_yaw(0.f),
	m_net_server_head_pitch(0.f),
	m_net_server_speed(0.f),
	m_net_server_actual_speed(0.f),
	m_net_server_position(),
	m_net_server_update_timestamp(0),
	m_net_server_active_slot(u16(-1)),
	m_net_server_active_item_id(u16(-1)),
	m_net_server_hud_state(u8(-1)),
	m_net_server_hud_next_state(u8(-1)),
	m_net_server_weapon_state(u8(-1)),
	m_net_server_weapon_next_state(u8(-1)),
	m_net_server_weapon_ammo_elapsed(0),
	m_net_server_weapon_ammo_type(u8(-1)),
	m_net_server_weapon_zoomed(0),
	m_net_server_weapon_working(0),
	m_net_server_object_handler_item_id(u16(-1)),
	m_net_server_object_handler_state_id(u16(-1)),
	m_net_server_animation_state_valid(false),
	m_net_server_animation_state_applied_time(0),
	m_net_server_anim_time_synced_motion{ u32(-1), u32(-1), u32(-1), u32(-1), u32(-1) },
	m_net_remote_smooth_valid(false),
	m_net_remote_smooth_time(0),
	m_net_anim_diag_valid(false),
	m_net_anim_diag_last_log_time(0),
	m_net_anim_diag_last_server_timestamp(0),
	m_net_anim_diag_last_server_yaw(0.f),
	m_net_anim_diag_last_server_target_yaw(0.f),
	m_net_anim_diag_last_server_speed(0.f),
	m_net_anim_diag_last_server_actual_speed(0.f),
	m_take_items_enabled(true),
	m_death_sound_enabled(true)
{
	m_net_server_position.set(0.f, 0.f, 0.f);
	m_net_remote_smooth_position.set(0.f, 0.f, 0.f);
	m_net_anim_diag_last_server_pos.set(0.f, 0.f, 0.f);
	for (u32 i = 0; i < kStalkerWorldStateExtAnimationTrackCount; ++i)
		m_net_anim_diag_last_motion[i] = u32(-1);
	reset_stalker_animation_state(m_net_server_anim_motion, m_net_server_anim_time, m_net_server_anim_flags);
	m_pPhysics_support = NULL;
	m_animation_manager = NULL;
	m_brain = NULL;
	m_sight_manager = NULL;
	m_weapon_shot_effector = NULL;
	m_sound_user_data_visitor = 0;
	m_movement_manager = 0;
	m_group_behaviour = true;
	m_boneHitProtection = NULL;
	m_power_fx_factor = flt_max;
	m_wounded = false;
#ifdef DEBUG
	m_debug_planner					= 0;
	m_dbg_hud_draw					= false;
#endif // DEBUG
	m_registered_in_combat_on_migration = false;
}

CAI_Stalker::~CAI_Stalker()
{
	xr_delete(m_pPhysics_support);
	xr_delete(m_animation_manager);
	xr_delete(m_brain);
	xr_delete(m_sight_manager);
	xr_delete(m_weapon_shot_effector);
	xr_delete(m_sound_user_data_visitor);
}

void CAI_Stalker::reinit()
{
	CObjectHandler::reinit(this);
	sight().reinit();
	CCustomMonster::reinit();
	animation().reinit();
	//	movement().reinit				();

	//загрузка спецевической звуковой схемы для сталкера согласно m_SpecificCharacter
	sound().sound_prefix(SpecificCharacter().sound_voice_prefix());

#ifdef DEBUG_MEMORY_MANAGER
	size_t				start = 0;
	if (g_bMEMO)
		start						= Memory.mem_usage();
#endif // DEBUG_MEMORY_MANAGER

	LoadSounds(*cNameSect());

#ifdef DEBUG_MEMORY_MANAGER
	if (g_bMEMO)
		Msg					("CAI_Stalker::LoadSounds() : %lld",Memory.mem_usage() - start);
#endif // DEBUG_MEMORY_MANAGER

	m_pPhysics_support->in_Init();

	m_best_item_to_kill = 0;
	m_best_item_value = 0.f;
	m_best_ammo = 0;
	m_best_found_item_to_kill = 0;
	m_best_found_ammo = 0;
	m_item_actuality = false;
	m_sell_info_actuality = false;
	m_net_server_state_valid = false;
	m_net_server_state_time = 0;
	m_net_server_movement_type = (u8)eMovementTypeStand;
	m_net_server_body_state = (u8)eBodyStateStand;
	m_net_server_mental_state = (u8)eMentalStateDanger;
	m_net_server_body_current_yaw = 0.f;
	m_net_server_body_target_yaw = 0.f;
	m_net_server_head_yaw = 0.f;
	m_net_server_head_pitch = 0.f;
	m_net_server_speed = 0.f;
	m_net_server_actual_speed = 0.f;
	m_net_server_position.set(0.f, 0.f, 0.f);
	m_net_server_update_timestamp = 0;
	m_net_server_active_slot = u16(-1);
	m_net_server_active_item_id = u16(-1);
	m_net_server_hud_state = u8(-1);
	m_net_server_hud_next_state = u8(-1);
	m_net_server_weapon_state = u8(-1);
	m_net_server_weapon_next_state = u8(-1);
	m_net_server_weapon_ammo_elapsed = 0;
	m_net_server_weapon_ammo_type = u8(-1);
	m_net_server_weapon_zoomed = 0;
	m_net_server_weapon_working = 0;
	m_net_server_object_handler_item_id = u16(-1);
	m_net_server_object_handler_state_id = u16(-1);
	m_net_server_animation_state_valid = false;
	m_net_server_animation_state_applied_time = 0;
	for (u32 i = 0; i < kStalkerWorldStateExtAnimationTrackCount; ++i)
		m_net_server_anim_time_synced_motion[i] = u32(-1);
	reset_stalker_animation_state(m_net_server_anim_motion, m_net_server_anim_time, m_net_server_anim_flags);
	m_net_remote_smooth_valid = false;
	m_net_remote_smooth_time = 0;
	m_net_server_position.set(0.f, 0.f, 0.f);
	m_net_remote_smooth_position.set(0.f, 0.f, 0.f);
	m_net_anim_diag_valid = false;
	m_net_anim_diag_last_log_time = 0;
	m_net_anim_diag_last_server_timestamp = 0;
	m_net_anim_diag_last_server_pos.set(0.f, 0.f, 0.f);
	m_net_anim_diag_last_server_yaw = 0.f;
	m_net_anim_diag_last_server_target_yaw = 0.f;
	m_net_anim_diag_last_server_speed = 0.f;
	m_net_anim_diag_last_server_actual_speed = 0.f;
	for (u32 i = 0; i < kStalkerWorldStateExtAnimationTrackCount; ++i)
		m_net_anim_diag_last_motion[i] = u32(-1);

	m_ce_close = xr_new<CCoverEvaluatorCloseToEnemy>(&movement().restrictions());
	m_ce_far = xr_new<CCoverEvaluatorFarFromEnemy>(&movement().restrictions());
	m_ce_best = xr_new<CCoverEvaluatorBest>(&movement().restrictions());
	m_ce_angle = xr_new<CCoverEvaluatorAngle>(&movement().restrictions());
	m_ce_safe = xr_new<CCoverEvaluatorSafe>(&movement().restrictions());
	m_ce_ambush = xr_new<CCoverEvaluatorAmbush>(&movement().restrictions());

	m_ce_close->set_inertia(3000);
	m_ce_far->set_inertia(3000);
	m_ce_best->set_inertia(1000);
	m_ce_angle->set_inertia(5000);
	m_ce_safe->set_inertia(1000);
	m_ce_ambush->set_inertia(3000);

	m_can_kill_enemy = false;
	m_can_kill_member = false;
	m_pick_distance = 0.f;
	m_pick_frame_id = 0;

	m_weapon_shot_random_seed = s32(Level().timeServer_Async());

	m_best_cover = 0;
	m_best_cover_actual = false;
	m_best_cover_value = flt_max;

	m_throw_actual = false;
	m_computed_object_position = Fvector().set(flt_max,flt_max,flt_max);
	m_computed_object_direction = Fvector().set(flt_max,flt_max,flt_max);

	m_throw_target_position = Fvector().set(flt_max,flt_max,flt_max);
	m_throw_ignore_object = 0;

	m_throw_position = Fvector().set(flt_max,flt_max,flt_max);
	m_throw_velocity = Fvector().set(flt_max,flt_max,flt_max);

	m_throw_collide_position = Fvector().set(flt_max,flt_max,flt_max);
	m_throw_enabled = false;

	m_last_throw_time = 0;

	m_can_throw_grenades = true;
	m_throw_time_interval = 20000;

	brain().CStalkerPlanner::m_storage.set_property(StalkerDecisionSpace::eWorldPropertyCriticallyWounded, false);

	{
		m_critical_wound_weights.clear();
		//		LPCSTR							weights = pSettings->r_string(cNameSect(),"critical_wound_weights");
		LPCSTR weights = SpecificCharacter().critical_wound_weights();
		string16 temp;
		for (int i = 0, n = _GetItemCount(weights); i < n; ++i)
			m_critical_wound_weights.push_back((float)atof(_GetItem(weights, i, temp)));
	}

	m_update_rotation_on_frame = false;
}

void CAI_Stalker::LoadSounds(LPCSTR section)
{
	LPCSTR head_bone_name = pSettings->r_string(section, "bone_head");
	sound().add(pSettings->r_string(section, "sound_death"), 100, SOUND_TYPE_MONSTER_DYING, 0,
	            u32(eStalkerSoundMaskDie), eStalkerSoundDie, head_bone_name, xr_new<CStalkerSoundData>(this));
	sound().add(pSettings->r_string(section, "sound_anomaly_death"), 100, SOUND_TYPE_MONSTER_DYING, 0,
	            u32(eStalkerSoundMaskDieInAnomaly), eStalkerSoundDieInAnomaly, head_bone_name, 0);
	sound().add(pSettings->r_string(section, "sound_hit"), 100, SOUND_TYPE_MONSTER_INJURING, 1,
	            u32(eStalkerSoundMaskInjuring), eStalkerSoundInjuring, head_bone_name, xr_new<CStalkerSoundData>(this));
	sound().add(pSettings->r_string(section, "sound_friendly_fire"), 100, SOUND_TYPE_MONSTER_INJURING, 1,
	            u32(eStalkerSoundMaskInjuringByFriend), eStalkerSoundInjuringByFriend, head_bone_name,
	            xr_new<CStalkerSoundData>(this));
	sound().add(pSettings->r_string(section, "sound_panic_human"), 100, SOUND_TYPE_MONSTER_TALKING, 2,
	            u32(eStalkerSoundMaskPanicHuman), eStalkerSoundPanicHuman, head_bone_name,
	            xr_new<CStalkerSoundData>(this));
	sound().add(pSettings->r_string(section, "sound_panic_monster"), 100, SOUND_TYPE_MONSTER_TALKING, 2,
	            u32(eStalkerSoundMaskPanicMonster), eStalkerSoundPanicMonster, head_bone_name,
	            xr_new<CStalkerSoundData>(this));
	sound().add(pSettings->r_string(section, "sound_grenade_alarm"), 100, SOUND_TYPE_MONSTER_TALKING, 3,
	            u32(eStalkerSoundMaskGrenadeAlarm), eStalkerSoundGrenadeAlarm, head_bone_name,
	            xr_new<CStalkerSoundData>(this));
	sound().add(pSettings->r_string(section, "sound_friendly_grenade_alarm"), 100, SOUND_TYPE_MONSTER_TALKING, 3,
	            u32(eStalkerSoundMaskFriendlyGrenadeAlarm), eStalkerSoundFriendlyGrenadeAlarm, head_bone_name,
	            xr_new<CStalkerSoundData>(this));
	sound().add(pSettings->r_string(section, "sound_tolls"), 100, SOUND_TYPE_MONSTER_TALKING, 4,
	            u32(eStalkerSoundMaskTolls), eStalkerSoundTolls, head_bone_name, xr_new<CStalkerSoundData>(this));
	sound().add(pSettings->r_string(section, "sound_wounded"), 100, SOUND_TYPE_MONSTER_TALKING, 4,
	            u32(eStalkerSoundMaskWounded), eStalkerSoundWounded, head_bone_name, xr_new<CStalkerSoundData>(this));
	sound().add(pSettings->r_string(section, "sound_alarm"), 100, SOUND_TYPE_MONSTER_TALKING, 5,
	            u32(eStalkerSoundMaskAlarm), eStalkerSoundAlarm, head_bone_name, xr_new<CStalkerSoundData>(this));
	sound().add(pSettings->r_string(section, "sound_attack_no_allies"), 100, SOUND_TYPE_MONSTER_TALKING, 5,
	            u32(eStalkerSoundMaskAttackNoAllies), eStalkerSoundAttackNoAllies, head_bone_name,
	            xr_new<CStalkerSoundData>(this));
	sound().add(pSettings->r_string(section, "sound_attack_allies_single_enemy"), 100, SOUND_TYPE_MONSTER_TALKING, 5,
	            u32(eStalkerSoundMaskAttackAlliesSingleEnemy), eStalkerSoundAttackAlliesSingleEnemy, head_bone_name,
	            xr_new<CStalkerSoundData>(this));
	sound().add(pSettings->r_string(section, "sound_attack_allies_several_enemies"), 100, SOUND_TYPE_MONSTER_TALKING, 5,
	            u32(eStalkerSoundMaskAttackAlliesSeveralEnemies), eStalkerSoundAttackAlliesSeveralEnemies,
	            head_bone_name, xr_new<CStalkerSoundData>(this));
	sound().add(pSettings->r_string(section, "sound_backup"), 100, SOUND_TYPE_MONSTER_TALKING, 5,
	            u32(eStalkerSoundMaskBackup), eStalkerSoundBackup, head_bone_name, xr_new<CStalkerSoundData>(this));
	sound().add(pSettings->r_string(section, "sound_detour"), 100, SOUND_TYPE_MONSTER_TALKING, 5,
	            u32(eStalkerSoundMaskDetour), eStalkerSoundDetour, head_bone_name, xr_new<CStalkerSoundData>(this));
	sound().add(pSettings->r_string(section, "sound_search1_no_allies"), 100, SOUND_TYPE_MONSTER_TALKING, 5,
	            u32(eStalkerSoundMaskSearch1NoAllies), eStalkerSoundSearch1NoAllies, head_bone_name,
	            xr_new<CStalkerSoundData>(this));
	sound().add(pSettings->r_string(section, "sound_search1_with_allies"), 100, SOUND_TYPE_MONSTER_TALKING, 5,
	            u32(eStalkerSoundMaskSearch1WithAllies), eStalkerSoundSearch1WithAllies, head_bone_name,
	            xr_new<CStalkerSoundData>(this));
	sound().add(pSettings->r_string(section, "sound_enemy_lost_no_allies"), 100, SOUND_TYPE_MONSTER_TALKING, 5,
	            u32(eStalkerSoundMaskEnemyLostNoAllies), eStalkerSoundEnemyLostNoAllies, head_bone_name,
	            xr_new<CStalkerSoundData>(this));
	sound().add(pSettings->r_string(section, "sound_enemy_lost_with_allies"), 100, SOUND_TYPE_MONSTER_TALKING, 5,
	            u32(eStalkerSoundMaskEnemyLostWithAllies), eStalkerSoundEnemyLostWithAllies, head_bone_name,
	            xr_new<CStalkerSoundData>(this));
	sound().add(pSettings->r_string(section, "sound_humming"), 100, SOUND_TYPE_MONSTER_TALKING, 6,
	            u32(eStalkerSoundMaskHumming), eStalkerSoundHumming, head_bone_name, 0);
	sound().add(pSettings->r_string(section, "sound_need_backup"), 100, SOUND_TYPE_MONSTER_TALKING, 4,
	            u32(eStalkerSoundMaskNeedBackup), eStalkerSoundNeedBackup, head_bone_name,
	            xr_new<CStalkerSoundData>(this));
	sound().add(pSettings->r_string(section, "sound_running_in_danger"), 100, SOUND_TYPE_MONSTER_TALKING, 6,
	            u32(eStalkerSoundMaskMovingInDanger), eStalkerSoundRunningInDanger, head_bone_name,
	            xr_new<CStalkerSoundData>(this));
	//	sound().add						(pSettings->r_string(section,"sound_walking_in_danger"),			100, SOUND_TYPE_MONSTER_TALKING,	6, u32(eStalkerSoundMaskMovingInDanger),			eStalkerSoundWalkingInDanger,			head_bone_name, xr_new<CStalkerSoundData>(this));
	sound().add(pSettings->r_string(section, "sound_kill_wounded"), 100, SOUND_TYPE_MONSTER_TALKING, 5,
	            u32(eStalkerSoundMaskKillWounded), eStalkerSoundKillWounded, head_bone_name,
	            xr_new<CStalkerSoundData>(this));
	sound().add(pSettings->r_string(section, "sound_enemy_critically_wounded"), 100, SOUND_TYPE_MONSTER_TALKING, 4,
	            u32(eStalkerSoundMaskEnemyCriticallyWounded), eStalkerSoundEnemyCriticallyWounded, head_bone_name,
	            xr_new<CStalkerSoundData>(this));
	sound().add(pSettings->r_string(section, "sound_enemy_killed_or_wounded"), 100, SOUND_TYPE_MONSTER_TALKING, 4,
	            u32(eStalkerSoundMaskEnemyKilledOrWounded), eStalkerSoundEnemyKilledOrWounded, head_bone_name,
	            xr_new<CStalkerSoundData>(this));
	sound().add(pSettings->r_string(section, "sound_throw_grenade"), 100, SOUND_TYPE_MONSTER_TALKING, 5,
	            u32(eStalkerSoundMaskKillWounded), eStalkerSoundThrowGrenade, head_bone_name,
	            xr_new<CStalkerSoundData>(this));
}

void CAI_Stalker::reload(LPCSTR section)
{
#ifdef DEBUG_MEMORY_MANAGER
	size_t					start = 0;
	if (g_bMEMO)
		start							= Memory.mem_usage();
#endif // DEBUG_MEMORY_MANAGER

	brain().setup(this);

#ifdef DEBUG_MEMORY_MANAGER
	if (g_bMEMO)
		Msg					("brain().setup() : %lld",Memory.mem_usage() - start);
#endif // DEBUG_MEMORY_MANAGER

	CCustomMonster::reload(section);
	if (!already_dead())
		CStepManager::reload(section);

	//	if (!already_dead())
	CObjectHandler::reload(section);

	if (!already_dead())
		sight().reload(section);

	if (!already_dead())
		movement().reload(section);

	m_disp_walk_stand = pSettings->r_float(section, "disp_walk_stand");
	m_disp_walk_crouch = pSettings->r_float(section, "disp_walk_crouch");
	m_disp_run_stand = pSettings->r_float(section, "disp_run_stand");
	m_disp_run_crouch = pSettings->r_float(section, "disp_run_crouch");
	m_disp_stand_stand = pSettings->r_float(section, "disp_stand_stand");
	m_disp_stand_crouch = pSettings->r_float(section, "disp_stand_crouch");
	m_disp_stand_stand_zoom = pSettings->r_float(section, "disp_stand_stand_zoom");
	m_disp_stand_crouch_zoom = pSettings->r_float(section, "disp_stand_crouch_zoom");

	m_can_select_weapon = true;

	LPCSTR queue_sect = pSettings->r_string(*cNameSect(), "fire_queue_section");
	if (xr_strcmp(queue_sect, "") && pSettings->section_exist(queue_sect))
	{
		m_pstl_min_queue_size_far = READ_IF_EXISTS(pSettings, r_u32, queue_sect, "pstl_min_queue_size_far", 1);
		m_pstl_max_queue_size_far = READ_IF_EXISTS(pSettings, r_u32, queue_sect, "pstl_max_queue_size_far", 1);
		m_pstl_min_queue_interval_far = READ_IF_EXISTS(pSettings, r_u32, queue_sect, "pstl_min_queue_interval_far",
		                                               1000);
		m_pstl_max_queue_interval_far = READ_IF_EXISTS(pSettings, r_u32, queue_sect, "pstl_max_queue_interval_far",
		                                               1250);

		m_pstl_min_queue_size_medium = READ_IF_EXISTS(pSettings, r_u32, queue_sect, "pstl_min_queue_size_medium", 2);
		m_pstl_max_queue_size_medium = READ_IF_EXISTS(pSettings, r_u32, queue_sect, "pstl_max_queue_size_medium", 4);
		m_pstl_min_queue_interval_medium = READ_IF_EXISTS(pSettings, r_u32, queue_sect,
		                                                  "pstl_min_queue_interval_medium", 750);
		m_pstl_max_queue_interval_medium = READ_IF_EXISTS(pSettings, r_u32, queue_sect,
		                                                  "pstl_max_queue_interval_medium", 1000);

		m_pstl_min_queue_size_close = READ_IF_EXISTS(pSettings, r_u32, queue_sect, "pstl_min_queue_size_close", 3);
		m_pstl_max_queue_size_close = READ_IF_EXISTS(pSettings, r_u32, queue_sect, "pstl_max_queue_size_close", 5);
		m_pstl_min_queue_interval_close = READ_IF_EXISTS(pSettings, r_u32, queue_sect, "pstl_min_queue_interval_close",
		                                                 500);
		m_pstl_max_queue_interval_close = READ_IF_EXISTS(pSettings, r_u32, queue_sect, "pstl_max_queue_interval_close",
		                                                 750);


		m_shtg_min_queue_size_far = READ_IF_EXISTS(pSettings, r_u32, queue_sect, "shtg_min_queue_size_far", 1);
		m_shtg_max_queue_size_far = READ_IF_EXISTS(pSettings, r_u32, queue_sect, "shtg_max_queue_size_far", 1);
		m_shtg_min_queue_interval_far = READ_IF_EXISTS(pSettings, r_u32, queue_sect, "shtg_min_queue_interval_far",
		                                               1250);
		m_shtg_max_queue_interval_far = READ_IF_EXISTS(pSettings, r_u32, queue_sect, "shtg_max_queue_interval_far",
		                                               1500);

		m_shtg_min_queue_size_medium = READ_IF_EXISTS(pSettings, r_u32, queue_sect, "shtg_min_queue_size_medium", 1);
		m_shtg_max_queue_size_medium = READ_IF_EXISTS(pSettings, r_u32, queue_sect, "shtg_max_queue_size_medium", 1);
		m_shtg_min_queue_interval_medium = READ_IF_EXISTS(pSettings, r_u32, queue_sect,
		                                                  "shtg_min_queue_interval_medium", 750);
		m_shtg_max_queue_interval_medium = READ_IF_EXISTS(pSettings, r_u32, queue_sect,
		                                                  "shtg_max_queue_interval_medium", 1250);

		m_shtg_min_queue_size_close = READ_IF_EXISTS(pSettings, r_u32, queue_sect, "shtg_min_queue_size_close", 1);
		m_shtg_max_queue_size_close = READ_IF_EXISTS(pSettings, r_u32, queue_sect, "shtg_max_queue_size_close", 1);
		m_shtg_min_queue_interval_close = READ_IF_EXISTS(pSettings, r_u32, queue_sect, "shtg_min_queue_interval_close",
		                                                 500);
		m_shtg_max_queue_interval_close = READ_IF_EXISTS(pSettings, r_u32, queue_sect, "shtg_max_queue_interval_close",
		                                                 1000);


		m_snp_min_queue_size_far = READ_IF_EXISTS(pSettings, r_u32, queue_sect, "snp_min_queue_size_far", 1);
		m_snp_max_queue_size_far = READ_IF_EXISTS(pSettings, r_u32, queue_sect, "snp_max_queue_size_far", 1);
		m_snp_min_queue_interval_far = READ_IF_EXISTS(pSettings, r_u32, queue_sect, "snp_min_queue_interval_far", 3000);
		m_snp_max_queue_interval_far = READ_IF_EXISTS(pSettings, r_u32, queue_sect, "snp_max_queue_interval_far", 4000);

		m_snp_min_queue_size_medium = READ_IF_EXISTS(pSettings, r_u32, queue_sect, "snp_min_queue_size_medium", 1);
		m_snp_max_queue_size_medium = READ_IF_EXISTS(pSettings, r_u32, queue_sect, "snp_max_queue_size_medium", 1);
		m_snp_min_queue_interval_medium = READ_IF_EXISTS(pSettings, r_u32, queue_sect, "snp_min_queue_interval_medium",
		                                                 3000);
		m_snp_max_queue_interval_medium = READ_IF_EXISTS(pSettings, r_u32, queue_sect, "snp_max_queue_interval_medium",
		                                                 4000);

		m_snp_min_queue_size_close = READ_IF_EXISTS(pSettings, r_u32, queue_sect, "snp_min_queue_size_close", 1);
		m_snp_max_queue_size_close = READ_IF_EXISTS(pSettings, r_u32, queue_sect, "snp_max_queue_size_close", 1);
		m_snp_min_queue_interval_close = READ_IF_EXISTS(pSettings, r_u32, queue_sect, "snp_min_queue_interval_close",
		                                                3000);
		m_snp_max_queue_interval_close = READ_IF_EXISTS(pSettings, r_u32, queue_sect, "snp_max_queue_interval_close",
		                                                4000);


		m_mchg_min_queue_size_far = READ_IF_EXISTS(pSettings, r_u32, queue_sect, "mchg_min_queue_size_far", 1);
		m_mchg_max_queue_size_far = READ_IF_EXISTS(pSettings, r_u32, queue_sect, "mchg_max_queue_size_far", 6);
		m_mchg_min_queue_interval_far =
			READ_IF_EXISTS(pSettings, r_u32, queue_sect, "mchg_min_queue_interval_far", 500);
		m_mchg_max_queue_interval_far = READ_IF_EXISTS(pSettings, r_u32, queue_sect, "mchg_max_queue_interval_far",
		                                               1000);

		m_mchg_min_queue_size_medium = READ_IF_EXISTS(pSettings, r_u32, queue_sect, "mchg_min_queue_size_medium", 4);
		m_mchg_max_queue_size_medium = READ_IF_EXISTS(pSettings, r_u32, queue_sect, "mchg_max_queue_size_medium", 6);
		m_mchg_min_queue_interval_medium = READ_IF_EXISTS(pSettings, r_u32, queue_sect,
		                                                  "mchg_min_queue_interval_medium", 500);
		m_mchg_max_queue_interval_medium = READ_IF_EXISTS(pSettings, r_u32, queue_sect,
		                                                  "mchg_max_queue_interval_medium", 750);

		m_mchg_min_queue_size_close = READ_IF_EXISTS(pSettings, r_u32, queue_sect, "mchg_min_queue_size_close", 4);
		m_mchg_max_queue_size_close = READ_IF_EXISTS(pSettings, r_u32, queue_sect, "mchg_max_queue_size_close", 10);
		m_mchg_min_queue_interval_close = READ_IF_EXISTS(pSettings, r_u32, queue_sect, "mchg_min_queue_interval_close",
		                                                 300);
		m_mchg_max_queue_interval_close = READ_IF_EXISTS(pSettings, r_u32, queue_sect, "mchg_max_queue_interval_close",
		                                                 500);


		m_auto_min_queue_size_far = READ_IF_EXISTS(pSettings, r_u32, queue_sect, "auto_min_queue_size_far", 1);
		m_auto_max_queue_size_far = READ_IF_EXISTS(pSettings, r_u32, queue_sect, "auto_max_queue_size_far", 6);
		m_auto_min_queue_interval_far =
			READ_IF_EXISTS(pSettings, r_u32, queue_sect, "auto_min_queue_interval_far", 500);
		m_auto_max_queue_interval_far = READ_IF_EXISTS(pSettings, r_u32, queue_sect, "auto_max_queue_interval_far",
		                                               1000);

		m_auto_min_queue_size_medium = READ_IF_EXISTS(pSettings, r_u32, queue_sect, "auto_min_queue_size_medium", 4);
		m_auto_max_queue_size_medium = READ_IF_EXISTS(pSettings, r_u32, queue_sect, "auto_max_queue_size_medium", 6);
		m_auto_min_queue_interval_medium = READ_IF_EXISTS(pSettings, r_u32, queue_sect,
		                                                  "auto_min_queue_interval_medium", 500);
		m_auto_max_queue_interval_medium = READ_IF_EXISTS(pSettings, r_u32, queue_sect,
		                                                  "auto_max_queue_interval_medium", 750);

		m_auto_min_queue_size_close = READ_IF_EXISTS(pSettings, r_u32, queue_sect, "auto_min_queue_size_close", 4);
		m_auto_max_queue_size_close = READ_IF_EXISTS(pSettings, r_u32, queue_sect, "auto_max_queue_size_close", 10);
		m_auto_min_queue_interval_close = READ_IF_EXISTS(pSettings, r_u32, queue_sect, "auto_min_queue_interval_close",
		                                                 300);
		m_auto_max_queue_interval_close = READ_IF_EXISTS(pSettings, r_u32, queue_sect, "auto_max_queue_interval_close",
		                                                 500);


		//		m_pstl_queue_fire_dist_close		= READ_IF_EXISTS(pSettings,r_float,queue_sect,"pstl_queue_fire_dist_close", 15.0f);
		m_pstl_queue_fire_dist_med = READ_IF_EXISTS(pSettings, r_float, queue_sect, "pstl_queue_fire_dist_med", 15.0f);
		m_pstl_queue_fire_dist_far = READ_IF_EXISTS(pSettings, r_float, queue_sect, "pstl_queue_fire_dist_far", 30.0f);
		//		m_shtg_queue_fire_dist_close		= READ_IF_EXISTS(pSettings,r_float,queue_sect,"shtg_queue_fire_dist_close", 15.0f);
		m_shtg_queue_fire_dist_med = READ_IF_EXISTS(pSettings, r_float, queue_sect, "shtg_queue_fire_dist_med", 15.0f);
		m_shtg_queue_fire_dist_far = READ_IF_EXISTS(pSettings, r_float, queue_sect, "shtg_queue_fire_dist_far", 30.0f);
		//		m_snp_queue_fire_dist_close			= READ_IF_EXISTS(pSettings,r_float,queue_sect,"snp_queue_fire_dist_close", 15.0f);
		m_snp_queue_fire_dist_med = READ_IF_EXISTS(pSettings, r_float, queue_sect, "snp_queue_fire_dist_med", 15.0f);
		m_snp_queue_fire_dist_far = READ_IF_EXISTS(pSettings, r_float, queue_sect, "snp_queue_fire_dist_far", 30.0f);
		//		m_mchg_queue_fire_dist_close			= READ_IF_EXISTS(pSettings,r_float,queue_sect,"mchg_queue_fire_dist_close", 15.0f);
		m_mchg_queue_fire_dist_med = READ_IF_EXISTS(pSettings, r_float, queue_sect, "mchg_queue_fire_dist_med", 15.0f);
		m_mchg_queue_fire_dist_far = READ_IF_EXISTS(pSettings, r_float, queue_sect, "mchg_queue_fire_dist_far", 30.0f);
		//		m_auto_queue_fire_dist_close		= READ_IF_EXISTS(pSettings,r_float,queue_sect,"auto_queue_fire_dist_close", 15.0f);
		m_auto_queue_fire_dist_med = READ_IF_EXISTS(pSettings, r_float, queue_sect, "auto_queue_fire_dist_med", 15.0f);
		m_auto_queue_fire_dist_far = READ_IF_EXISTS(pSettings, r_float, queue_sect, "auto_queue_fire_dist_far", 30.0f);
	}
	else
	{
		m_pstl_min_queue_size_far = READ_IF_EXISTS(pSettings, r_u32, *cNameSect(), "pstl_min_queue_size_far", 1);
		m_pstl_max_queue_size_far = READ_IF_EXISTS(pSettings, r_u32, *cNameSect(), "pstl_max_queue_size_far", 1);
		m_pstl_min_queue_interval_far = READ_IF_EXISTS(pSettings, r_u32, *cNameSect(), "pstl_min_queue_interval_far",
		                                               1000);
		m_pstl_max_queue_interval_far = READ_IF_EXISTS(pSettings, r_u32, *cNameSect(), "pstl_max_queue_interval_far",
		                                               1250);

		m_pstl_min_queue_size_medium = READ_IF_EXISTS(pSettings, r_u32, *cNameSect(), "pstl_min_queue_size_medium", 2);
		m_pstl_max_queue_size_medium = READ_IF_EXISTS(pSettings, r_u32, *cNameSect(), "pstl_max_queue_size_medium", 4);
		m_pstl_min_queue_interval_medium = READ_IF_EXISTS(pSettings, r_u32, *cNameSect(),
		                                                  "pstl_min_queue_interval_medium", 750);
		m_pstl_max_queue_interval_medium = READ_IF_EXISTS(pSettings, r_u32, *cNameSect(),
		                                                  "pstl_max_queue_interval_medium", 1000);

		m_pstl_min_queue_size_close = READ_IF_EXISTS(pSettings, r_u32, *cNameSect(), "pstl_min_queue_size_close", 3);
		m_pstl_max_queue_size_close = READ_IF_EXISTS(pSettings, r_u32, *cNameSect(), "pstl_max_queue_size_close", 5);
		m_pstl_min_queue_interval_close = READ_IF_EXISTS(pSettings, r_u32, *cNameSect(),
		                                                 "pstl_min_queue_interval_close", 500);
		m_pstl_max_queue_interval_close = READ_IF_EXISTS(pSettings, r_u32, *cNameSect(),
		                                                 "pstl_max_queue_interval_close", 750);


		m_shtg_min_queue_size_far = READ_IF_EXISTS(pSettings, r_u32, *cNameSect(), "shtg_min_queue_size_far", 1);
		m_shtg_max_queue_size_far = READ_IF_EXISTS(pSettings, r_u32, *cNameSect(), "shtg_max_queue_size_far", 1);
		m_shtg_min_queue_interval_far = READ_IF_EXISTS(pSettings, r_u32, *cNameSect(), "shtg_min_queue_interval_far",
		                                               1250);
		m_shtg_max_queue_interval_far = READ_IF_EXISTS(pSettings, r_u32, *cNameSect(), "shtg_max_queue_interval_far",
		                                               1500);

		m_shtg_min_queue_size_medium = READ_IF_EXISTS(pSettings, r_u32, *cNameSect(), "shtg_min_queue_size_medium", 1);
		m_shtg_max_queue_size_medium = READ_IF_EXISTS(pSettings, r_u32, *cNameSect(), "shtg_max_queue_size_medium", 1);
		m_shtg_min_queue_interval_medium = READ_IF_EXISTS(pSettings, r_u32, *cNameSect(),
		                                                  "shtg_min_queue_interval_medium", 750);
		m_shtg_max_queue_interval_medium = READ_IF_EXISTS(pSettings, r_u32, *cNameSect(),
		                                                  "shtg_max_queue_interval_medium", 1250);

		m_shtg_min_queue_size_close = READ_IF_EXISTS(pSettings, r_u32, *cNameSect(), "shtg_min_queue_size_close", 1);
		m_shtg_max_queue_size_close = READ_IF_EXISTS(pSettings, r_u32, *cNameSect(), "shtg_max_queue_size_close", 1);
		m_shtg_min_queue_interval_close = READ_IF_EXISTS(pSettings, r_u32, *cNameSect(),
		                                                 "shtg_min_queue_interval_close", 500);
		m_shtg_max_queue_interval_close = READ_IF_EXISTS(pSettings, r_u32, *cNameSect(),
		                                                 "shtg_max_queue_interval_close", 1000);


		m_snp_min_queue_size_far = READ_IF_EXISTS(pSettings, r_u32, *cNameSect(), "snp_min_queue_size_far", 1);
		m_snp_max_queue_size_far = READ_IF_EXISTS(pSettings, r_u32, *cNameSect(), "snp_max_queue_size_far", 1);
		m_snp_min_queue_interval_far = READ_IF_EXISTS(pSettings, r_u32, *cNameSect(), "snp_min_queue_interval_far",
		                                              3000);
		m_snp_max_queue_interval_far = READ_IF_EXISTS(pSettings, r_u32, *cNameSect(), "snp_max_queue_interval_far",
		                                              4000);

		m_snp_min_queue_size_medium = READ_IF_EXISTS(pSettings, r_u32, *cNameSect(), "snp_min_queue_size_medium", 1);
		m_snp_max_queue_size_medium = READ_IF_EXISTS(pSettings, r_u32, *cNameSect(), "snp_max_queue_size_medium", 1);
		m_snp_min_queue_interval_medium = READ_IF_EXISTS(pSettings, r_u32, *cNameSect(),
		                                                 "snp_min_queue_interval_medium", 3000);
		m_snp_max_queue_interval_medium = READ_IF_EXISTS(pSettings, r_u32, *cNameSect(),
		                                                 "snp_max_queue_interval_medium", 4000);

		m_snp_min_queue_size_close = READ_IF_EXISTS(pSettings, r_u32, *cNameSect(), "snp_min_queue_size_close", 1);
		m_snp_max_queue_size_close = READ_IF_EXISTS(pSettings, r_u32, *cNameSect(), "snp_max_queue_size_close", 1);
		m_snp_min_queue_interval_close = READ_IF_EXISTS(pSettings, r_u32, *cNameSect(), "snp_min_queue_interval_close",
		                                                3000);
		m_snp_max_queue_interval_close = READ_IF_EXISTS(pSettings, r_u32, *cNameSect(), "snp_max_queue_interval_close",
		                                                4000);

		m_mchg_min_queue_size_far = READ_IF_EXISTS(pSettings, r_u32, *cNameSect(), "mchg_min_queue_size_far", 1);
		m_mchg_max_queue_size_far = READ_IF_EXISTS(pSettings, r_u32, *cNameSect(), "mchg_max_queue_size_far", 6);
		m_mchg_min_queue_interval_far = READ_IF_EXISTS(pSettings, r_u32, *cNameSect(), "mchg_min_queue_interval_far",
		                                               500);
		m_mchg_max_queue_interval_far = READ_IF_EXISTS(pSettings, r_u32, *cNameSect(), "mchg_max_queue_interval_far",
		                                               1000);

		m_mchg_min_queue_size_medium = READ_IF_EXISTS(pSettings, r_u32, *cNameSect(), "mchg_min_queue_size_medium", 4);
		m_mchg_max_queue_size_medium = READ_IF_EXISTS(pSettings, r_u32, *cNameSect(), "mchg_max_queue_size_medium", 6);
		m_mchg_min_queue_interval_medium = READ_IF_EXISTS(pSettings, r_u32, *cNameSect(),
		                                                  "mchg_min_queue_interval_medium", 500);
		m_mchg_max_queue_interval_medium = READ_IF_EXISTS(pSettings, r_u32, *cNameSect(),
		                                                  "mchg_max_queue_interval_medium", 750);

		m_mchg_min_queue_size_close = READ_IF_EXISTS(pSettings, r_u32, *cNameSect(), "mchg_min_queue_size_close", 4);
		m_mchg_max_queue_size_close = READ_IF_EXISTS(pSettings, r_u32, *cNameSect(), "mchg_max_queue_size_close", 10);
		m_mchg_min_queue_interval_close = READ_IF_EXISTS(pSettings, r_u32, *cNameSect(),
		                                                 "mchg_min_queue_interval_close", 300);
		m_mchg_max_queue_interval_close = READ_IF_EXISTS(pSettings, r_u32, *cNameSect(),
		                                                 "mchg_max_queue_interval_close", 500);

		m_auto_min_queue_size_far = READ_IF_EXISTS(pSettings, r_u32, *cNameSect(), "auto_min_queue_size_far", 1);
		m_auto_max_queue_size_far = READ_IF_EXISTS(pSettings, r_u32, *cNameSect(), "auto_max_queue_size_far", 6);
		m_auto_min_queue_interval_far = READ_IF_EXISTS(pSettings, r_u32, *cNameSect(), "auto_min_queue_interval_far",
		                                               500);
		m_auto_max_queue_interval_far = READ_IF_EXISTS(pSettings, r_u32, *cNameSect(), "auto_max_queue_interval_far",
		                                               1000);

		m_auto_min_queue_size_medium = READ_IF_EXISTS(pSettings, r_u32, *cNameSect(), "auto_min_queue_size_medium", 4);
		m_auto_max_queue_size_medium = READ_IF_EXISTS(pSettings, r_u32, *cNameSect(), "auto_max_queue_size_medium", 6);
		m_auto_min_queue_interval_medium = READ_IF_EXISTS(pSettings, r_u32, *cNameSect(),
		                                                  "auto_min_queue_interval_medium", 500);
		m_auto_max_queue_interval_medium = READ_IF_EXISTS(pSettings, r_u32, *cNameSect(),
		                                                  "auto_max_queue_interval_medium", 750);

		m_auto_min_queue_size_close = READ_IF_EXISTS(pSettings, r_u32, *cNameSect(), "auto_min_queue_size_close", 4);
		m_auto_max_queue_size_close = READ_IF_EXISTS(pSettings, r_u32, *cNameSect(), "auto_max_queue_size_close", 10);
		m_auto_min_queue_interval_close = READ_IF_EXISTS(pSettings, r_u32, *cNameSect(),
		                                                 "auto_min_queue_interval_close", 300);
		m_auto_max_queue_interval_close = READ_IF_EXISTS(pSettings, r_u32, *cNameSect(),
		                                                 "auto_max_queue_interval_close", 500);

		//		m_pstl_queue_fire_dist_close		= READ_IF_EXISTS(pSettings,r_float,*cNameSect(),"pstl_queue_fire_dist_close", 15.0f);
		m_pstl_queue_fire_dist_med =
			READ_IF_EXISTS(pSettings, r_float, *cNameSect(), "pstl_queue_fire_dist_med", 15.0f);
		m_pstl_queue_fire_dist_far =
			READ_IF_EXISTS(pSettings, r_float, *cNameSect(), "pstl_queue_fire_dist_far", 30.0f);
		//		m_shtg_queue_fire_dist_close		= READ_IF_EXISTS(pSettings,r_float,*cNameSect(),"shtg_queue_fire_dist_close", 15.0f);
		m_shtg_queue_fire_dist_med =
			READ_IF_EXISTS(pSettings, r_float, *cNameSect(), "shtg_queue_fire_dist_med", 15.0f);
		m_shtg_queue_fire_dist_far =
			READ_IF_EXISTS(pSettings, r_float, *cNameSect(), "shtg_queue_fire_dist_far", 30.0f);
		//		m_snp_queue_fire_dist_close			= READ_IF_EXISTS(pSettings,r_float,*cNameSect(),"snp_queue_fire_dist_close", 15.0f);
		m_snp_queue_fire_dist_med = READ_IF_EXISTS(pSettings, r_float, *cNameSect(), "snp_queue_fire_dist_med", 15.0f);
		m_snp_queue_fire_dist_far = READ_IF_EXISTS(pSettings, r_float, *cNameSect(), "snp_queue_fire_dist_far", 30.0f);
		//		m_mchg_queue_fire_dist_close			= READ_IF_EXISTS(pSettings,r_float,*cNameSect(),"mchg_queue_fire_dist_close", 15.0f);
		m_mchg_queue_fire_dist_med =
			READ_IF_EXISTS(pSettings, r_float, *cNameSect(), "mchg_queue_fire_dist_med", 15.0f);
		m_mchg_queue_fire_dist_far =
			READ_IF_EXISTS(pSettings, r_float, *cNameSect(), "mchg_queue_fire_dist_far", 30.0f);
		//		m_auto_queue_fire_dist_close		= READ_IF_EXISTS(pSettings,r_float,**cNameSect(),"auto_queue_fire_dist_close", 15.0f);
		m_auto_queue_fire_dist_med =
			READ_IF_EXISTS(pSettings, r_float, *cNameSect(), "auto_queue_fire_dist_med", 15.0f);
		m_auto_queue_fire_dist_far =
			READ_IF_EXISTS(pSettings, r_float, *cNameSect(), "auto_queue_fire_dist_far", 30.0f);
	}
	m_power_fx_factor = pSettings->r_float(section, "power_fx_factor");
}

#include "monster_community.h"
#include "alife_registry_wrappers.h"
void CAI_Stalker::Die(CObject* who)
{
	//const_cast<CALifeSimulator*>(ai().get_alife())->spawn_item(cNameSect_str(), Position(), ai_location().level_vertex_id(), ai_location().game_vertex_id(), ALife::_OBJECT_ID(-1));
	movement().on_death();

	notify_on_wounded_or_killed(who);

	SelectAnimation(XFORM().k, movement().detail().direction(), movement().speed());

	if (m_death_sound_enabled)
	{
		//sound().set_sound_mask		((u32)eStalkerSoundMaskDie);
		if (is_special_killer(who))
			sound().play(eStalkerSoundDieInAnomaly);
		else
			sound().play(eStalkerSoundDie);
	}

	m_hammer_is_clutched = m_clutched_hammer_enabled && !CObjectHandler::planner()
	                                                     .m_storage.property(ObjectHandlerSpace::eWorldPropertyStrapped)
		&& !::Random.randI(0, 2);

	inherited::Die(who);

	//запретить использование слотов в инвенторе
	inventory().SetSlotsUseful(false);

	if (inventory().GetActiveSlot() == NO_ACTIVE_SLOT)
		return;

	CInventoryItem* active_item = inventory().ActiveItem();
	if (!active_item)
		return;

	CWeapon* weapon = smart_cast<CWeapon*>(active_item);
	if (!weapon)
		return;

	{
		TIItemContainer::iterator I = inventory().m_all.begin();
		TIItemContainer::iterator E = inventory().m_all.end();
		for (; I != E; ++I)
		{
			if (std::find(weapon->m_ammoTypes.begin(), weapon->m_ammoTypes.end(), (*I)->object().cNameSect()) == weapon
			                                                                                                     ->
			                                                                                                     m_ammoTypes
			                                                                                                     .end())
				continue;

			NET_Packet packet;
			u_EventGen(packet, GE_DESTROY, (*I)->object().ID());
			u_EventSend(packet);
		}
	}
}

void CAI_Stalker::Load(LPCSTR section)
{
	CCustomMonster::Load(section);
	CObjectHandler::Load(section);
	sight().Load(section);

	// skeleton physics
	m_pPhysics_support->in_Load(section);

	m_can_select_items = !!pSettings->r_bool(section, "can_select_items");
}

BOOL CAI_Stalker::net_Spawn(CSE_Abstract* DC)
{
#ifdef DEBUG_MEMORY_MANAGER
	size_t				start = 0;
	if (g_bMEMO)
		start						= Memory.mem_usage();
#endif // DEBUG_MEMORY_MANAGER

	CSE_Abstract* e = (CSE_Abstract*)(DC);
	CSE_ALifeHumanStalker* tpHuman = smart_cast<CSE_ALifeHumanStalker*>(e);
	R_ASSERT(tpHuman);


	//static bool first_time			= true;
	//if ( first_time ) {
	//	tpHuman->o_Position.z		-= 3.f;
	//	first_time					= false;
	//}

	m_group_behaviour = !!tpHuman->m_flags.test(CSE_ALifeObject::flGroupBehaviour);

	if (!CObjectHandler::net_Spawn(DC) || !inherited::net_Spawn(DC))
		return (FALSE);

	set_money(tpHuman->m_dwMoney, false);

#ifdef DEBUG_MEMORY_MANAGER
	size_t					_start = 0;
	if (g_bMEMO)
		_start							= Memory.mem_usage();
#endif // DEBUG_MEMORY_MANAGER

	animation().reload();

#ifdef DEBUG_MEMORY_MANAGER
	if (g_bMEMO)
		Msg					("CStalkerAnimationManager::reload() : %lld",Memory.mem_usage() - _start);
#endif // DEBUG_MEMORY_MANAGER

	movement().m_head.current.yaw = movement().m_head.target.yaw = movement().m_body.current.yaw = movement()
	                                                                                               .m_body.target.yaw =
		angle_normalize_signed(-tpHuman->o_torso.yaw);
	movement().m_body.current.pitch = movement().m_body.target.pitch = 0;

	if (ai().game_graph().valid_vertex_id(tpHuman->m_tGraphID))
		ai_location().game_vertex(tpHuman->m_tGraphID);

	if (ai().game_graph().valid_vertex_id(tpHuman->m_tNextGraphID) && movement().restrictions().accessible(
		ai().game_graph().vertex(
			     tpHuman->m_tNextGraphID)->
		     level_point()))
		movement().set_game_dest_vertex(tpHuman->m_tNextGraphID);

	R_ASSERT2(
		ai().get_game_graph() &&
		ai().get_level_graph() &&
		ai().get_cross_table() &&
		(ai().level_graph().level_id() != u32(-1)),
		"There is no AI-Map, level graph, cross table, or graph is not compiled into the game graph!"
	);

	setEnabled(TRUE);


	if (!IIsServer() && !Level().CurrentViewEntity())
		Level().SetEntity(this);

	if (!g_Alive())
		sound().set_sound_mask(u32(eStalkerSoundMaskDie));

	//загрузить иммунитеты из модельки сталкера
	IKinematics* pKinematics = smart_cast<IKinematics*>(Visual());
	VERIFY(pKinematics);
	CInifile* ini = pKinematics->LL_UserData();
	if (ini)
	{
		if (ini->section_exist("immunities"))
		{
			LPCSTR imm_sect = ini->r_string("immunities", "immunities_sect");
			conditions().LoadImmunities(imm_sect, pSettings);
		}

		if (ini->line_exist("bone_protection", "bones_protection_sect"))
		{
			m_boneHitProtection = xr_new<SBoneProtections>();
			m_boneHitProtection->reload(ini->r_string("bone_protection", "bones_protection_sect"), pKinematics);
		}
	}

	//вычислить иммунета в зависимости от ранга
	static float novice_rank_immunity = pSettings->r_float("ranks_properties", "immunities_novice_k");
	static float expirienced_rank_immunity = pSettings->r_float("ranks_properties", "immunities_experienced_k");

	static float novice_rank_visibility = pSettings->r_float("ranks_properties", "visibility_novice_k");
	static float expirienced_rank_visibility = pSettings->r_float("ranks_properties", "visibility_experienced_k");

	static float novice_rank_dispersion = pSettings->r_float("ranks_properties", "dispersion_novice_k");
	static float expirienced_rank_dispersion = pSettings->r_float("ranks_properties", "dispersion_experienced_k");


	CHARACTER_RANK_VALUE rank = Rank();
	clamp(rank, 0, 100);
	float rank_k = float(rank) / 100.f;
	m_fRankImmunity = novice_rank_immunity + (expirienced_rank_immunity - novice_rank_immunity) * rank_k;
	m_fRankVisibility = novice_rank_visibility + (expirienced_rank_visibility - novice_rank_visibility) * rank_k;
	m_fRankDisperison = expirienced_rank_dispersion + (novice_rank_dispersion - expirienced_rank_dispersion) * (1 -
		rank_k);

	if (!fis_zero(SpecificCharacter().panic_threshold()))
		m_panic_threshold = SpecificCharacter().panic_threshold();

	sight().setup(CSightAction(SightManager::eSightTypeCurrentDirection));

#ifdef _DEBUG
	if (ai().get_alife() && !Level().MapManager().HasMapLocation("debug_stalker",ID())) {
		CMapLocation				*map_location = 
			Level().MapManager().AddMapLocation(
				"debug_stalker",
				ID()
			);

		map_location->SetHint		(cName());
	}
#endif // _DEBUG

#ifdef DEBUG_MEMORY_MANAGER
	if (g_bMEMO) {
		Msg							("CAI_Stalker::net_Spawn() : %lld",Memory.mem_usage() - start);
	}
#endif // DEBUG_MEMORY_MANAGER

	if (SpecificCharacter().terrain_sect().size())
	{
		movement().locations().Load(*SpecificCharacter().terrain_sect());
	}

	sight().update();
	Exec_Look(.001f);

	m_pPhysics_support->in_NetSpawn(e);

	return (TRUE);
}

void CAI_Stalker::net_Destroy()
{
	inherited::net_Destroy();
	CInventoryOwner::net_Destroy();
	m_pPhysics_support->in_NetDestroy();

	xr_delete(m_ce_close);
	xr_delete(m_ce_far);
	xr_delete(m_ce_best);
	xr_delete(m_ce_angle);
	xr_delete(m_ce_safe);
	xr_delete(m_ce_ambush);
	xr_delete(m_boneHitProtection);
}

void CAI_Stalker::net_Save(NET_Packet& P)
{
	inherited::net_Save(P);
	m_pPhysics_support->in_NetSave(P);
}

BOOL CAI_Stalker::net_SaveRelevant()
{
	return (inherited::net_SaveRelevant() || BOOL(PPhysicsShell() != NULL));
}

void CAI_Stalker::net_Export(NET_Packet& P)
{
	R_ASSERT(Local());

	// export last known packet
	if (NET.empty()) {
		Msg("![CAI_Stalker::net_Export] net_update deque is empty for %s, section %s, id %d, crash", cName().c_str(), cNameSect().c_str(), ID());
		//R_ASSERT(!NET.empty());
		return;
	}
	net_update& N = NET.back();
	static u32 s_sp_net_export_next = 0;
	static u32 s_sp_net_export_count = 0;
	if (sp_net_diag_allow(s_sp_net_export_next, s_sp_net_export_count, 1000, 12))
	{
		const Fvector& current_pos = Position();
		Msg("* [SP_NET][NPC_EXPORT] id=%hu name=[%s] sect=[%s] local=%d ready=%d net=%u ts=%u cur=(%.2f %.2f %.2f) net_pos=(%.2f %.2f %.2f) health=%.3f yaw=%.3f",
			ID(), cName().c_str(), cNameSect().c_str(), Local() ? 1 : 0, getReady() ? 1 : 0, (u32)NET.size(), N.dwTimeStamp,
			current_pos.x, current_pos.y, current_pos.z, N.p_pos.x, N.p_pos.y, N.p_pos.z, GetfHealth(), N.o_model);
	}
	//	P.w_float						(inventory().TotalWeight());
	//	P.w_u32							(m_dwMoney);

	P.w_float(GetfHealth());

	P.w_u32(N.dwTimeStamp);
	P.w_u8(0);
	P.w_vec3(N.p_pos);
	P.w_float /*w_angle8*/(N.o_model);
	P.w_float /*w_angle8*/(N.o_torso.yaw);
	P.w_float /*w_angle8*/(N.o_torso.pitch);
	P.w_float /*w_angle8*/(N.o_torso.roll);
	P.w_u8(u8(g_Team()));
	P.w_u8(u8(g_Squad()));
	P.w_u8(u8(g_Group()));


	float f1 = 0;
	GameGraph::_GRAPH_ID l_game_vertex_id = ai_location().game_vertex_id();
	P.w(&l_game_vertex_id, sizeof(l_game_vertex_id));
	P.w(&l_game_vertex_id, sizeof(l_game_vertex_id));
	//	P.w						(&f1,						sizeof(f1));
	//	P.w						(&f1,						sizeof(f1));
	if (ai().game_graph().valid_vertex_id(l_game_vertex_id))
	{
		f1 = Position().distance_to(ai().game_graph().vertex(l_game_vertex_id)->level_point());
		P.w(&f1, sizeof(f1));
		f1 = Position().distance_to(ai().game_graph().vertex(l_game_vertex_id)->level_point());
		P.w(&f1, sizeof(f1));
	}
	else
	{
		P.w(&f1, sizeof(f1));
		P.w(&f1, sizeof(f1));
	}

	P.w_stringZ(m_sStartDialog);

	if (IIsServer())
	{
		// Local M_UPDATE is read back by CSE_ALifeHumanStalker::UPDATE_Read on the server.
		// Keep the extended runtime state only for server->client object updates.
		if (!packet_begins_with_message(P, M_UPDATE))
			write_stalker_server_state(*this, P);
	}
}

void CAI_Stalker::net_Import(NET_Packet& P)
{
	R_ASSERT(Remote());
	net_update N;

	u8 flags;
	const u32 read_start = P.r_tell();
	const Fvector before_pos = Position();
	const u32 net_size_before = (u32)NET.size();


	float health;
	P.r_float(health);
	SetfHealth(health);
	//	fEntityHealth = health;

	P.r_u32(N.dwTimeStamp);
	P.r_u8(flags);
	P.r_vec3(N.p_pos);
	P.r_float /*r_angle8*/(N.o_model);
	P.r_float /*r_angle8*/(N.o_torso.yaw);
	P.r_float /*r_angle8*/(N.o_torso.pitch);
	P.r_float /*r_angle8*/(N.o_torso.roll);
	id_Team = P.r_u8();
	id_Squad = P.r_u8();
	id_Group = P.r_u8();


	GameGraph::_GRAPH_ID graph_vertex_id = movement().game_dest_vertex_id();
	P.r(&graph_vertex_id, sizeof(GameGraph::_GRAPH_ID));
	graph_vertex_id = ai_location().game_vertex_id();
	P.r(&graph_vertex_id, sizeof(GameGraph::_GRAPH_ID));

	const bool accepted_update = NET.empty() || (NET.back().dwTimeStamp < N.dwTimeStamp);
	if (accepted_update)
	{
		NET.push_back(N);
		NET_WasInterpolating = TRUE;
	}

	float graph_dist_0 = 0.f;
	float graph_dist_1 = 0.f;
	P.r_float(graph_dist_0);
	P.r_float(graph_dist_1);

	P.r_stringZ(m_sStartDialog);

	const u32 remaining_before_state = object_update_chunk_remaining(P);
	bool has_server_state = false;
	bool has_server_inventory_state = false;
	u8 server_movement_type = 0;
	u8 server_body_state = 0;
	u8 server_mental_state = 0;
	float server_body_current_yaw = 0.f;
	float server_body_target_yaw = 0.f;
	float server_head_yaw = 0.f;
	float server_head_pitch = 0.f;
	float server_speed = 0.f;
	float server_actual_speed = 0.f;
	u16 server_active_slot = u16(-1);
	u16 server_active_item_id = u16(-1);
	u8 server_hud_state = u8(-1);
	u8 server_hud_next_state = u8(-1);
	u8 server_weapon_state = u8(-1);
	u8 server_weapon_next_state = u8(-1);
	u16 server_weapon_ammo_elapsed = 0;
	u8 server_weapon_ammo_type = u8(-1);
	u8 server_weapon_zoomed = 0;
	u8 server_weapon_working = 0;
	u16 server_object_handler_item_id = u16(-1);
	u16 server_object_handler_state_id = u16(-1);

	bool has_server_animation_state = false;
	u32 server_anim_motion[kStalkerWorldStateExtAnimationTrackCount];
	u16 server_anim_time[kStalkerWorldStateExtAnimationTrackCount];
	u8 server_anim_flags[kStalkerWorldStateExtAnimationTrackCount];
	reset_stalker_animation_state(server_anim_motion, server_anim_time, server_anim_flags);

	if (remaining_before_state >= kStalkerWorldStateExtBaseSize)
	{
		const u32 state_pos = P.r_tell();
		const u8 marker = P.r_u8();
		const u8 version = P.r_u8();
		if ((marker == kStalkerWorldStateExtMarker) && (version >= 2) && (version <= kStalkerWorldStateExtVersion) && (object_update_chunk_remaining(P) >= (kStalkerWorldStateExtBaseSize - 2 * sizeof(u8))))
		{
			server_movement_type = P.r_u8();
			server_body_state = P.r_u8();
			server_mental_state = P.r_u8();
			P.r_float(server_body_current_yaw);
			P.r_float(server_body_target_yaw);
			P.r_float(server_head_yaw);
			P.r_float(server_head_pitch);
			P.r_float(server_speed);
			P.r_float(server_actual_speed);
			if ((version >= 3) && (object_update_chunk_remaining(P) >= kStalkerWorldStateExtV3ExtraSize))
			{
				server_active_slot = P.r_u16();
				server_active_item_id = P.r_u16();
				server_hud_state = P.r_u8();
				server_hud_next_state = P.r_u8();
				server_weapon_state = P.r_u8();
				server_weapon_next_state = P.r_u8();
				server_weapon_ammo_elapsed = P.r_u16();
				server_weapon_ammo_type = P.r_u8();
				server_weapon_zoomed = P.r_u8();
				server_weapon_working = P.r_u8();
				has_server_inventory_state = true;
			}
			if ((version >= 4) && (object_update_chunk_remaining(P) >= kStalkerWorldStateExtV4AnimExtraSize))
			{
				for (u32 i = 0; i < kStalkerWorldStateExtAnimationTrackCount; ++i)
				{
					server_anim_motion[i] = P.r_u32();
					server_anim_time[i] = P.r_u16();
					server_anim_flags[i] = P.r_u8();
				}
				has_server_animation_state = true;
			}
			if ((version >= 5) && (object_update_chunk_remaining(P) >= kStalkerWorldStateExtV5ObjectHandlerExtraSize))
			{
				server_object_handler_item_id = P.r_u16();
				server_object_handler_state_id = P.r_u16();
			}
			has_server_state = true;
		}
		else
			P.r_seek(state_pos);
	}

	if (has_server_state)
	{
		m_net_server_movement_type = server_movement_type;
		m_net_server_body_state = server_body_state;
		m_net_server_mental_state = server_mental_state;
		m_net_server_body_current_yaw = server_body_current_yaw;
		m_net_server_body_target_yaw = server_body_target_yaw;
		m_net_server_head_yaw = server_head_yaw;
		m_net_server_head_pitch = server_head_pitch;
		m_net_server_speed = server_speed;
		m_net_server_actual_speed = server_actual_speed;
		m_net_server_position.set(N.p_pos);
		m_net_server_update_timestamp = N.dwTimeStamp;
		if (has_server_inventory_state)
		{
			m_net_server_active_slot = server_active_slot;
			m_net_server_active_item_id = server_active_item_id;
			m_net_server_hud_state = server_hud_state;
			m_net_server_hud_next_state = server_hud_next_state;
			m_net_server_weapon_state = server_weapon_state;
			m_net_server_weapon_next_state = server_weapon_next_state;
			m_net_server_weapon_ammo_elapsed = server_weapon_ammo_elapsed;
			m_net_server_weapon_ammo_type = server_weapon_ammo_type;
			m_net_server_weapon_zoomed = server_weapon_zoomed;
			m_net_server_weapon_working = server_weapon_working;
			m_net_server_object_handler_item_id = server_object_handler_item_id;
			m_net_server_object_handler_state_id = server_object_handler_state_id;
		}
				m_net_server_animation_state_valid = has_server_animation_state;
		if (has_server_animation_state)
		{
			for (u32 i = 0; i < kStalkerWorldStateExtAnimationTrackCount; ++i)
			{
				m_net_server_anim_motion[i] = server_anim_motion[i];
				m_net_server_anim_time[i] = server_anim_time[i];
				m_net_server_anim_flags[i] = server_anim_flags[i];
			}
		}
		m_net_server_state_valid = true;
		m_net_server_state_time = Device.dwTimeGlobal;
	}

	if (has_server_state && g_Alive() && getReady())
	{
		MonsterSpace::EMovementType imported_movement_type = server_movement_type <= (u8)eMovementTypeStand ? (MonsterSpace::EMovementType)server_movement_type : eMovementTypeStand;
		MonsterSpace::EBodyState imported_body_state = server_body_state <= (u8)eBodyStateStand ? (MonsterSpace::EBodyState)server_body_state : eBodyStateStand;
		MonsterSpace::EMentalState imported_mental_state = server_mental_state <= (u8)eMentalStatePanic ? (MonsterSpace::EMentalState)server_mental_state : eMentalStateDanger;
		if ((imported_body_state == eBodyStateCrouch) && (imported_mental_state == eMentalStateFree))
			imported_mental_state = eMentalStateDanger;

		const float server_motion_speed = (server_speed > 0.03f) ? clampr(server_actual_speed, 0.f, 10.f) : 0.f;
		if (server_motion_speed < 0.03f)
			imported_movement_type = eMovementTypeStand;

		movement().setup_state_from_network(imported_body_state, imported_movement_type, imported_mental_state, server_motion_speed);
		movement().m_body.current.yaw = server_body_current_yaw;
		movement().m_body.target.yaw = server_body_target_yaw;
		movement().m_head.current.yaw = server_head_yaw;
		movement().m_head.target.yaw = server_head_yaw;
		movement().m_head.current.pitch = server_head_pitch;
		movement().m_head.target.pitch = server_head_pitch;
	}

	if (has_server_inventory_state && g_Alive() && getReady())
		apply_stalker_server_inventory_state(*this, server_active_slot, server_active_item_id, server_hud_state, server_hud_next_state, server_weapon_state, server_weapon_next_state, server_weapon_ammo_elapsed, server_weapon_ammo_type, server_object_handler_item_id, server_object_handler_state_id);

	const u32 remaining_after_state = object_update_chunk_remaining(P);
	static u32 s_sp_net_import_next = 0;
	static u32 s_sp_net_import_count = 0;
	if (sp_net_diag_allow(s_sp_net_import_next, s_sp_net_import_count, 1000, 16))
	{
		Msg("* [SP_NET][NPC_IMPORT] id=%hu name=[%s] sect=[%s] local=%d ready=%d read=%u->%u accepted=%d net=%u->%u health=%.3f flags=%u ts=%u before=(%.2f %.2f %.2f) net_pos=(%.2f %.2f %.2f) yaw=%.3f team=%u/%u/%u graph_dist=(%.2f %.2f) server_state=%d state=%u/%u/%u state_speed=%.3f actual_speed=%.3f stuck=%d slot=%u item=%u hud=%u/%u wpn=%u/%u ammo=%u type=%u zoom=%u fire=%u oh=%u/%u anim=%d yaw_state=%.3f/%.3f head=%.3f anim_g=%u/%u/%u anim_h=%u/%u/%u anim_t=%u/%u/%u anim_l=%u/%u/%u anim_s=%u/%u/%u rem=%u dialog=[%s]",
			ID(), cName().c_str(), cNameSect().c_str(), Local() ? 1 : 0, getReady() ? 1 : 0,
			read_start, P.r_tell(), accepted_update ? 1 : 0, net_size_before, (u32)NET.size(),
			health, flags, N.dwTimeStamp, before_pos.x, before_pos.y, before_pos.z, N.p_pos.x, N.p_pos.y, N.p_pos.z,
			N.o_model, id_Team, id_Squad, id_Group, graph_dist_0, graph_dist_1, has_server_state ? 1 : 0, server_movement_type, server_body_state, server_mental_state, server_speed, server_actual_speed, ((server_speed > 0.05f) && (server_actual_speed < 0.03f)) ? 1 : 0, server_active_slot, server_active_item_id, server_hud_state, server_hud_next_state, server_weapon_state, server_weapon_next_state, server_weapon_ammo_elapsed, server_weapon_ammo_type, server_weapon_zoomed, server_weapon_working, server_object_handler_item_id, server_object_handler_state_id, has_server_animation_state ? 1 : 0, server_body_current_yaw, server_body_target_yaw, server_head_yaw, server_anim_motion[eStalkerNetAnimGlobal], server_anim_time[eStalkerNetAnimGlobal], server_anim_flags[eStalkerNetAnimGlobal], server_anim_motion[eStalkerNetAnimHead], server_anim_time[eStalkerNetAnimHead], server_anim_flags[eStalkerNetAnimHead], server_anim_motion[eStalkerNetAnimTorso], server_anim_time[eStalkerNetAnimTorso], server_anim_flags[eStalkerNetAnimTorso], server_anim_motion[eStalkerNetAnimLegs], server_anim_time[eStalkerNetAnimLegs], server_anim_flags[eStalkerNetAnimLegs], server_anim_motion[eStalkerNetAnimScript], server_anim_time[eStalkerNetAnimScript], server_anim_flags[eStalkerNetAnimScript], remaining_after_state, m_sStartDialog.c_str());
	}

	setVisible(TRUE);
	setEnabled(TRUE);
}
void CAI_Stalker::update_object_handler()
{
	if (!g_Alive())
		return;

	if (getDestroy())
		return;

	bool reset_obj_handler = false;
	TRYC(CObjectHandler::update())
	CATCHC(reset_obj_handler = true)

	if(reset_obj_handler)
	{
		TRY
		CObjectHandler::set_goal(eObjectActionIdle);
		CObjectHandler::update();
		CATCH
	}
}

void CAI_Stalker::create_anim_mov_ctrl(CBlend* b, Fmatrix* start_pose, bool local_animation)
{
	inherited::create_anim_mov_ctrl(b, start_pose, local_animation);
}

void CAI_Stalker::destroy_anim_mov_ctrl()
{
	inherited::destroy_anim_mov_ctrl();

	if (!g_Alive())
		return;

	if (getDestroy())
		return;

	movement().m_head.current.yaw = movement().m_body.current.yaw;
	movement().m_head.current.pitch = movement().m_body.current.pitch;
	movement().m_head.target.yaw = movement().m_body.current.yaw;
	movement().m_head.target.pitch = movement().m_body.current.pitch;

	movement().cleanup_after_animation_selector();
	movement().update(0);
}

void CAI_Stalker::UpdateCL()
{
	START_PROFILE("stalker")
		START_PROFILE("stalker/client_update")
			VERIFY2(PPhysicsShell()||getEnabled(), *cName());

			if (g_Alive())
			{
					START_PROFILE("stalker/client_update/object_handler")
						update_object_handler();
					STOP_PROFILE

				if (
					(movement().speed(character_physics_support()->movement()) > EPS_L)
					&&
					(eMovementTypeStand != movement().movement_type())
					&&
					(eMentalStateDanger == movement().mental_state())
				)
				{
					if (
						(eBodyStateStand == movement().body_state())
						&&
						(eMovementTypeRun == movement().movement_type())
					)
					{
						sound().play(eStalkerSoundRunningInDanger);
					}
					else
					{
						//				sound().play	(eStalkerSoundWalkingInDanger);
					}
				}
			}

			const bool remote_update = !!Remote();
			const bool remote_tpose_update = remote_update && stalker_remote_tpose_enabled();
			const bool remote_client_authority = remote_update && IIsClient() && g_Alive();
			Fvector remote_pos_before;
			u32 remote_net_count_before = 0;
			u32 remote_time_cl = 0;
			u32 remote_ts_first = 0;
			u32 remote_ts_second = 0;
			u32 remote_ts_last = 0;
			s32 remote_interp_select = -1;
			float remote_interp_factor = -1.f;
			u8 remote_interp_mode = 0;
			bool anim_diag_remote_ready = false;
			float anim_diag_target_step = 0.f;
			float anim_diag_frame_speed = 0.f;
			float anim_diag_target_frame_speed = 0.f;
			float anim_diag_server_visual_speed = 0.f;
			u32 anim_diag_server_anim_time_applied = 0;
			u32 anim_diag_server_anim_missing = 0;
			u32 anim_diag_server_anim_mismatched = 0;
			u32 anim_diag_server_anim_forced = 0;
			u32 anim_diag_server_anim_authority = 0;
			bool anim_diag_phys_sync = false;
			float anim_diag_phys_sync_dist = 0.f;
			Fvector anim_diag_phys_sync_ctrl_before;
			Fvector anim_diag_phys_sync_ctrl_after;
			anim_diag_phys_sync_ctrl_before.set(0.f, 0.f, 0.f);
			anim_diag_phys_sync_ctrl_after.set(0.f, 0.f, 0.f);
			bool remote_final_anim_root_killed = false;
			bool remote_final_phys_reapplied = false;
			const bool remote_final_sight_skipped = remote_client_authority;
			Fvector remote_final_raw_net_pos;
			Fvector remote_final_server_pos;
			Fvector remote_final_pos;
			remote_final_raw_net_pos.set(0.f, 0.f, 0.f);
			remote_final_server_pos.set(0.f, 0.f, 0.f);
			remote_final_pos.set(0.f, 0.f, 0.f);
			float remote_final_raw_step = 0.f;
			float remote_final_step = 0.f;
			float remote_final_raw_frame_speed = 0.f;
			float remote_final_frame_speed = 0.f;
			float remote_final_server_visual_speed = 0.f;
			float remote_final_raw_yaw = 0.f;
			float remote_final_server_yaw = 0.f;
			float remote_final_yaw = 0.f;
			if (remote_update)
			{
				remote_pos_before.set(Position());
				remote_net_count_before = (u32)NET.size();
				if (!NET.empty())
				{
					const u32 remote_time_server = Level().timeServer();
					remote_time_cl = remote_time_server > NET_Latency ? remote_time_server - NET_Latency : 0;
					remote_ts_first = NET[0].dwTimeStamp;
					remote_ts_second = NET.size() > 1 ? NET[1].dwTimeStamp : remote_ts_first;
					remote_ts_last = NET.back().dwTimeStamp;
					if ((remote_time_cl > remote_ts_last) || (NET.size() < 2))
						remote_interp_mode = 2;
					else
					{
						remote_interp_mode = 3;
						for (u32 id = 0; id < NET.size() - 1; ++id)
						{
							if ((NET[id].dwTimeStamp <= remote_time_cl) && (remote_time_cl <= NET[id + 1].dwTimeStamp))
							{
				remote_interp_select = (s32)id;
				const u32 d1 = remote_time_cl - NET[id].dwTimeStamp;
				const u32 d2 = NET[id + 1].dwTimeStamp - NET[id].dwTimeStamp;
				remote_interp_factor = d2 ? float(d1) / float(d2) : 1.f;
				remote_interp_mode = 1;
				break;
							}
						}
					}
				}
			}

			const Fvector sp_ucl_pos_before = Position();
			CPHMovementControl* sp_ucl_control = character_physics_support() ? character_physics_support()->movement() : nullptr;
			Fvector sp_ucl_ctrl_before;
			Fvector sp_ucl_ctrl_after_inherited;
			Fvector sp_ucl_ctrl_after_physics;
			Fvector sp_ucl_vel_before;
			Fvector sp_ucl_vel_after_physics;
			sp_ucl_ctrl_before = sp_ucl_pos_before;
			sp_ucl_ctrl_after_inherited = sp_ucl_pos_before;
			sp_ucl_ctrl_after_physics = sp_ucl_pos_before;
			sp_ucl_vel_before.set(0.f, 0.f, 0.f);
			sp_ucl_vel_after_physics.set(0.f, 0.f, 0.f);
			float sp_ucl_vel_actual_before = -1.f;
			float sp_ucl_vel_actual_after_physics = -1.f;
			int sp_ucl_enabled_before = -1;
			int sp_ucl_enabled_after_physics = -1;
			if (sp_ucl_control)
			{
				sp_ucl_control->GetPosition(sp_ucl_ctrl_before);
				sp_ucl_vel_before = sp_ucl_control->GetVelocity();
				sp_ucl_vel_actual_before = sp_ucl_control->GetVelocityActual();
				sp_ucl_enabled_before = sp_ucl_control->IsCharacterEnabled() ? 1 : 0;
			}
			if (remote_client_authority && getReady())
				remote_final_anim_root_killed = stalker_destroy_remote_anim_root(*this) || remote_final_anim_root_killed;
			if (remote_tpose_update && g_Alive() && getReady())
				remote_final_anim_root_killed = stalker_hold_remote_tpose(*this) || remote_final_anim_root_killed;

			START_PROFILE("stalker/client_update/inherited")
				inherited::UpdateCL();
			STOP_PROFILE

			Fvector sp_ucl_pos_after_inherited = Position();
			if (remote_tpose_update && g_Alive() && getReady())
				remote_final_anim_root_killed = stalker_hold_remote_tpose(*this) || remote_final_anim_root_killed;
			if (sp_ucl_control)
			{
				sp_ucl_control->GetPosition(sp_ucl_ctrl_after_inherited);
				anim_diag_phys_sync_ctrl_after = sp_ucl_ctrl_after_inherited;
			}
			if (remote_client_authority && sp_ucl_control && sp_ucl_control->CharacterExist())
			{
				const Fvector object_pos = Position();
				const Fvector ctrl_pos_before_sync = sp_ucl_ctrl_after_inherited;
				const float ctrl_dist_before = ctrl_pos_before_sync.distance_to(object_pos);
				anim_diag_phys_sync_dist = ctrl_dist_before;
				anim_diag_phys_sync_ctrl_before = ctrl_pos_before_sync;
				const bool ctrl_enabled_before_sync = sp_ucl_control->IsCharacterEnabled();
				if ((ctrl_dist_before > 0.05f) || ctrl_enabled_before_sync)
				{
					sp_ucl_control->SetPosition(object_pos);
					sp_ucl_control->DisableCharacter();
					sp_ucl_control->GetPosition(sp_ucl_ctrl_after_inherited);
					anim_diag_phys_sync = true;
					anim_diag_phys_sync_ctrl_after = sp_ucl_ctrl_after_inherited;

					static u32 s_sp_net_phys_sync_next = 0;
					static u32 s_sp_net_phys_sync_count = 0;
					if (sp_net_diag_allow(s_sp_net_phys_sync_next, s_sp_net_phys_sync_count, 1000, 32))
					{
						Msg("* [SP_NET][NPC_PHYS_SYNC] id=%hu name=[%s] ready=%d alive=%d enabled=%d->%d dist_before=%.3f dist_after=%.3f obj=(%.2f %.2f %.2f) ctrl_before=(%.2f %.2f %.2f) ctrl_after=(%.2f %.2f %.2f)",
							ID(), cName().c_str(), getReady() ? 1 : 0, g_Alive() ? 1 : 0, ctrl_enabled_before_sync ? 1 : 0, sp_ucl_control->IsCharacterEnabled() ? 1 : 0,
							ctrl_dist_before, sp_ucl_ctrl_after_inherited.distance_to(object_pos),
							VPUSH(object_pos), VPUSH(ctrl_pos_before_sync), VPUSH(sp_ucl_ctrl_after_inherited));
					}
				}
			}
			if (remote_update)
			{
				const float frame_dt = client_update_fdelta();
				const Fvector target_pos = Position();
				const float target_step = target_pos.distance_to(remote_pos_before);
				const float target_frame_speed = frame_dt > EPS_L ? target_step / frame_dt : 0.f;
				const Fvector& pos = Position();
				const float frame_step = pos.distance_to(remote_pos_before);
				const float frame_speed = frame_dt > EPS_L ? frame_step / frame_dt : 0.f;

				const bool has_recent_server_state =
					m_net_server_state_valid && (Device.dwTimeGlobal - m_net_server_state_time <= 1000);
				const float server_visual_speed = (has_recent_server_state && (m_net_server_speed > 0.03f)) ? clampr(m_net_server_actual_speed, 0.f, 10.f) : 0.f;
				u32 server_anim_authority = 0;
				u32 server_anim_time_applied = 0;
				u32 server_anim_missing = 0;
				u32 server_anim_mismatched = 0;
				u32 server_anim_forced = 0;
				if (g_Alive() && getReady() && has_recent_server_state)
				{
					MonsterSpace::EMovementType imported_movement_type = m_net_server_movement_type <= (u8)eMovementTypeStand ? (MonsterSpace::EMovementType)m_net_server_movement_type : eMovementTypeStand;
					MonsterSpace::EBodyState imported_body_state = m_net_server_body_state <= (u8)eBodyStateStand ? (MonsterSpace::EBodyState)m_net_server_body_state : eBodyStateStand;
					MonsterSpace::EMentalState imported_mental_state = m_net_server_mental_state <= (u8)eMentalStatePanic ? (MonsterSpace::EMentalState)m_net_server_mental_state : eMentalStateDanger;
					if ((imported_body_state == eBodyStateCrouch) && (imported_mental_state == eMentalStateFree))
						imported_mental_state = eMentalStateDanger;

					if (server_visual_speed < 0.03f)
						imported_movement_type = eMovementTypeStand;

					movement().setup_state_from_network(imported_body_state, imported_movement_type, imported_mental_state, server_visual_speed);
					movement().m_body.current.yaw = m_net_server_body_current_yaw;
					movement().m_body.target.yaw = m_net_server_body_target_yaw;
					movement().m_head.current.yaw = m_net_server_head_yaw;
					movement().m_head.target.yaw = m_net_server_head_yaw;
					movement().m_head.current.pitch = m_net_server_head_pitch;
					movement().m_head.target.pitch = m_net_server_head_pitch;
					apply_stalker_server_inventory_state(*this, m_net_server_active_slot, m_net_server_active_item_id, m_net_server_hud_state, m_net_server_hud_next_state, m_net_server_weapon_state, m_net_server_weapon_next_state, m_net_server_weapon_ammo_elapsed, m_net_server_weapon_ammo_type, m_net_server_object_handler_item_id, m_net_server_object_handler_state_id);
					if (remote_tpose_update)
					{
						remote_final_anim_root_killed = stalker_hold_remote_tpose(*this) || remote_final_anim_root_killed;
						server_anim_authority = 4;
					}
					else if (m_net_server_animation_state_valid)
					{
						u8 animation_apply_flags[kStalkerWorldStateExtAnimationTrackCount];
						for (u32 i = 0; i < kStalkerWorldStateExtAnimationTrackCount; ++i)
						{
							animation_apply_flags[i] = m_net_server_anim_flags[i];
							if ((m_net_server_anim_flags[i] & 2) && (m_net_server_anim_time_synced_motion[i] == m_net_server_anim_motion[i]))
								animation_apply_flags[i] &= ~u8(2);
						}

						animation().apply_network_animation_state(m_net_server_anim_motion, m_net_server_anim_time, animation_apply_flags, kStalkerWorldStateExtAnimationTrackCount, server_anim_time_applied, server_anim_missing, server_anim_mismatched, server_anim_forced);
						for (u32 i = 0; i < kStalkerWorldStateExtAnimationTrackCount; ++i)
						{
							if ((animation_apply_flags[i] & 2) && (m_net_server_anim_flags[i] & 1))
								m_net_server_anim_time_synced_motion[i] = m_net_server_anim_motion[i];
						}
						m_net_server_animation_state_applied_time = m_net_server_state_time;
						server_anim_authority = 1;
					}
					else
					{
						animation().reset_network_animation_state();
						server_anim_authority = 2;
					}
				}
				else if (g_Alive() && getReady())
				{
					if (remote_tpose_update)
					{
						remote_final_anim_root_killed = stalker_hold_remote_tpose(*this) || remote_final_anim_root_killed;
						server_anim_authority = 4;
					}
					else
					{
						animation().reset_network_animation_state();
						server_anim_authority = 3;
					}
				}

				if (remote_client_authority && getReady())
					remote_final_anim_root_killed = stalker_destroy_remote_anim_root(*this) || remote_final_anim_root_killed;

				anim_diag_remote_ready = true;
				anim_diag_target_step = target_step;
				anim_diag_frame_speed = frame_speed;
				anim_diag_target_frame_speed = target_frame_speed;
				anim_diag_server_visual_speed = server_visual_speed;
				anim_diag_server_anim_time_applied = server_anim_time_applied;
				anim_diag_server_anim_missing = server_anim_missing;
				anim_diag_server_anim_mismatched = server_anim_mismatched;
				anim_diag_server_anim_forced = server_anim_forced;
				anim_diag_server_anim_authority = server_anim_authority;

				static u32 s_sp_net_cl_next = 0;
				static u32 s_sp_net_cl_count = 0;
				if (sp_net_diag_allow(s_sp_net_cl_next, s_sp_net_cl_count, 1000, 16))
				{
					CPHMovementControl* movement_control = character_physics_support() ? character_physics_support()->movement() : nullptr;
					const float speed_phys = movement_control ? movement().speed(movement_control) : -1.f;
					Msg("* [SP_NET][NPC_CL] id=%hu name=[%s] ready=%d alive=%d enabled=%d net=%u/%u interp=%u sel=%d factor=%.3f tcl=%u ts=%u/%u/%u target=(%.2f %.2f %.2f) final=(%.2f %.2f %.2f) net_last=(%.2f %.2f %.2f) target_step=%.3f frame_step=%.3f frame_speed=%.3f target_speed=%.3f speed_calc=%.3f speed_phys=%.3f server_state=%d state_speed=%.3f actual_speed=%.3f visual_speed=%.3f stuck=%d slot=%u item=%u hud=%u/%u wpn=%u/%u ammo=%u type=%u zoom=%u fire=%u oh=%u/%u anim=%d anim_auth=%u anim_sync=%u/%u/%u/%u yaw_srv=%.3f/%.3f head=%.3f anim_g=%u/%u/%u anim_h=%u/%u/%u anim_t=%u/%u/%u anim_l=%u/%u/%u anim_s=%u/%u/%u move=%d body=%d mental=%d anim_ctrl=%d",
						ID(), cName().c_str(), getReady() ? 1 : 0, g_Alive() ? 1 : 0, getEnabled() ? 1 : 0, (u32)NET.size(), remote_net_count_before,
						remote_interp_mode, remote_interp_select, remote_interp_factor, remote_time_cl, remote_ts_first, remote_ts_second, remote_ts_last,
						target_pos.x, target_pos.y, target_pos.z, pos.x, pos.y, pos.z, NET_Last.p_pos.x, NET_Last.p_pos.y, NET_Last.p_pos.z,
						target_step, frame_step, frame_speed, target_frame_speed, movement().speed(), speed_phys,
						has_recent_server_state ? 1 : 0, m_net_server_speed, m_net_server_actual_speed, server_visual_speed, ((m_net_server_speed > 0.05f) && (m_net_server_actual_speed < 0.03f)) ? 1 : 0, m_net_server_active_slot, m_net_server_active_item_id, m_net_server_hud_state, m_net_server_hud_next_state, m_net_server_weapon_state, m_net_server_weapon_next_state, m_net_server_weapon_ammo_elapsed, m_net_server_weapon_ammo_type, m_net_server_weapon_zoomed, m_net_server_weapon_working, m_net_server_object_handler_item_id, m_net_server_object_handler_state_id, m_net_server_animation_state_valid ? 1 : 0, server_anim_authority, server_anim_time_applied, server_anim_missing, server_anim_mismatched, server_anim_forced, m_net_server_body_current_yaw, m_net_server_body_target_yaw, m_net_server_head_yaw, m_net_server_anim_motion[eStalkerNetAnimGlobal], m_net_server_anim_time[eStalkerNetAnimGlobal], m_net_server_anim_flags[eStalkerNetAnimGlobal], m_net_server_anim_motion[eStalkerNetAnimHead], m_net_server_anim_time[eStalkerNetAnimHead], m_net_server_anim_flags[eStalkerNetAnimHead], m_net_server_anim_motion[eStalkerNetAnimTorso], m_net_server_anim_time[eStalkerNetAnimTorso], m_net_server_anim_flags[eStalkerNetAnimTorso], m_net_server_anim_motion[eStalkerNetAnimLegs], m_net_server_anim_time[eStalkerNetAnimLegs], m_net_server_anim_flags[eStalkerNetAnimLegs], m_net_server_anim_motion[eStalkerNetAnimScript], m_net_server_anim_time[eStalkerNetAnimScript], m_net_server_anim_flags[eStalkerNetAnimScript],
						(int)movement().movement_type(), (int)movement().body_state(), (int)movement().mental_state(), animation_movement_controlled() ? 1 : 0);
				}
			}
			START_PROFILE("stalker/client_update/physics")
				m_pPhysics_support->in_UpdateCL();
			STOP_PROFILE

			if (remote_update && IIsClient() && !remote_client_authority && sp_ucl_control && sp_ucl_control->CharacterExist() && sp_ucl_control->IsCharacterEnabled())
			{
				sp_ucl_control->SetPosition(Position());
				sp_ucl_control->DisableCharacter();
			}

			Fvector sp_ucl_pos_after_physics = Position();
			if (sp_ucl_control)
			{
				sp_ucl_control->GetPosition(sp_ucl_ctrl_after_physics);
				sp_ucl_vel_after_physics = sp_ucl_control->GetVelocity();
				sp_ucl_vel_actual_after_physics = sp_ucl_control->GetVelocityActual();
				sp_ucl_enabled_after_physics = sp_ucl_control->IsCharacterEnabled() ? 1 : 0;
			}

			if (remote_client_authority && getReady())
			{
				remote_final_anim_root_killed = stalker_destroy_remote_anim_root(*this) || remote_final_anim_root_killed;

				const bool has_recent_server_state = m_net_server_state_valid && (Device.dwTimeGlobal - m_net_server_state_time <= 1000);
				remote_final_raw_net_pos.set(NET_Last.p_pos);
				if (NET.empty() && !has_recent_server_state)
					remote_final_raw_net_pos.set(Position());
				remote_final_server_pos.set(has_recent_server_state ? m_net_server_position : remote_final_raw_net_pos);
				remote_final_raw_yaw = NET_Last.o_model;
				remote_final_server_yaw = has_recent_server_state ? m_net_server_body_current_yaw : remote_final_raw_yaw;
				remote_final_yaw = remote_final_server_yaw;

				const float remote_body_target_yaw = has_recent_server_state ? m_net_server_body_target_yaw : remote_final_yaw;
				const float remote_head_yaw = has_recent_server_state ? m_net_server_head_yaw : remote_final_yaw;
				const float remote_head_pitch = has_recent_server_state ? m_net_server_head_pitch : NET_Last.o_torso.pitch;
				movement().m_body.current.yaw = remote_final_yaw;
				movement().m_body.target.yaw = remote_body_target_yaw;
				movement().m_head.current.yaw = remote_head_yaw;
				movement().m_head.target.yaw = remote_head_yaw;
				movement().m_head.current.pitch = remote_head_pitch;
				movement().m_head.target.pitch = remote_head_pitch;

				Fvector smooth_from;
				if (m_net_remote_smooth_valid)
					smooth_from.set(m_net_remote_smooth_position);
				else
					smooth_from.set(remote_pos_before);

				const float frame_dt = client_update_fdelta();
				remote_final_raw_step = remote_final_raw_net_pos.distance_to(smooth_from);
				remote_final_raw_frame_speed = frame_dt > EPS_L ? remote_final_raw_step / frame_dt : 0.f;
				remote_final_server_visual_speed = has_recent_server_state ? _max(m_net_server_speed, m_net_server_actual_speed) : 0.f;
				const float max_visual_step = _max(0.035f, (remote_final_server_visual_speed + 0.75f) * frame_dt + 0.015f);
				const bool teleport = remote_final_raw_step > _max(2.5f, remote_final_server_visual_speed * 1.5f + 1.f);

				if (teleport || !m_net_remote_smooth_valid)
				{
					remote_final_pos.set(remote_final_raw_net_pos);
					m_net_remote_smooth_valid = true;
				}
				else if (remote_final_raw_step > max_visual_step)
				{
					Fvector direction;
					direction.sub(remote_final_raw_net_pos, smooth_from);
					direction.normalize_safe();
					remote_final_pos.mad(smooth_from, direction, max_visual_step);
				}
				else
					remote_final_pos.set(remote_final_raw_net_pos);

				remote_final_step = remote_final_pos.distance_to(remote_pos_before);
				remote_final_frame_speed = frame_dt > EPS_L ? remote_final_step / frame_dt : 0.f;
				m_net_remote_smooth_position.set(remote_final_pos);
				m_net_remote_smooth_time = Device.dwTimeGlobal;

				Fmatrix remote_final_xform;
				remote_final_xform.rotateY(remote_final_yaw);
				remote_final_xform.c.set(remote_final_pos);
				XFORM().set(remote_final_xform);

				if (sp_ucl_control && sp_ucl_control->CharacterExist())
				{
					Fvector ctrl_pos_before_final;
					sp_ucl_control->GetPosition(ctrl_pos_before_final);
					const float ctrl_dist_before_final = ctrl_pos_before_final.distance_to(remote_final_pos);
					const bool ctrl_enabled_before_final = sp_ucl_control->IsCharacterEnabled();
					if ((ctrl_dist_before_final > 0.005f) || ctrl_enabled_before_final)
					{
						sp_ucl_control->SetPosition(remote_final_pos);
						sp_ucl_control->DisableCharacter();
						remote_final_phys_reapplied = true;
						anim_diag_phys_sync = anim_diag_phys_sync || (ctrl_dist_before_final > 0.035f) || ctrl_enabled_before_final;
						anim_diag_phys_sync_dist = _max(anim_diag_phys_sync_dist, ctrl_dist_before_final);
						anim_diag_phys_sync_ctrl_before = ctrl_pos_before_final;
					}
					sp_ucl_control->GetPosition(sp_ucl_ctrl_after_physics);
					anim_diag_phys_sync_ctrl_after = sp_ucl_ctrl_after_physics;
					sp_ucl_vel_after_physics = sp_ucl_control->GetVelocity();
					sp_ucl_vel_actual_after_physics = sp_ucl_control->GetVelocityActual();
					sp_ucl_enabled_after_physics = sp_ucl_control->IsCharacterEnabled() ? 1 : 0;
				}

				sp_ucl_pos_after_physics.set(Position());
				anim_diag_target_step = remote_final_raw_step;
				anim_diag_frame_speed = remote_final_frame_speed;
				anim_diag_target_frame_speed = remote_final_raw_frame_speed;
				anim_diag_server_visual_speed = remote_final_server_visual_speed;

				const bool moving_filter = (m_net_server_speed > 0.05f) || (m_net_server_actual_speed > 0.05f) || (remote_final_raw_step > 0.02f) || (remote_final_step > 0.02f);
				const bool watch_filter = stalker_anim_diag_watch_object(*this);
				const bool near_filter = (moving_filter || watch_filter) ? false : stalker_anim_diag_near_actor(*this, 80.f);
				if (moving_filter || watch_filter || near_filter)
				{
					static u32 s_sp_remote_xform_next = 0;
					static u32 s_sp_remote_xform_count = 0;
					if (stalker_rate_allow(s_sp_remote_xform_next, s_sp_remote_xform_count, 1000, 96))
					{
						const bool client_interp_jump = (remote_final_raw_step > 0.08f) && (remote_final_raw_frame_speed > _max(2.5f, remote_final_server_visual_speed * 3.f + 0.5f));
						const bool client_phys_sync = anim_diag_phys_sync && (anim_diag_phys_sync_dist > 0.035f);
						Msg("* [SP_REMOTE_XFORM] id=%hu name=[%s] sect=[%s] filter=move:%d watch:%d near:%d raw_net_pos=(%.2f %.2f %.2f) final_pos=(%.2f %.2f %.2f) server_pos=(%.2f %.2f %.2f) prev_pos=(%.2f %.2f %.2f) raw_step=%.4f final_step=%.4f raw_speed=%.3f final_speed=%.3f raw_yaw=%.3f server_body_yaw=%.3f final_yaw=%.3f sight_skipped=%d phys_reapplied=%d anim_root_killed=%d server_speed=%.3f server_actual=%.3f visual_speed=%.3f interp=%u/%d/%.3f tcl=%u ts=%u/%u/%u net=%u/%u CLIENT_INTERP_JUMP=%d CLIENT_PHYS_SYNC=%d ctrl=(%.2f %.2f %.2f)->(%.2f %.2f %.2f) enabled=%d",
							ID(), cName().c_str(), cNameSect().c_str(), moving_filter ? 1 : 0, watch_filter ? 1 : 0, near_filter ? 1 : 0,
							VPUSH(remote_final_raw_net_pos), VPUSH(remote_final_pos), VPUSH(remote_final_server_pos), VPUSH(remote_pos_before), remote_final_raw_step, remote_final_step, remote_final_raw_frame_speed, remote_final_frame_speed,
							remote_final_raw_yaw, remote_final_server_yaw, remote_final_yaw, remote_final_sight_skipped ? 1 : 0, remote_final_phys_reapplied ? 1 : 0, remote_final_anim_root_killed ? 1 : 0,
							m_net_server_speed, m_net_server_actual_speed, remote_final_server_visual_speed, remote_interp_mode, remote_interp_select, remote_interp_factor, remote_time_cl, remote_ts_first, remote_ts_second, remote_ts_last, (u32)NET.size(), remote_net_count_before,
							client_interp_jump ? 1 : 0, client_phys_sync ? 1 : 0, VPUSH(anim_diag_phys_sync_ctrl_before), VPUSH(sp_ucl_ctrl_after_physics), sp_ucl_enabled_after_physics);
					}
				}
			}
			if (remote_tpose_update && anim_diag_remote_ready)
			{
				const float inherited_step = sp_ucl_pos_before.distance_to(sp_ucl_pos_after_inherited);
				const float physics_step = sp_ucl_pos_after_inherited.distance_to(sp_ucl_pos_after_physics);
				const float frame_step = sp_ucl_pos_before.distance_to(sp_ucl_pos_after_physics);
				float server_packet_delta = -1.f;
				if (NET.size() >= 2)
					server_packet_delta = NET[NET.size() - 1].p_pos.distance_to(NET[NET.size() - 2].p_pos);

				const bool moving_filter = (m_net_server_speed > 0.05f) || (m_net_server_actual_speed > 0.05f) || (frame_step > 0.02f);
				const bool watch_filter = stalker_anim_diag_watch_object(*this);
				const bool near_filter = (moving_filter || watch_filter) ? false : stalker_anim_diag_near_actor(*this, 80.f);
				if (moving_filter || watch_filter || near_filter)
				{
					static u32 s_sp_tpose_diag_next = 0;
					static u32 s_sp_tpose_diag_count = 0;
					if (stalker_rate_allow(s_sp_tpose_diag_next, s_sp_tpose_diag_count, 1000, 48))
					{
						float visual_yaw = 0.f;
						float visual_pitch = 0.f;
						float visual_bank = 0.f;
						XFORM().getHPB(visual_yaw, visual_pitch, visual_bank);
						Msg("* [SP_TPOSE_DIAG] id=%hu name=[%s] sect=[%s] filter=move:%d watch:%d near:%d pos=(%.2f %.2f %.2f)->(%.2f %.2f %.2f)->(%.2f %.2f %.2f) frame_step=%.4f inherited_step=%.4f physics_step=%.4f server_delta=%.4f yaw=%.3f target=%.3f head=%.3f visual=%.3f net_last=%.3f speed=%.3f actual=%.3f interp=%u/%d/%.3f tcl=%u ts=%u/%u/%u net=%u/%u phys_sync=%d dist=%.3f ctrl=(%.2f %.2f %.2f)->(%.2f %.2f %.2f)->(%.2f %.2f %.2f) anim_valid=%d anim_auth=%u",
							ID(), cName().c_str(), cNameSect().c_str(), moving_filter ? 1 : 0, watch_filter ? 1 : 0, near_filter ? 1 : 0,
							VPUSH(sp_ucl_pos_before), VPUSH(sp_ucl_pos_after_inherited), VPUSH(sp_ucl_pos_after_physics), frame_step, inherited_step, physics_step, server_packet_delta,
							m_net_server_body_current_yaw, m_net_server_body_target_yaw, m_net_server_head_yaw, visual_yaw, NET_Last.o_model, m_net_server_speed, m_net_server_actual_speed,
							remote_interp_mode, remote_interp_select, remote_interp_factor, remote_time_cl, remote_ts_first, remote_ts_second, remote_ts_last, (u32)NET.size(), remote_net_count_before,
							anim_diag_phys_sync ? 1 : 0, anim_diag_phys_sync_dist, VPUSH(anim_diag_phys_sync_ctrl_before), VPUSH(anim_diag_phys_sync_ctrl_after), VPUSH(sp_ucl_ctrl_after_physics),
							m_net_server_animation_state_valid ? 1 : 0, anim_diag_server_anim_authority);
					}
				}
			}

			if (remote_update && anim_diag_remote_ready && stalker_anim_diag_enabled())
			{
				const bool has_recent_diag_state = m_net_server_state_valid && (Device.dwTimeGlobal - m_net_server_state_time <= 1000);
				const float server_yaw_delta = m_net_anim_diag_valid ? stalker_anim_diag_abs_yaw_delta(m_net_server_body_current_yaw, m_net_anim_diag_last_server_yaw) : 0.f;
				const float server_target_yaw_delta = m_net_anim_diag_valid ? stalker_anim_diag_abs_yaw_delta(m_net_server_body_target_yaw, m_net_anim_diag_last_server_target_yaw) : 0.f;
				const float server_pos_delta = m_net_anim_diag_valid ? m_net_server_position.distance_to(m_net_anim_diag_last_server_pos) : 0.f;
				bool server_move_yaw_valid = false;
				bool visual_move_yaw_valid = false;
				const float server_move_yaw = m_net_anim_diag_valid ? stalker_anim_diag_yaw_from_delta(m_net_anim_diag_last_server_pos, m_net_server_position, server_move_yaw_valid) : 0.f;
				const float visual_move_yaw = stalker_anim_diag_yaw_from_delta(remote_pos_before, Position(), visual_move_yaw_valid);
				const float server_move_body_delta = server_move_yaw_valid ? stalker_anim_diag_abs_yaw_delta(m_net_server_body_current_yaw, server_move_yaw) : 0.f;
				const float server_move_target_delta = server_move_yaw_valid ? stalker_anim_diag_abs_yaw_delta(m_net_server_body_target_yaw, server_move_yaw) : 0.f;
				const float visual_move_body_delta = visual_move_yaw_valid ? stalker_anim_diag_abs_yaw_delta(m_net_server_body_current_yaw, visual_move_yaw) : 0.f;
				const float visual_vs_server_move_delta = (server_move_yaw_valid && visual_move_yaw_valid) ? stalker_anim_diag_abs_yaw_delta(server_move_yaw, visual_move_yaw) : 0.f;
				bool anim_changed = false;
				for (u32 i = eStalkerNetAnimHead; i <= eStalkerNetAnimScript; ++i)
					anim_changed = anim_changed || ((m_net_server_anim_flags[i] & 1) && (m_net_server_anim_motion[i] != m_net_anim_diag_last_motion[i]));

				const bool event_server_stuck = has_recent_diag_state && (m_net_server_speed > 0.05f) && (m_net_server_actual_speed < 0.03f);
				const bool event_server_yaw_snap = m_net_anim_diag_valid && (m_net_server_update_timestamp != m_net_anim_diag_last_server_timestamp) && (_max(server_yaw_delta, server_target_yaw_delta) > 0.65f);
				const bool event_server_turn_in_place = has_recent_diag_state && m_net_anim_diag_valid && (m_net_server_speed > 0.05f) && (server_pos_delta < 0.04f) && (_max(server_yaw_delta, server_target_yaw_delta) > 0.25f);
				const bool event_server_sideways_turn = has_recent_diag_state && server_move_yaw_valid && (m_net_server_speed > 0.05f) && (server_pos_delta > 0.035f) && (_min(server_move_body_delta, server_move_target_delta) > 1.05f);
				const bool event_server_backstep_turn = event_server_sideways_turn && (_min(server_move_body_delta, server_move_target_delta) > 2.35f);
				const bool event_client_interp_jump = (anim_diag_target_step > 0.08f) && (anim_diag_frame_speed > _max(2.5f, anim_diag_server_visual_speed * 3.f + 0.5f));
				const bool event_client_turn_mismatch = (server_move_yaw_valid && visual_move_yaw_valid && (visual_vs_server_move_delta > 0.70f)) || (visual_move_yaw_valid && (visual_move_body_delta > 1.05f) && (!server_move_yaw_valid || (server_move_body_delta < 0.70f)));
				const bool event_client_phys_sync = anim_diag_phys_sync && (anim_diag_phys_sync_dist > 0.035f);
				const bool event_anim_track_suspect = m_net_anim_diag_valid && m_net_server_animation_state_valid && (((anim_diag_server_anim_forced + anim_diag_server_anim_mismatched) > 1) || (anim_changed && (anim_diag_frame_speed < 0.15f) && (m_net_server_speed < 0.05f)));
				const bool has_event = event_server_stuck || event_server_yaw_snap || event_server_turn_in_place || event_server_sideways_turn || event_server_backstep_turn || event_client_interp_jump || event_client_turn_mismatch || event_client_phys_sync || event_anim_track_suspect;
				const bool moving_filter = (m_net_server_speed > 0.05f) || (m_net_server_actual_speed > 0.05f) || (anim_diag_frame_speed > 0.05f);
				const bool watch_filter = stalker_anim_diag_watch_object(*this);
				const bool near_filter = (moving_filter || watch_filter) ? false : stalker_anim_diag_near_actor(*this, 80.f);
				const bool pass_filter = moving_filter || watch_filter || near_filter;
				const bool pass_rate = !m_net_anim_diag_valid || !m_net_anim_diag_last_log_time || (Device.dwTimeGlobal - m_net_anim_diag_last_log_time >= 500);

				if (has_event && pass_filter && pass_rate)
				{
					string128 anim_head_name;
					string128 anim_torso_name;
					string128 anim_legs_name;
					string128 anim_script_name;
					stalker_anim_diag_motion_name(*this, m_net_server_anim_motion[eStalkerNetAnimHead], anim_head_name);
					stalker_anim_diag_motion_name(*this, m_net_server_anim_motion[eStalkerNetAnimTorso], anim_torso_name);
					stalker_anim_diag_motion_name(*this, m_net_server_anim_motion[eStalkerNetAnimLegs], anim_legs_name);
					stalker_anim_diag_motion_name(*this, m_net_server_anim_motion[eStalkerNetAnimScript], anim_script_name);
					Msg("* [SP_ANIM_DIAG_EVENT] id=%hu name=[%s] sect=[%s] events=SERVER_STUCK:%d SERVER_YAW_SNAP:%d SERVER_TURN_IN_PLACE:%d SERVER_SIDEWAYS_TURN:%d SERVER_BACKSTEP_TURN:%d CLIENT_INTERP_JUMP:%d CLIENT_TURN_MISMATCH:%d CLIENT_PHYS_SYNC:%d ANIM_TRACK_SUSPECT:%d filter=move:%d watch:%d near:%d ts=%u->%u pos_delta=%.3f yaw_delta=%.3f/%.3f move_yaw=%.3f/%d visual_yaw=%.3f/%d move_body_delta=%.3f move_target_delta=%.3f visual_body_delta=%.3f visual_vs_server=%.3f state=%u/%u/%u speed=%.3f actual=%.3f visual=%.3f interp=%u/%d/%.3f target_step=%.3f frame_speed=%.3f target_frame_speed=%.3f phys_sync=%d dist=%.3f ctrl=(%.2f %.2f %.2f)->(%.2f %.2f %.2f)->(%.2f %.2f %.2f) obj=(%.2f %.2f %.2f) srv_pos=(%.2f %.2f %.2f) anim_auth=%u anim_sync=%u/%u/%u/%u changed=%d anim_h=%u/%u/%u[%s] anim_t=%u/%u/%u[%s] anim_l=%u/%u/%u[%s] anim_s=%u/%u/%u[%s]",
						ID(), cName().c_str(), cNameSect().c_str(), event_server_stuck ? 1 : 0, event_server_yaw_snap ? 1 : 0, event_server_turn_in_place ? 1 : 0, event_server_sideways_turn ? 1 : 0, event_server_backstep_turn ? 1 : 0, event_client_interp_jump ? 1 : 0, event_client_turn_mismatch ? 1 : 0, event_client_phys_sync ? 1 : 0, event_anim_track_suspect ? 1 : 0,
						moving_filter ? 1 : 0, watch_filter ? 1 : 0, near_filter ? 1 : 0, m_net_anim_diag_last_server_timestamp, m_net_server_update_timestamp, server_pos_delta, server_yaw_delta, server_target_yaw_delta,
						server_move_yaw, server_move_yaw_valid ? 1 : 0, visual_move_yaw, visual_move_yaw_valid ? 1 : 0, server_move_body_delta, server_move_target_delta, visual_move_body_delta, visual_vs_server_move_delta,
						m_net_server_movement_type, m_net_server_body_state, m_net_server_mental_state, m_net_server_speed, m_net_server_actual_speed, anim_diag_server_visual_speed,
						remote_interp_mode, remote_interp_select, remote_interp_factor, anim_diag_target_step, anim_diag_frame_speed, anim_diag_target_frame_speed,
						anim_diag_phys_sync ? 1 : 0, anim_diag_phys_sync_dist, VPUSH(anim_diag_phys_sync_ctrl_before), VPUSH(anim_diag_phys_sync_ctrl_after), VPUSH(sp_ucl_ctrl_after_physics), VPUSH(Position()), VPUSH(m_net_server_position),
						anim_diag_server_anim_authority, anim_diag_server_anim_time_applied, anim_diag_server_anim_missing, anim_diag_server_anim_mismatched, anim_diag_server_anim_forced, anim_changed ? 1 : 0,
						m_net_server_anim_motion[eStalkerNetAnimHead], m_net_server_anim_time[eStalkerNetAnimHead], m_net_server_anim_flags[eStalkerNetAnimHead], anim_head_name,
						m_net_server_anim_motion[eStalkerNetAnimTorso], m_net_server_anim_time[eStalkerNetAnimTorso], m_net_server_anim_flags[eStalkerNetAnimTorso], anim_torso_name,
						m_net_server_anim_motion[eStalkerNetAnimLegs], m_net_server_anim_time[eStalkerNetAnimLegs], m_net_server_anim_flags[eStalkerNetAnimLegs], anim_legs_name,
						m_net_server_anim_motion[eStalkerNetAnimScript], m_net_server_anim_time[eStalkerNetAnimScript], m_net_server_anim_flags[eStalkerNetAnimScript], anim_script_name);
					m_net_anim_diag_last_log_time = Device.dwTimeGlobal;
				}

				m_net_anim_diag_valid = true;
				m_net_anim_diag_last_server_timestamp = m_net_server_update_timestamp;
				m_net_anim_diag_last_server_pos.set(m_net_server_position);
				m_net_anim_diag_last_server_yaw = m_net_server_body_current_yaw;
				m_net_anim_diag_last_server_target_yaw = m_net_server_body_target_yaw;
				m_net_anim_diag_last_server_speed = m_net_server_speed;
				m_net_anim_diag_last_server_actual_speed = m_net_server_actual_speed;
				for (u32 i = 0; i < kStalkerWorldStateExtAnimationTrackCount; ++i)
					m_net_anim_diag_last_motion[i] = m_net_server_anim_motion[i];
			}

			static u32 s_sp_ucl_next = 0;
			static u32 s_sp_ucl_count = 0;
			if (sp_net_diag_allow(s_sp_ucl_next, s_sp_ucl_count, 1000, 128))
			{
				const float inherited_step = sp_ucl_pos_before.distance_to(sp_ucl_pos_after_inherited);
				const float physics_step = sp_ucl_pos_after_inherited.distance_to(sp_ucl_pos_after_physics);
				const float frame_step = sp_ucl_pos_before.distance_to(sp_ucl_pos_after_physics);
				const float ctrl_inherited_step = sp_ucl_ctrl_before.distance_to(sp_ucl_ctrl_after_inherited);
				const float ctrl_physics_step = sp_ucl_ctrl_after_inherited.distance_to(sp_ucl_ctrl_after_physics);
				const u32 detail_size = (u32)movement().detail().path().size();
				const u32 detail_index = detail_size ? movement().detail().curr_travel_point_index() : u32(-1);

				Msg("* [SP_UCL] id=%hu name=[%s] sect=[%s] remote=%d local=%d enabled=%d->%d pos=(%.2f %.2f %.2f)->(%.2f %.2f %.2f)->(%.2f %.2f %.2f) inherited_step=%.4f physics_step=%.4f frame_step=%.4f ctrl=(%.2f %.2f %.2f)->(%.2f %.2f %.2f)->(%.2f %.2f %.2f) ctrl_inherited_step=%.4f ctrl_physics_step=%.4f vel=(%.3f %.3f %.3f)->(%.3f %.3f %.3f) vel_actual=%.3f->%.3f state_speed=%.3f detail_size=%u detail_idx=%u move=%d body=%d mental=%d withactor=%d actorproxy=%d",
					ID(), cName().c_str(), cNameSect().c_str(), Remote() ? 1 : 0, Local() ? 1 : 0, sp_ucl_enabled_before, sp_ucl_enabled_after_physics,
					VPUSH(sp_ucl_pos_before), VPUSH(sp_ucl_pos_after_inherited), VPUSH(sp_ucl_pos_after_physics), inherited_step, physics_step, frame_step,
					VPUSH(sp_ucl_ctrl_before), VPUSH(sp_ucl_ctrl_after_inherited), VPUSH(sp_ucl_ctrl_after_physics), ctrl_inherited_step, ctrl_physics_step,
					VPUSH(sp_ucl_vel_before), VPUSH(sp_ucl_vel_after_physics), sp_ucl_vel_actual_before, sp_ucl_vel_actual_after_physics,
					movement().speed(), detail_size, detail_index, (int)movement().movement_type(), (int)movement().body_state(), (int)movement().mental_state(), stalker_net_has_param("-withactor") ? 1 : 0, 0);
			}
			if (g_Alive())
			{
				if (!remote_client_authority)
				{
					START_PROFILE("stalker/client_update/sight_manager")
						VERIFY(!m_pPhysicsShell);
						try
						{
							sight().update();
						}
						catch (...)
						{
							sight().setup(CSightAction(SightManager::eSightTypeCurrentDirection));
							sight().update();
						}

						Exec_Look(client_update_fdelta());
					STOP_PROFILE
				}

				START_PROFILE("stalker/client_update/step_manager")
					CStepManager::update(false);
				STOP_PROFILE

				START_PROFILE("stalker/client_update/weapon_shot_effector")
					if (weapon_shot_effector().IsActive())
						weapon_shot_effector().Update();
				STOP_PROFILE
			}
#ifdef DEBUG
	debug_text	();
#endif
		STOP_PROFILE
	STOP_PROFILE
}
void CAI_Stalker::PHHit(SHit& H)
{
	m_pPhysics_support->in_Hit(H, false);
}

CPHDestroyable* CAI_Stalker::ph_destroyable()
{
	return smart_cast<CPHDestroyable*>(character_physics_support());
}

#include "../../enemy_manager.h"

void CAI_Stalker::shedule_Update(u32 DT)
{
	// Optimization update
//	if (Device.dwFrame % 2) return;

	START_PROFILE("stalker")
		START_PROFILE("stalker/schedule_update")
			VERIFY2(getEnabled()||PPhysicsShell(), *cName());

			if (!CObjectHandler::planner().initialized())
			{
				START_PROFILE("stalker/client_update/object_handler")
					update_object_handler();
				STOP_PROFILE
			}
			//	if (Position().distance_to(Level().CurrentEntity()->Position()) <= 50.f)
			//		Msg				("[%6d][SH][%s]",Device.dwTimeGlobal,*cName());
			// Queue shrink
			VERIFY(_valid(Position()));
			u32 dwTimeCL = Level().timeServer() - NET_Latency;
			VERIFY(!NET.empty());
			while ((NET.size() > 2) && (NET[1].dwTimeStamp < dwTimeCL)) NET.pop_front();

			Fvector vNewPosition = Position();
			VERIFY(_valid(Position()));
			// *** general stuff
			float dt = float(DT) / 1000.f;

			if (g_Alive())
			{
				animation().play_delayed_callbacks();

#ifndef USE_SCHEDULER_IN_AGENT_MANAGER
				agent_manager().update();
#endif // USE_SCHEDULER_IN_AGENT_MANAGER

				//		bool			check = !!memory().enemy().selected();
#if 0//def DEBUG
		memory().visual().check_visibles();
#endif
				static DWORD this_thread_id = GetCurrentThreadId();
				Device.parallel_render_tasks.run([this]()
				{
					if (this_thread_id != GetCurrentThreadId()) { PROF_THREAD("X-Ray PPL Thread") }
					Core.InitializeCOM();
					//CTimer T;
					//T.Start();
					update_can_kill_info();
					Exec_Visibility();
					if (character_physics_support())
						character_physics_support()->movement()->update_last_material();
					//if (T.GetElapsed_ms() > 1)
					//{
					//	Msg("! Exec_Visibility and update_can_kill_info");
					//	Msg("!Error in visual %s %s %s %s", Name(), *cNameSect(), *cName(), *cNameVisual());
					//	Msg("Position %f %f %f", VPUSH(Position()));
					//	Msg("XFORM().k %f %f %f", VPUSH(XFORM().k));
					//	Msg("XFORM().i %f %f %f", VPUSH(XFORM().i));
					//	Msg("XFORM().j %f %f %f", VPUSH(XFORM().j));
					//	Msg("XFORM().c %f %f %f", VPUSH(XFORM().c));
					//	Msg("Radius %f", Radius());
					//}
				});

				CScriptEntity::process_sound_callbacks();

				START_PROFILE("stalker/schedule_update/memory")
					START_PROFILE("stalker/schedule_update/memory/process")
						process_enemies();
					STOP_PROFILE

					START_PROFILE("stalker/schedule_update/memory/update")
						memory().update(dt);
					STOP_PROFILE

				STOP_PROFILE
			}

			START_PROFILE("stalker/schedule_update/inherited")
				inherited::inherited::shedule_Update(DT);
			STOP_PROFILE

			if (Remote())
			{
			}
			else
			{
				// here is monster AI call
				VERIFY(_valid(Position()));
				m_fTimeUpdateDelta = dt;
				Device.Statistic->AI_Think.Begin();
				if (GetScriptControl())
					ProcessScripts();
				else
#ifdef DEBUG
			if (Device.dwFrame > (spawn_time() + g_AI_inactive_time))
#endif
					Think();
				m_dwLastUpdateTime = Device.dwTimeGlobal;
				Device.Statistic->AI_Think.End();
				VERIFY(_valid(Position()));

				// Look and action streams
				float temp = conditions().health();
				if (temp > 0)
				{
					START_PROFILE("stalker/schedule_update/feel_touch")
						Fvector C;
						float R;
						Center(C);
						R = Radius();
						feel_touch_update(C, R);
					STOP_PROFILE

					START_PROFILE("stalker/schedule_update/net_update")
						net_update uNext;
						uNext.dwTimeStamp = Level().timeServer();
						uNext.o_model = movement().m_body.current.yaw;
						uNext.o_torso = movement().m_head.current;
						uNext.p_pos = vNewPosition;
						uNext.fHealth = GetfHealth();
						NET.push_back(uNext);
						static u32 s_sp_net_push_next = 0;
						static u32 s_sp_net_push_count = 0;
						if (sp_net_diag_allow(s_sp_net_push_next, s_sp_net_push_count, 1000, 16))
						{
							const Fvector& current_pos = Position();
							Msg("* [SP_NET][NPC_PUSH] id=%hu name=[%s] sect=[%s] local=%d ready=%d alive=%d dt=%u net=%u ts=%u cur=(%.2f %.2f %.2f) queued=(%.2f %.2f %.2f) yaw=%.3f health=%.3f",
				ID(), cName().c_str(), cNameSect().c_str(), Local() ? 1 : 0, getReady() ? 1 : 0, g_Alive() ? 1 : 0, DT,
				(u32)NET.size(), uNext.dwTimeStamp, current_pos.x, current_pos.y, current_pos.z,
				uNext.p_pos.x, uNext.p_pos.y, uNext.p_pos.z, uNext.o_model, uNext.fHealth);
						}
					STOP_PROFILE
				}
				else
				{
					START_PROFILE("stalker/schedule_update/net_update")
						net_update uNext;
						uNext.dwTimeStamp = Level().timeServer();
						uNext.o_model = movement().m_body.current.yaw;
						uNext.o_torso = movement().m_head.current;
						uNext.p_pos = vNewPosition;
						uNext.fHealth = GetfHealth();
						NET.push_back(uNext);
						static u32 s_sp_net_push_next = 0;
						static u32 s_sp_net_push_count = 0;
						if (sp_net_diag_allow(s_sp_net_push_next, s_sp_net_push_count, 1000, 16))
						{
							const Fvector& current_pos = Position();
							Msg("* [SP_NET][NPC_PUSH] id=%hu name=[%s] sect=[%s] local=%d ready=%d alive=%d dt=%u net=%u ts=%u cur=(%.2f %.2f %.2f) queued=(%.2f %.2f %.2f) yaw=%.3f health=%.3f",
				ID(), cName().c_str(), cNameSect().c_str(), Local() ? 1 : 0, getReady() ? 1 : 0, g_Alive() ? 1 : 0, DT,
				(u32)NET.size(), uNext.dwTimeStamp, current_pos.x, current_pos.y, current_pos.z,
				uNext.p_pos.x, uNext.p_pos.y, uNext.p_pos.z, uNext.o_model, uNext.fHealth);
						}
					STOP_PROFILE
				}
			}
			VERIFY(_valid(Position()));

			START_PROFILE("stalker/schedule_update/inventory_owner")
				UpdateInventoryOwner(DT);
			STOP_PROFILE

			//#ifdef DEBUG
			//	if (psAI_Flags.test(aiALife)) {
			//		smart_cast<CSE_ALifeHumanStalker*>(ai().alife().objects().object(ID()))->check_inventory_consistency();
			//	}
			//#endif

			START_PROFILE("stalker/schedule_update/physics")
				VERIFY(_valid(Position()));
				const Fvector sp_phys_obj_before = Position();
				CPHMovementControl* sp_phys_control = character_physics_support() ? character_physics_support()->movement() : nullptr;
				Fvector sp_phys_ctrl_pos_before;
				Fvector sp_phys_ctrl_pos_after;
				Fvector sp_phys_char_pos_before;
				Fvector sp_phys_char_pos_after;
				Fvector sp_phys_vel_before;
				Fvector sp_phys_vel_after;
				sp_phys_ctrl_pos_before = sp_phys_obj_before;
				sp_phys_ctrl_pos_after = sp_phys_obj_before;
				sp_phys_char_pos_before = sp_phys_obj_before;
				sp_phys_char_pos_after = sp_phys_obj_before;
				sp_phys_vel_before.set(0.f, 0.f, 0.f);
				sp_phys_vel_after.set(0.f, 0.f, 0.f);
				float sp_phys_vel_actual_before = -1.f;
				float sp_phys_vel_actual_after = -1.f;
				float sp_phys_vel_mag_before = -1.f;
				float sp_phys_vel_mag_after = -1.f;
				int sp_phys_enabled_before = -1;
				int sp_phys_enabled_after = -1;
				if (sp_phys_control)
				{
					sp_phys_control->GetPosition(sp_phys_ctrl_pos_before);
					sp_phys_control->GetCharacterPosition(sp_phys_char_pos_before);
					sp_phys_vel_before = sp_phys_control->GetVelocity();
					sp_phys_vel_actual_before = sp_phys_control->GetVelocityActual();
					sp_phys_vel_mag_before = sp_phys_control->GetVelocityMagnitude();
					sp_phys_enabled_before = sp_phys_control->IsCharacterEnabled() ? 1 : 0;
				}
				m_pPhysics_support->in_shedule_Update(DT);
				const Fvector sp_phys_obj_after = Position();
				if (sp_phys_control)
				{
					sp_phys_control->GetPosition(sp_phys_ctrl_pos_after);
					sp_phys_control->GetCharacterPosition(sp_phys_char_pos_after);
					sp_phys_vel_after = sp_phys_control->GetVelocity();
					sp_phys_vel_actual_after = sp_phys_control->GetVelocityActual();
					sp_phys_vel_mag_after = sp_phys_control->GetVelocityMagnitude();
					sp_phys_enabled_after = sp_phys_control->IsCharacterEnabled() ? 1 : 0;
				}
				static u32 s_sp_phys_next = 0;
				static u32 s_sp_phys_count = 0;
				if (sp_net_diag_allow(s_sp_phys_next, s_sp_phys_count, 1000, 512))
				{
					const float obj_step = sp_phys_obj_before.distance_to(sp_phys_obj_after);
					const float ctrl_step = sp_phys_ctrl_pos_before.distance_to(sp_phys_ctrl_pos_after);
					const float char_step = sp_phys_char_pos_before.distance_to(sp_phys_char_pos_after);
					const u32 detail_size = (u32)movement().detail().path().size();
					const u32 detail_index = detail_size ? movement().detail().curr_travel_point_index() : u32(-1);
					const int stuck_obj = ((movement().speed() > 0.05f) && (obj_step < 0.002f)) ? 1 : 0;
					SStalkerMoveDiag sp_phys_move_diag;
					fill_stalker_move_diag(*this, sp_phys_move_diag);

                    Msg("* [SP_PHYS] dedicated=%d server=%d id=%hu name=[%s] sect=[%s] dt=%u enabled=%d->%d pos=(%.2f %.2f %.2f)->(%.2f %.2f %.2f) obj_step=%.4f ctrl=(%.2f %.2f %.2f)->(%.2f %.2f %.2f) ctrl_step=%.4f char=(%.2f %.2f %.2f)->(%.2f %.2f %.2f) char_step=%.4f vel=(%.3f %.3f %.3f)->(%.3f %.3f %.3f) vel_mag=%.3f->%.3f vel_actual=%.3f->%.3f state_speed=%.3f stuck_obj=%d detail_size=%u detail_idx=%u path_target_idx=%u path_target=(%.2f %.2f %.2f) path_target_dist=%.3f near_stalker=%hu near_dist=%.3f near_name=[%s] global_actor=%hu global_actor_dist=%.3f global_actor_name=[%s] nearest_actor=%hu nearest_actor_dist=%.3f nearest_actor_name=[%s] move=%d body=%d mental=%d withactor=%d actorproxy=%d",
						g_dedicated_server ? 1 : 0, IIsServer() ? 1 : 0, ID(), cName().c_str(), cNameSect().c_str(), DT, sp_phys_enabled_before, sp_phys_enabled_after,
						VPUSH(sp_phys_obj_before), VPUSH(sp_phys_obj_after), obj_step,
						VPUSH(sp_phys_ctrl_pos_before), VPUSH(sp_phys_ctrl_pos_after), ctrl_step,
						VPUSH(sp_phys_char_pos_before), VPUSH(sp_phys_char_pos_after), char_step,
						VPUSH(sp_phys_vel_before), VPUSH(sp_phys_vel_after), sp_phys_vel_mag_before, sp_phys_vel_mag_after,
						sp_phys_vel_actual_before, sp_phys_vel_actual_after, movement().speed(), stuck_obj,
						detail_size, detail_index, sp_phys_move_diag.path_target_index, VPUSH(sp_phys_move_diag.path_target), sp_phys_move_diag.path_target_dist, sp_phys_move_diag.nearest_stalker_id, sp_phys_move_diag.nearest_stalker_dist, sp_phys_move_diag.nearest_stalker_name, sp_phys_move_diag.global_actor_id, sp_phys_move_diag.global_actor_dist, sp_phys_move_diag.global_actor_name, sp_phys_move_diag.nearest_actor_id, sp_phys_move_diag.nearest_actor_dist, sp_phys_move_diag.nearest_actor_name, (int)movement().movement_type(), (int)movement().body_state(), (int)movement().mental_state(), stalker_net_has_param("-withactor") ? 1 : 0, 0);
				}
				VERIFY(_valid(Position()));
			STOP_PROFILE
		STOP_PROFILE
	STOP_PROFILE
}

float CAI_Stalker::Radius() const
{
	float R = inherited::Radius();
	CWeapon* W = inventory().ActiveItem() ? inventory().ActiveItem()->cast_weapon() : NULL;
	if (W) R += W->Radius();
	return R;
}

void CAI_Stalker::spawn_supplies()
{
	inherited::spawn_supplies();
	CObjectHandler::spawn_supplies();
}

void CAI_Stalker::Think()
{
	START_PROFILE("stalker/schedule_update/think")
		u32 update_delta = Device.dwTimeGlobal - m_dwLastUpdateTime;

		START_PROFILE("stalker/schedule_update/think/brain")
			//	try {
			//		try {
			TRY
			brain().update(update_delta);
			CATCH
			//		}
#ifdef DEBUG
			//		catch (luabind::cast_failed &message) {
			//			Msg						("! Expression \"%s\" from luabind::object to %s",message.what(),message.info()->name());
			//throw;
			//		}
#endif
			//		catch (std::exception &message) {
			//			Msg						("! Expression \"%s\"",message.what());
			//			throw;
			//		}
			//		catch (...) {
			//			Msg						("! unknown exception occured");
			//			throw;
			//		}
			//	}
			//	catch(...) {
#ifdef DEBUG
			//		Msg						("! Last action being executed : %s",brain().current_action().m_action_name);
#endif
			//		brain().setup			(this);
			//		brain().update			(update_delta);
			//	}
		STOP_PROFILE

		START_PROFILE("stalker/schedule_update/think/movement")
			if (!g_Alive())
				return;

			//	try {
			movement().update(update_delta);
			static u32 s_sp_move_next = 0;
			static u32 s_sp_move_count = 0;
			if (sp_net_diag_allow(s_sp_move_next, s_sp_move_count, 1000, 512))
			{
				CPHMovementControl* movement_control = character_physics_support() ? character_physics_support()->movement() : nullptr;
				const float phys_speed = movement_control ? movement().speed(movement_control) : -1.f;
				const u32 detail_size = (u32)movement().detail().path().size();
				const u32 detail_index = detail_size ? movement().detail().curr_travel_point_index() : u32(-1);
				const Fvector& detail_direction = movement().detail().direction();
				SStalkerMoveDiag sp_move_diag;
				fill_stalker_move_diag(*this, sp_move_diag);

                Msg("* [SP_MOVE] dedicated=%d server=%d id=%hu name=[%s] sect=[%s] dt=%u upd=%u pos=(%.2f %.2f %.2f) lv=%u gv=%u move=%d body=%d mental=%d state_speed=%.3f phys_speed=%.3f stuck=%d detail_size=%u detail_idx=%u dir=(%.2f %.2f %.2f) path_target_idx=%u path_target=(%.2f %.2f %.2f) path_target_dist=%.3f near_stalker=%hu near_dist=%.3f near_name=[%s] global_actor=%hu global_actor_dist=%.3f global_actor_name=[%s] nearest_actor=%hu nearest_actor_dist=%.3f nearest_actor_name=[%s] withactor=%d actorproxy=%d",
					g_dedicated_server ? 1 : 0, IIsServer() ? 1 : 0, ID(), cName().c_str(), cNameSect().c_str(), update_delta, update_delta, VPUSH(Position()),
					ai_location().level_vertex_id(), (u32)ai_location().game_vertex_id(),
					(int)movement().movement_type(), (int)movement().body_state(), (int)movement().mental_state(),
					movement().speed(), phys_speed, ((movement().speed() > 0.05f) && (phys_speed >= 0.f) && (phys_speed < 0.03f)) ? 1 : 0,
					detail_size, detail_index, VPUSH(detail_direction), sp_move_diag.path_target_index, VPUSH(sp_move_diag.path_target), sp_move_diag.path_target_dist, sp_move_diag.nearest_stalker_id, sp_move_diag.nearest_stalker_dist, sp_move_diag.nearest_stalker_name, sp_move_diag.global_actor_id, sp_move_diag.global_actor_dist, sp_move_diag.global_actor_name, sp_move_diag.nearest_actor_id, sp_move_diag.nearest_actor_dist, sp_move_diag.nearest_actor_name, stalker_net_has_param("-withactor") ? 1 : 0, 0);
			}
			//	}
#if 0//def DEBUG
	catch (luabind::cast_failed &message) {
		Msg						("! Expression \"%s\" from luabind::object to %s",message.what(),message.info()->name());
		movement().initialize	();
		movement().update		(update_delta);
		throw;
	}
	catch (std::exception &message) {
		Msg						("! Expression \"%s\"",message.what());
		movement().initialize	();
		movement().update		(update_delta);
		throw;
	}
	catch (...) {
		Msg						("! unknown exception occured");
		movement().initialize	();
		movement().update		(update_delta);
		throw;
	}
#endif // DEBUG

		STOP_PROFILE
	STOP_PROFILE
}

void CAI_Stalker::SelectAnimation(const Fvector& view, const Fvector& move, float speed)
{
	if (Remote() && stalker_remote_tpose_enabled())
		return;

	if (!Device.Paused())
		animation().update();
}

const SRotation CAI_Stalker::Orientation() const
{
	return (movement().m_head.current);
}

const MonsterSpace::SBoneRotation& CAI_Stalker::head_orientation() const
{
	return (movement().head_orientation());
}

void CAI_Stalker::net_Relcase(CObject* O)
{
	inherited::net_Relcase(O);

	sight().remove_links(O);
	movement().remove_links(O);

	if (!g_Alive())
		return;

	if (Level().seniority_holder().team(g_Team()).squad(g_Squad()).group(g_Group()).has_agent_manager())
		agent_manager().remove_links(O);
	m_pPhysics_support->in_NetRelcase(O);
}

CMovementManager* CAI_Stalker::create_movement_manager()
{
	return (m_movement_manager = xr_new<stalker_movement_manager_smart_cover>(this));
}

CSound_UserDataVisitor* CAI_Stalker::create_sound_visitor()
{
	return (m_sound_user_data_visitor = xr_new<CStalkerSoundDataVisitor>(this));
}

CMemoryManager* CAI_Stalker::create_memory_manager()
{
	return (xr_new<CMemoryManager>(this, create_sound_visitor()));
}

DLL_Pure* CAI_Stalker::_construct()
{
#ifdef DEBUG_MEMORY_MANAGER
	size_t					start = 0;
	if (g_bMEMO)
		start							= Memory.mem_usage();
#endif // DEBUG_MEMORY_MANAGER

	m_pPhysics_support = xr_new<CCharacterPhysicsSupport>(CCharacterPhysicsSupport::etStalker, this);
	CCustomMonster::_construct();
	CObjectHandler::_construct();
	CStepManager::_construct();


	m_actor_relation_flags.zero();
	m_animation_manager = xr_new<CStalkerAnimationManager>(this);
	m_brain = xr_new<CStalkerPlanner>();
	m_sight_manager = xr_new<CSightManager>(this);
	m_weapon_shot_effector = xr_new<CWeaponShotEffector>();

#ifdef DEBUG_MEMORY_MANAGER
	if (g_bMEMO)
		Msg				("CAI_Stalker::_construct() : %lld",Memory.mem_usage() - start);
#endif // DEBUG_MEMORY_MANAGER

	return (this);
}

bool CAI_Stalker::use_center_to_aim() const
{
	return (!wounded() && (movement().body_state() != eBodyStateCrouch));
}

void CAI_Stalker::UpdateCamera()
{
	float new_range = eye_range, new_fov = eye_fov;
	Fvector temp = eye_matrix.k;
	if (g_Alive())
	{
		update_range_fov(new_range, new_fov, memory().visual().current_state().m_max_view_distance * eye_range,
		                 eye_fov);
		if (weapon_shot_effector().IsActive())
			temp = weapon_shot_effector_direction(temp);
	}

	g_pGameLevel->Cameras().Update(eye_matrix.c, temp, eye_matrix.j, new_fov, .75f, new_range, 0);
}

bool CAI_Stalker::can_attach(const CInventoryItem* inventory_item) const
{
	if (already_dead())
		return (false);

	return (CObjectHandler::can_attach(inventory_item));
}

void CAI_Stalker::save(NET_Packet& packet)
{
	inherited::save(packet);
	CInventoryOwner::save(packet);
	brain().save(packet);
}

void CAI_Stalker::load(IReader& packet)
{
	inherited::load(packet);
	CInventoryOwner::load(packet);
	brain().load(packet);
}

void CAI_Stalker::load_critical_wound_bones()
{
	fill_bones_body_parts("head", critical_wound_type_head);
	fill_bones_body_parts("torso", critical_wound_type_torso);
	fill_bones_body_parts("hand_left", critical_wound_type_hand_left);
	fill_bones_body_parts("hand_right", critical_wound_type_hand_right);
	fill_bones_body_parts("leg_left", critical_wound_type_leg_left);
	fill_bones_body_parts("leg_right", critical_wound_type_leg_right);
}

void CAI_Stalker::fill_bones_body_parts(LPCSTR bone_id, const ECriticalWoundType& wound_type)
{
	LPCSTR body_parts_section_id = pSettings->r_string(cNameSect(), "body_parts_section_id");
	VERIFY(body_parts_section_id);

	LPCSTR body_part_section_id = pSettings->r_string(body_parts_section_id, bone_id);
	VERIFY(body_part_section_id);

	IKinematics* kinematics = smart_cast<IKinematics*>(Visual());
	VERIFY(kinematics);

	CInifile::Sect& body_part_section = pSettings->r_section(body_part_section_id);
	CInifile::SectCIt I = body_part_section.Data.begin();
	CInifile::SectCIt E = body_part_section.Data.end();
	for (; I != E; ++I)
		m_bones_body_parts.insert(
			std::make_pair(
				kinematics->LL_BoneID((*I).first),
				u32(wound_type)
			)
		);
}

void CAI_Stalker::on_before_change_team()
{
	m_registered_in_combat_on_migration =
			Level().seniority_holder().team(g_Team()).squad(g_Squad()).group(g_Group()).has_agent_manager() && agent_manager().member().registered_in_combat(this);
}

void CAI_Stalker::on_after_change_team()
{
	if (!m_registered_in_combat_on_migration || !Level().seniority_holder().team(g_Team()).squad(g_Squad()).group(g_Group()).has_agent_manager())
		return;

	agent_manager().member().register_in_combat(this);
}

float CAI_Stalker::shedule_Scale()
{
	if (sniper_update_rate())
		return (0.f);

	const float scale = inherited::shedule_Scale();

	if (IIsServer() && g_sp_diag_net_updates)
	{
		static u32 s_sp_sched_next = 0;
		static u32 s_sp_sched_count = 0;
		if (sp_net_diag_allow(s_sp_sched_next, s_sp_sched_count, 1000, 256))
		{
			u16 anchor_actor_id = u16(-1);
			LPCSTR anchor_actor_name = "";
			float anchor_actor_dist = -1.f;
			Fvector anchor_actor_position;
			anchor_actor_position.set(0.f, 0.f, 0.f);
			const bool used_actor_anchor = find_nearest_runtime_actor(*this, anchor_actor_id, anchor_actor_name, anchor_actor_dist, &anchor_actor_position);

			SStalkerMoveDiag diag;
			fill_stalker_move_diag(*this, diag);
			const float camera_distance = Device.vCameraPosition.distance_to(Position());

			Msg("* [SP_SCHED] dedicated=%d server=%d id=%hu name=[%s] sect=[%s] scale=%.3f anchor=%s/%hu/%s/%.3f cam_dist=%.3f anchor_pos=(%.2f %.2f %.2f) cam=(%.2f %.2f %.2f) pos=(%.2f %.2f %.2f) actor=%hu/%s/%.3f path_idx=%u path_dist=%.3f",
				g_dedicated_server ? 1 : 0, IIsServer() ? 1 : 0, ID(), cName().c_str(), cNameSect().c_str(), scale,
				used_actor_anchor ? "mp_actor" : "NO_ANCHOR", anchor_actor_id, anchor_actor_name, anchor_actor_dist,
				camera_distance, VPUSH(anchor_actor_position), VPUSH(Device.vCameraPosition), VPUSH(Position()),
				diag.global_actor_id, diag.global_actor_name, diag.global_actor_dist,
				diag.path_target_index, diag.path_target_dist);
		}
	}

	return (scale);
}

void CAI_Stalker::aim_bone_id(shared_str const& bone_id)
{
	//	IKinematics				*kinematics = smart_cast<IKinematics*>(Visual());
	//	VERIFY2					(kinematics->LL_BoneID(bone_id) != BI_NONE, make_string("Cannot find bone %s",bone_id));
	m_aim_bone_id = bone_id;
}

shared_str const& CAI_Stalker::aim_bone_id() const
{
	return (m_aim_bone_id);
}

void aim_target(shared_str const& aim_bone_id, Fvector& result, const CGameObject* object)
{
	result = Fvector(object->Visual()->dcast_PKinematics()->LL_GetTransform(object->Visual()->dcast_PKinematics()->LL_BoneID(aim_bone_id)).c).add(object->XFORM().c);
}

void CAI_Stalker::aim_target(Fvector& result, const CGameObject* object)
{
	VERIFY(m_aim_bone_id.size());

	::aim_target(m_aim_bone_id, result, object);
}

BOOL CAI_Stalker::AlwaysTheCrow()
{
	VERIFY(character_physics_support ());
	return (character_physics_support()->interactive_motion());
}

smart_cover::cover const* CAI_Stalker::get_current_smart_cover()
{
	if (movement().current_params().cover_id() != movement().target_params().cover_id())
		return 0;

	return movement().current_params().cover();
}

smart_cover::loophole const* CAI_Stalker::get_current_loophole()
{
	if (movement().current_params().cover_id() != movement().target_params().cover_id())
		return 0;

	if (movement().current_params().cover_loophole_id() != movement().target_params().cover_loophole_id())
		return 0;

	return movement().current_params().cover_loophole();
}

bool CAI_Stalker::can_fire_right_now()
{
	if (!ready_to_kill())
		return (false);

	VERIFY(best_weapon());
	CWeapon& best_weapon = smart_cast<CWeapon&>(*this->best_weapon());
	return best_weapon.GetAmmoElapsed() > 0;
}

bool CAI_Stalker::unlimited_ammo()
{
	return infinite_ammo() && CObjectHandler::planner().object().g_Alive();
}

void CAI_Stalker::ResetBoneProtections(LPCSTR imm_sect, LPCSTR bone_sect)
{
	IKinematics* pKinematics = renderable.visual->dcast_PKinematics();
	CInifile* ini = pKinematics->LL_UserData();
	conditions().LoadImmunities(
		(ini && ini->section_exist("immunities") && ini->line_exist("immunities", "immunities_sect"))
			? ini->r_string("immunities", "immunities_sect")
			: (imm_sect ? imm_sect : "stalker_immunities"), pSettings);

	m_boneHitProtection->reload(
		(ini && ini->section_exist("bone_protection") && ini->line_exist("bone_protection", "bones_protection_sect"))
			? ini->r_string("bone_protection", "bones_protection_sect")
			: (bone_sect ? bone_sect : "stalker_damage"), pKinematics);
}

void CAI_Stalker::ChangeVisual(shared_str NewVisual)
{
	if (!NewVisual.size()) return;
	if (cNameVisual().size())
	{
		if (cNameVisual() == NewVisual) return;
	}

	cNameVisual_set(NewVisual);

	IKinematicsAnimated* V = smart_cast<IKinematicsAnimated*>(Visual());
	if (V)
	{
		if (!g_Alive())
		{
			m_pPhysics_support->in_Die(false);
		}
		else
		{
			CStepManager::reload(cNameSect_str());
		}

		CDamageManager::reload(cNameSect_str(), "damage", pSettings);
		ResetBoneProtections(NULL, NULL);
		reattach_items();
		m_pPhysics_support->in_ChangeVisual();
		animation().reload();
		movement().reload(cNameSect_str());
	}

	Visual()->dcast_PKinematics()->CalculateBones_Invalidate();
	Visual()->dcast_PKinematics()->CalculateBones(TRUE);
};
