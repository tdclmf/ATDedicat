// WeaponFire.cpp: implementation of the CWeapon class.
// function responsible for firing with CWeapon
//////////////////////////////////////////////////////////////////////

#include "stdafx.h"
#include "Weapon.h"
#include "ParticlesObject.h"
#include "entity.h"
#include "actor.h"

#include "actoreffector.h"
#include "effectorshot.h"

#include "level_bullet_manager.h"

#include "game_cl_mp.h"
#include "../Layers/xrRender/xrRender_console.h"
#include "weapon_trace.h"

#define FLAME_TIME 0.05f


float _nrand(float sigma)
{
#define ONE_OVER_SIGMA_EXP (1.0f / 0.7975f)

	if (sigma == 0) return 0;

	float y;
	do
	{
		y = -logf(Random.randF());
	}
	while (Random.randF() > expf(-_sqr(y - 1.0f) * 0.5f));
	if (rand() & 0x1) return y * sigma * ONE_OVER_SIGMA_EXP;
	else return -y * sigma * ONE_OVER_SIGMA_EXP;
}

void random_dir(Fvector& tgt_dir, const Fvector& src_dir, float dispersion)
{
	float sigma = dispersion / 3.f;
	float alpha = clampr(_nrand(sigma), -dispersion, dispersion);
	float theta = Random.randF(0, PI);
	float r = tan(alpha);
	Fvector U, V, T;
	Fvector::generate_orthonormal_basis(src_dir, U, V);
	U.mul(r * _sin(theta));
	V.mul(r * _cos(theta));
	T.add(U, V);
	tgt_dir.add(src_dir, T).normalize();
}

float CWeapon::GetWeaponDeterioration()
{
	return conditionDecreasePerShot * f_weapon_deterioration;
}

void CWeapon::FireStart()
{
	if (ParentIsActor())
		Actor()->StopSprint();

	CShootingObject::FireStart();
}

void CWeapon::FireTrace(const Fvector& P, const Fvector& D)
{
	VERIFY(m_magazine.size());

	CCartridge& l_cartridge = m_magazine.back();
	//	Msg("ammo - %s", l_cartridge.m_ammoSect.c_str());
	VERIFY(u16(-1) != l_cartridge.bullet_material_idx);
	//-------------------------------------------------------------	
	bool is_tracer = m_bHasTracers && l_cartridge.m_flags.test(CCartridge::cfTracer) && !IsSilencerAttached();
	l_cartridge.m_flags.set(CCartridge::cfTracer, is_tracer);
	if (m_u8TracerColorID != u8(-1))
		l_cartridge.param_s.u8ColorID = m_u8TracerColorID;
	//-------------------------------------------------------------
	//повысить изношенность оружия с учетом влияния конкретного патрона
	//	float Deterioration = GetWeaponDeterioration();
	//	Msg("Deterioration = %f", Deterioration);
	ChangeCondition(-GetWeaponDeterioration() * l_cartridge.param_s.impair * cur_silencer_koef.condition_shot_dec);


	float fire_disp = 0.f;
	CActor* tmp_actor = NULL;
	if (OnClient()) // werasik2aa not sure
	{
		tmp_actor = smart_cast<CActor*>(Level().CurrentControlEntity());
		if (tmp_actor)
		{
			CEntity::SEntityState state;
			tmp_actor->g_State(state);
			if (m_first_bullet_controller.is_bullet_first(state.fVelocity))
			{
				fire_disp = m_first_bullet_controller.get_fire_dispertion();
				m_first_bullet_controller.make_shot();
			}
		}
	}
	if (fsimilar(fire_disp, 0.f))
	{
		//CActor* tmp_actor = smart_cast<CActor*>(Level().CurrentControlEntity());
		if (H_Parent() && (H_Parent() == tmp_actor))
		{
			fire_disp = tmp_actor->GetFireDispertion();
		}
		else
		{
			fire_disp = GetFireDispersion(true);
		}
	}


	bool SendHit = SendHitAllowed(H_Parent());
	//выстерлить пулю (с учетом возможной стрельбы дробью)
	for (int i = 0; i < l_cartridge.param_s.buckShot; ++i)
	{
		FireBullet(P, D, fire_disp, l_cartridge, H_Parent()->ID(), ID(), SendHit, iAmmoElapsed);
	}

	StartShotParticles();

	if (m_bLightShotEnabled)
		Light_Start();

	// Interactive Grass FX
	extern Fvector4 ps_ssfx_int_grass_params_2;
	Fvector ShotPos = Fvector().mad(P, D, 1.5f);
	g_pGamePersistent->GrassBendersAddShot(cast_game_object()->ID(), ShotPos, D, 3.0f, 20.0f, ps_ssfx_int_grass_params_2.z, ps_ssfx_int_grass_params_2.w);

	// Ammo
    const int ammo_before = iAmmoElapsed;
    m_lastCartridge = l_cartridge;
    m_magazine.pop_back();
    --iAmmoElapsed;
    CActor* actor_owner = smart_cast<CActor*>(H_Parent());
    if (OnClient() && actor_owner && actor_owner->Local())
    {
        WPN_TRACE("Weapon::FireTrace local ammo consume weapon=%s before=%d after=%d send_hit=%d",
            cName().c_str(),
            ammo_before,
            iAmmoElapsed,
            SendHit ? 1 : 0);
    }

	VERIFY((u32)iAmmoElapsed == m_magazine.size());
}

void CWeapon::StopShooting()
{
	//	SetPending			(TRUE);

	//принудительно останавливать зацикленные партиклы
	if (m_pFlameParticles && m_pFlameParticles->IsLooped())
		StopFlameParticles();

	if (!m_current_motion_def || !m_playFullShotAnim)
		SwitchState(eIdle);

	FireEnd();
}

void CWeapon::FireEnd()
{
	CShootingObject::FireEnd();
	StopShotEffector();
}

//--DSR-- SilencerOverheat_start
void CWeapon::FireBullet(const Fvector& pos,
	const Fvector& shot_dir,
	float fire_disp,
	const CCartridge& cartridge,
	u16 parent_id,
	u16 weapon_id,
	bool send_hit, int iShotNum)
{
	CShootingObject::FireBullet(pos, shot_dir, fire_disp, cartridge, parent_id, weapon_id, send_hit, iShotNum);
	
	temperature += sil_glow_shot_temp;
	if (temperature > sil_glow_max_temp)
		temperature = sil_glow_max_temp;
}

float CWeapon::GetGlowing()
{
	return temperature;
}
//--DSR-- SilencerOverheat_end

void CWeapon::StartFlameParticles2()
{
	CShootingObject::StartParticles(m_pFlameParticles2, *m_sFlameParticles2, get_LastFP2());
}

void CWeapon::StopFlameParticles2()
{
	CShootingObject::StopParticles(m_pFlameParticles2);
}

void CWeapon::UpdateFlameParticles2()
{
	if (m_pFlameParticles2) CShootingObject::UpdateParticles(m_pFlameParticles2, get_LastFP2());
}
