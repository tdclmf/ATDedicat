#include "stdafx.h"
#include "player_actor_context.h"
#include "Level.h"
#include "actor.h"
#include "../xrEngine/xr_object_list.h"

namespace player_actor_context
{
	namespace
	{
		bool IsObjectStillRegistered(const CActor* actor)
		{
			if (!g_pGameLevel || !actor)
				return false;

			return Level().Objects.net_Find(actor->ID()) == actor;
		}
	}

	bool IsRuntimePlayerActor(const CActor* actor)
	{
		if (!IsObjectStillRegistered(actor))
			return false;

		if (!actor || actor->ID() == 0 || !actor->getEnabled() || !actor->g_Alive())
			return false;

		return 0 == xr_strcmp(actor->cNameSect().c_str(), "mp_actor");
	}

	void RegisterRuntimePlayer(CActor* actor)
	{
		if (!g_pGameLevel || !IsRuntimePlayerActor(actor))
			return;

		xr_vector<CActor*>& players = Level().m_mp_players;
		if (std::find(players.begin(), players.end(), actor) == players.end())
			players.push_back(actor);
	}

	void UnregisterRuntimePlayer(CActor* actor)
	{
		if (!g_pGameLevel || !actor)
			return;

		xr_vector<CActor*>& players = Level().m_mp_players;
		players.erase(std::remove(players.begin(), players.end(), actor), players.end());
	}

	CActor* FindNearestRuntimePlayer(const Fvector& position, float* distance)
	{
		if (distance)
			*distance = -1.f;

		if (!g_pGameLevel)
			return nullptr;

		CActor* best_actor = nullptr;
		float best_distance = flt_max;

		xr_vector<CActor*>& players = Level().m_mp_players;
		for (xr_vector<CActor*>::iterator it = players.begin(); it != players.end(); )
		{
			CActor* actor = *it;
			if (!IsObjectStillRegistered(actor))
			{
				it = players.erase(it);
				continue;
			}

			++it;

			if (!IsRuntimePlayerActor(actor))
				continue;

			const float current_distance = position.distance_to(actor->Position());
			if (current_distance >= best_distance)
				continue;

			best_actor = actor;
			best_distance = current_distance;
		}

		if (!best_actor)
		{
			const u32 object_count = Level().Objects.o_count();
			for (u32 i = 0; i < object_count; ++i)
			{
				CObject* object = Level().Objects.o_get_by_iterator(i);
				CActor* actor = smart_cast<CActor*>(object);
				if (!IsRuntimePlayerActor(actor))
					continue;

				RegisterRuntimePlayer(actor);

				const float current_distance = position.distance_to(actor->Position());
				if (current_distance >= best_distance)
					continue;

				best_actor = actor;
				best_distance = current_distance;
			}
		}

		if (!best_actor)
			return nullptr;

		if (distance)
			*distance = best_distance;

		return best_actor;
	}

	bool FindNearestRuntimePlayerAnchor(const Fvector& position, SActorAnchor& anchor)
	{
		float distance = -1.f;
		CActor* actor = FindNearestRuntimePlayer(position, &distance);
		if (!actor)
			return false;

		anchor.actor = actor;
		anchor.position.set(actor->Position());
		anchor.id = actor->ID();
		anchor.name = actor->cName().c_str();
		anchor.distance = distance;
		return true;
	}
}
