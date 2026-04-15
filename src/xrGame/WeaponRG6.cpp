#include "stdafx.h"
#include "WeaponRG6.h"
#include "entity.h"
#include "explosiveRocket.h"
#include "level.h"
#include "../xrphysics/MathUtils.h"
#include "../xrphysics/PhysicsShell.h"
#include "actor.h"
#include "inventory_item.h"
#include "weapon_trace.h"

#ifdef DEBUG
#	include "phdebug.h"
#endif

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

		WPN_TRACE("WeaponRG6::ApplyAuthoritativeRocketLaunch owner=%s rocket=%u flags=%u has_pos=%d has_vel=%d initiator=%u pos=(%.3f,%.3f,%.3f) vel=(%.3f,%.3f,%.3f)",
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


CWeaponRG6::~CWeaponRG6()
{
}

BOOL CWeaponRG6::net_Spawn(CSE_Abstract* DC)
{
	BOOL l_res = inheritedSG::net_Spawn(DC);
	if (!l_res) return l_res;

	SyncServerRocketsToAmmo();


	return l_res;
};

void CWeaponRG6::Load(LPCSTR section)
{
	inheritedRL::Load(section);
	inheritedSG::Load(section);
}

#include "inventory.h"
#include "inventoryOwner.h"

void CWeaponRG6::FireStart()
{
	if (!IsPending() && GetState() == eIdle && GetNextState() != eReload && getRocketCount() && iAmmoElapsed)
	{
		inheritedSG::FireStart();
#ifdef CROCKETLAUNCHER_CHANGE
		LPCSTR ammo_name = m_ammoTypes[m_ammoType].c_str();
		float launch_speed = READ_IF_EXISTS(pSettings, r_float, ammo_name, "ammo_grenade_vel", CRocketLauncher::m_fLaunchSpeed);
#endif

		Fvector p1, d;
		p1.set(get_LastFP());
		d.set(get_LastFD());

		CEntity* E = smart_cast<CEntity*>(H_Parent());
		if (E)
		{
			CInventoryOwner* io = smart_cast<CInventoryOwner*>(H_Parent());
			if (NULL == io->inventory().ActiveItem())
			{
				Log("current_state", GetState());
				Log("next_state", GetNextState());
				Log("item_sect", cNameSect().c_str());
				Log("H_Parent", H_Parent()->cNameSect().c_str());
			}
			E->g_fireParams(this, p1, d);
		}

		Fmatrix launch_matrix;
		launch_matrix.identity();
		launch_matrix.k.set(d);
		Fvector::generate_orthonormal_basis(launch_matrix.k,
		                                    launch_matrix.j, launch_matrix.i);
		launch_matrix.c.set(p1);

		if (IsZoomed() && H_Parent() == Actor())
		{
			H_Parent()->setEnabled(FALSE);
			setEnabled(FALSE);

			collide::rq_result RQ;
			BOOL HasPick = Level().ObjectSpace.RayPick(p1, d, 300.0f, collide::rqtStatic, RQ, this);

			setEnabled(TRUE);
			H_Parent()->setEnabled(TRUE);

			if (HasPick)
			{
				//			collide::rq_result& RQ = HUD().GetCurrentRayQuery();
				Fvector Transference;
				//Transference.add(p1, Fvector().mul(d, RQ.range));				
				Transference.mul(d, RQ.range);
				Fvector res[2];
				/*#ifdef		DEBUG
								DBG_OpenCashedDraw();
								DBG_DrawLine(p1,Fvector().add(p1,d),D3DCOLOR_XRGB(255,0,0));
				#endif*/
#ifdef CROCKETLAUNCHER_CHANGE
				u8 canfire0 = TransferenceAndThrowVelToThrowDir(Transference, launch_speed, EffectiveGravity(), res);
#else
				u8 canfire0 = TransferenceAndThrowVelToThrowDir(Transference, CRocketLauncher::m_fLaunchSpeed,
				                                                EffectiveGravity(), res);
#endif
				/*#ifdef DEBUG
								if(canfire0>0)DBG_DrawLine(p1,Fvector().add(p1,res[0]),D3DCOLOR_XRGB(0,255,0));
								if(canfire0>1)DBG_DrawLine(p1,Fvector().add(p1,res[1]),D3DCOLOR_XRGB(0,0,255));
								DBG_ClosedCashedDraw(30000);
				#endif*/
				if (canfire0 != 0)
				{
					//					Msg ("d[%f,%f,%f] - res [%f,%f,%f]", d.x, d.y, d.z, res[0].x, res[0].y, res[0].z);
					d = res[0];
				};
			}
		};

		d.normalize();
#ifdef CROCKETLAUNCHER_CHANGE
		d.mul(launch_speed);
#else
		d.mul(m_fLaunchSpeed);
#endif
		VERIFY2(_valid(launch_matrix), "CWeaponRG6::FireStart. Invalid launch_matrix");
		const u16 launched_rocket_id = u16(getCurrentRocket()->ID());
		CRocketLauncher::LaunchRocket(launch_matrix, d, zero_vel);

		CExplosiveRocket* pGrenade = smart_cast<CExplosiveRocket*>(Level().Objects.net_Find(launched_rocket_id));
		VERIFY(pGrenade);
		pGrenade->SetInitiator(H_Parent()->ID());

		if (OnServer())
		{
			inheritedRL::DetachRocket(launched_rocket_id, true);
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

u8 CWeaponRG6::AddCartridge(u8 cnt)
{
	u8 t = inheritedSG::AddCartridge(cnt);
	u8 k = cnt - t;
	shared_str fake_grenade_name = pSettings->r_string(m_ammoTypes[m_ammoType].c_str(), "fake_grenade_name");
	while (k)
	{
		--k;
		inheritedRL::SpawnRocket(*fake_grenade_name, this);
	}
	return t;
}

void CWeaponRG6::ReloadMagazine()
{
	inheritedSG::ReloadMagazine();
	SyncServerRocketsToAmmo();
}
void CWeaponRG6::OnEvent(NET_Packet& P, u16 type)
{
	inheritedSG::OnEvent(P, type);

	u16 id;
	switch (type)
	{
	case GE_WPN_AMMO_SYNC:
		SyncServerRocketsToAmmo();
		break;
	case GE_OWNERSHIP_TAKE:
		{
			P.r_u16(id);
			inheritedRL::AttachRocket(id, this);
		}
		break;
	case GE_OWNERSHIP_REJECT:
	case GE_LAUNCH_ROCKET:
		{
			bool bLaunch = (type == GE_LAUNCH_ROCKET);
			P.r_u16(id);
			inheritedRL::DetachRocket(id, bLaunch);
			if (bLaunch && OnClient())
				ApplyAuthoritativeRocketLaunchEvent(P, id, cName().c_str());
		}
		break;
	}
}

void CWeaponRG6::SyncServerRocketsToAmmo()
{
	if (!OnServer())
		return;

	if (iAmmoElapsed <= 0)
		return;

	if (m_ammoTypes.empty() || m_ammoType >= m_ammoTypes.size())
		return;

	const shared_str grenade_name = m_ammoTypes[m_ammoType];
	if (!grenade_name.size())
		return;

	const shared_str fake_grenade_name = pSettings->r_string(grenade_name, "fake_grenade_name");
	if (!fake_grenade_name.size())
		return;

	const u32 current_rocket_count = getRocketCount();
	const u32 desired_rocket_count = u32(_max(iAmmoElapsed, 0));
	const u32 to_spawn = (desired_rocket_count > current_rocket_count)
		? (desired_rocket_count - current_rocket_count)
		: 0;

	for (u32 i = 0; i < to_spawn; ++i)
		inheritedRL::SpawnRocket(*fake_grenade_name, this);

	WPN_TRACE("WeaponRG6::SyncServerRocketsToAmmo weapon=%s ammo=%d rockets=%u spawn=%u",
		cName().c_str(),
		iAmmoElapsed,
		current_rocket_count,
		to_spawn);
}

#ifdef CROCKETLAUNCHER_CHANGE
void CWeaponRG6::UnloadRocket()
{
	while (getRocketCount() > 0)
	{
		Msg("%s:%d [%d]-[%s]", __FUNCTION__, __LINE__, getRocketCount(), getCurrentRocket()->cNameSect_str());
		NET_Packet P;
		u_EventGen(P, GE_OWNERSHIP_REJECT, ID());
		P.w_u16(u16(getCurrentRocket()->ID()));
		u_EventSend(P);
		dropCurrentRocket();
	}
}
#endif
