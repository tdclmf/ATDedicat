#pragma once

class CActor;
class CGameObject;

namespace player_actor_context
{
	struct SActorAnchor
	{
		SActorAnchor() { position.set(0.f, 0.f, 0.f); }

		CActor* actor = nullptr;
		Fvector position;
		u16 id = u16(-1);
		LPCSTR name = "";
		float distance = -1.f;
	};

	bool IsRuntimePlayerActor(const CActor* actor);
	void RegisterRuntimePlayer(CActor* actor);
	void UnregisterRuntimePlayer(CActor* actor);
	CActor* FindNearestRuntimePlayer(const Fvector& position, float* distance = nullptr);
	bool FindNearestRuntimePlayerAnchor(const Fvector& position, SActorAnchor& anchor);
}
