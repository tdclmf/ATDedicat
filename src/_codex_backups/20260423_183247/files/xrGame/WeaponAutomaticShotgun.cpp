#include "stdafx.h"
#include "WeaponAutomaticShotgun.h"
#include "entity.h"
#include "ParticlesObject.h"
#include "xr_level_controller.h"
#include "inventory.h"
#include "level.h"
#include "actor.h"
#include "script_game_object.h"

CWeaponAutomaticShotgun::CWeaponAutomaticShotgun()
{
	m_eSoundClose = ESoundTypes(SOUND_TYPE_WEAPON_SHOOTING);
	m_eSoundAddCartridge = ESoundTypes(SOUND_TYPE_WEAPON_SHOOTING);
}

CWeaponAutomaticShotgun::~CWeaponAutomaticShotgun()
{
}

void CWeaponAutomaticShotgun::Load(LPCSTR section)
{
	inherited::Load(section);

	if (pSettings->line_exist(section, "tri_state_reload"))
	{
		m_bTriStateReload = !!pSettings->r_bool(section, "tri_state_reload");
	};
	if (pSettings->line_exist(section, "bas_state_reload"))
	{
		IsCustomReloadAvaible = !!pSettings->r_bool(section, "bas_state_reload");
	}
	
	if (m_bTriStateReload)
	{
		m_sounds.LoadSound(section, "snd_open_weapon", "sndOpen", false, m_eSoundOpen);

		m_sounds.LoadSound(section, "snd_add_cartridge", "sndAddCartridge", false, m_eSoundAddCartridge);

		m_sounds.LoadSound(section, "snd_close_weapon", "sndClose", false, m_eSoundClose);

		m_sounds.LoadSound(section, "snd_close_weapon_empty", "sndCloseEmpty", false, m_eSoundClose);

		m_sounds.LoadSound(section, "snd_shoot_last", "sndShootL", false, m_eSoundShot);
	};
}

bool CWeaponAutomaticShotgun::Action(u16 cmd, u32 flags)
{
	if (cmd == kWPN_FIRE && flags & CMD_START)
	{
		if (fShotTimeCounter > 0.f)
			return true;
	}
	if (inherited::Action(cmd, flags)) return true;

	if (m_bTriStateReload && GetState() == eReload &&
		cmd == kWPN_FIRE && flags & CMD_START &&
		m_sub_state == eSubstateReloadInProcess || m_sub_state == eSubstateReloadInProcessEmptyEnd) //постановить перезагрузку
	{	
		AddCartridge(1);
		m_sub_state = eSubstateReloadEnd;
		if(IsCustomReloadAvaible) SwitchState(eReload);
		return true;
	}
	return false;
}

void CWeaponAutomaticShotgun::OnAnimationEnd(u32 state)
{
	if (!m_bTriStateReload || state != eReload)
		return inherited::OnAnimationEnd(state);

	switch (m_sub_state)
	{
	case eSubstateReloadBegin:
		{
			m_sub_state = eSubstateReloadInProcess;
			SwitchState(eReload);
		}
		break;

	case eSubstateReloadInProcess:
		{
			if (0 != AddCartridge(1))
			{
				m_sub_state = eSubstateReloadEnd;
			}
			else if (BeginReloadWasEmpty && IsCustomReloadAvaible)
			{
				m_sub_state = eSubstateReloadInProcessEmptyEnd;
				
			}
			SwitchState(eReload);
		}
		break;

	case eSubstateReloadEnd:
		{
			BeginReloadWasEmpty = false;
			m_sub_state = eSubstateReloadBegin;
			SwitchState(eIdle);
		}
		break;
	case eSubstateReloadInProcessEmptyEnd:
		{
			m_sub_state = eSubstateReloadBegin;
			SwitchState(eReload);
		}
		break;
	};
}

void CWeaponAutomaticShotgun::Reload()
{
	if (m_bTriStateReload)
	{
		TriStateReload();
	}
	else
		inherited::Reload();
}

void CWeaponAutomaticShotgun::TriStateReload()
{
	if (m_magazine.size() == (u32)iMagazineSize || !HaveCartridgeInInventory(1))return;
	CWeapon::Reload();
	m_sub_state = eSubstateReloadBegin;
	SwitchState(eReload);
}

void CWeaponAutomaticShotgun::OnStateSwitch(u32 S, u32 oldState)
{
	if (!m_bTriStateReload || S != eReload)
	{
		inherited::OnStateSwitch(S, oldState);
		return;
	}

	CWeapon::OnStateSwitch(S, oldState);

	if (m_magazine.size() == (u32)iMagazineSize || !HaveCartridgeInInventory(1))
	{
		switch2_EndReload();
		m_sub_state = eSubstateReloadEnd;
		return;
	};

	switch (m_sub_state)
	{
	case eSubstateReloadBegin:
		if (HaveCartridgeInInventory(1))
			switch2_StartReload();
		break;
	case eSubstateReloadInProcess:
		if (HaveCartridgeInInventory(1))
			switch2_AddCartgidge();
		break;
	case eSubstateReloadEnd:
		switch2_EndReload();
		break;
	case eSubstateReloadInProcessEmptyEnd:
		switch2_EndReload();
		break;
	};
}

void CWeaponAutomaticShotgun::PlaySoundShot()
{
	// Temp fix for shotguns last shot
	if (iAmmoElapsed > 1 || !HudAnimationExist("anm_shot_l"))
	{
		inherited::PlaySoundShot();
	}
	else
	{
		if (m_sounds.FindSoundItem("sndShootL", false))
		{
			m_sounds.PlaySound("sndShootL", get_LastFP(), H_Root(), !!GetHUDmode(), false, (u8)-1);
		}
		else
		{
			inherited::PlaySoundShot();
		}
	}
}

void CWeaponAutomaticShotgun::switch2_StartReload()
{
	BeginReloadWasEmpty = !m_magazine.size();
	PlaySound("sndOpen", get_LastFP());
	PlayAnimOpenWeapon();
	SetPending(TRUE);
}

void CWeaponAutomaticShotgun::switch2_AddCartgidge()
{
	if (ParentIsActor()) Actor()->callback(GameObject::eWeaponNoAmmoAvailable)(lua_game_object(), GetSuitableAmmoTotal());
	PlaySound("sndAddCartridge", get_LastFP());
	PlayAnimAddOneCartridgeWeapon();
	SetPending(TRUE);
}

void CWeaponAutomaticShotgun::switch2_EndReload()
{
	SetPending(FALSE);
	if (BeginReloadWasEmpty && m_sounds.FindSoundItem("sndCloseEmpty", false))
		PlaySound("sndCloseEmpty", get_LastFP());
	else
		PlaySound("sndClose", get_LastFP());

	PlayAnimCloseWeapon();
}

void CWeaponAutomaticShotgun::PlayAnimOpenWeapon()
{
	VERIFY(GetState()==eReload);
	PlayHUDMotion("anm_open", TRUE, this, GetState(), 1.f, 0.f, false);
}

void CWeaponAutomaticShotgun::PlayAnimAddOneCartridgeWeapon()
{
	VERIFY(GetState()==eReload);
	PlayHUDMotion("anm_add_cartridge", FALSE, this, GetState());
}

void CWeaponAutomaticShotgun::PlayAnimCloseWeapon()
{
	VERIFY(GetState()==eReload);

	if (BeginReloadWasEmpty && HudAnimationExist("anm_close_empty"))
		PlayHUDMotion("anm_close_empty", FALSE, this, GetState());
	else
		PlayHUDMotion("anm_close", FALSE, this, GetState());
}

bool CWeaponAutomaticShotgun::HaveCartridgeInInventory(u8 cnt)
{
	if (unlimited_ammo()) return true;
	if (!m_pInventory) return false;

	u32 ac = GetAmmoCount(m_ammoType);
	if (ac < cnt)
	{
		for (u8 i = 0; i < u8(m_ammoTypes.size()); ++i)
		{
			if (m_ammoType == i) continue;
			ac += GetAmmoCount(i);
			if (ac >= cnt)
			{
				m_ammoType = i;
				break;
			}
		}
	}
	return ac >= cnt;
}


u8 CWeaponAutomaticShotgun::AddCartridge(u8 cnt)
{
	if (IsMisfire()) bMisfire = false;

	if (m_set_next_ammoType_on_reload != undefined_ammo_type)
	{
		m_ammoType = m_set_next_ammoType_on_reload;
		m_set_next_ammoType_on_reload = undefined_ammo_type;
	}

	if (!HaveCartridgeInInventory(1) || iAmmoElapsed >= iMagazineSize)
		return 0;

	m_pCurrentAmmo = smart_cast<CWeaponAmmo*>(m_pInventory->GetAny(m_ammoTypes[m_ammoType].c_str()));
	VERIFY((u32)iAmmoElapsed == m_magazine.size());


	if (m_DefaultCartridge.m_LocalAmmoType != m_ammoType)
		m_DefaultCartridge.Load(m_ammoTypes[m_ammoType].c_str(), m_ammoType, m_APk);
	CCartridge l_cartridge = m_DefaultCartridge;
	while (cnt)
	{
		if (!unlimited_ammo())
		{
			if (!m_pCurrentAmmo->Get(l_cartridge)) break;
		}
		--cnt;
		++iAmmoElapsed;
		l_cartridge.m_LocalAmmoType = m_ammoType;
		m_magazine.push_back(l_cartridge);
		//		m_fCurrentCartirdgeDisp = l_cartridge.m_kDisp;
	}

	VERIFY((u32)iAmmoElapsed == m_magazine.size());

	//выкинуть коробку патронов, если она пустая
	if (m_pCurrentAmmo && !m_pCurrentAmmo->m_boxCurr && OnServer())
		m_pCurrentAmmo->SetDropManual(TRUE);

	return cnt;
}

void CWeaponAutomaticShotgun::net_Export(NET_Packet& P)
{
	inherited::net_Export(P);
	P.w_u8(u8(m_magazine.size()));
	for (u32 i = 0; i < m_magazine.size(); i++)
	{
		CCartridge& l_cartridge = *(m_magazine.begin() + i);
		P.w_u8(l_cartridge.m_LocalAmmoType);
	}
}

void CWeaponAutomaticShotgun::net_Import(NET_Packet& P)
{
	inherited::net_Import(P);
	if (P.r_elapsed() < sizeof(u8))
		return;

	u8 ammo_count = P.r_u8();
	const u32 available_types = P.r_elapsed();
	if (ammo_count > available_types)
	{
		Msg("! [NET] CWeaponAutomaticShotgun::net_Import truncated ammo list count=%u left=%u for [%s][%u]",
			ammo_count, available_types, cNameSect().c_str(), ID());
		ammo_count = (u8)available_types;
	}

	for (u32 i = 0; i < ammo_count; ++i)
	{
		if (P.r_elapsed() < sizeof(u8))
			break;

		u8 local_ammo_type = P.r_u8();
		if (i >= m_magazine.size())
			continue;

		if (local_ammo_type >= m_ammoTypes.size())
			continue;

		CCartridge& local_cartridge = *(m_magazine.begin() + i);
		if (local_ammo_type == local_cartridge.m_LocalAmmoType)
			continue;

		const shared_str& ammo_sect = m_ammoTypes[local_ammo_type];
		if (!ammo_sect.size() || !pSettings->section_exist(ammo_sect.c_str()))
			continue;

#ifdef DEBUG
		Msg("! %s reload to %s", *local_cartridge.m_ammoSect, ammo_sect.c_str());
#endif
		local_cartridge.Load(ammo_sect.c_str(), local_ammo_type, m_APk);
	}
}
