#include "stdafx.h"
#include "missile.h"
//.#include "WeaponHUD.h"
#include "../xrphysics/PhysicsShell.h"
#include "../xrphysics/PHCollideValidator.h"
#include "actor.h"
#include "../xrEngine/CameraBase.h"
#include "xrserver_objects_alife.h"
#include "ActorEffector.h"
#include "level.h"
#include "xr_level_controller.h"
#include "../Include/xrRender/Kinematics.h"
#include "ai_object_location.h"
#include "../xrphysics/ExtendedGeom.h"
#include "../xrphysics/MathUtils.h"
#include "characterphysicssupport.h"
#include "inventory.h"
#include "../xrEngine/IGame_Persistent.h"
#include "../xrServerEntities/ai_sounds.h"
#include "player_hud.h"
#include "game_cl_base.h"
#include "weapon_trace.h"
#include "explosive.h"

#ifdef DEBUG
#	include "phdebug.h"
#endif


#define PLAYING_ANIM_TIME 10000

#include "ui/UIProgressShape.h"
#include "ui/UIXmlInit.h"
#include "physicsshellholder.h"

CUIProgressShape* g_MissileForceShape = NULL;

namespace
{
	constexpr u32 kDedicatedThrowStartDelayMs = 420;
	constexpr u32 kDedicatedThrowDelayMs = 460;
	constexpr u32 kDedicatedThrowEndDelayMs = 220;
	constexpr u32 kDedicatedAuthoritativeExplodeGraceMs = 220;

	u32 MissileClockNow(const CMissile* missile)
	{
		// Dedicated/host side uses synchronized server clock.
		// Pure client side uses local monotonic device clock to avoid net-time jumps
		// that can instantly expire grenade fuses.
		if (g_pGameLevel && missile && OnServer())
			return Level().timeServer();

		return Device.dwTimeGlobal;
	}

	bool IsDedicatedSingleLocalMissileOwner(const CMissile* missile)
	{
		if (!g_pGameLevel || !missile || g_dedicated_server || !OnClient())
			return false;

		const CActor* actor = smart_cast<const CActor*>(missile->H_Parent());
		if (!actor)
			return false;

		game_cl_GameState* game_cl = smart_cast<game_cl_GameState*>(&Game());
		const bool local_player_id_match =
			game_cl && game_cl->local_player && game_cl->local_player->GameID == actor->ID();

		return local_player_id_match || Level().CurrentControlEntity() == actor || Level().CurrentEntity() == actor;
	}
}

void create_force_progress()
{
	VERIFY(!g_MissileForceShape);
	CUIXml uiXml;
	uiXml.Load(CONFIG_PATH, UI_PATH, "grenade.xml");


	CUIXmlInit xml_init;
	g_MissileForceShape = xr_new<CUIProgressShape>();
	xml_init.InitProgressShape(uiXml, "progress", 0, g_MissileForceShape);
}

CMissile::CMissile(void)
{
	m_dwStateTime = 0;
	m_fake_missile_spawn_pending = false;
	m_waiting_authoritative_explode = false;
}

CMissile::~CMissile(void)
{
}

void CMissile::reinit()
{
	inherited::reinit();
	m_throw = false;
	m_constpower = false;
	m_fThrowForce = 0;
	m_dwDestroyTime = 0xffffffff;
	SetPending(FALSE);
	m_fake_missile = NULL;
	m_fake_missile_spawn_pending = false;
	m_waiting_authoritative_explode = false;
	SetState(eHidden);
}

void CMissile::Load(LPCSTR section)
{
	inherited::Load(section);

	m_fMinForce = pSettings->r_float(section, "force_min");
	m_fConstForce = pSettings->r_float(section, "force_const");
	m_fMaxForce = pSettings->r_float(section, "force_max");
	m_fForceGrowSpeed = pSettings->r_float(section, "force_grow_speed");

	m_dwDestroyTimeMax = pSettings->r_u32(section, "destroy_time");

	m_vThrowPoint = pSettings->r_fvector3(section, "throw_point");
	m_vThrowDir = pSettings->r_fvector3(section, "throw_dir");

	m_ef_weapon_type = READ_IF_EXISTS(pSettings, r_u32, section, "ef_weapon_type", u32(-1));

	if (pSettings->line_exist(section, "snd_draw"))
		m_sounds.LoadSound(section, "snd_draw", "sndShow", false, SOUND_TYPE_ITEM_HIDING);

	if (pSettings->line_exist(section, "snd_holster"))
		m_sounds.LoadSound(section, "snd_holster", "sndHide", false, SOUND_TYPE_ITEM_HIDING);

	if (pSettings->line_exist(section, "snd_throw"))
		m_sounds.LoadSound(section, "snd_throw", "sndThrow", false, SOUND_TYPE_ITEM_HIDING);

	if (pSettings->line_exist(section, "snd_checkout"))
		m_sounds.LoadSound(section, "snd_checkout", "sndCheckout", false, SOUND_TYPE_WEAPON_RECHARGING);

	if (!g_MissileForceShape)
		create_force_progress();
}

BOOL CMissile::net_Spawn(CSE_Abstract* DC)
{
	BOOL l_res = inherited::net_Spawn(DC);

	dwXF_Frame = 0xffffffff;

	m_throw_direction.set(0.0f, 1.0f, 0.0f);
	m_throw_matrix.identity();

	return l_res;
}

void CMissile::net_Destroy()
{
	inherited::net_Destroy();
	m_fake_missile = 0;
	m_fake_missile_spawn_pending = false;
	m_waiting_authoritative_explode = false;
	m_dwStateTime = 0;
}

void CMissile::set_destroy_time(u32 delta_destroy_time)
{
	const u32 now = MissileClockNow(this);
	m_dwDestroyTime = now + delta_destroy_time;
	m_waiting_authoritative_explode = false;
	WPN_TRACE("Missile::set_destroy_time missile=%s id=%u delta=%u now=%u destroy=%u on_client=%d on_server=%d",
		cName().c_str(), ID(), delta_destroy_time, now, m_dwDestroyTime, OnClient() ? 1 : 0, OnServer() ? 1 : 0);
}

int CMissile::time_from_begin_throw() const
{
	const u32 now = MissileClockNow(this);
	return (now + m_dwDestroyTimeMax - m_dwDestroyTime);
}
void CMissile::PH_A_CrPr()
{
	if (m_just_after_spawn)
	{
		CPhysicsShellHolder& obj = CInventoryItem::object();
		VERIFY(obj.Visual());
		IKinematics* K = obj.Visual()->dcast_PKinematics();
		VERIFY(K);
		if (!obj.PPhysicsShell())
		{
			Msg("! ERROR: PhysicsShell is NULL, object [%s][%d]", obj.cName().c_str(), obj.ID());
			return;
		}
		if (!obj.PPhysicsShell()->isFullActive())
		{
			K->CalculateBones_Invalidate();
			K->CalculateBones(TRUE);
		}
		obj.PPhysicsShell()->GetGlobalTransformDynamic(&obj.XFORM());
		K->CalculateBones_Invalidate();
		K->CalculateBones(TRUE);
		obj.spatial_move();
		m_just_after_spawn = false;
	}
}

void CMissile::OnActiveItem()
{
	SwitchState(eShowing);
	inherited::OnActiveItem();
	SetState(eIdle);
	SetNextState(eIdle);
}

void CMissile::OnHiddenItem()
{
	SwitchState(eHiding);
	inherited::OnHiddenItem();
	SetState(eHidden);
	SetNextState(eHidden);
}


void CMissile::spawn_fake_missile()
{
	if (OnClient()) return;
	if (m_fake_missile || m_fake_missile_spawn_pending) return;

	if (!getDestroy())
	{
		m_fake_missile_spawn_pending = true;
		CSE_Abstract* object = Level().spawn_item(
			*cNameSect(),
			Position(),
			(g_dedicated_server) ? u32(-1) : ai_location().level_vertex_id(),
			ID(),
			true
		);

		CSE_ALifeObject* alife_object = smart_cast<CSE_ALifeObject*>(object);
		VERIFY(alife_object);
		alife_object->m_flags.set(CSE_ALifeObject::flCanSave,FALSE);

		NET_Packet P;
		object->Spawn_Write(P,TRUE);
		Level().Send(P, net_flags(TRUE));
		F_entity_Destroy(object);
	}
}

void CMissile::OnH_A_Chield()
{
	inherited::OnH_A_Chield();

	//	if(!m_fake_missile && !smart_cast<CMissile*>(H_Parent())) 
	//		spawn_fake_missile	();
}


void CMissile::OnH_B_Independent(bool just_before_destroy)
{
	inherited::OnH_B_Independent(just_before_destroy);

	if (!just_before_destroy)
	{
		VERIFY(PPhysicsShell());
		PPhysicsShell()->SetAirResistance(0.f, 0.f);
		PPhysicsShell()->set_DynamicScales(1.f, 1.f);

		if (GetState() == eThrow)
		{
			Msg("Throw on reject");
			Throw();
		}
	}

	if (!m_dwDestroyTime && (Local() || IsDedicatedSingleLocalMissileOwner(this)))
	{
		DestroyObject();
		return;
	}
}

extern u32 hud_adj_mode;

void CMissile::UpdateCL()
{
	m_dwStateTime += Device.dwTimeDelta;

	inherited::UpdateCL();

	CActor* pActor = smart_cast<CActor*>(H_Parent());
	if (pActor && pActor == Level().CurrentControlEntity() && !pActor->AnyMove() && this == pActor->inventory().ActiveItem())
	{
		if (hud_adj_mode == 0 && g_player_hud->script_anim_part == u8(-1) && GetState() == eIdle && (Device.dwTimeGlobal - m_dw_curr_substate_time > 20000))
		{
			if (!pActor->is_safemode())
				SwitchState(eBore);

			ResetSubStateTime();
		}
	}


	if (GetState() == eReady)
	{
		if (m_throw)
		{
			SwitchState(eThrow);
		}
		else
		{
			CActor* actor = smart_cast<CActor*>(H_Parent());
			if (actor)
			{
				m_fThrowForce += (m_fForceGrowSpeed * Device.dwTimeDelta) * .001f;
				clamp(m_fThrowForce, m_fMinForce, m_fMaxForce);
			}
		}
	}

	if (g_dedicated_server)
	{
		switch (GetState())
		{
		case eThrowStart:
			{
				if (m_fake_missile && m_dwStateTime > kDedicatedThrowStartDelayMs)
				{
					if (m_throw)
						SwitchState(eThrow);
					else
						SwitchState(eReady);
				}
			}
			break;
		case eThrow:
			{
				if (m_fake_missile && H_Parent() && m_dwStateTime > kDedicatedThrowDelayMs)
				{
					Throw();
					SwitchState(eThrowEnd);
				}
			}
			break;
		case eThrowEnd:
			{
				if (m_dwStateTime > kDedicatedThrowEndDelayMs)
					SwitchState(eShowing);
			}
			break;
		}
	}

	Fvector P;
	Center(P);

	m_sounds.UpdateAllSoundsPositions(P);
}

void CMissile::shedule_Update(u32 dt)
{
	inherited::shedule_Update(dt);
	if (!H_Parent() && getVisible() && m_pPhysicsShell)
	{
		const u32 now = MissileClockNow(this);
		if (m_dwDestroyTime != u32(0xffffffff) && m_dwDestroyTime <= now)
		{
			const bool dedicated_single_server_bridge =
				g_dedicated_server && OnServer() && !OnClient() && g_pGameLevel && (Game().Type() == eGameIDSingle);
			if (dedicated_single_server_bridge && !m_waiting_authoritative_explode)
			{
				m_waiting_authoritative_explode = true;
				m_dwDestroyTime = now + kDedicatedAuthoritativeExplodeGraceMs;
				WPN_TRACE("Missile::shedule_Update arm authoritative explode grace missile=%s id=%u now=%u next=%u",
					cName().c_str(), ID(), now, m_dwDestroyTime);
				return;
			}

			m_waiting_authoritative_explode = false;
			m_dwDestroyTime = 0xffffffff;
			VERIFY(!m_pInventory);
			WPN_TRACE("Missile::shedule_Update destroy timer reached missile=%s id=%u visible=%d has_shell=%d",
				cName().c_str(), ID(), getVisible() ? 1 : 0, m_pPhysicsShell ? 1 : 0);
			Destroy();
			return;
		}
	}
}

void CMissile::State(u32 state, u32 old_state)
{
	switch (state)
	{
	case eShowing:
		{
			if (ParentIsActor() && !g_dedicated_server) g_player_hud->attach_item(this);

			if (g_dedicated_server)
			{
				if (H_Parent())
					setVisible(TRUE);
				SetPending(FALSE);
				SwitchState(eIdle);
				break;
			}

			SetPending(TRUE);
			PlayHUDMotion("anm_show", FALSE, this, GetState(), 1.f, 0.f, false);

			if (m_sounds.FindSoundItem("sndShow", false))
				m_sounds.PlaySound("sndShow", H_Root()->Position(), H_Root(), !!GetHUDmode());
		}
		break;
	case eIdle:
		{
			SetPending(FALSE);
			PlayAnimIdle();
		}
		break;
	case eHiding:
		{
			if (H_Parent())
			{
				if (old_state != eHiding)
				{
					if (g_dedicated_server)
					{
						setVisible(FALSE);
						setEnabled(FALSE);
						SetPending(FALSE);
						SwitchState(eHidden);
						break;
					}

					SetPending(TRUE);
					PlayHUDMotion("anm_hide", TRUE, this, GetState());

					if (m_sounds.FindSoundItem("sndHide", false))
						m_sounds.PlaySound("sndHide", H_Root()->Position(), H_Root(), !!GetHUDmode());
				}
			}
		}
		break;
	case eHidden:
		{
			if (1 /*GetHUD()*/)
			{
				StopCurrentAnimWithoutCallback();
			};

			if (H_Parent())
			{
				setVisible(FALSE);
				setEnabled(FALSE);
			};
			SetPending(FALSE);
		}
		break;
	case eThrowStart:
		{
			SetPending(TRUE);
			m_fThrowForce = m_fMinForce;

			if (g_dedicated_server)
			{
				if (!m_fake_missile && !smart_cast<CMissile*>(H_Parent()))
					spawn_fake_missile();
				break;
			}

			PlayHUDMotion("anm_throw_begin", TRUE, this, GetState());

			if (m_sounds.FindSoundItem("sndCheckout", false))
			{
				Fvector C;
				Center(C);
				PlaySound("sndCheckout", C);
			}
		}
		break;
	case eReady:
		{
			PlayHUDMotion("anm_throw_idle", TRUE, this, GetState());
		}
		break;
	case eThrow:
		{
			SetPending(TRUE);
			m_throw = false;

			if (g_dedicated_server)
				break;

			PlayHUDMotion("anm_throw", TRUE, this, GetState());

			if (m_sounds.FindSoundItem("sndThrow", false))
			{
				Fvector C;
				Center(C);
				PlaySound("sndThrow", C);
			}
		}
		break;
	case eThrowEnd:
		{
			if (g_dedicated_server)
				break;
			SwitchState(eShowing);
		}
		break;
		/*	case eBore:
				{
					PlaySound			(sndPlaying,Position());
					PlayHUDMotion		("anm_bore", TRUE, this, GetState());
				} break;
		*/
	}
}

void CMissile::OnStateSwitch(u32 S, u32 oldState)
{
	m_dwStateTime = 0;
	inherited::OnStateSwitch(S, oldState);
	State(S, oldState);
}


void CMissile::OnAnimationEnd(u32 state)
{
	switch (state)
	{
	case eHiding:
		{
			setVisible(FALSE);
			SwitchState(eHidden);
		}
		break;
	case eShowing:
		{
			setVisible(TRUE);
			SwitchState(eIdle);
		}
		break;
	case eThrowStart:
		{
			if (!m_fake_missile && !smart_cast<CMissile*>(H_Parent()))
				spawn_fake_missile();

			if (m_throw)
				SwitchState(eThrow);
			else
				SwitchState(eReady);
		}
		break;
	case eThrow:
		{
			SwitchState(eThrowEnd);
		}
		break;
	case eThrowEnd:
		{
			SwitchState(eShowing);
		}
		break;
	default:
		inherited::OnAnimationEnd(state);
	}
}


void CMissile::UpdatePosition(const Fmatrix& trans)
{
	XFORM().mul(trans, offset());
}

void CMissile::UpdateXForm()
{
	if (0 == H_Parent()) return;

	// Get access to entity and its visual
	CEntityAlive* E = smart_cast<CEntityAlive*>(H_Parent());

	if (!E) return;

	const CInventoryOwner* parent = smart_cast<const CInventoryOwner*>(E);
	if (parent && parent->use_simplified_visual())
		return;

	if (parent->attached(this))
		return;

	VERIFY(E);
	IKinematics* V = smart_cast<IKinematics*>(E->Visual());
	VERIFY(V);

	// Get matrices
	int boneL = -1, boneR = -1, boneR2 = -1;
	E->g_WeaponBones(boneL, boneR, boneR2);
	if (boneR == -1) return;


	boneL = boneR2;

	Fmatrix mL = V->LL_GetTransform(u16(boneL));
	Fmatrix mR = V->LL_GetTransform(u16(boneR));

	// Calculate
	Fmatrix mRes;
	Fvector R, D,N;
	D.sub(mL.c, mR.c);
	D.normalize_safe();
	R.crossproduct(mR.j, D);
	R.normalize_safe();
	N.crossproduct(D, R);
	N.normalize_safe();
	mRes.set(R,N, D, mR.c);
	mRes.mulA_43(E->XFORM());
	UpdatePosition(mRes);
}

void CMissile::setup_throw_params()
{
	CEntity* entity = smart_cast<CEntity*>(H_Parent());
	VERIFY(entity);
	CInventoryOwner* inventory_owner = smart_cast<CInventoryOwner*>(H_Parent());
	VERIFY(inventory_owner);
	Fmatrix trans;
	trans.identity();
	Fvector FirePos, FireDir;
	if (this == inventory_owner->inventory().ActiveItem())
	{
		CInventoryOwner* io = smart_cast<CInventoryOwner*>(H_Parent());
		if (NULL == io->inventory().ActiveItem())
		{
			Log("current_state", GetState());
			Log("next_state", GetNextState());
			Log("state_time", m_dwStateTime);
			Log("item_sect", cNameSect().c_str());
			Log("H_Parent", H_Parent()->cNameSect().c_str());
		}

		entity->g_fireParams(this, FirePos, FireDir);
	}
	else
	{
		CEntity* parent_entity = smart_cast<CEntity*>(H_Parent());
		if (parent_entity)
			parent_entity->g_fireParams(this, FirePos, FireDir);
		else
		{
			FirePos = XFORM().c;
			FireDir = XFORM().k;
		}
	}
	trans.k.set(FireDir);
	Fvector::generate_orthonormal_basis(trans.k, trans.j, trans.i);
	trans.c.set(FirePos);
	m_throw_matrix.set(trans);
	m_throw_direction.set(trans.k);
}

void CMissile::OnMotionMark(u32 state, const motion_marks& M)
{
	inherited::OnMotionMark(state, M);
	if (state == eThrow && !m_throw)
	{
		if (H_Parent())
			Throw();
	}
}


void CMissile::Throw()
{
#ifndef MASTER_GOLD
	Msg("throw [%d]", Device.dwFrame);
#endif // #ifndef MASTER_GOLD
	VERIFY(smart_cast<CEntity*>(H_Parent()));
	setup_throw_params();

	m_fake_missile->m_throw_direction = m_throw_direction;
	m_fake_missile->m_throw_matrix = m_throw_matrix;
	//.	m_fake_missile->m_throw				= true;
	//.	Msg("fm %d",m_fake_missile->ID());

	CInventoryOwner* inventory_owner = smart_cast<CInventoryOwner*>(H_Parent());
	VERIFY(inventory_owner);
	if (inventory_owner->use_default_throw_force())
		m_fake_missile->m_fThrowForce = m_constpower ? m_fConstForce : m_fThrowForce;
	else
		m_fake_missile->m_fThrowForce = inventory_owner->missile_throw_force();

	m_fThrowForce = m_fMinForce;

	if ((Local() || IsDedicatedSingleLocalMissileOwner(this)) && H_Parent())
	{
		Fvector throw_position = m_fake_missile->m_throw_matrix.c;
		Fvector throw_velocity = m_fake_missile->m_throw_direction;
		if (throw_velocity.square_magnitude() < EPS)
			throw_velocity.set(0.f, 0.f, 1.f);
		else
			throw_velocity.normalize();
		throw_velocity.mul(m_fake_missile->m_fThrowForce);

		if (CEntityAlive* entity_alive = smart_cast<CEntityAlive*>(H_Root()))
		{
			if (entity_alive->character_physics_support())
			{
				Fvector parent_velocity{};
				entity_alive->character_physics_support()->movement()->GetCharacterVelocity(parent_velocity);
				throw_velocity.add(parent_velocity);
			}
		}

		NET_Packet P;
		u_EventGen(P, GE_OWNERSHIP_REJECT, ID());
		P.w_u16(u16(m_fake_missile->ID()));
		P.w_u8(0);
		P.w_vec3(throw_position);
		P.w_vec3(throw_velocity);
		WPN_TRACE("Missile::Throw net payload missile=%s parent=%u fake=%u pos=(%.3f,%.3f,%.3f) vel=(%.3f,%.3f,%.3f) force=%.3f",
			cName().c_str(),
			H_Parent() ? H_Parent()->ID() : u16(-1),
			m_fake_missile ? m_fake_missile->ID() : u16(-1),
			throw_position.x, throw_position.y, throw_position.z,
			throw_velocity.x, throw_velocity.y, throw_velocity.z,
			m_fake_missile ? m_fake_missile->m_fThrowForce : 0.f);
		u_EventSend(P);
	}
}

void CMissile::OnEvent(NET_Packet& P, u16 type)
{
	inherited::OnEvent(P, type);
	u16 id;
	switch (type)
	{
	case GE_OWNERSHIP_TAKE:
		{
			P.r_u16(id);
			CMissile* missile = smart_cast<CMissile*>(Level().Objects.net_Find(id));
			m_fake_missile = missile;
			m_fake_missile_spawn_pending = false;
			missile->H_SetParent(this);
			missile->Position().set(Position());
			break;
		}
	case GE_OWNERSHIP_REJECT:
		{
			P.r_u16(id);
			bool just_before_destroy = FALSE;
			if (!P.r_eof())
				just_before_destroy = !!P.r_u8();

			Fvector drop_position{};
			Fvector drop_velocity{};
			bool has_drop_position = false;
			bool has_drop_velocity = false;

			const u32 unread_payload_after_flag = (P.B.count > P.r_tell()) ? (P.B.count - P.r_tell()) : 0;
			if (unread_payload_after_flag >= sizeof(Fvector))
			{
				P.r_vec3(drop_position);
				if (_valid(drop_position))
					has_drop_position = true;
			}

			const u32 unread_payload_after_pos = (P.B.count > P.r_tell()) ? (P.B.count - P.r_tell()) : 0;
			if (unread_payload_after_pos >= sizeof(Fvector))
			{
				P.r_vec3(drop_velocity);
				if (_valid(drop_velocity))
					has_drop_velocity = true;
			}

			const bool dedicated_single_local_owner =
				OnClient() && !OnServer() && IsDedicatedSingleLocalMissileOwner(this);
			const bool ignore_non_authoritative_transform =
				dedicated_single_local_owner && !has_drop_position && !has_drop_velocity;
			if (ignore_non_authoritative_transform)
				WPN_TRACE("Missile::OnEvent GE_OWNERSHIP_REJECT local echo without payload missile=%s id=%u",
					cName().c_str(), id);

			bool IsFakeMissile = false;
			if (m_fake_missile && (id == m_fake_missile->ID()))
			{
				m_fake_missile = NULL;
				m_fake_missile_spawn_pending = false;
				IsFakeMissile = true;
			}

			CMissile* missile = smart_cast<CMissile*>(Level().Objects.net_Find(id));
			if (!missile)
			{
				break;
			}

			missile->H_SetParent(0, just_before_destroy);

			if (!ignore_non_authoritative_transform && (has_drop_position || has_drop_velocity))
			{
				if (has_drop_position)
				{
					missile->Position().set(drop_position);
					missile->XFORM().c.set(drop_position);
				}

				// Recreate shell for authoritative detach to avoid stale inactive/fixed runtime shell state.
				CPhysicsShell* physics_shell = missile->PPhysicsShell();
				if (physics_shell)
				{
					if (physics_shell->isActive())
						physics_shell->Deactivate();
					xr_delete(missile->m_pPhysicsShell);
					physics_shell = nullptr;
				}

				missile->create_physic_shell();
				physics_shell = missile->PPhysicsShell();
				if (physics_shell)
				{
					// Detached projectile shells can keep fixed/NCStatic state from parent context.
					// Force dynamic world simulation or grenade may stay at throw origin until fuse timeout.
					const u16 element_count = physics_shell->get_ElementsNumber();
					for (u16 element_id = 0; element_id < element_count; ++element_id)
					{
						CPhysicsElement* element = physics_shell->get_ElementByStoreOrder(element_id);
						if (element && element->isFixed())
							element->ReleaseFixed();
					}
					physics_shell->collide_class_bits().set(CPHCollideValidator::cbNCStatic, FALSE);

					Fmatrix physics_xform = missile->XFORM();
					if (has_drop_position)
						physics_xform.c.set(drop_position);

					if (has_drop_velocity)
						physics_shell->Activate(physics_xform, drop_velocity, zero_vel);
					else
						physics_shell->Activate(physics_xform, false);

					physics_shell->Enable();
					physics_shell->EnableCollision();
					physics_shell->set_ApplyByGravity(TRUE);
					physics_shell->SetAirResistance(0.f, 0.f);
					physics_shell->set_DynamicScales(1.f, 1.f);
					physics_shell->SetTransform(physics_xform, mh_unspecified);
					if (has_drop_velocity)
						physics_shell->set_LinearVel(drop_velocity);

					// Keep owner-contact suppression for network detach path as in native throw activation.
					// Without this, projectile can collide with thrower immediately and "explode near start".
					CPhysicsShellHolder* owner_holder = nullptr;
					if (CActor* root_actor = smart_cast<CActor*>(missile->H_Root()))
					{
						owner_holder = smart_cast<CPhysicsShellHolder*>(root_actor);
					}
					if (!owner_holder)
					{
						if (CExplosive* explosive = smart_cast<CExplosive*>(missile))
						{
							const u16 initiator_id = explosive->Initiator();
							if (initiator_id != u16(-1) && g_pGameLevel)
							{
								if (CObject* initiator_object = Level().Objects.net_Find(initiator_id))
									owner_holder = smart_cast<CPhysicsShellHolder*>(initiator_object);
							}
						}
					}
					if (owner_holder)
					{
						physics_shell->remove_ObjectContactCallback(ExitContactCallback);
						physics_shell->set_CallbackData(owner_holder);
						physics_shell->add_ObjectContactCallback(ExitContactCallback);
						WPN_TRACE("Missile::OnEvent GE_OWNERSHIP_REJECT contact owner resolved missile=%s id=%u owner=%u",
							missile->cName().c_str(), missile->ID(), owner_holder->ID());
					}
					else
					{
						WPN_TRACE("Missile::OnEvent GE_OWNERSHIP_REJECT contact owner unresolved missile=%s id=%u initiator_fallback_failed=1",
							missile->cName().c_str(), missile->ID());
					}
					physics_shell->SetAllGeomTraced();

					Fvector shell_velocity{};
					physics_shell->get_LinearVel(shell_velocity);
					WPN_TRACE("Missile::OnEvent GE_OWNERSHIP_REJECT shell recreated missile=%s id=%u active=%d gravity=%d vel=(%.3f,%.3f,%.3f)",
						missile->cName().c_str(),
						missile->ID(),
						physics_shell->isActive() ? 1 : 0,
						physics_shell->get_ApplyByGravity() ? 1 : 0,
						shell_velocity.x, shell_velocity.y, shell_velocity.z);
				}
				missile->setVisible(TRUE);
				missile->setEnabled(TRUE);
				if (CInventoryItem* dropped_item = smart_cast<CInventoryItem*>(missile))
					dropped_item->ClearNetInterpolationQueue();
				if (smart_cast<CExplosive*>(missile) && missile->destroy_time() == u32(0xffffffff))
					missile->set_destroy_time(missile->m_dwDestroyTimeMax);
				missile->processing_activate();
			}

			if (IsFakeMissile && OnClient())
				missile->set_destroy_time(m_dwDestroyTimeMax);
			break;
		}
	}
}

void CMissile::Destroy()
{
	if (Local() || IsDedicatedSingleLocalMissileOwner(this))
		DestroyObject();
}

bool CMissile::Action(u16 cmd, u32 flags)
{
	if (inherited::Action(cmd, flags)) return true;

	switch (cmd)
	{
	case kWPN_FIRE:
		{
			m_constpower = true;
			if (flags & CMD_START)
			{
				if (GetState() == eIdle)
				{
					m_throw = true;
					SwitchState(eThrowStart);
				}
			}
			return true;
		}
		break;

	case kWPN_ZOOM:
		{
			m_constpower = false;
			if (flags & CMD_START)
			{
				m_throw = false;
				if (GetState() == eIdle)
					SwitchState(eThrowStart);
				else if (GetState() == eReady)
				{
					m_throw = true;
				}
			}
			else if (GetState() == eReady || GetState() == eThrowStart || GetState() == eIdle)
			{
				m_throw = true;
				if (GetState() == eReady)
					SwitchState(eThrow);
			}
			return true;
		}
		break;
	}
	return false;
}

void CMissile::UpdateFireDependencies_internal()
{
	if (0 == H_Parent()) return;

	if (GetHUDmode() && !IsHidden())
	{
		R_ASSERT(0); //implement this!!!
		/*
					// 1st person view - skeletoned
					CKinematics* V			= smart_cast<CKinematics*>(GetHUD()->Visual());
					VERIFY					(V);
					V->CalculateBones		();
		
					// fire point&direction
					Fmatrix& parent			= GetHUD()->Transform	();
					m_throw_direction.set	(parent.k);
		*/
	}
	else
	{
		// 3rd person
		Fmatrix& parent = H_Parent()->XFORM();
		m_throw_direction.set(m_vThrowDir);
		parent.transform_dir(m_throw_direction);
	}
}

void CMissile::activate_physic_shell()
{
	if (!smart_cast<CMissile*>(H_Parent()))
	{
		inherited::activate_physic_shell();
		if (m_pPhysicsShell && m_pPhysicsShell->isActive() && OnClient())
		{
			m_pPhysicsShell->add_ObjectContactCallback(ExitContactCallback);
			m_pPhysicsShell->set_CallbackData(smart_cast<CPhysicsShellHolder*>(H_Root()));
		}
		return;
	}

	Fvector l_vel;
	l_vel.set(m_throw_direction);
	l_vel.normalize_safe();
	l_vel.mul(m_fThrowForce);

	Fvector a_vel;
	CInventoryOwner* inventory_owner = smart_cast<CInventoryOwner*>(H_Root());
	if (inventory_owner && inventory_owner->use_throw_randomness())
	{
		float fi, teta, r;
		fi = ::Random.randF(0.f, 2.f * M_PI);
		teta = ::Random.randF(0.f,M_PI);
		r = ::Random.randF(2.f * M_PI, 3.f * M_PI);
		float rxy = r * _sin(teta);
		a_vel.set(rxy * _cos(fi), rxy * _sin(fi), r * _cos(teta));
	}
	else
		a_vel.set(0.f, 0.f, 0.f);

	XFORM().set(m_throw_matrix);

	CEntityAlive* entity_alive = smart_cast<CEntityAlive*>(H_Root());
	if (entity_alive && entity_alive->character_physics_support())
	{
		Fvector parent_vel;
		entity_alive->character_physics_support()->movement()->GetCharacterVelocity(parent_vel);
		l_vel.add(parent_vel);
	}

	R_ASSERT(!m_pPhysicsShell);
	create_physic_shell();
	m_pPhysicsShell->Activate(m_throw_matrix, l_vel, a_vel);
	//	m_pPhysicsShell->AddTracedGeom		();
	m_pPhysicsShell->SetAllGeomTraced();
	m_pPhysicsShell->add_ObjectContactCallback(ExitContactCallback);
	m_pPhysicsShell->set_CallbackData(smart_cast<CPhysicsShellHolder*>(entity_alive));
	//	m_pPhysicsShell->remove_ObjectContactCallback	(ExitContactCallback);
	m_pPhysicsShell->SetAirResistance(0.f, 0.f);
	m_pPhysicsShell->set_DynamicScales(1.f, 1.f);

	IKinematics* kinematics = smart_cast<IKinematics*>(Visual());
	VERIFY(kinematics);
	kinematics->CalculateBones_Invalidate();
	kinematics->CalculateBones(TRUE);
}

void CMissile::net_Relcase(CObject* O)
{
	CHudItem::net_Relcase(O);
	if (PPhysicsShell() && PPhysicsShell()->isActive())
	{
		if (O == smart_cast<CObject*>((CPhysicsShellHolder*)PPhysicsShell()->get_CallbackData()))
		{
			PPhysicsShell()->remove_ObjectContactCallback(ExitContactCallback);
			PPhysicsShell()->set_CallbackData(NULL);
		}
	}
}

void CMissile::create_physic_shell()
{
	//create_box2sphere_physic_shell();
	CInventoryItemObject::CreatePhysicsShell();
}

void CMissile::setup_physic_shell()
{
	R_ASSERT(!m_pPhysicsShell);
	create_physic_shell();
	m_pPhysicsShell->Activate(XFORM(), 0, XFORM()); //,true 
	IKinematics* kinematics = smart_cast<IKinematics*>(Visual());
	R_ASSERT(kinematics);
	kinematics->CalculateBones_Invalidate();
	kinematics->CalculateBones(TRUE);
}

u32 CMissile::ef_weapon_type() const
{
	VERIFY(m_ef_weapon_type != u32(-1));
	return (m_ef_weapon_type);
}


bool CMissile::render_item_ui_query()
{
	bool b_is_active_item = m_pInventory->ActiveItem() == this;
	return b_is_active_item && (GetState() == eReady) && !m_throw && smart_cast<CActor*>(H_Parent());
}

void CMissile::render_item_ui()
{
	if (!H_Parent() || !H_Parent()->cast_actor()) return;

	float k = (m_fThrowForce - m_fMinForce) / (m_fMaxForce - m_fMinForce);
	g_MissileForceShape->SetPos(k);
	g_MissileForceShape->Draw();
}

void CMissile::ExitContactCallback(bool& do_colide, bool bo1, dContact& c, SGameMtl* /*material_1*/,
                                   SGameMtl* /*material_2*/)
{
	dxGeomUserData *gd1 = NULL, *gd2 = NULL;
	if (bo1)
	{
		gd1 = PHRetrieveGeomUserData(c.geom.g1);
		gd2 = PHRetrieveGeomUserData(c.geom.g2);
	}
	else
	{
		gd2 = PHRetrieveGeomUserData(c.geom.g1);
		gd1 = PHRetrieveGeomUserData(c.geom.g2);
	}
	if (gd1 && gd2 && (CPhysicsShellHolder*)gd1->callback_data == gd2->ph_ref_object)
		do_colide = false;
}

bool CMissile::GetBriefInfo(II_BriefInfo& info)
{
	info.clear();
	info.name._set(m_nameShort);
	return true;
}

Fmatrix CMissile::RayTransform()
{
	Fmatrix matrix = Device.mInvView;
	matrix.mulB_43(Fmatrix().translate(m_vThrowPoint));
	float h, p;
	m_vThrowDir.getHP(h, p);
	matrix.mulB_43(Fmatrix().setHPB(h, p, 0));
	return matrix;
}

void CMissile::g_fireParams(SPickParam& pp)
{
	Fmatrix matrix = RayTransform();
	Device.hud_to_world(matrix);
	pp.defs.start = matrix.c;
	pp.defs.dir = matrix.k;
}
