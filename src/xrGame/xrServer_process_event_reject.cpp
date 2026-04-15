#include "stdafx.h"
#include "xrserver.h"
#include "xrserver_objects.h"
#include "xrServer_Objects_ALife_Monsters.h"
#include "Level.h"
#include "GameObject.h"
#include "Actor.h"
#include "Missile.h"
#include "../xrEngine/CameraBase.h"
#include "Weapon.h"
#include "inventory_item.h"
#include "CustomRocket.h"
#include "Explosive.h"
#include "PhysicsShellHolder.h"
#include "../xrphysics/PhysicsShell.h"
#include "../xrphysics/PHCollideValidator.h"
#include "weapon_trace.h"

namespace
{
	bool TryConsumeRuntimeDropHint(CObject* parent_runtime, Fvector& hinted_position, Fvector& hinted_direction)
	{
		CActor* runtime_actor = smart_cast<CActor*>(parent_runtime);
		if (!runtime_actor)
			return false;

		if (!runtime_actor->GetDropHint(hinted_position, hinted_direction, 15000))
			return false;

		runtime_actor->InvalidateDropHint();
		if (!_valid(hinted_position) || !_valid(hinted_direction) || hinted_direction.square_magnitude() < EPS)
			return false;

		hinted_direction.normalize();
		return true;
	}

	Fvector CalcServerDropLookDirection(const CSE_Abstract* parent, CObject* parent_runtime)
	{
		Fvector drop_dir;
		bool have_dir = false;

		// For mp_actor on dedicated server we prefer live camera direction from runtime actor.
		if (CActor* runtime_actor = smart_cast<CActor*>(parent_runtime))
		{
			if (CCameraBase* active_cam = runtime_actor->cam_Active())
			{
				drop_dir = active_cam->Direction();
				if (_valid(drop_dir) && drop_dir.square_magnitude() > EPS)
					have_dir = true;
			}
		}

		if ((!have_dir || drop_dir.square_magnitude() < EPS || !_valid(drop_dir)) && parent_runtime)
		{
			drop_dir = parent_runtime->Direction();
			if (_valid(drop_dir) && drop_dir.square_magnitude() > EPS)
				have_dir = true;
		}

		if (const CSE_ALifeCreatureActor* actor_parent = smart_cast<const CSE_ALifeCreatureActor*>(parent))
		{
			// If runtime direction exists, keep yaw from runtime and only sync pitch from server torso.
			if (have_dir && _valid(drop_dir) && drop_dir.square_magnitude() > EPS)
			{
				float runtime_yaw = 0.f;
				float runtime_pitch = 0.f;
				drop_dir.getHP(runtime_yaw, runtime_pitch);
				drop_dir.setHP(runtime_yaw, actor_parent->o_torso.pitch);
			}
			else
			{
				drop_dir.setHP(actor_parent->o_torso.yaw, actor_parent->o_torso.pitch);
				have_dir = true;
			}
		}
		else if (const CSE_ALifeCreatureAbstract* creature_parent = smart_cast<const CSE_ALifeCreatureAbstract*>(parent))
		{
			if (have_dir && _valid(drop_dir) && drop_dir.square_magnitude() > EPS)
			{
				float runtime_yaw = 0.f;
				float runtime_pitch = 0.f;
				drop_dir.getHP(runtime_yaw, runtime_pitch);
				drop_dir.setHP(runtime_yaw, creature_parent->o_torso.pitch);
			}
			else
			{
				drop_dir.setHP(creature_parent->o_torso.yaw, creature_parent->o_torso.pitch);
				have_dir = true;
			}
		}

		if ((!have_dir || drop_dir.square_magnitude() < EPS || !_valid(drop_dir)) && parent)
		{
			drop_dir.setHP(parent->o_Angle.y, 0.f);
			have_dir = true;
		}

		if (drop_dir.square_magnitude() < EPS)
			drop_dir.set(0.f, 0.f, 1.f);
		else
			drop_dir.normalize();
		return drop_dir;
	}

	struct ServerDropBasePosition
	{
		Fvector position;
		bool from_runtime;
	};

	ServerDropBasePosition CalcServerDropBasePosition(const CSE_Abstract* parent, CObject* parent_runtime)
	{
		ServerDropBasePosition result{};
		result.from_runtime = false;

		Fvector base_position;
		base_position.set(0.f, 0.f, 0.f);
		bool have_position = false;

		if (CActor* runtime_actor = smart_cast<CActor*>(parent_runtime))
		{
			if (CCameraBase* active_cam = runtime_actor->cam_Active())
			{
				base_position = active_cam->Position();
				if (_valid(base_position))
					have_position = true;
			}
		}

		if ((!have_position || !_valid(base_position)) && parent_runtime)
		{
			parent_runtime->Center(base_position);
			if (_valid(base_position))
				have_position = true;
		}

		if ((!have_position || !_valid(base_position)) && parent_runtime)
		{
			base_position = parent_runtime->Position();
			if (_valid(base_position))
				have_position = true;
		}

		if ((!have_position || !_valid(base_position)) && parent)
		{
			base_position = parent->o_Position;
			have_position = true;
		}

		if (!_valid(base_position))
		{
			base_position.set(0.f, 0.f, 0.f);
			have_position = false;
		}

		result.position = base_position;
		result.from_runtime = have_position && parent_runtime != nullptr;
		return result;
	}

struct ServerDropData
{
	Fvector position;
	Fvector velocity;
	bool used_client_hint;
	bool used_client_payload;
};

	struct ServerDropContext
	{
		const CSE_Abstract* parent_server;
		CObject* parent_runtime;
	};

	ServerDropContext ResolveServerDropContextForReject(xrServer* server, const CSE_Abstract* parent, CObject* parent_runtime)
	{
		ServerDropContext context{};
		context.parent_server = parent;
		context.parent_runtime = parent_runtime;

		const bool runtime_is_actor = (smart_cast<CActor*>(parent_runtime) != nullptr);
		if (runtime_is_actor)
			return context;

		if (parent_runtime)
		{
			if (CObject* runtime_owner = parent_runtime->H_Parent())
			{
				if (smart_cast<CActor*>(runtime_owner))
					context.parent_runtime = runtime_owner;
			}
		}

		if (!parent || parent->ID_Parent == u16(-1))
			return context;

		CSE_Abstract* owner_server = server ? server->game->get_entity_from_eid(parent->ID_Parent) : nullptr;
		if (owner_server)
		{
			context.parent_server = owner_server;
			if (g_pGameLevel)
			{
				CObject* owner_runtime = Level().Objects.net_Find(parent->ID_Parent);
				if (owner_runtime)
					context.parent_runtime = owner_runtime;
			}
		}

		return context;
	}

	ServerDropData CalcServerAuthoritativeDropData(const CSE_Abstract* parent, CObject* parent_runtime)
	{
	ServerDropData result{};
	result.used_client_hint = false;
	result.used_client_payload = false;

		Fvector hinted_position{};
		Fvector hinted_direction{};
		if (TryConsumeRuntimeDropHint(parent_runtime, hinted_position, hinted_direction))
		{
			result.position = hinted_position;
			result.position.mad(hinted_direction, 1.15f);
			result.position.y += 0.10f;

			result.velocity = hinted_direction;
			result.velocity.mul(9.5f);
			result.velocity.y += 1.25f;
			result.used_client_hint = true;
			return result;
		}

		const Fvector drop_dir = CalcServerDropLookDirection(parent, parent_runtime);
		const ServerDropBasePosition base_position = CalcServerDropBasePosition(parent, parent_runtime);

		result.position = base_position.position;
		result.position.mad(drop_dir, 1.15f);
		result.position.y += base_position.from_runtime ? 0.10f : 1.05f;

		result.velocity = drop_dir;
		result.velocity.mul(9.5f);
		result.velocity.y += 1.25f;
		return result;
	}

	void ApplyAuthoritativeRuntimeDropForProjectile(CObject* runtime_object, const Fvector& position,
		const Fvector& velocity, bool has_velocity, bool just_before_destroy, CPhysicsShellHolder* contact_owner)
	{
		CGameObject* game_object = smart_cast<CGameObject*>(runtime_object);
		if (!game_object)
			return;

		// For dedicated bridge this path is authoritative and can be the only place
		// where runtime transform gets applied for detached projectile entities.
		if (game_object->H_Parent())
			game_object->H_SetParent(nullptr, just_before_destroy);

		game_object->Position().set(position);
		game_object->XFORM().c.set(position);

		CPhysicsShellHolder* shell_holder = smart_cast<CPhysicsShellHolder*>(game_object);
		CPhysicsShell* physics_shell = shell_holder ? shell_holder->PPhysicsShell() : nullptr;
		if (!physics_shell)
		{
			if (CMissile* missile = smart_cast<CMissile*>(game_object))
			{
				missile->setup_physic_shell();
				physics_shell = missile->PPhysicsShell();
			}
		}

		if (physics_shell)
		{
			physics_shell->Enable();
			physics_shell->EnableCollision();
			physics_shell->set_ApplyByGravity(TRUE);
			physics_shell->SetAirResistance(0.f, 0.f);
			physics_shell->set_DynamicScales(1.f, 1.f);

			// Newly detached projectile shells can remain fixed/NCStatic depending on parent state.
			// Force dynamic world simulation, otherwise projectile stays at throw start point.
			const u16 element_count = physics_shell->get_ElementsNumber();
			for (u16 element_id = 0; element_id < element_count; ++element_id)
			{
				CPhysicsElement* element = physics_shell->get_ElementByStoreOrder(element_id);
				if (element && element->isFixed())
					element->ReleaseFixed();
			}
			physics_shell->collide_class_bits().set(CPHCollideValidator::cbNCStatic, FALSE);

			Fmatrix physics_xform = game_object->XFORM();
			physics_xform.c.set(position);
			physics_shell->SetTransform(physics_xform, mh_unspecified);

			// Re-activate with authoritative launch velocity to avoid stale zero-velocity active shell.
			if (has_velocity)
			{
				if (physics_shell->isActive())
					physics_shell->Deactivate();
				Fvector angular_velocity;
				angular_velocity.set(0.f, 0.f, 0.f);
				physics_shell->Activate(physics_xform, velocity, angular_velocity);
				physics_shell->set_LinearVel(velocity);
			}
			else if (!physics_shell->isActive())
			{
				physics_shell->Activate(physics_xform, false);
			}

			if (CMissile* missile = smart_cast<CMissile*>(game_object))
			{
				// Ignore initial owner contact just like normal throw path (owner must be actor holder, not missile itself).
				if (contact_owner)
				{
					physics_shell->remove_ObjectContactCallback(CMissile::ExitContactCallback);
					physics_shell->set_CallbackData(contact_owner);
					physics_shell->add_ObjectContactCallback(CMissile::ExitContactCallback);
				}
				physics_shell->SetAllGeomTraced();
			}

			Fvector shell_linear_velocity{};
			physics_shell->get_LinearVel(shell_linear_velocity);
			WPN_TRACE("xrServer::Process_event_reject projectile shell state item=%u enabled=%d active=%d gravity=%d vel=(%.3f,%.3f,%.3f)",
				game_object->ID(),
				physics_shell->isEnabled() ? 1 : 0,
				physics_shell->isActive() ? 1 : 0,
				physics_shell->get_ApplyByGravity() ? 1 : 0,
				shell_linear_velocity.x, shell_linear_velocity.y, shell_linear_velocity.z);
		}

		game_object->setVisible(TRUE);
		game_object->setEnabled(TRUE);
		if (CInventoryItem* dropped_item = smart_cast<CInventoryItem*>(game_object))
			dropped_item->ClearNetInterpolationQueue();
		game_object->processing_activate();
	}
}

bool xrServer::Process_event_reject(NET_Packet& P, const ClientID sender, const u32 time, const u16 id_parent,
                                    const u16 id_entity, bool send_message, bool is_launch_rocket)
{
	// Parse message
	CSE_Abstract* e_parent = game->get_entity_from_eid(id_parent);
	CSE_Abstract* e_entity = game->get_entity_from_eid(id_entity);

	//	R_ASSERT2( e_entity, make_string( "entity not found. parent_id = [%d], entity_id = [%d], frame = [%d]", id_parent, id_entity, Device.dwFrame ).c_str() );
	VERIFY2(e_entity,
	        make_string( "entity not found. parent_id = [%d], entity_id = [%d], frame = [%d]", id_parent, id_entity,
		        Device.dwFrame ).c_str());
	if (!e_entity)
	{
		Msg("! ERROR on rejecting: entity not found. parent_id = [%d], entity_id = [%d], frame = [%d].", id_parent,
		    id_entity, Device.dwFrame);
		return false;
	}

	//	R_ASSERT2( e_parent, make_string( "parent not found. parent_id = [%d], entity_id = [%d], frame = [%d]", id_parent, id_entity, Device.dwFrame ).c_str() );
	VERIFY2(e_parent,
	        make_string( "parent not found. parent_id = [%d], entity_id = [%d], frame = [%d]", id_parent, id_entity,
		        Device.dwFrame ).c_str());
	if (!e_parent)
	{
		Msg("! ERROR on rejecting: parent not found. parent_id = [%d], entity_id = [%d], frame = [%d].", id_parent,
		    id_entity, Device.dwFrame);
		return false;
	}

#ifdef MP_LOGGING
	Msg ( "--- SV: Process reject: parent[%d][%s], item[%d][%s]", id_parent, e_parent->name_replace(), id_entity, e_entity->name());
#endif // MP_LOGGING

	xr_vector<u16>& C = e_parent->children;
	xr_vector<u16>::iterator c = std::find(C.begin(), C.end(), id_entity);
	if (c == C.end())
	{
		xr_string clildrenList;
		for (const u16& childID : e_parent->children)
		{
			clildrenList.append("! ").append(game->get_entity_from_eid(childID)->name_replace()).append("\n");
		}
		Msg("! WARNING: SV: can't find child [%s] of parent [%s]! Children list:\n%s", e_entity->name_replace(),
		    e_parent->name_replace(), clildrenList.c_str());
		return false;
	}

	if (0xffff == e_entity->ID_Parent)
	{
#ifndef MASTER_GOLD
		Msg	("! ERROR: can't detach independant object. entity[%s][%d], parent[%s][%d], section[%s]",
			e_entity->name_replace(), id_entity, e_parent->name_replace(), id_parent, e_entity->s_name.c_str() );
#endif // #ifndef MASTER_GOLD
		return (false);
	}

	// Rebuild parentness
	//.	Msg("---ID_Parent [%d], id_parent [%d]", e_entity->ID_Parent, id_parent);

	//R_ASSERT(e_entity->ID_Parent == id_parent);
	if (e_entity->ID_Parent != id_parent)
	{
		Msg("! ERROR: e_entity->ID_Parent = [%d]  parent = [%d][%s]  entity_id = [%d]  frame = [%d]",
		    e_entity->ID_Parent, id_parent, e_parent->name_replace(), id_entity, Device.dwFrame);
		//it can't be !!!
	}

	game->OnDetach(id_parent, id_entity);

	//R_ASSERT3(C.end()!=c,e_entity->name_replace(),e_parent->name_replace());
	e_entity->ID_Parent = 0xffff;
	C.erase(c);

	CObject* parent_runtime_object = nullptr;
	CObject* entity_runtime_object = nullptr;
	if (g_pGameLevel)
	{
		parent_runtime_object = Level().Objects.net_Find(id_parent);
		entity_runtime_object = Level().Objects.net_Find(id_entity);
	}

	const bool dedicated_single_bridge = (game && game->Type() == eGameIDSingle);
	xrClientData* sv_client_data = SV_Client ? static_cast<xrClientData*>(SV_Client) : nullptr;
	const bool entity_is_custom_rocket = (smart_cast<CCustomRocket*>(entity_runtime_object) != nullptr);
	const bool entity_is_missile = (smart_cast<CMissile*>(entity_runtime_object) != nullptr);

	// Keep original MP launch semantics for rockets:
	// GE_LAUNCH_ROCKET is an ownership detach with launch payload, not an inventory drop.
	// But in dedicated-single bridge we still migrate launched projectile ownership to SV_Client
	// so server-side simulation and explode position stay authoritative.
	if (is_launch_rocket)
	{
		if (dedicated_single_bridge && sv_client_data && entity_is_custom_rocket && e_entity->owner != sv_client_data)
		{
			WPN_TRACE("xrServer::Process_event_reject migrate launch entity owner item=%u from=%s to=%s",
				id_entity,
				e_entity->owner ? e_entity->owner->name.c_str() : "<none>",
				sv_client_data->name.c_str());
			e_entity->owner = sv_client_data;
		}

		if (send_message)
		{
			DWORD MODE = net_flags(TRUE,TRUE, FALSE, TRUE);
			SendBroadcast(BroadcastCID, P, MODE);
		}
		return (true);
	}

	// Dedicated server authoritative drop transform.
	const u32 payload_begin = P.r_tell();
	u8 just_before_destroy = FALSE;
	Fvector incoming_drop_position{};
	Fvector incoming_drop_velocity{};
	bool has_incoming_drop_position = false;
	bool has_incoming_drop_velocity = false;
	if (P.B.count > payload_begin)
	{
		just_before_destroy = P.r_u8();

		const u32 unread_after_flag = (P.B.count > P.r_tell()) ? (P.B.count - P.r_tell()) : 0;
		if (unread_after_flag >= sizeof(Fvector))
		{
			P.r_vec3(incoming_drop_position);
			has_incoming_drop_position = _valid(incoming_drop_position);
		}

		const u32 unread_after_pos = (P.B.count > P.r_tell()) ? (P.B.count - P.r_tell()) : 0;
		if (unread_after_pos >= sizeof(Fvector))
		{
			P.r_vec3(incoming_drop_velocity);
			has_incoming_drop_velocity = _valid(incoming_drop_velocity);
		}
	}

	const bool parent_is_runtime_actor = (smart_cast<CActor*>(parent_runtime_object) != nullptr);
	const bool parent_is_server_actor = (smart_cast<const CSE_ALifeCreatureActor*>(e_parent) != nullptr);
	const bool parent_is_actor = parent_is_runtime_actor || parent_is_server_actor;
	const CInventoryItem* dropped_inventory_item = smart_cast<CInventoryItem*>(entity_runtime_object);
	const CCustomRocket* dropped_custom_rocket = smart_cast<CCustomRocket*>(entity_runtime_object);
	const bool is_inventory_drop_candidate = parent_is_actor && (dropped_inventory_item != nullptr) && (dropped_custom_rocket == nullptr);

	Fvector hinted_position{};
	Fvector hinted_direction{};
	const bool has_runtime_drop_hint = is_inventory_drop_candidate &&
		TryConsumeRuntimeDropHint(parent_runtime_object, hinted_position, hinted_direction);
	const bool has_client_drop_payload = has_incoming_drop_position || has_incoming_drop_velocity;
	const bool is_projectile_payload_detach_candidate =
		dedicated_single_bridge &&
		(entity_is_missile || entity_is_custom_rocket) &&
		has_client_drop_payload;
	const bool should_apply_authoritative_drop =
		((game && game->Type() == eGameIDSingle) &&
		 is_inventory_drop_candidate &&
		 (has_runtime_drop_hint || has_client_drop_payload)) ||
		is_projectile_payload_detach_candidate;

	if (!should_apply_authoritative_drop)
	{
		// For detached projectiles (hand grenades / rockets) in dedicated-single bridge
		// force owner migration to SV_Client, otherwise position updates can stall and
		// timeout explosion may occur at launch/throw point on server.
		if (dedicated_single_bridge && sv_client_data && (entity_is_missile || entity_is_custom_rocket) && e_entity->owner != sv_client_data)
		{
			WPN_TRACE("xrServer::Process_event_reject migrate projectile owner item=%u from=%s to=%s launch=%d",
				id_entity,
				e_entity->owner ? e_entity->owner->name.c_str() : "<none>",
				sv_client_data->name.c_str(),
				is_launch_rocket ? 1 : 0);
			e_entity->owner = sv_client_data;
		}

		WPN_TRACE("xrServer::Process_event_reject pass-through parent=%u item=%u launch=%d candidate=%d hint=%d payload=%d parent_actor=%d inv_item=%d rocket=%d",
			id_parent, id_entity,
			is_launch_rocket ? 1 : 0,
			is_inventory_drop_candidate ? 1 : 0,
			has_runtime_drop_hint ? 1 : 0,
			has_client_drop_payload ? 1 : 0,
			parent_is_actor ? 1 : 0,
			dropped_inventory_item ? 1 : 0,
			dropped_custom_rocket ? 1 : 0);
		if (send_message)
		{
			DWORD MODE = net_flags(TRUE, TRUE, FALSE, TRUE);
			SendBroadcast(BroadcastCID, P, MODE);
		}
		return (true);
	}

	// Dedicated-single bridge:
	// world item physics is simulated by the local server bridge client only
	// (xrServer::Process_update accepts updates only from bLocal client).
	// After detach we must hand entity ownership back to SV_Client, otherwise
	// non-owner clients can see stale/flying dropped items.
	if (sv_client_data && e_entity->owner != sv_client_data)
	{
		WPN_TRACE("xrServer::Process_event_reject migrate dropped entity owner item=%u from=%s to=%s",
			id_entity,
			e_entity->owner ? e_entity->owner->name.c_str() : "<none>",
			sv_client_data->name.c_str());
		e_entity->owner = sv_client_data;
	}

	const ServerDropContext drop_context = ResolveServerDropContextForReject(this, e_parent, parent_runtime_object);
	ServerDropData drop_data{};
	if (has_runtime_drop_hint)
	{
		drop_data.position = hinted_position;
		drop_data.position.mad(hinted_direction, 1.15f);
		drop_data.position.y += 0.10f;
		drop_data.velocity = hinted_direction;
		drop_data.velocity.mul(9.5f);
		drop_data.velocity.y += 1.25f;
		drop_data.used_client_hint = true;
		drop_data.used_client_payload = false;
	}
	else
	{
		drop_data = CalcServerAuthoritativeDropData(drop_context.parent_server, drop_context.parent_runtime);
	}

	if (has_client_drop_payload)
	{
		if (has_incoming_drop_position)
			drop_data.position.set(incoming_drop_position);
		if (has_incoming_drop_velocity)
			drop_data.velocity.set(incoming_drop_velocity);
		drop_data.used_client_payload = true;
	}

	e_entity->o_Position.set(drop_data.position);
	WPN_TRACE("xrServer::Process_event_reject authoritative drop parent=%u item=%u hint=%d payload=%d ctx_parent=%u ctx_runtime=%u pos=(%.3f,%.3f,%.3f) vel=(%.3f,%.3f,%.3f)",
		id_parent, id_entity,
		drop_data.used_client_hint ? 1 : 0,
		drop_data.used_client_payload ? 1 : 0,
		drop_context.parent_server ? drop_context.parent_server->ID : u16(-1),
		drop_context.parent_runtime ? drop_context.parent_runtime->ID() : u16(-1),
		drop_data.position.x, drop_data.position.y, drop_data.position.z,
		drop_data.velocity.x, drop_data.velocity.y, drop_data.velocity.z);

	if (CWeapon* dropped_weapon = smart_cast<CWeapon*>(entity_runtime_object))
		dropped_weapon->SetActivationSpeedOverride(drop_data.velocity);

	if (is_projectile_payload_detach_candidate && entity_runtime_object)
	{
		const bool has_runtime_velocity = _valid(drop_data.velocity) && (drop_data.velocity.square_magnitude() > EPS);
		CPhysicsShellHolder* projectile_contact_owner = smart_cast<CPhysicsShellHolder*>(drop_context.parent_runtime);
		ApplyAuthoritativeRuntimeDropForProjectile(entity_runtime_object, drop_data.position, drop_data.velocity,
			has_runtime_velocity, !!just_before_destroy, projectile_contact_owner);
		if (CExplosive* explosive = smart_cast<CExplosive*>(entity_runtime_object))
		{
			u16 initiator_id = id_parent;
			if (drop_context.parent_runtime)
				initiator_id = drop_context.parent_runtime->ID();
			else if (drop_context.parent_server)
				initiator_id = drop_context.parent_server->ID;
			explosive->SetInitiator(initiator_id);
			WPN_TRACE("xrServer::Process_event_reject projectile initiator apply item=%u initiator=%u",
				id_entity, u32(initiator_id));
		}
		WPN_TRACE("xrServer::Process_event_reject runtime projectile apply item=%u has_vel=%d pos=(%.3f,%.3f,%.3f) vel=(%.3f,%.3f,%.3f)",
			id_entity,
			has_runtime_velocity ? 1 : 0,
			drop_data.position.x, drop_data.position.y, drop_data.position.z,
			drop_data.velocity.x, drop_data.velocity.y, drop_data.velocity.z);
		WPN_TRACE("xrServer::Process_event_reject runtime projectile contact owner item=%u owner=%u",
			id_entity,
			projectile_contact_owner ? projectile_contact_owner->ID() : u16(-1));
	}

	// Signal to everyone (including sender)
	if (send_message)
	{
		// Normalize and overwrite optional payload for clients:
		// Actor::OnEvent(GE_OWNERSHIP_REJECT) supports:
		// [u8 just_before_destroy][vec3 drop_position][vec3 drop_velocity(optional)].
		// We always rewrite drop transform from server authority to avoid stale/invalid client coordinates.
		P.r_pos = payload_begin;
		P.B.count = payload_begin;
		P.w_u8(just_before_destroy);
		P.w_vec3(drop_data.position);
		P.w_vec3(drop_data.velocity);

		DWORD MODE = net_flags(TRUE,TRUE, FALSE, TRUE);
		SendBroadcast(BroadcastCID, P, MODE);
	}

	return (true);
}
