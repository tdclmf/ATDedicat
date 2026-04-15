#include "stdafx.h"
#include "weaponrpg7.h"
#include "xrserver_objects_alife_items.h"
#include "explosiverocket.h"
#include "entity.h"
#include "level.h"
#include "player_hud.h"
#include "hudmanager.h"
#include "inventory_item.h"
#include "../xrphysics/PhysicsShell.h"
#include "weapon_trace.h"

namespace
{
	void ApplyAuthoritativeRocketLaunchEvent(NET_Packet& P, const u16 rocket_id, LPCSTR owner_name)
	{
		u8 launch_flags = 0;
		Fvector launch_pos{};
		Fvector launch_vel{};
		u16 launch_initiator = u16(-1);
		bool has_pos = false;
		bool has_vel = false;

		const u32 unread_after_id = (P.B.count > P.r_tell()) ? (P.B.count - P.r_tell()) : 0;
		if (unread_after_id >= sizeof(u8))
			launch_flags = P.r_u8();

		const u32 unread_after_flags = (P.B.count > P.r_tell()) ? (P.B.count - P.r_tell()) : 0;
		if (unread_after_flags >= sizeof(Fvector))
		{
			P.r_vec3(launch_pos);
			has_pos = _valid(launch_pos);
		}

		const u32 unread_after_pos = (P.B.count > P.r_tell()) ? (P.B.count - P.r_tell()) : 0;
		if (unread_after_pos >= sizeof(Fvector))
		{
			P.r_vec3(launch_vel);
			has_vel = _valid(launch_vel);
		}
		const u32 unread_after_vel = (P.B.count > P.r_tell()) ? (P.B.count - P.r_tell()) : 0;
		if (unread_after_vel >= sizeof(u16))
			launch_initiator = P.r_u16();

		CCustomRocket* rocket = smart_cast<CCustomRocket*>(Level().Objects.net_Find(rocket_id));
		if (!rocket)
			return;

		Fmatrix launch_xform = rocket->XFORM();
		if (has_vel && launch_vel.square_magnitude() > EPS)
		{
			Fvector launch_dir = launch_vel;
			launch_dir.normalize_safe();
			launch_xform.k.set(launch_dir);
			Fvector::generate_orthonormal_basis(launch_xform.k, launch_xform.j, launch_xform.i);
		}
		if (has_pos)
			launch_xform.c.set(launch_pos);
		rocket->ApplyAuthoritativeLaunch(launch_xform, launch_vel, has_vel);
		if (launch_initiator != u16(-1))
			if (CExplosiveRocket* explosive_rocket = smart_cast<CExplosiveRocket*>(rocket))
				explosive_rocket->SetInitiator(launch_initiator);
		if (CInventoryItem* inv_rocket = smart_cast<CInventoryItem*>(rocket))
			inv_rocket->ClearNetInterpolationQueue();

		WPN_TRACE("WeaponRPG7::ApplyAuthoritativeRocketLaunch owner=%s rocket=%u flags=%u has_pos=%d has_vel=%d initiator=%u pos=(%.3f,%.3f,%.3f) vel=(%.3f,%.3f,%.3f)",
			owner_name ? owner_name : "<null>",
			rocket_id,
			u32(launch_flags),
			has_pos ? 1 : 0,
			has_vel ? 1 : 0,
			u32(launch_initiator),
			launch_pos.x, launch_pos.y, launch_pos.z,
			launch_vel.x, launch_vel.y, launch_vel.z);
	}
}

CWeaponRPG7::CWeaponRPG7()
{
}

CWeaponRPG7::~CWeaponRPG7()
{
}

void CWeaponRPG7::Load(LPCSTR section)
{
	inherited::Load(section);
	CRocketLauncher::Load(section);

	m_zoom_params.m_fScopeZoomFactor = pSettings->r_float(section, "max_zoom_factor");

	m_sRocketSection = pSettings->r_string(section, "rocket_class");
}

bool CWeaponRPG7::AllowBore()
{
	return inherited::AllowBore() && 0 != iAmmoElapsed;
}

void CWeaponRPG7::FireTrace(const Fvector& P, const Fvector& D)
{
	inherited::FireTrace(P, D);
	UpdateMissileVisibility();
}

void CWeaponRPG7::on_a_hud_attach()
{
	inherited::on_a_hud_attach();
	UpdateMissileVisibility();
}

void CWeaponRPG7::UpdateMissileVisibility()
{
	bool vis_hud, vis_weap;
	vis_hud = (!!iAmmoElapsed || GetState() == eReload);
	vis_weap = !!iAmmoElapsed;

	if (GetHUDmode())
	{
		HudItemData()->set_bone_visible("grenade", vis_hud,TRUE);
	}

	IKinematics* pWeaponVisual = smart_cast<IKinematics*>(Visual());
	VERIFY(pWeaponVisual);
	if (pWeaponVisual) pWeaponVisual->LL_SetBoneVisible(pWeaponVisual->LL_BoneID("grenade"), vis_weap, TRUE);
}

BOOL CWeaponRPG7::net_Spawn(CSE_Abstract* DC)
{
	BOOL l_res = inherited::net_Spawn(DC);

	UpdateMissileVisibility();
	SyncServerRocketToAmmo();

	return l_res;
}

void CWeaponRPG7::OnStateSwitch(u32 S, u32 oldState)
{
	inherited::OnStateSwitch(S, oldState);
	UpdateMissileVisibility();
}

void CWeaponRPG7::UnloadMagazine(bool spawn_ammo)
{
	inherited::UnloadMagazine(spawn_ammo);
	UpdateMissileVisibility();
}

void CWeaponRPG7::ReloadMagazine()
{
	inherited::ReloadMagazine();
	SyncServerRocketToAmmo();
}

void CWeaponRPG7::SwitchState(u32 S)
{
	inherited::SwitchState(S);
}

void CWeaponRPG7::FireStart()
{
	if (IsPending() || GetState() == eReload || GetNextState() == eReload)
		return;
	if (!getRocketCount() || iAmmoElapsed <= 0)
		return;

	inherited::FireStart();
}

#include "inventory.h"
#include "inventoryOwner.h"

void CWeaponRPG7::switch2_Fire()
{
	m_iShotNum = 0;
	m_bFireSingleShot = true;
	bWorking = false;

	if (GetState() == eFire && getRocketCount())
	{
		Fvector p1, d1, p;
		Fvector p2, d2, d;
		p1.set(get_LastFP());
		d1.set(get_LastFD());
		p = p1;
		d = d1;
		CEntity* E = smart_cast<CEntity*>(H_Parent());
		if (E)
		{
			E->g_fireParams(this, p2, d2);
			p = p2;
			d = d2;

			if (GetHUDmode())
			{
				Fvector p0;
				float dist = GetRQ().range;
				p0.mul(d2, dist);
				p0.add(p1);
				p = p1;
				d.sub(p0, p1);
				d.normalize_safe();
			}
		}

		Fmatrix launch_matrix;
		launch_matrix.identity();
		launch_matrix.k.set(d);
		Fvector::generate_orthonormal_basis(launch_matrix.k,
		                                    launch_matrix.j, launch_matrix.i);
		launch_matrix.c.set(p);

		d.normalize();
		d.mul(m_fLaunchSpeed);

		const u16 launched_rocket_id = u16(getCurrentRocket()->ID());
		CRocketLauncher::LaunchRocket(launch_matrix, d, zero_vel);

		CExplosiveRocket* pGrenade = smart_cast<CExplosiveRocket*>(Level().Objects.net_Find(launched_rocket_id));
		VERIFY(pGrenade);
		pGrenade->SetInitiator(H_Parent()->ID());

		if (OnServer())
		{
			CRocketLauncher::DetachRocket(launched_rocket_id, true);
			NET_Packet P;
			u_EventGen(P, GE_LAUNCH_ROCKET, ID());
			P.w_u16(launched_rocket_id);
			P.w_u8(0);
			P.w_vec3(launch_matrix.c);
			P.w_vec3(d);
			P.w_u16(H_Parent() ? H_Parent()->ID() : u16(-1));
			u_EventSend(P);
		}
	}
}

void CWeaponRPG7::PlayAnimReload()
{
	VERIFY(GetState()==eReload);
	PlayHUDMotion("anm_reload", TRUE, this, GetState(), 1.f, 0.f, false);
}

void CWeaponRPG7::OnEvent(NET_Packet& P, u16 type)
{
	inherited::OnEvent(P, type);
	u16 id;
	switch (type)
	{
	case GE_WPN_AMMO_SYNC:
		SyncServerRocketToAmmo();
		break;
	case GE_OWNERSHIP_TAKE:
		{
			P.r_u16(id);
			CRocketLauncher::AttachRocket(id, this);
		}
		break;
	case GE_OWNERSHIP_REJECT:
	case GE_LAUNCH_ROCKET:
		{
			bool bLaunch = (type == GE_LAUNCH_ROCKET);
			P.r_u16(id);
			CRocketLauncher::DetachRocket(id, bLaunch);
			if (bLaunch)
			{
				if (OnClient())
					ApplyAuthoritativeRocketLaunchEvent(P, id, cName().c_str());
				UpdateMissileVisibility();
			}
		}
		break;
	}
}

void CWeaponRPG7::net_Import(NET_Packet& P)
{
	inherited::net_Import(P);
	UpdateMissileVisibility();
}

void CWeaponRPG7::SyncServerRocketToAmmo()
{
	if (!OnServer())
		return;

	if (iAmmoElapsed > 0 && !getRocketCount())
		CRocketLauncher::SpawnRocket(m_sRocketSection.c_str(), this);
}
