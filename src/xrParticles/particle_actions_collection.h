//---------------------------------------------------------------------------
#ifndef particle_actions_collectionH
#define particle_actions_collectionH

#include "psystem.h"
#include "particle_core.h"

namespace PAPI
{
#define _METHODS	virtual void 	Load		(IReader& F);\
                    virtual void 	Save		(IWriter& F);\
                    virtual void 	Execute		(ParticleEffect *pe, const float dt, float& m_max);\
                    virtual void 	Transform	(const Fmatrix& m);

	// refs
	struct ParticleEffect;

	struct ParticleAction
	{
		enum
		{
			ALLOW_ROTATE = (1 << 1)
		};

		Flags32 m_Flags;
		PActionEnum type; // Type field
		ParticleAction() { m_Flags.zero(); }

		virtual void Execute(ParticleEffect* pe, const float dt, float& m_max) = 0;
		virtual void Transform(const Fmatrix& m) = 0;

		virtual void Load(IReader& F) = 0;
		virtual void Save(IWriter& F) = 0;
	};


	struct  PAAvoid : ParticleAction
	{
		pDomain positionL; // Avoid region (in local space)
		pDomain position; // Avoid region
		float look_ahead; // how many time units ahead to look
		float magnitude; // what percent of the way to go each time
		float epsilon; // add to r^2 for softening

		_METHODS;
	};

	struct  PABounce : ParticleAction
	{
		pDomain positionL; // Bounce region (in local space)
		pDomain position; // Bounce region
		float oneMinusFriction; // Friction tangent to surface
		float resilience; // Resilence perpendicular to surface
		float cutoffSqr; // cutoff velocity; friction applies iff v > cutoff

		_METHODS;
	};

	struct  PACopyVertexB : ParticleAction
	{
		BOOL copy_pos; // True to copy pos to posB.

		_METHODS;
	};

	struct  PADamping : ParticleAction
	{
		pVector damping; // Damping constant applied to velocity
		float vlowSqr; // Low and high cutoff velocities
		float vhighSqr;

		_METHODS;
	};

	struct  PAExplosion : ParticleAction
	{
		pVector centerL; // The center of the explosion (in local space)
		pVector center; // The center of the explosion
		float velocity; // Of shock wave
		float magnitude; // At unit radius
		float stdev; // Sharpness or width of shock wave
		float age; // How long it's been going on
		float epsilon; // Softening parameter

		_METHODS;
	};

	struct  PAFollow : ParticleAction
	{
		float magnitude; // The grav of each particle
		float epsilon; // Softening parameter
		float max_radius; // Only influence particles within max_radius

		_METHODS;
	};

	struct  PAGravitate : ParticleAction
	{
		float magnitude; // The grav of each particle
		float epsilon; // Softening parameter
		float max_radius; // Only influence particles within max_radius

		_METHODS;
	};

	struct  PAGravity : ParticleAction
	{
		pVector directionL; // Amount to increment velocity (in local space)
		pVector direction; // Amount to increment velocity

		_METHODS;
	};

	struct  PAJet : ParticleAction
	{
		pVector centerL; // Center of the fan (in local space)
		pDomain accL; // Acceleration vector domain  (in local space)
		pVector center; // Center of the fan
		pDomain acc; // Acceleration vector domain
		float magnitude; // Scales acceleration
		float epsilon; // Softening parameter
		float max_radius; // Only influence particles within max_radius

		_METHODS;
	};

	struct  PAKillOld : ParticleAction
	{
		float age_limit; // Exact age at which to kill particles.
		BOOL kill_less_than; // True to kill particles less than limit.

		_METHODS;
	};

	struct  PAMatchVelocity : ParticleAction
	{
		float magnitude; // The grav of each particle
		float epsilon; // Softening parameter
		float max_radius; // Only influence particles within max_radius

		_METHODS;
	};

	struct  PAMove : ParticleAction
	{
		_METHODS;
	};

	struct  PAOrbitLine : ParticleAction
	{
		pVector pL, axisL; // Endpoints of line to which particles are attracted (in local space)
		pVector p, axis; // Endpoints of line to which particles are attracted
		float magnitude; // Scales acceleration
		float epsilon; // Softening parameter
		float max_radius; // Only influence particles within max_radius

		_METHODS;
	};

	struct  PAOrbitPoint : ParticleAction
	{
		pVector centerL; // Point to which particles are attracted (in local space)
		pVector center; // Point to which particles are attracted
		float magnitude; // Scales acceleration
		float epsilon; // Softening parameter
		float max_radius; // Only influence particles within max_radius

		_METHODS;
	};

	struct  PARandomAccel : ParticleAction
	{
		pDomain gen_accL; // The domain of random accelerations.(in local space)
		pDomain gen_acc; // The domain of random accelerations.

		_METHODS;
	};

	struct  PARandomDisplace : ParticleAction
	{
		pDomain gen_dispL; // The domain of random displacements.(in local space)
		pDomain gen_disp; // The domain of random displacements.

		_METHODS;
	};

	struct  PARandomVelocity : ParticleAction
	{
		pDomain gen_velL; // The domain of random velocities.(in local space)
		pDomain gen_vel; // The domain of random velocities.

		_METHODS;
	};

	struct  PARestore : ParticleAction
	{
		float time_left; // Time remaining until they should be in position.

		_METHODS;
	};

	struct  PAScatter : ParticleAction
	{
		pVector centerL; // Center of the fan (in local space)
		pVector center; // Center of the fan
		float magnitude; // Scales acceleration
		float epsilon; // Softening parameter
		float max_radius; // Only influence particles within max_radius

		_METHODS;
	};

	struct  PASink : ParticleAction
	{
		BOOL kill_inside; // True to dispose of particles *inside* domain
		pDomain positionL; // Disposal region (in local space)
		pDomain position; // Disposal region

		_METHODS;
	};

	struct  PASinkVelocity : ParticleAction
	{
		BOOL kill_inside; // True to dispose of particles with vel *inside* domain
		pDomain velocityL; // Disposal region (in local space)
		pDomain velocity; // Disposal region

		_METHODS;
	};

	struct  PASpeedLimit : ParticleAction
	{
		float min_speed; // Clamp speed to this minimum.
		float max_speed; // Clamp speed to this maximum.

		_METHODS;
	};

	struct  PASource : ParticleAction
	{
		enum
		{
			flSingleSize = (1ul << 29ul),
			// True to get positionB from position.
			flSilent = (1ul << 30ul),
			flVertexB_tracks = (1ul << 31ul),
			// True to get positionB from position.
			fl_FORCEDWORD = u32(-1)
		};

		pDomain positionL; // Choose a position in this domain. (local_space)
		pDomain velocityL; // Choose a velocity in this domain. (local_space)
		pDomain position; // Choose a position in this domain.
		pDomain velocity; // Choose a velocity in this domain.
		pDomain rot; // Choose a rotation in this domain.
		pDomain size; // Choose a size in this domain.
		pDomain color; // Choose a color in this domain.
		float alpha; // Alpha of all generated particles
		float particle_rate; // Particles to generate per unit time
		float age; // Initial age of the particles
		float age_sigma; // St. dev. of initial age of the particles
		pVector parent_vel;
		float parent_motion;

		_METHODS;
	};

	struct  PATargetColor : ParticleAction
	{
		PATargetColor(): timeFrom(0.0f), timeTo(1.0f)
		{
		}

		pVector color; // Color to shift towards
		float alpha; // Alpha value to shift towards
		float scale; // Amount to shift by (1 == all the way)
		float timeFrom;
		float timeTo;

		_METHODS;
	};

	struct  PATargetSize : ParticleAction
	{
		pVector size; // Size to shift towards
		pVector scale; // Amount to shift by per frame (1 == all the way)

		_METHODS;
	};

	struct  PATargetRotate : ParticleAction
	{
		pVector rot; // Rotation to shift towards
		float scale; // Amount to shift by per frame (1 == all the way)

		_METHODS;
	};

	struct  PATargetVelocity : ParticleAction
	{
		pVector velocityL; // Velocity to shift towards (in local space)
		pVector velocity; // Velocity to shift towards
		float scale; // Amount to shift by (1 == all the way)

		_METHODS;
	};

	struct  PAVortex : ParticleAction
	{
		pVector centerL; // Center of vortex (in local space)
		pVector axisL; // Axis around which vortex is applied (in local space)
		pVector center; // Center of vortex
		pVector axis; // Axis around which vortex is applied
		float magnitude; // Scale for rotation around axis
		float epsilon; // Softening parameter
		float max_radius; // Only influence particles within max_radius

		_METHODS;
	};

	struct  PATurbulence : ParticleAction
	{
		float frequency; // Frequency
		int octaves; // Octaves
		float magnitude; // Scale for rotation around axis
		float epsilon; // Softening parameter
		pVector offset; // Offset
		float age;

		_METHODS;
	};
};

//---------------------------------------------------------------------------
#endif
