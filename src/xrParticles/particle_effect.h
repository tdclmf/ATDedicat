//---------------------------------------------------------------------------
#ifndef particle_effectH
#define particle_effectH
#include "../xrServerEntities/object_broker.h"
#include "particle_actions_collection.h"
namespace PAPI
{
	// A effect of particles - Info and an array of Particles
	struct ParticleEffect
	{
		u32 p_count = 0; // Number of particles currently existing.
		u32 max_particles = 1; // Max particles allowed in effect.
		u32 particles_allocated = 1; // Actual allocated size.
		Particle* particles; // Actually, num_particles in size
		xr_vector<ParticleAction*> actions;
		void* real_ptr; // Base, possible not aligned pointer
		OnBirthParticleCB b_cb = 0;
		OnDeadParticleCB d_cb = 0;
		void* owner = NULL;
		u32 param = 0;

		ParticleEffect() : real_ptr(xr_malloc(sizeof(Particle)* (max_particles + 1)))
		{
			particles = (Particle*)((uintptr_t)real_ptr + (64 - ((uintptr_t)real_ptr & 63)));
		}

		~ParticleEffect()
		{
			xr_free(real_ptr);
			delete_data(actions);
		}

		IC void PlayEffect()
		{
			// Step through all the actions in the action list.
			for (ParticleAction* pact : actions)
			{
				switch (pact->type)
				{
				case PASourceID: 		static_cast<PASource*>(pact)->m_Flags.set(PASource::flSilent, FALSE); break;
				case PAExplosionID:		static_cast<PAExplosion*>(pact)->age = 0.f; break;
				case PATurbulenceID:	static_cast<PATurbulence*>(pact)->age = 0.f; break;
				}
			}
		}

		IC void StopEffect(BOOL deffered)
		{
			// Step through all the actions in the action list.
			for (ParticleAction* pact : actions)
			{
				switch (pact->type)
				{
				case PASourceID:
					static_cast<PASource*>(pact)->m_Flags.set(PASource::flSilent, TRUE);
					break;
				}
			}
			if (!deffered)
				p_count = 0;
		}

		IC void Update(float dt)
		{
			// Step through all the actions in the action list.
			float kill_old_time = 3.0f;
			for (ParticleAction* pact : actions)
				pact->Execute(this, dt, kill_old_time);
		}

		IC void Transform(const Fmatrix& full, const Fvector& vel)
		{
			Fmatrix mT;
			mT.translate(full.c);

			// Step through all the actions in the action list.
			for (ParticleAction* pact : actions)
			{
				BOOL r = pact->m_Flags.is(ParticleAction::ALLOW_ROTATE);
				const Fmatrix& m = r ? full : mT;
				pact->Transform(m);
				switch (pact->type)
				{
				case PASourceID:
					static_cast<PASource*>(pact)->parent_vel = pVector(vel.x, vel.y, vel.z) * static_cast<PASource*>(pact)->parent_motion;
					break;
				}
			}
		}

		IC int SetMaxParticles(u32 max_count)
		{
			// Reducing max.
			if (particles_allocated >= max_count)
			{
				max_particles = max_count;

				// May have to kill particles.
				if (p_count > max_particles)
					p_count = max_particles;

				return max_count;
			}

			// Allocate particles.
			void* new_real_ptr = xr_malloc(sizeof(Particle) * (max_count + 1));

			if (new_real_ptr == NULL)
			{
				// ERROR - Not enough memory. Just give all we've got.
				max_particles = particles_allocated;
				return max_particles;
			}

			Particle* new_particles = (Particle*)((uintptr_t)new_real_ptr + (64 - ((uintptr_t)new_real_ptr & 63)));
			//Msg( "Re-allocated %u bytes (%u particles) with base address 0x%p" , max_count * sizeof( Particle ) , max_count , new_particles );

			CopyMemory(new_particles, particles, p_count * sizeof(Particle));
			xr_free(real_ptr);

			particles = new_particles;
			real_ptr = new_real_ptr;

			max_particles = max_count;
			particles_allocated = max_count;
			return max_count;
		}

		IC void SetCallback(OnBirthParticleCB b, OnDeadParticleCB d, void* ow, u32 par)
		{
			b_cb = b;
			d_cb = d;
			owner = ow;
			param = par;
		}

		IC void GetParticles(Particle*& pvec, u32& cnt)
		{
			pvec = particles;
			cnt = p_count;
		}

		IC u32 GetParticlesCount()
		{
			return p_count;
		}

		IC void RemoveParticle(int i)
		{
			if (0 == p_count) return;
			Particle& m = particles[i];
			if (d_cb) d_cb(owner, param, m, i);
			m = particles[--p_count]; // не менять правило удаления !!! (dependence ParticleGroup)
			// Msg( "pDel() : %u" , p_count );
		}

		IC BOOL Add(const pVector& pos, const pVector& posB,
		            const pVector& size, const pVector& rot, const pVector& vel, u32 color,
		            const float age = 0.0f, u16 frame = 0, u16 flags = 0)
		{
			if (p_count < max_particles)
			{
				Particle& P = particles[p_count];
				P.pos = pos;
				P.posI = pos;
				P.posB = posB;
				P.size = size;
				P.sizeI = size;
				P.rot.x = rot.x;
				P.rotI.x = rot.x;
				P.vel = vel;
				P.velI = vel;
				static constexpr float f = float(1.0) / float(255.0);
				P.colorA = f * ((color >> 24) & 0xff);
				P.colorR = f * ((color >> 16) & 0xff);
				P.colorG = f * ((color >> 8) & 0xff);
				P.colorB = f * ((color >> 0) & 0xff);
				P.age = age;
				P.frame = frame;
				P.flags.assign(flags);
				if (b_cb) b_cb(owner, param, P, p_count);
				p_count++;
				return TRUE;
			}
			return FALSE;
		}

		IC u32 LoadActions(IReader& R)
		{
			if (R.length())
			{
				u32 cnt = R.r_u32();
				actions.reserve(cnt);
				for (u32 k = 0; k < cnt; k++)
				{
					u32 type = R.r_u32();
					if (type == u32(-1))
						continue;
					ParticleAction* pa = NULL;
					switch ((PActionEnum)type)
					{
					case PAAvoidID: pa = xr_new<PAAvoid>();
						break;
					case PABounceID: pa = xr_new<PABounce>();
						break;
					case PACopyVertexBID: pa = xr_new<PACopyVertexB>();
						break;
					case PADampingID: pa = xr_new<PADamping>();
						break;
					case PAExplosionID: pa = xr_new<PAExplosion>();
						break;
					case PAFollowID: pa = xr_new<PAFollow>();
						break;
					case PAGravitateID: pa = xr_new<PAGravitate>();
						break;
					case PAGravityID: pa = xr_new<PAGravity>();
						break;
					case PAJetID: pa = xr_new<PAJet>();
						break;
					case PAKillOldID: pa = xr_new<PAKillOld>();
						break;
					case PAMatchVelocityID: pa = xr_new<PAMatchVelocity>();
						break;
					case PAMoveID: pa = xr_new<PAMove>();
						break;
					case PAOrbitLineID: pa = xr_new<PAOrbitLine>();
						break;
					case PAOrbitPointID: pa = xr_new<PAOrbitPoint>();
						break;
					case PARandomAccelID: pa = xr_new<PARandomAccel>();
						break;
					case PARandomDisplaceID: pa = xr_new<PARandomDisplace>();
						break;
					case PARandomVelocityID: pa = xr_new<PARandomVelocity>();
						break;
					case PARestoreID: pa = xr_new<PARestore>();
						break;
					case PASinkID: pa = xr_new<PASink>();
						break;
					case PASinkVelocityID: pa = xr_new<PASinkVelocity>();
						break;
					case PASourceID: pa = xr_new<PASource>();
						break;
					case PASpeedLimitID: pa = xr_new<PASpeedLimit>();
						break;
					case PATargetColorID: pa = xr_new<PATargetColor>();
						break;
					case PATargetSizeID: pa = xr_new<PATargetSize>();
						break;
					case PATargetRotateID: pa = xr_new<PATargetRotate>();
						break;
					case PATargetRotateDID: pa = xr_new<PATargetRotate>();
						break;
					case PATargetVelocityID: pa = xr_new<PATargetVelocity>();
						break;
					case PATargetVelocityDID: pa = xr_new<PATargetVelocity>();
						break;
					case PAVortexID: pa = xr_new<PAVortex>();
						break;
					case PATurbulenceID: pa = xr_new<PATurbulence>();
						break;
					case PAScatterID: pa = xr_new<PAScatter>();
						break;
					default: NODEFAULT;
					}

					if (pa)
					{
						pa->type = (PActionEnum)type;

						pa->Load(R);
						actions.push_back(pa);
					}
				}
			}
			return actions.size();
		}

		IC void SaveActions(IWriter& W)
		{
			W.w_u32(actions.size());
			for (ParticleAction* pact : actions)
			{
				if (!pact) {
					W.w_u32((u32)-1);
					continue;
				}

				W.w_u32((pact->type));
				pact->Save(W);
			}
		}
	};
};

//---------------------------------------------------------------------------
#endif
