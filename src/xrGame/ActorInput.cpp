#include "stdafx.h"
#include <dinput.h>
#include "Actor.h"
#include "Torch.h"
#include "trade.h"
#include "../xrEngine/CameraBase.h"

#ifdef DEBUG
#	include "PHDebug.h"
#endif

#include "hit.h"
#include "PHDestroyable.h"
#include "UIGameSP.h"
#include "inventory.h"
#include "level.h"
#include "game_cl_base.h"
#include "xr_level_controller.h"
#include "UsableScriptObject.h"
#include "actorcondition.h"
#include "actor_input_handler.h"
#include "string_table.h"
#include "UI/UIStatic.h"
#include "UI/UIActorMenu.h"
#include "UI/UIDragDropReferenceList.h"
#include "CharacterPhysicsSupport.h"
#include "InventoryBox.h"
#include "player_hud.h"
#include "space_restrictor.h"
#include "../xrEngine/xr_input.h"
#include "flare.h"
#include "CustomDetector.h"
#include "clsid_game.h"
#include "hudmanager.h"
#include "Weapon.h"
#include "Flashlight.h"
#include "../xrPhysics/IElevatorState.h"
#include "holder_custom.h"
#include "Weapon.h"
#include "CustomOutfit.h"
#include "3rd party/luabind/luabind/luabind.hpp"
#include "weapon_trace.h"
#include "script_game_object.h"

extern u32 hud_adj_mode;

static bool IsWeaponTraceCmd(int cmd)
{
	switch (cmd)
	{
	case kWPN_1:
	case kWPN_2:
	case kWPN_3:
	case kWPN_4:
	case kWPN_5:
	case kWPN_6:
	case kWPN_FIRE:
	case kWPN_RELOAD:
	case kWPN_ZOOM:
	case kWPN_NEXT:
	case kWPN_FUNC:
	case kWPN_FIREMODE_NEXT:
	case kWPN_FIREMODE_PREV:
	case kNEXT_SLOT:
	case kPREV_SLOT:
	case kDROP:
		return true;
	default:
		return false;
	}
}

static LPCSTR WeaponTraceActionName(int cmd)
{
	return id_to_action_name((EGameActions)cmd);
}

namespace
{
bool IsDoorObject(const CGameObject* object)
{
	if (!object)
		return false;

	const CScriptGameObject* script_object = object->lua_game_object();
	return script_object && script_object->m_door;
}

bool IsMPActorSectionName(const CGameObject* object)
{
	return object && (xr_strcmp(object->cNameSect().c_str(), "mp_actor") == 0);
}

bool IsMPActorTalkBlocked(const CActor* actor, const CInventoryOwner* partner)
{
	const CGameObject* actor_object = smart_cast<const CGameObject*>(actor);
	const CGameObject* partner_object = smart_cast<const CGameObject*>(partner);
	return IsMPActorSectionName(actor_object) && IsMPActorSectionName(partner_object);
}

bool IsKnownSleepZoneName(LPCSTR object_name)
{
	if (!object_name || !*object_name)
		return false;

	static LPCSTR kSleepZones[] = {
		"actor_surge_hide_2",
		"agr_army_sleep",
		"agr_sr_sleep_tunnel",
		"agr_sr_sleep_wagon",
		"bar_actor_sleep_zone",
		"cit_merc_sleep",
		"ds_farmhouse_sleep",
		"esc_basement_sleep_area",
		"esc_secret_sleep",
		"gar_angar_sleep",
		"gar_dolg_sleep",
		"jup_a6_sr_sleep",
		"mar_a3_sr_sleep",
		"mil_freedom_sleep",
		"mil_smart_terran_2_4_sleep",
		"pri_a16_sr_sleep",
		"pri_monolith_sleep",
		"pri_room27_sleep",
		"rad_sleep_room",
		"ros_vagon_sleep",
		"val_abandoned_house_sleep",
		"val_vagon_sleep",
		"yan_bunker_sleep_restrictor",
		"zat_a2_sr_sleep",
		"pol_secret_sleep"
	};

	for (LPCSTR sleep_zone : kSleepZones)
	{
		if (0 == xr_strcmp(object_name, sleep_zone))
			return true;
	}

	return false;
}

bool TryUseSleepZoneFallback(CActor* actor, CGameObject* object)
{
	if (!actor)
		return false;

	LPCSTR object_name = "<nearby>";
	u16 object_id = u16(-1);
	if (object)
	{
		if (!smart_cast<CSpaceRestrictor*>(object))
			return false;

		object_name = object->cName().c_str();
		object_id = object->ID();
		if (!IsKnownSleepZoneName(object_name))
			return false;
	}

	::luabind::functor<void> sleeper_use;
	if (!ai().script_engine().functor("ui_sleep_dialog.sleep_in_zone", sleeper_use))
	{
		Msg("! [SLEEP_FALLBACK] missing functor ui_sleep_dialog.sleep_in_zone for [%s][%hu]", object_name, object_id);
		return false;
	}

	CScriptGameObject* actor_lua = actor->lua_game_object();
	CScriptGameObject* object_lua = object ? object->lua_game_object() : actor_lua;
	if (!actor_lua || !object_lua)
		return false;

	try
	{
		sleeper_use(actor_lua, object_lua);
		Msg("* [SLEEP_FALLBACK] invoked for [%s][%hu]", object_name, object_id);
		return true;
	}
	catch (...)
	{
		Msg("! [SLEEP_FALLBACK] invoke failed for [%s][%hu]", object_name, object_id);
	}

	return false;
}
}

void CActor::IR_OnKeyboardPress(int cmd)
{
	const bool trace = IsWeaponTraceCmd(cmd);
	if (trace)
	{
		WPN_TRACE("Actor::IR_OnKeyboardPress actor=%u cmd=%d(%s) local=%d remote=%d alive=%d dedicated=%d on_client=%d on_server=%d",
			ID(), cmd, WeaponTraceActionName(cmd), Local() ? 1 : 0, Remote() ? 1 : 0, g_Alive() ? 1 : 0,
			g_dedicated_server ? 1 : 0, OnClient() ? 1 : 0, OnServer() ? 1 : 0);
	}

	if (hud_adj_mode && pInput->iGetAsyncKeyState(DIK_LSHIFT))
	{
		if (trace)
			WPN_TRACE("Actor::IR_OnKeyboardPress blocked: hud_adj_mode+shift actor=%u cmd=%d", ID(), cmd);
		return;
	}

	if (Remote())
	{
		if (trace)
			WPN_TRACE("Actor::IR_OnKeyboardPress blocked: Remote actor=%u cmd=%d", ID(), cmd);
		return;
	}

	if (IsTalking())
	{
		if (trace)
			WPN_TRACE("Actor::IR_OnKeyboardPress blocked: IsTalking actor=%u cmd=%d", ID(), cmd);
		return;
	}
	if (m_input_external_handler && !m_input_external_handler->authorized(cmd))
	{
		if (trace)
			WPN_TRACE("Actor::IR_OnKeyboardPress blocked: external handler denied actor=%u cmd=%d", ID(), cmd);
		return;
	}

	switch (cmd)
	{
	case kWPN_FIRE:
		{
			u16 slot = inventory().GetActiveSlot();
			if (inventory().ActiveItem() && (slot == INV_SLOT_3 || slot == INV_SLOT_2))
				mstate_wishful &= ~mcSprint;
			if (trace)
			{
				WPN_TRACE("Actor::IR_OnKeyboardPress fire branch actor=%u slot=%u active_item=%s sprint_state=%u",
					ID(), slot, inventory().ActiveItem() ? inventory().ActiveItem()->object().cName().c_str() : "<none>",
					(mstate_wishful & mcSprint) ? 1 : 0);
			}

			// Tronex: export to allow/prevent weapon fire if returned false
			extern luabind::functor<bool>* CActor_Fire;
			const bool allow_fire_lua_callback = (CActor_Fire != nullptr) && !g_dedicated_server;
			if ((CActor_Fire != nullptr) && g_dedicated_server && trace)
			{
				// Dedicated server does not have client-side db.actor context used by this callback.
				WPN_TRACE("Actor::IR_OnKeyboardPress fire callback skipped on dedicated actor=%u", ID());
			}
			if (allow_fire_lua_callback)
			{
				if (!(*CActor_Fire)())
				{
					if (trace)
						WPN_TRACE("Actor::IR_OnKeyboardPress fire blocked by script callback actor=%u", ID());
					return;
				}
			}

			//-----------------------------
			if (OnServer())
			{
				NET_Packet P;
				P.w_begin(M_PLAYER_FIRE);
				P.w_u16(ID());
				u_EventSend(P);
				if (trace)
					WPN_TRACE("Actor::IR_OnKeyboardPress sent M_PLAYER_FIRE actor=%u", ID());
			}
			else if (trace)
				WPN_TRACE("Actor::IR_OnKeyboardPress fire: OnServer=false, no M_PLAYER_FIRE actor=%u", ID());
		}
		break;
	default:
		{
		}
		break;
	}

	if (g_Alive())
	{
		if (m_holder && kUSE != cmd)
		{
			m_holder->OnKeyboardPress(cmd);
			if (m_holder->allowWeapon())
			{
				const bool consumed = inventory().Action((u16)cmd, CMD_START);
				if (trace)
					WPN_TRACE("Actor::IR_OnKeyboardPress holder->inventory action actor=%u cmd=%d consumed=%d",
						ID(), cmd, consumed ? 1 : 0);
				if (consumed) return;
			}
			else if (trace)
				WPN_TRACE("Actor::IR_OnKeyboardPress holder present but allowWeapon=false actor=%u cmd=%d", ID(), cmd);
			if (trace)
				WPN_TRACE("Actor::IR_OnKeyboardPress exit after holder branch actor=%u cmd=%d", ID(), cmd);
			return;
		}
		else
		{
			const bool consumed = inventory().Action((u16)cmd, CMD_START);
			if (trace)
				WPN_TRACE("Actor::IR_OnKeyboardPress inventory action actor=%u cmd=%d consumed=%d",
					ID(), cmd, consumed ? 1 : 0);
			if (consumed) return;
		}
	}


	if(psActorFlags.test(AF_NO_CLIP))
	{
		if (trace)
			WPN_TRACE("Actor::IR_OnKeyboardPress no-clip branch actor=%u cmd=%d", ID(), cmd);
		NoClipFly(cmd);
		return;
	}

	if (!g_Alive())
	{
		if (trace)
			WPN_TRACE("Actor::IR_OnKeyboardPress blocked after inventory: actor dead actor=%u cmd=%d", ID(), cmd);
		return;
	}

	switch (cmd)
	{
	case kJUMP:
		{
			mstate_wishful |= mcJump;
		}
		break;
	case kSPRINT_TOGGLE:
		{
			if (psActorFlags.test(AF_SPRINT_TOGGLE))
			{
				if (psActorFlags.test(AF_WALK_TOGGLE)) mstate_wishful &= ~mcAccel;
				if (psActorFlags.test(AF_CROUCH_TOGGLE)) mstate_wishful &= ~mcCrouch;
				mstate_wishful ^= mcSprint;
			}
		}
		break;
	case kFREELOOK:
	{
		if (psActorFlags.test(AF_FREELOOK_TOGGLE))
		{
			if (cam_freelook == eflDisabled && CanUseFreelook())
			{
				cam_SetFreelook();
			}
			else if (cam_freelook == eflEnabled)
			{
				cam_UnsetFreelook();
			}
		}
	}
	break;
	case kCROUCH:
		{
			if (psActorFlags.test(AF_CROUCH_TOGGLE))
				mstate_wishful ^= mcCrouch;
		}
		break;
	case kACCEL:
		{
			if (psActorFlags.test(AF_WALK_TOGGLE))
				mstate_wishful ^= mcAccel;
		}
		break;
	case kL_LOOKOUT:
		{
			if (psActorFlags.test(AF_LOOKOUT_TOGGLE))
			{
				mstate_wishful &= ~mcRLookout;
				mstate_wishful ^= mcLLookout;
			}
		}
		break;
	case kR_LOOKOUT:
		{
			if (psActorFlags.test(AF_LOOKOUT_TOGGLE))
			{
				mstate_wishful &= ~mcLLookout;
				mstate_wishful ^= mcRLookout;
			}
		}
		break;
	case kCAM_1: cam_Set(eacFirstEye);
		break;
	case kCAM_2: cam_Set(eacLookAt);
		break;
	case kCAM_3: cam_Set(eacFreeLook);
		break;
	case kNIGHT_VISION:
		{
			//SwitchNightVision(); //Rezy: now it's controlled via LUA scripts for timing and animations
			break;
		}
	case kTORCH:
		{
			//SwitchTorch(); //Tronex: now it's controlled via LUA scripts for timing and animations
			break;
		}

	case kDETECTOR:
		{
			PIItem dev_active = inventory().ItemFromSlot(DETECTOR_SLOT);
			if (dev_active)
			{
				CCustomDevice* dev = smart_cast<CCustomDevice*>(dev_active);
				if (dev)
					dev->ToggleDevice(g_player_hud->attached_item(0) != NULL);
			}
		}
		break;
		/*
			case kFLARE:{
					PIItem fl_active = inventory().ItemFromSlot(FLARE_SLOT);
					if(fl_active)
					{
						CFlare* fl			= smart_cast<CFlare*>(fl_active);
						fl->DropFlare		();
						return				;
					}
		
					PIItem fli = inventory().Get(CLSID_DEVICE_FLARE, true);
					if(!fli)			return;
		
					CFlare* fl			= smart_cast<CFlare*>(fli);
					
					if(inventory().Slot(fl))
						fl->ActivateFlare	();
				}break;
		*/
	case kUSE:
		ActorUse();
		break;
	case kDROP:
		b_DropActivated = TRUE;
		f_DropPower = 0;
		break;
	case kNEXT_SLOT:
		{
			OnNextWeaponSlot();
		}
		break;
	case kPREV_SLOT:
		{
			OnPrevWeaponSlot();
		}
		break;

	case kQUICK_USE_1:
	case kQUICK_USE_2:
	case kQUICK_USE_3:
	case kQUICK_USE_4:
		{
			const shared_str& item_name = g_quick_use_slots[cmd - kQUICK_USE_1];
			if (item_name.size())
			{
				PIItem itm = inventory().GetAny(item_name.c_str());

				extern luabind::functor<bool>* CInventory__eat;
				if (itm && CInventory__eat)
				{
					CGameObject* GO = itm->cast_game_object();
					if (GO && (*CInventory__eat)(GO->lua_game_object()))
					{
						if (OnServer())
							inventory().Eat(itm);
						else
							inventory().ClientEat(itm);

						StaticDrawableWrapper* _s = CurrentGameUI()->AddCustomStatic("item_used", true);
						string1024 str;
						strconcat(sizeof(str), str, *CStringTable().translate("st_item_used"), ": ", itm->NameItem());
						_s->wnd()->TextItemControl()->SetText(str);
					}
				}
			}
		}
		break;
	}
}

// demonized: switch to disable mouse wheel weapon change
BOOL mouseWheelChangeWeapon = TRUE;

// demonized: switch to invert mouse wheel weapon change
BOOL mouseWheelInvertChangeWeapons = FALSE;

// mbehm: switch to allow inverting mouse wheel zoom direction
BOOL mouseWheelInvertZoom = FALSE;
void CActor::IR_OnMouseWheel(int direction)
{
	WPN_TRACE("Actor::IR_OnMouseWheel actor=%u direction=%d dedicated=%d", ID(), direction, g_dedicated_server ? 1 : 0);
	if (g_dedicated_server)
	{
		WPN_TRACE("Actor::IR_OnMouseWheel blocked: dedicated actor=%u", ID());
		return;
	}

	if (hud_adj_mode)
	{
		g_player_hud->tune(Ivector().set(0, 0, direction));
		WPN_TRACE("Actor::IR_OnMouseWheel hud tune mode actor=%u direction=%d", ID(), direction);
		return;
	}

	if (mouseWheelInvertZoom) {
		const u16 zoom_cmd = (direction > 0) ? (u16)kWPN_ZOOM_DEC : (u16)kWPN_ZOOM_INC;
		const bool consumed = inventory().Action(zoom_cmd, CMD_START);
		WPN_TRACE("Actor::IR_OnMouseWheel zoom action actor=%u cmd=%d(%s) consumed=%d",
			ID(), zoom_cmd, WeaponTraceActionName(zoom_cmd), consumed ? 1 : 0);
		if (consumed) return;
	} else {
		const u16 zoom_cmd = (direction > 0) ? (u16)kWPN_ZOOM_INC : (u16)kWPN_ZOOM_DEC;
		const bool consumed = inventory().Action(zoom_cmd, CMD_START);
		WPN_TRACE("Actor::IR_OnMouseWheel zoom action actor=%u cmd=%d(%s) consumed=%d",
			ID(), zoom_cmd, WeaponTraceActionName(zoom_cmd), consumed ? 1 : 0);
		if (consumed) return;
	}

	if (mouseWheelChangeWeapon) {
		if (mouseWheelInvertChangeWeapons) {
			if (direction < 0)
				OnNextWeaponSlot();
			else
				OnPrevWeaponSlot();
		} else {
			if (direction > 0)
				OnNextWeaponSlot();
			else
				OnPrevWeaponSlot();
		}
		WPN_TRACE("Actor::IR_OnMouseWheel switched slot actor=%u direction=%d invert=%d",
			ID(), direction, mouseWheelInvertChangeWeapons ? 1 : 0);
	}
}

void CActor::IR_OnKeyboardRelease(int cmd)
{
	const bool trace = IsWeaponTraceCmd(cmd);
	if (trace)
	{
		WPN_TRACE("Actor::IR_OnKeyboardRelease actor=%u cmd=%d(%s) local=%d remote=%d alive=%d dedicated=%d on_client=%d on_server=%d",
			ID(), cmd, WeaponTraceActionName(cmd), Local() ? 1 : 0, Remote() ? 1 : 0, g_Alive() ? 1 : 0,
			g_dedicated_server ? 1 : 0, OnClient() ? 1 : 0, OnServer() ? 1 : 0);
	}

	if (hud_adj_mode && pInput->iGetAsyncKeyState(DIK_LSHIFT))
	{
		if (trace)
			WPN_TRACE("Actor::IR_OnKeyboardRelease blocked: hud_adj_mode+shift actor=%u cmd=%d", ID(), cmd);
		return;
	}

	if (Remote())
	{
		if (trace)
			WPN_TRACE("Actor::IR_OnKeyboardRelease blocked: Remote actor=%u cmd=%d", ID(), cmd);
		return;
	}

	if (m_input_external_handler && !m_input_external_handler->authorized(cmd))
	{
		if (trace)
			WPN_TRACE("Actor::IR_OnKeyboardRelease blocked: external handler denied actor=%u cmd=%d", ID(), cmd);
		return;
	}

	if (g_Alive())
	{
		if (cmd == kUSE && !psActorFlags.test(AF_MULTI_ITEM_PICKUP))
			m_bPickupMode = false;

		if (m_holder)
		{
			m_holder->OnKeyboardRelease(cmd);

			if (m_holder->allowWeapon())
			{
				const bool consumed = inventory().Action((u16)cmd, CMD_STOP);
				if (trace)
					WPN_TRACE("Actor::IR_OnKeyboardRelease holder->inventory action actor=%u cmd=%d consumed=%d",
						ID(), cmd, consumed ? 1 : 0);
				if (consumed) return;
			}
			else if (trace)
				WPN_TRACE("Actor::IR_OnKeyboardRelease holder present but allowWeapon=false actor=%u cmd=%d", ID(), cmd);
			if (trace)
				WPN_TRACE("Actor::IR_OnKeyboardRelease exit after holder branch actor=%u cmd=%d", ID(), cmd);
			return;
		}
		else
		{
			const bool consumed = inventory().Action((u16)cmd, CMD_STOP);
			if (trace)
				WPN_TRACE("Actor::IR_OnKeyboardRelease inventory action actor=%u cmd=%d consumed=%d",
					ID(), cmd, consumed ? 1 : 0);
			if (consumed) return;
		}


		switch (cmd)
		{
		case kJUMP: mstate_wishful &= ~mcJump;
			break;
		case kDROP: if (GAME_PHASE_INPROGRESS == Game().Phase()) g_PerformDrop();
			break;
		}
	}
	else if (trace)
	{
		WPN_TRACE("Actor::IR_OnKeyboardRelease ignored because actor dead actor=%u cmd=%d", ID(), cmd);
	}
}

void CActor::IR_OnKeyboardHold(int cmd)
{
	const bool trace = IsWeaponTraceCmd(cmd);
	if (trace)
		WPN_TRACE("Actor::IR_OnKeyboardHold actor=%u cmd=%d(%s)", ID(), cmd, WeaponTraceActionName(cmd));
	if (g_dedicated_server)
	{
		if (trace)
			WPN_TRACE("Actor::IR_OnKeyboardHold blocked: dedicated actor=%u cmd=%d", ID(), cmd);
		return;
	}

	if (hud_adj_mode && pInput->iGetAsyncKeyState(DIK_LSHIFT))
	{
		if (trace)
			WPN_TRACE("Actor::IR_OnKeyboardHold blocked: hud_adj_mode+shift actor=%u cmd=%d", ID(), cmd);
		return;
	}

	if (Remote())
	{
		if (trace)
			WPN_TRACE("Actor::IR_OnKeyboardHold blocked: Remote actor=%u cmd=%d", ID(), cmd);
		return;
	}
	if(g_Alive())
	{
		if (m_input_external_handler && !m_input_external_handler->authorized(cmd))
		{
			if (trace)
				WPN_TRACE("Actor::IR_OnKeyboardHold blocked: external handler denied actor=%u cmd=%d", ID(), cmd);
			return;
		}
		if (IsTalking())
		{
			if (trace)
				WPN_TRACE("Actor::IR_OnKeyboardHold blocked: IsTalking actor=%u cmd=%d", ID(), cmd);
			return;
		}

		if (m_holder)
		{
			m_holder->OnKeyboardHold(cmd);
			if (trace)
				WPN_TRACE("Actor::IR_OnKeyboardHold redirected to holder actor=%u cmd=%d", ID(), cmd);
			return;
		}
	}
	else if (trace)
	{
		WPN_TRACE("Actor::IR_OnKeyboardHold ignored because actor dead actor=%u cmd=%d", ID(), cmd);
	}


	if(psActorFlags.test(AF_NO_CLIP) && (cmd==kFWD || cmd==kBACK || cmd==kL_STRAFE || cmd==kR_STRAFE 
		|| cmd==kJUMP || cmd==kCROUCH))
	{
		NoClipFly(cmd);
		return;
	}

	if (!g_Alive()) return;

	float LookFactor = GetLookFactor();
	switch (cmd)
	{
	case kUP:
	case kDOWN:
		if(cam_freelook != eflEnabling && cam_freelook != eflDisabling) cam_Active()->Move((cmd == kUP) ? kDOWN : kUP, 0, LookFactor);
		break;
	case kCAM_ZOOM_IN:
	case kCAM_ZOOM_OUT:
		cam_Active()->Move(cmd);
		break;
	case kLEFT:
	case kRIGHT:
		if (eacFreeLook != cam_active && cam_freelook != eflEnabling && cam_freelook != eflDisabling) cam_Active()->Move(cmd, 0, LookFactor);
		break;
	case kL_STRAFE: mstate_wishful |= mcLStrafe;
		break;
	case kR_STRAFE: mstate_wishful |= mcRStrafe;
		break;
	case kL_LOOKOUT:
		{
			if (!psActorFlags.test(AF_LOOKOUT_TOGGLE) && cam_freelook == eflDisabled)
				mstate_wishful |= mcLLookout;
		}
		break;
	case kR_LOOKOUT:
		{
			if (!psActorFlags.test(AF_LOOKOUT_TOGGLE) && cam_freelook == eflDisabled)
				mstate_wishful |= mcRLookout;
		}
		break;
	case kFWD: mstate_wishful |= mcFwd;
		break;
	case kBACK: mstate_wishful |= mcBack;
		break;
	case kCROUCH:
		{
			if (!psActorFlags.test(AF_CROUCH_TOGGLE))
				mstate_wishful |= mcCrouch;
		}
		break;
	case kACCEL:
		{
			if (!psActorFlags.test(AF_WALK_TOGGLE))
				mstate_wishful |= mcAccel;
		}
	break;
	case kFREELOOK:
	{
		if (!psActorFlags.test(AF_FREELOOK_TOGGLE))
		{
			if (cam_freelook == eflDisabled && CanUseFreelook())
			{
				cam_SetFreelook();
			}
		}
	}
	break;
	case kSPRINT_TOGGLE:
		{
			if (!psActorFlags.test(AF_SPRINT_TOGGLE))
			{
				if (psActorFlags.test(AF_WALK_TOGGLE)) mstate_wishful &= ~mcAccel;
				if (psActorFlags.test(AF_CROUCH_TOGGLE)) mstate_wishful &= ~mcCrouch;
				mstate_wishful |= mcSprint;
			}
		}
		break;
	}
}

void CActor::IR_OnMouseMove(int dx, int dy)
{
	if (g_dedicated_server) return;

	if (hud_adj_mode)
	{
		g_player_hud->tune(Ivector().set(dx, dy, 0));
		return;
	}

	PIItem iitem = inventory().ActiveItem();
	if (iitem && iitem->cast_hud_item())
		iitem->cast_hud_item()->ResetSubStateTime();

	if (Remote()) return;

	if (m_holder)
	{
		m_holder->OnMouseMove(dx, dy);
		return;
	}

	if (cam_freelook == eflEnabling || cam_freelook == eflDisabling)
		return;

	const float LookFactor = GetLookFactor();

	CCameraBase* C = cameras[cam_active];
    float scale = (C->f_fov / g_fov) * (psMouseSens * sens_multiple) * psMouseSensScale / 50.f / LookFactor;
	if (dx)
	{
		float d = float(dx) * scale;
		cam_Active()->Move((d < 0) ? kLEFT : kRIGHT, _abs(d));
	}
	if (dy)
	{
		float d = ((psMouseInvert.test(1)) ? -1 : 1) * float(dy) * scale * psMouseSensVerticalK * 3.f / 4.f;
		cam_Active()->Move((d > 0) ? kUP : kDOWN, _abs(d));
	}
}

#include "HudItem.h"

bool CActor::use_Holder(CHolderCustom* holder)
{
	if (m_holder)
	{
		bool b = use_HolderEx(0, false);

		if (inventory().ActiveItem())
		{
			CHudItem* hi = smart_cast<CHudItem*>(inventory().ActiveItem());
			if (hi) hi->OnAnimationEnd(hi->GetState());
		}

		return b;
	}
	else
	{
		bool b = use_HolderEx(holder, false);

		if (b)
		{
			//used succesfully
			// switch off torch...
			CAttachableItem* I = CAttachmentOwner::attachedItem(CLSID_DEVICE_TORCH);
			if (I)
			{
				CTorch* torch = smart_cast<CTorch*>(I);
				if (torch) torch->Switch(false);
			}
		}

		if (inventory().ActiveItem())
		{
			CHudItem* hi = smart_cast<CHudItem*>(inventory().ActiveItem());
			if (hi) hi->OnAnimationEnd(hi->GetState());
		}

		return b;
	}
}

void CActor::ActorUse()
{
	static u32 s_last_door_use_time = 0;
	static u16 s_last_door_use_id = u16(-1);

	bool used_script_use = false;

	if (m_holder)
	{
		CGameObject* GO = smart_cast<CGameObject*>(m_holder);
		NET_Packet P;
		CGameObject::u_EventGen(P, GEG_PLAYER_DETACH_HOLDER, ID());
		P.w_u16(GO->ID());
		CGameObject::u_EventSend(P);
		return;
	}

	if (!psActorFlags.test(AF_MULTI_ITEM_PICKUP))
		m_bPickupMode = true;

	if (character_physics_support()->movement()->PHCapture())
		character_physics_support()->movement()->PHReleaseObject();


	if (m_pUsableObject && m_pObjectWeLookingAt && NULL == m_pObjectWeLookingAt->cast_inventory_item())
	{
		if (IsDoorObject(m_pObjectWeLookingAt))
		{
			const u32 now = Device.dwTimeGlobal;
			if (s_last_door_use_id == m_pObjectWeLookingAt->ID() && (now - s_last_door_use_time) < 250)
				return;
		}

		used_script_use = m_pUsableObject->use(this);
		if (used_script_use && IsDoorObject(m_pObjectWeLookingAt))
		{
			s_last_door_use_id = m_pObjectWeLookingAt->ID();
			s_last_door_use_time = Device.dwTimeGlobal;
		}
	}

	if (m_pInvBoxWeLookingAt && m_pInvBoxWeLookingAt->nonscript_usable())
	{
		CUIGameSP* pGameSP = smart_cast<CUIGameSP*>(CurrentGameUI());
		if (pGameSP) //single
		{
			if (!m_pInvBoxWeLookingAt->closed())
			{
				pGameSP->StartCarBody(this, m_pInvBoxWeLookingAt);
			}
		}
		return;
	}

	if (!m_pUsableObject || m_pUsableObject->nonscript_usable())
	{
		bool bCaptured = false;

		collide::rq_result& RQ = HUD().GetRQ();
		CPhysicsShellHolder* object = smart_cast<CPhysicsShellHolder*>(RQ.O);
		u16 element = BI_NONE;
		if (object)
			element = (u16)RQ.element;

		if (object && Level().IR_GetKeyState(DIK_LSHIFT))
		{
			bool b_allow = !!pSettings->line_exist("ph_capture_visuals", object->cNameVisual());
			extern luabind::functor<bool>* CActor__OnBeforePHCapture;
			if (CActor__OnBeforePHCapture)
				b_allow = (*CActor__OnBeforePHCapture)(object->lua_game_object(), b_allow);

			if (b_allow && !character_physics_support()->movement()->PHCapture())
			{
				character_physics_support()->movement()->PHCaptureObject(object, element);
				bCaptured = true;
			}
		}
		else
		{
			if (object && smart_cast<CHolderCustom*>(object))
			{
				NET_Packet P;
				CGameObject::u_EventGen(P, GEG_PLAYER_ATTACH_HOLDER, ID());
				P.w_u16(object->ID());
				CGameObject::u_EventSend(P);
				return;
			}
		}

		if (m_pPersonWeLookingAt)
		{
			CEntityAlive* pEntityAliveWeLookingAt =
				smart_cast<CEntityAlive*>(m_pPersonWeLookingAt);

			VERIFY(pEntityAliveWeLookingAt);

			if (pEntityAliveWeLookingAt->g_Alive())
			{
				if (IsMPActorTalkBlocked(this, m_pPersonWeLookingAt))
				{
					WPN_TRACE("Actor::ActorUse talk blocked for mp_actor pair actor=%u partner=%u",
						ID(), m_pPersonWeLookingAt->object_id());
				}
				else
				{
					TryToTalk();
				}
			}
			else if (!bCaptured)
			{
				// Single-player mode only.
				CUIGameSP* pGameSP = smart_cast<CUIGameSP*>(CurrentGameUI());
				if (pGameSP)
				{
					if (!m_pPersonWeLookingAt->deadbody_closed_status())
					{
						if (pEntityAliveWeLookingAt->AlreadyDie() &&
							pEntityAliveWeLookingAt->GetLevelDeathTime() + 3000 < Device.dwTimeGlobal)
							// 99.9% dead
							pGameSP->StartCarBody(this, m_pPersonWeLookingAt);
					}
				}
			}
		}
	}

	// Final fallback for sleeping zones: when no actionable object is targeted,
	// or when the targeted object is a sleep restrictor without a working use callback.
	if (!used_script_use && (!m_pObjectWeLookingAt || smart_cast<CSpaceRestrictor*>(m_pObjectWeLookingAt)))
		TryUseSleepZoneFallback(this, m_pObjectWeLookingAt);
}
extern BOOL firstPersonDeath;
BOOL CActor::HUDview() const
{
	if (g_dedicated_server) return FALSE;
	if (Level().CurrentControlEntity() != this) return FALSE;
	return IsFocused() && cam_active == eacFirstEye &&
		(!m_holder || m_holder && m_holder->allowWeapon() && m_holder->HUDView()) && (firstPersonDeath ? g_Alive() : true);
}

static u16 SlotsToCheck [] = {
	KNIFE_SLOT, // 0
	INV_SLOT_2, // 1
	INV_SLOT_3, // 2
	GRENADE_SLOT, // 3
	ARTEFACT_SLOT, // 10
	PDA_SLOT
};

void CActor::OnNextWeaponSlot()
{
	u32 ActiveSlot = inventory().GetActiveSlot();
	if (ActiveSlot == NO_ACTIVE_SLOT)
		ActiveSlot = inventory().GetPrevActiveSlot();

	if (ActiveSlot == NO_ACTIVE_SLOT)
		ActiveSlot = KNIFE_SLOT;

	u32 NumSlotsToCheck = sizeof(SlotsToCheck) / sizeof(SlotsToCheck[0]);

	u32 CurSlot = 0;
	for (; CurSlot < NumSlotsToCheck; CurSlot++)
	{
		if (SlotsToCheck[CurSlot] == ActiveSlot) break;
	};

	if (CurSlot >= NumSlotsToCheck)
		return;

	for (u32 i = CurSlot + 1; i < NumSlotsToCheck; i++)
	{
		if (inventory().ItemFromSlot(SlotsToCheck[i]))
		{
			if (SlotsToCheck[i] == ARTEFACT_SLOT)
			{
				IR_OnKeyboardPress(kARTEFACT);
			}
			else if (SlotsToCheck[i] == PDA_SLOT)
			{
				IR_OnKeyboardPress(kACTIVE_JOBS);
			}
			else
				IR_OnKeyboardPress(kWPN_1 + i);
			return;
		}
	}
};

void CActor::OnPrevWeaponSlot()
{
	u32 ActiveSlot = inventory().GetActiveSlot();
	if (ActiveSlot == NO_ACTIVE_SLOT)
		ActiveSlot = inventory().GetPrevActiveSlot();

	if (ActiveSlot == NO_ACTIVE_SLOT)
		ActiveSlot = KNIFE_SLOT;

	u32 NumSlotsToCheck = sizeof(SlotsToCheck) / sizeof(SlotsToCheck[0]);
	u32 CurSlot = 0;

	for (; CurSlot < NumSlotsToCheck; CurSlot++)
	{
		if (SlotsToCheck[CurSlot] == ActiveSlot) break;
	};

	if (CurSlot >= NumSlotsToCheck)
		CurSlot = NumSlotsToCheck - 1; //last in row

	for (s32 i = s32(CurSlot - 1); i >= 0; i--)
	{
		if (inventory().ItemFromSlot(SlotsToCheck[i]))
		{
			if (SlotsToCheck[i] == ARTEFACT_SLOT)
			{
				IR_OnKeyboardPress(kARTEFACT);
			}
			else if (SlotsToCheck[i] == PDA_SLOT)
			{
				IR_OnKeyboardPress(kACTIVE_JOBS);
			}
			else
				IR_OnKeyboardPress(kWPN_1 + i);
			return;
		}
	}
};

extern float g_AimLookFactor;

// momopate: Optional restoration of the handling gimmick from the original games
BOOL g_allow_weapon_control_inertion_factor = 0;
BOOL g_allow_outfit_control_inertion_factor = 0;

float CActor::GetLookFactor()
{
	if (g_dedicated_server) return 0;
	if (m_input_external_handler)
		return m_input_external_handler->mouse_scale_factor();

	float factor = 1.f;

	if (m_bZoomAimingMode)
		factor /= g_AimLookFactor;

	if (g_allow_weapon_control_inertion_factor)
	{
		PIItem pItem = inventory().ActiveItem();
		if (pItem)
			factor *= pItem->GetControlInertionFactor();
	}

	if (g_allow_outfit_control_inertion_factor)
	{
		CCustomOutfit* outfit = GetOutfit();
		if (outfit)
			factor *= outfit->GetControlInertionFactor();
	}

    VERIFY(!fis_zero(factor));

	if (cam_freelook != eflDisabled)
		return 1.5f;

	return factor;
}

void CActor::set_input_external_handler(CActorInputHandler* handler)
{
	// clear state
	if (handler)
		mstate_wishful = 0;

	// release fire button
	if (handler)
		IR_OnKeyboardRelease(kWPN_FIRE);

	// set handler
	m_input_external_handler = handler;
}

void CActor::SwitchNightVision()
{
	SwitchNightVision(!m_bNightVisionOn);
}

void CActor::SwitchTorch()
{
	CTorch* pTorch = smart_cast<CTorch*>(inventory().ItemFromSlot(TORCH_SLOT));
	if (pTorch)
		pTorch->Switch();
}

void CActor::NoClipFly(int cmd)
{
	Fvector cur_pos, right, left;
	cur_pos.set(0, 0, 0);
	float scale = 5.f;
	if (pInput->iGetAsyncKeyState(DIK_LSHIFT))
		scale = 2.0f;
	else if (pInput->iGetAsyncKeyState(DIK_X))
		scale = 7.0f;
	else if (pInput->iGetAsyncKeyState(DIK_LMENU))
		scale = 10.0f;
	else if (pInput->iGetAsyncKeyState(DIK_CAPSLOCK))
		scale = 15.0f;
	else if (pInput->iGetAsyncKeyState(DIK_DELETE)) {
		Fvector result;
		collide::rq_result RQ;
		BOOL HasPick = Level().ObjectSpace.RayPick(Device.vCameraPosition, Device.vCameraDirection, 1000.0f,
			collide::rqtBoth, RQ, Level().CurrentControlEntity());
		result = Fvector(Device.vCameraPosition).add(Fvector(Device.vCameraDirection).mul(RQ.range));

		if (HasPick)
			SetPhPosition(XFORM().translate(result));
	}

	switch (cmd) {
	case kJUMP:
		cur_pos.y += 2.f;
		break;
	case kCROUCH:
		cur_pos.y -= 2.f;
		break;
	case kFWD:
		cur_pos.mad(cam_Active()->vDirection, scale / 2.0f);
		if (m_pPhysicsShell)
			m_pPhysicsShell->applyImpulseTrace(cur_pos, cam_Active()->vDirection, scale * 100);
		break;
	case kBACK:
		cur_pos.mad(cam_Active()->vDirection, -scale / 2.0f);
		if (m_pPhysicsShell)
			m_pPhysicsShell->applyImpulseTrace(cur_pos, Fvector(cam_Active()->vDirection).invert(), scale * 100);
		break;
	case kL_STRAFE:
		left.crossproduct(cam_Active()->vNormal, cam_Active()->vDirection);
		cur_pos.mad(left, -scale / 2.0f);
		if (m_pPhysicsShell)
			m_pPhysicsShell->applyImpulseTrace(cur_pos, left, -scale * 100);
		break;
	case kR_STRAFE:
		right.crossproduct(cam_Active()->vNormal, cam_Active()->vDirection);
		cur_pos.mad(right, scale / 2.0f);
		if (m_pPhysicsShell)
			m_pPhysicsShell->applyImpulseTrace(cur_pos, right, scale * 100);
		break;
	case kCAM_1:
		cam_Set(eacFirstEye);
		break;
	case kCAM_2:
		cam_Set(eacLookAt);
		break;
	case kCAM_3:
		cam_Set(eacFreeLook);
		break;
	case kNIGHT_VISION:
		SwitchNightVision();
		break;
	case kTORCH:
		SwitchTorch();
		break;
	case kUSE:
		ActorUse();
		break;
	case kDETECTOR:
	{
		PIItem dev_active = inventory().ItemFromSlot(DETECTOR_SLOT);
		if (dev_active)
		{
			CCustomDevice* dev = smart_cast<CCustomDevice*>(dev_active);
			if (dev)
				dev->ToggleDevice(g_player_hud->attached_item(0) != NULL);
		}
	}break;
	}
	SetPhPosition(XFORM().translate_add(cur_pos.mul(scale * Device.fTimeDelta)));
}

