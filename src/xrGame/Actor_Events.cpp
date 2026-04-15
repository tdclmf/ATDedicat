#include "stdafx.h"
#include "actor.h"
#include "customdetector.h"
#include "weapon.h"
#include "artefact.h"
#include "scope.h"
#include "silencer.h"
#include "grenadelauncher.h"
#include "inventory.h"
#include "level.h"
#include "xr_level_controller.h"
#include "FoodItem.h"
#include "ActorCondition.h"
#include "Grenade.h"
#include "inventory_item.h"

#include "CameraLook.h"
#include "CameraFirstEye.h"
#include "holder_custom.h"
//.#include "ui/uiinventoryWnd.h"
#include "game_base_space.h"
#include "game_cl_base.h"
#include "weapon_trace.h"
#include "PhysicsShellHolder.h"
#include "../xrphysics/PhysicsShell.h"
#include "../xrphysics/PHCollideValidator.h"
#ifdef DEBUG
#include "PHDebug.h"
#endif
#include "3rd party/luabind/luabind/luabind.hpp"
#include "script_game_object.h"

void CActor::OnEvent(NET_Packet& P, u16 type)
{
	inherited::OnEvent(P, type);
	CInventoryOwner::OnEvent(P, type);

	u16 id;
	switch (type)
	{
	case GE_TRADE_BUY:
	case GE_OWNERSHIP_TAKE:
		{
			P.r_u16(id);
			CObject* Obj = Level().Objects.net_Find(id);

			//			R_ASSERT2( Obj, make_string("GE_OWNERSHIP_TAKE: Object not found. object_id = [%d]", id).c_str() );
			VERIFY2(Obj, make_string("GE_OWNERSHIP_TAKE: Object not found. object_id = [%d]", id).c_str());
			if (!Obj)
			{
				Msg("! GE_OWNERSHIP_TAKE: Object not found. object_id = [%d]", id);
				break;
			}

			CGameObject* _GO = smart_cast<CGameObject*>(Obj);
			if (!g_Alive())
			{
				Msg("! WARNING: dead player [%d][%s] can't take items [%d][%s]",
				    ID(), Name(), _GO->ID(), _GO->cNameSect().c_str());
				break;
			}

			if (inventory().CanTakeItem(smart_cast<CInventoryItem*>(_GO)))
			{
				Obj->H_SetParent(smart_cast<CObject*>(this));

#ifdef MP_LOGGING
				string64 act;
				xr_strcpy( act, (type == GE_TRADE_BUY)? "buys" : "takes" );
				Msg("--- Actor [%d][%s]  %s  [%d][%s]", ID(), Name(), act, _GO->ID(), _GO->cNameSect().c_str());
#endif // MP_LOGGING

				inventory().Take(_GO, false, true);
			}
			else
			{
				// werasik2aa FORGOT
				Msg("! ERROR: Actor [%d][%s]  tries to drop on take [%d][%s]", ID(), Name(), _GO->ID(),
					_GO->cNameSect().c_str());
			}
		}
		break;
	case GE_TRADE_SELL:
	case GE_OWNERSHIP_REJECT:
		{
			P.r_u16(id);
			CObject* Obj = Level().Objects.net_Find(id);

			//			R_ASSERT2( Obj, make_string("GE_OWNERSHIP_REJECT: Object not found, id = %d", id).c_str() );
			VERIFY2(Obj, make_string("GE_OWNERSHIP_REJECT: Object not found, id = %d", id).c_str());
			if (!Obj)
			{
				Msg("! GE_OWNERSHIP_REJECT: Object not found, id = %d", id);
				break;
			}

			bool just_before_destroy = !P.r_eof() && P.r_u8();
			const bool pure_client = OnClient() && !OnServer();
			// Keep physics shell creation enabled for normal drop reject even on pure client.
			// Otherwise dropped item may stay static in air (has_shell=0 in trace).
			bool dont_create_shell = (type == GE_TRADE_SELL) || just_before_destroy;
			Obj->SetTmpPreDestroy(just_before_destroy);

			CGameObject* GO = smart_cast<CGameObject*>(Obj);

#ifdef MP_LOGGING
			string64 act;
			xr_strcpy( act, (type == GE_TRADE_SELL)? "sells" : "rejects" );
			Msg("--- Actor [%d][%s]  %s  [%d][%s]", ID(), Name(), act, GO->ID(), GO->cNameSect().c_str());
#endif // MP_LOGGING

			VERIFY(GO->H_Parent());
			if (!GO->H_Parent())
			{
				Msg("! ERROR: Actor [%d][%s] tries to reject item [%d][%s] that has no parent",
				    ID(), Name(), GO->ID(), GO->cNameSect().c_str());
				break;
			}

			VERIFY2(GO->H_Parent()->ID() == ID(),
			        make_string("actor [%d][%s] tries to drop not own object [%d][%s]",
				        ID(), Name(), GO->ID(), GO->cNameSect().c_str() ).c_str());

			if (GO->H_Parent()->ID() != ID())
			{
				CActor* real_parent = smart_cast<CActor*>(GO->H_Parent());
				Msg("! ERROR: Actor [%d][%s] tries to drop not own item [%d][%s], his parent is [%d][%s]",
				    ID(), Name(), GO->ID(), GO->cNameSect().c_str(), real_parent->ID(), real_parent->Name());
				break;
			}

			CObject* drop_parent = GO->H_Parent();
			Fvector fallback_drop_position = GO->Position();
			bool has_fallback_drop_position = false;
			if (drop_parent)
			{
				CGameObject* drop_parent_go = smart_cast<CGameObject*>(drop_parent);
				if (drop_parent_go)
				{
					Fvector drop_dir = drop_parent_go->XFORM().k;
					if (drop_dir.square_magnitude() < EPS)
						drop_dir.set(0.f, 0.f, 1.f);
					else
						drop_dir.normalize();

					fallback_drop_position = drop_parent_go->Position();
					fallback_drop_position.mad(drop_dir, 0.55f);
					fallback_drop_position.y += 1.05f;
					has_fallback_drop_position = true;
				}
			}

			Fvector dropPosition{};
			Fvector dropVelocity{};
			bool has_drop_position = false;
			bool has_drop_velocity = false;
			const u32 unread_payload = (P.B.count > P.r_tell()) ? (P.B.count - P.r_tell()) : 0;
			if (unread_payload >= sizeof(Fvector))
			{
				P.r_vec3(dropPosition);
				has_drop_position = true;
			}
			const u32 unread_payload_after_pos = (P.B.count > P.r_tell()) ? (P.B.count - P.r_tell()) : 0;
			if (unread_payload_after_pos >= sizeof(Fvector))
			{
				P.r_vec3(dropVelocity);
				if (_valid(dropVelocity))
					has_drop_velocity = true;
			}

			const bool dropped = !Obj->getDestroy() && inventory().DropItem(GO, just_before_destroy, dont_create_shell);

			if (dropped && !has_drop_position && has_fallback_drop_position && !pure_client)
			{
				dropPosition = fallback_drop_position;
				has_drop_position = true;
				WPN_TRACE("Actor::OnEvent GE_OWNERSHIP_REJECT fallback drop pos actor=%u item=%u parent=%u pos=(%.3f,%.3f,%.3f)",
					ID(), GO->ID(), drop_parent ? drop_parent->ID() : u16(-1),
					dropPosition.x, dropPosition.y, dropPosition.z);
			}

			WPN_TRACE("Actor::OnEvent GE_OWNERSHIP_REJECT actor=%u item=%u dropped=%d has_drop_pos=%d pos=(%.3f,%.3f,%.3f) has_drop_vel=%d vel=(%.3f,%.3f,%.3f) local=%d remote=%d on_client=%d on_server=%d",
				ID(), GO->ID(), dropped ? 1 : 0, has_drop_position ? 1 : 0,
				dropPosition.x, dropPosition.y, dropPosition.z,
				has_drop_velocity ? 1 : 0,
				dropVelocity.x, dropVelocity.y, dropVelocity.z,
				Local() ? 1 : 0, Remote() ? 1 : 0, OnClient() ? 1 : 0, OnServer() ? 1 : 0);

			if (dropped)
			{
				//O->H_SetParent(0,just_before_destroy);//moved to DropItem
				//feel_touch_deny(O,2000);
				Level().m_feel_deny.feel_touch_deny(Obj, 1000);
			}

			// [12.11.07] Alexander Maniluk: extended GE_OWNERSHIP_REJECT packet for drop item to selected position.
			// In dedicated-single MP bridge CGameObject::MoveTo is a no-op for most inventory objects,
			// so we must apply transform directly. If authoritative velocity is provided, push it
			// to physics shell right after transform to preserve throw dynamics.
			if (dropped && has_drop_position)
			{
				GO->Position().set(dropPosition);
				GO->XFORM().c.set(dropPosition);

				CPhysicsShellHolder* shell_holder = smart_cast<CPhysicsShellHolder*>(GO);
				const bool has_shell = shell_holder && shell_holder->PPhysicsShell();
				bool shell_enabled = false;
				bool shell_active = false;
				bool shell_apply_gravity = false;
				bool shell_fixed = false;
				bool shell_ignore_static = false;
				WPN_TRACE("Actor::OnEvent GE_OWNERSHIP_REJECT apply drop transform actor=%u item=%u has_shell=%d set_vel=%d",
					ID(), GO->ID(), has_shell ? 1 : 0, (has_shell && has_drop_velocity) ? 1 : 0);
				if (has_shell)
				{
					CPhysicsShell* physics_shell = shell_holder->PPhysicsShell();
					shell_enabled = physics_shell->isEnabled();
					shell_active = physics_shell->isActive();
					shell_apply_gravity = physics_shell->get_ApplyByGravity();
					shell_ignore_static = physics_shell->collide_class_bits().test(CPHCollideValidator::cbNCStatic);

					const u16 element_count = physics_shell->get_ElementsNumber();
					for (u16 element_id = 0; element_id < element_count; ++element_id)
					{
						CPhysicsElement* element = physics_shell->get_ElementByStoreOrder(element_id);
						if (!element)
							continue;

						if (element->isFixed())
						{
							shell_fixed = true;
							element->ReleaseFixed();
						}
					}

					if (!shell_enabled)
					{
						physics_shell->Enable();
						shell_enabled = true;
					}
					physics_shell->EnableCollision();

					if (!shell_apply_gravity)
						physics_shell->set_ApplyByGravity(TRUE);

					shell_fixed = false;
					for (u16 element_id = 0; element_id < element_count; ++element_id)
					{
						CPhysicsElement* element = physics_shell->get_ElementByStoreOrder(element_id);
						if (element && element->isFixed())
						{
							shell_fixed = true;
							break;
						}
					}
					if (shell_ignore_static)
					{
						physics_shell->collide_class_bits().set(CPHCollideValidator::cbNCStatic, FALSE);
						shell_ignore_static = physics_shell->collide_class_bits().test(CPHCollideValidator::cbNCStatic);
					}
					shell_active = physics_shell->isActive();
					shell_apply_gravity = physics_shell->get_ApplyByGravity();

					Fmatrix physics_xform = GO->XFORM();
					physics_xform.c.set(dropPosition);
					physics_shell->SetTransform(physics_xform, mh_unspecified);
					if (!physics_shell->isActive())
						physics_shell->Activate(false);
					if (has_drop_velocity)
						physics_shell->set_LinearVel(dropVelocity);
					shell_active = physics_shell->isActive();
					shell_enabled = physics_shell->isEnabled();
					WPN_TRACE("Actor::OnEvent GE_OWNERSHIP_REJECT shell state actor=%u item=%u enabled=%d active=%d gravity=%d fixed=%d ignore_static=%d vel=(%.3f,%.3f,%.3f)",
						ID(), GO->ID(),
						shell_enabled ? 1 : 0,
						shell_active ? 1 : 0,
						shell_apply_gravity ? 1 : 0,
						shell_fixed ? 1 : 0,
						shell_ignore_static ? 1 : 0,
						dropVelocity.x, dropVelocity.y, dropVelocity.z);
				}

				if (CInventoryItem* dropped_item = smart_cast<CInventoryItem*>(GO))
					dropped_item->ClearNetInterpolationQueue();

				GO->processing_activate();
			}
		}
		break;
	case GE_INV_ACTION:
		{
			u16 cmd;
			P.r_u16(cmd);
			u32 flags;
			P.r_u32(flags);
			s32 ZoomRndSeed = P.r_s32();
			s32 ShotRndSeed = P.r_s32();
			Fvector drop_hint_pos{};
			Fvector drop_hint_dir{};
			bool has_drop_hint = false;
			const bool supports_action_hint = (cmd == kDROP || cmd == kWPN_FIRE || cmd == kWPN_ZOOM);
			if (supports_action_hint)
			{
				const u32 unread_payload = (P.B.count > P.r_tell()) ? (P.B.count - P.r_tell()) : 0;
				if (unread_payload >= (sizeof(Fvector) * 2))
				{
					P.r_vec3(drop_hint_pos);
					P.r_vec3(drop_hint_dir);
					if (_valid(drop_hint_pos) && _valid(drop_hint_dir) && drop_hint_dir.square_magnitude() > EPS)
					{
						drop_hint_dir.normalize();
						has_drop_hint = true;
						SetDropHint(drop_hint_pos, drop_hint_dir, Level().timeServer_Async());
					}
				}
				else
				{
					InvalidateDropHint();
				}
			}
			WPN_TRACE("Actor::OnEvent GE_INV_ACTION actor=%u cmd=%d(%s) flags=0x%08x zoom_seed=%d shot_seed=%d alive=%d local=%d remote=%d on_client=%d on_server=%d",
				ID(), cmd, id_to_action_name((EGameActions)cmd), flags, ZoomRndSeed, ShotRndSeed,
				g_Alive() ? 1 : 0, Local() ? 1 : 0, Remote() ? 1 : 0, OnClient() ? 1 : 0, OnServer() ? 1 : 0);
			if (supports_action_hint)
			{
				WPN_TRACE("Actor::OnEvent GE_INV_ACTION action hint actor=%u cmd=%d(%s) has_hint=%d pos=(%.3f,%.3f,%.3f) dir=(%.3f,%.3f,%.3f)",
					ID(),
					cmd,
					id_to_action_name((EGameActions)cmd),
					has_drop_hint ? 1 : 0,
					drop_hint_pos.x, drop_hint_pos.y, drop_hint_pos.z,
					drop_hint_dir.x, drop_hint_dir.y, drop_hint_dir.z);
			}
			if (!IsGameTypeSingle() && !g_Alive())
			{
				Msg("! WARNING: dead player tries to rize inventory action");
				WPN_TRACE("Actor::OnEvent GE_INV_ACTION blocked: actor dead in non-single actor=%u cmd=%d", ID(), cmd);
				break;
			}

			game_cl_GameState* game_cl = smart_cast<game_cl_GameState*>(&Game());
			const bool local_player_id_match =
				game_cl && game_cl->local_player && game_cl->local_player->GameID == ID();

			const bool dedicated_single_local_actor =
				OnClient() &&
				!g_dedicated_server &&
				(local_player_id_match || Level().CurrentControlEntity() == this || Level().CurrentEntity() == this);

			// Dedicated-single bridge:
			// local actor already generated this input event client-side.
			// Do not replay echoed GE_INV_ACTION from server, otherwise
			// it creates press/release loops (fire/zoom spam, slot flicker).
			if (dedicated_single_local_actor)
			{
				WPN_TRACE("Actor::OnEvent GE_INV_ACTION ignored local echo actor=%u cmd=%d flags=0x%08x", ID(), cmd, flags);
				break;
			}

			if (flags & CMD_START)
			{
				if (cmd == kWPN_ZOOM)
					SetZoomRndSeed(ZoomRndSeed);
				if (cmd == kWPN_FIRE)
					SetShotRndSeed(ShotRndSeed);
				WPN_TRACE("Actor::OnEvent GE_INV_ACTION -> IR_OnKeyboardPress actor=%u cmd=%d(%s)", ID(), cmd, id_to_action_name((EGameActions)cmd));
				IR_OnKeyboardPress(cmd);
			}
			else
			{
				WPN_TRACE("Actor::OnEvent GE_INV_ACTION -> IR_OnKeyboardRelease actor=%u cmd=%d(%s)", ID(), cmd, id_to_action_name((EGameActions)cmd));
				IR_OnKeyboardRelease(cmd);
			}
		}
		break;
	case GEG_PLAYER_ITEM2SLOT:
	case GEG_PLAYER_ITEM2BELT:
	case GEG_PLAYER_ITEM2RUCK:
	case GEG_PLAYER_ITEM_EAT:
	case GEG_PLAYER_ACTIVATEARTEFACT:
		{
			P.r_u16(id);
			CObject* Obj = Level().Objects.net_Find(id);

			//			R_ASSERT2( Obj, make_string("GEG_PLAYER_ITEM_EAT(use): Object not found. object_id = [%d]", id).c_str() );
			VERIFY2(Obj, make_string("GEG_PLAYER_ITEM_EAT(use): Object not found. object_id = [%d]", id).c_str());
			if (!Obj)
			{
				//				Msg                 ( "! GEG_PLAYER_ITEM_EAT(use): Object not found. object_id = [%d]", id );
				break;
			}

			//			R_ASSERT2( !Obj->getDestroy(), make_string("GEG_PLAYER_ITEM_EAT(use): Object is destroying. object_id = [%d]", id).c_str() );
			VERIFY2(!Obj->getDestroy(),
			        make_string("GEG_PLAYER_ITEM_EAT(use): Object is destroying. object_id = [%d]", id).c_str());
			if (Obj->getDestroy())
			{
				//				Msg                                ( "! GEG_PLAYER_ITEM_EAT(use): Object is destroying. object_id = [%d]", id );
				break;
			}

			if (!IsGameTypeSingle() && !g_Alive())
			{
				Msg("! WARNING: dead player [%d][%s] can't use items [%d][%s]",
				    ID(), Name(), Obj->ID(), Obj->cNameSect().c_str());
				break;
			}

			if (type == GEG_PLAYER_ACTIVATEARTEFACT)
			{
				CArtefact* pArtefact = smart_cast<CArtefact*>(Obj);
				//			R_ASSERT2( pArtefact, make_string("GEG_PLAYER_ACTIVATEARTEFACT: Artefact not found. artefact_id = [%d]", id).c_str() );
				VERIFY2(pArtefact,
				        make_string("GEG_PLAYER_ACTIVATEARTEFACT: Artefact not found. artefact_id = [%d]", id).c_str());
				if (!pArtefact)
				{
					Msg("! GEG_PLAYER_ACTIVATEARTEFACT: Artefact not found. artefact_id = [%d]", id);
					break; //1
				}

				pArtefact->ActivateArtefact();
				break; //1
			}

			PIItem iitem = smart_cast<CInventoryItem*>(Obj);
			R_ASSERT(iitem);

			switch (type)
			{
			case GEG_PLAYER_ITEM2SLOT:
				{
					u16 slot_id = P.r_u16();
					WPN_TRACE("Actor::OnEvent GEG_PLAYER_ITEM2SLOT actor=%u item=%u slot=%u item_name=%s",
						ID(), iitem->object_id(), slot_id, iitem->object().cName().c_str());
					inventory().Slot(slot_id, iitem);

					// Dedicated-single bridge:
					// slot event is received, but client-side Activate() path is server-gated.
					// For local controlled actor, raise slotted hud item immediately.
					game_cl_GameState* game_cl = smart_cast<game_cl_GameState*>(&Game());
					const bool local_player_id_match =
						game_cl && game_cl->local_player && game_cl->local_player->GameID == ID();

					if (OnClient() && !g_dedicated_server &&
						(local_player_id_match || Level().CurrentControlEntity() == this || Level().CurrentEntity() == this))
					{
						const u16 next_slot = inventory().GetNextActiveSlot();
						if (slot_id != NO_ACTIVE_SLOT && next_slot == NO_ACTIVE_SLOT)
						{
							WPN_TRACE("Actor::OnEvent GEG_PLAYER_ITEM2SLOT ignored stale local re-activate actor=%u slot=%u next_slot=%u",
								ID(), slot_id, next_slot);
							break;
						}

						inventory().SetActiveSlot(slot_id);
						PIItem slotted_item = inventory().ItemFromSlot(slot_id);
						if (slotted_item)
							slotted_item->ActivateItem();
						WPN_TRACE("Actor::OnEvent GEG_PLAYER_ITEM2SLOT local client apply actor=%u slot=%u slotted_item=%s",
							ID(), slot_id, slotted_item ? slotted_item->object().cName().c_str() : "<none>");
					}
				}
				break; //2
			case GEG_PLAYER_ITEM2BELT:
				inventory().Belt(iitem);
				break; //2
			case GEG_PLAYER_ITEM2RUCK:
				inventory().Ruck(iitem);
				break; //2
			case GEG_PLAYER_ITEM_EAT:
				extern luabind::functor<bool>* CInventory__eat;
				if (iitem && CInventory__eat)
				{
					CGameObject* GO = iitem->cast_game_object();
					if (GO && (*CInventory__eat)(GO->lua_game_object()))
					{
						inventory().Eat(iitem);
						break; //2
					}
				}
			} //switch
		}
		break; //1
	case GEG_PLAYER_ACTIVATE_SLOT:
		{
			u16 slot_id;
			P.r_u16(slot_id);
			WPN_TRACE("Actor::OnEvent GEG_PLAYER_ACTIVATE_SLOT actor=%u slot=%u local=%d on_client=%d",
				ID(), slot_id, Local() ? 1 : 0, OnClient() ? 1 : 0);

			// Dedicated-single bridge:
			// Inventory::Activate() early-outs on client side, so remote/local client
			// receives slot change but does not actually raise item into hands.
			// Mirror CActorMP behavior and force local slot/apply on client.
			game_cl_GameState* game_cl = smart_cast<game_cl_GameState*>(&Game());
			const bool local_player_id_match =
				game_cl && game_cl->local_player && game_cl->local_player->GameID == ID();

			if (OnClient() && !g_dedicated_server &&
				(local_player_id_match || Level().CurrentControlEntity() == this || Level().CurrentEntity() == this))
			{
				const u16 next_slot = inventory().GetNextActiveSlot();
				if (slot_id != NO_ACTIVE_SLOT && next_slot == NO_ACTIVE_SLOT)
				{
					WPN_TRACE("Actor::OnEvent GEG_PLAYER_ACTIVATE_SLOT ignored stale local re-activate actor=%u slot=%u next_slot=%u",
						ID(), slot_id, next_slot);
					break;
				}

				inventory().SetActiveSlot(slot_id);
				WPN_TRACE("Actor::OnEvent GEG_PLAYER_ACTIVATE_SLOT local apply active_slot=%u", inventory().GetActiveSlot());

				if (slot_id != NO_ACTIVE_SLOT)
				{
					PIItem item = inventory().ItemFromSlot(slot_id);
					if (item)
						item->ActivateItem();
					WPN_TRACE("Actor::OnEvent GEG_PLAYER_ACTIVATE_SLOT local activate item=%s",
						item ? item->object().cName().c_str() : "<none>");
				}
			}
			else
			{
				WPN_TRACE("Actor::OnEvent GEG_PLAYER_ACTIVATE_SLOT -> inventory().Activate slot=%u", slot_id);
				inventory().Activate(slot_id);
			}
		}
		break;

	case GEG_PLAYER_DISABLE_SPRINT:
		{
			s8 cmd = P.r_s8();
			m_block_sprint_counter = m_block_sprint_counter + cmd;
			//Msg("m_block_sprint_counter=%d", m_block_sprint_counter);
			if (m_block_sprint_counter > 0)
			{
				mstate_wishful &= ~mcSprint;
			}
			else
				m_block_sprint_counter = 0;
			WPN_TRACE("Actor::OnEvent GEG_PLAYER_DISABLE_SPRINT actor=%u cmd=%d counter=%d sprint_state=%d",
				ID(), cmd, m_block_sprint_counter, (mstate_wishful & mcSprint) ? 1 : 0);
		}
		break;

	case GEG_PLAYER_WEAPON_HIDE_STATE:
		{
			u16 State = P.r_u16();
			BOOL Set = !!P.r_u8();
			WPN_TRACE("Actor::OnEvent GEG_PLAYER_WEAPON_HIDE_STATE actor=%u state=0x%04x set=%d", ID(), State, Set ? 1 : 0);
			inventory().SetSlotsBlocked(State, !!Set);
		}
		break;
	case GE_MOVE_ACTOR:
		{
			Fvector NewPos, NewRot;
			P.r_vec3(NewPos);
			P.r_vec3(NewRot);

			MoveActor(NewPos, NewRot);
		}
		break;
	case GE_ACTOR_MAX_POWER:
		{
			conditions().MaxPower();
			conditions().ClearWounds();
		}
		break;
	case GE_ACTOR_MAX_HEALTH:
		{
			SetfHealth(GetMaxHealth());
		}
		break;
	case GEG_PLAYER_ATTACH_HOLDER:
		{
			u16 id = P.r_u16();
			CObject* O = Level().Objects.net_Find(id);
			if (!O)
			{
				Msg("! Error: No object to attach holder [%d]", id);
				break;
			}
			VERIFY(m_holder==NULL);
			CHolderCustom* holder = smart_cast<CHolderCustom*>(O);
			if (!holder->Engaged()) use_Holder(holder);
		}
		break;
	case GEG_PLAYER_DETACH_HOLDER:
		{
			if (!m_holder) break;
			u16 id = P.r_u16();
			CGameObject* GO = smart_cast<CGameObject*>(m_holder);
			VERIFY(id==GO->ID());
			use_Holder(NULL);
		}
		break;
	case GEG_PLAYER_PLAY_HEADSHOT_PARTICLE:
		{
			OnPlayHeadShotParticle(P);
		}
		break;
	case GE_ACTOR_JUMPING:
		{
			/*
			Fvector dir;
			P.r_dir(dir);
			float jump = P.r_float();
			NET_SavedAccel = dir;
			extern float NET_Jump;
			NET_Jump = jump;
			m_bInInterpolation = false;
			mstate_real |= mcJump;
			*/
		}
		break;
	}
}

void CActor::MoveActor(Fvector NewPos, Fvector NewDir)
{
	Fmatrix M = XFORM();
	M.translate(NewPos);
	r_model_yaw = NewDir.y;
	r_torso.yaw = NewDir.y;
	r_torso.pitch = -NewDir.x;
	unaffected_r_torso.yaw = r_torso.yaw;
	unaffected_r_torso.pitch = r_torso.pitch;
	unaffected_r_torso.roll = 0; //r_torso.roll;

	r_torso_tgt_roll = 0;
	cam_Active()->Set(-unaffected_r_torso.yaw, unaffected_r_torso.pitch, unaffected_r_torso.roll);
	ForceTransform(M);

	m_bInInterpolation = false;
}
