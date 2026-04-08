#include "stdafx.h"

#include "IGame_Level.h"
#include "Feel_Vision.h"
#include "Render.h"
#include "xr_collide_form.h"
#include "cl_intersect.h"
using Feel::Vision;

Vision::Vision(CObject const* owner) :
	pure_relcase(&Vision::feel_vision_relcase),
	m_owner(owner)
{
}

Vision::~Vision()
{
}

struct SFeelParam {
	Vision* parent;
	Vision::feel_visible_Item* item;
	float						vis;
	float						vis_threshold;
	SFeelParam(Vision* _parent, Vision::feel_visible_Item* _item, float _vis_threshold) :parent(_parent), item(_item), vis(1.f), vis_threshold(_vis_threshold) {}
};
IC BOOL feel_vision_callback(collide::rq_result& result, LPVOID params)
{
	SFeelParam* fp = (SFeelParam*)params;
	float vis = result.O&&result.O->ParentOwner()&&fp->item->O==result.O->ParentOwner() ? 1.f : fp->parent->feel_vision_mtl_transp(result.O, result.element);
	fp->vis *= vis;
	if (nullptr == result.O && fis_zero(vis)) {
		CDB::TRI* T = g_pGameLevel->ObjectSpace.GetStaticTris() + result.element;
		Fvector* V = g_pGameLevel->ObjectSpace.GetStaticVerts();
		fp->item->Cache.verts[0].set(V[T->verts[0]]);
		fp->item->Cache.verts[1].set(V[T->verts[1]]);
		fp->item->Cache.verts[2].set(V[T->verts[2]]);
	}
	return (fp->vis > fp->vis_threshold);
}

void	Vision::o_new(CObject* O)
{
	feel_visible[items_idx_calc].push_back(feel_visible_Item());
	feel_visible_Item& I = feel_visible[items_idx_calc].back();
	I.O = O;
	I.Cache_vis = 1.f;
	I.Cache.verts[0].set(0, 0, 0);
	I.Cache.verts[1].set(0, 0, 0);
	I.Cache.verts[2].set(0, 0, 0);
	I.fuzzy = -EPS_S;
	TRY
	I.cp_LP = O->get_new_local_point_on_mesh(I.bone_id);
	I.cp_LAST = O->get_last_local_point_on_mesh(I.cp_LP, I.bone_id);
	CATCH
}
void	Vision::o_delete(CObject* O)
{
	auto it = std::find_if(feel_visible[items_idx_calc].begin(), feel_visible[items_idx_calc].end(),
		[O](const feel_visible_Item& item) { return item.O == O; });

	if (it != feel_visible[items_idx_calc].end()) {
		feel_visible[items_idx_calc].erase(it);
	}
}

void	Vision::feel_vision_clear()
{
	{
		feel_visible[0].clear();
		feel_visible[1].clear();
	}
}

void	Vision::feel_vision_relcase(CObject* object)
{
	{
		auto it = std::find_if(feel_visible[0].begin(), feel_visible[0].end(),
			[=](const feel_visible_Item& item) { return item.O == object; });

		if (it != feel_visible[0].end())
			feel_visible[0].erase(it);
	}
	{
		auto it = std::find_if(feel_visible[1].begin(), feel_visible[1].end(),
			[=](const feel_visible_Item& item) { return item.O == object; });

		if (it != feel_visible[1].end())
			feel_visible[1].erase(it);
	}
}

void Vision::feel_vision_query(Fmatrix& mFull, Fvector& P)
{
	{
		xrCriticalSection::raii guard(cs);
		u8 idx = items_idx_calc;
		items_idx_get = idx;
		items_idx_calc = (idx + u8(1)) % u8(2);
	}

	Frustum.CreateFromMatrix(mFull, FRUSTUM_P_LRTB | FRUSTUM_P_FAR);
}

void	Vision::feel_vision_update(CObject* parent, Fvector& P, float dt, float vis_threshold)
{
	PROF_EVENT("feel_vision_update");
	// Traverse object database

	g_SpatialSpace->q_frustum
	(
		r_spatial,
		0,
		STYPE_VISIBLEFORAI,
		Frustum
	);
	feel_visible[items_idx_calc].clear();
	for (ISpatial* spatial : r_spatial)
	{

		CObject* object = spatial->dcast_CObject();
		if (!object || object->getDestroy())
			continue;
		if (object->ParentOwner())
		{
			object = object->ParentOwner();
		}
		if (!object || object->getDestroy())
			continue;
		if (!object->Visual() || !feel_vision_isRelevant(object))
			continue;

		o_new(object);
	}
	o_trace(P, dt, vis_threshold);
}

void Vision::o_trace(Fvector& P, float dt, float vis_threshold)
{
	RQR.r_clear();
	for (feel_visible_Item& item : feel_visible[items_idx_calc])
	{
		if (!item.O->CFORM())
			{ item.fuzzy = -1; continue; }

		item.cp_LR_dst = item.O->Position();
		item.cp_LR_src = P;
		TRY
		item.cp_LAST = item.O->get_last_local_point_on_mesh(item.cp_LP, item.bone_id);
		CATCH

		// 
		Fvector D, OP = item.cp_LAST;
		D.sub(OP, P);
		if (fis_zero(D.magnitude()))
		{
			item.fuzzy = 1.f;
			continue;
		}

		float f = D.magnitude() + .2f;
		if (f > fuzzy_guaranteed)
		{
			D.div(f);
			// setup ray defs & feel params
			collide::ray_defs RD(P, D, f, CDB::OPT_CULL, collide::rq_target(collide::rqtStatic |/**/collide::rqtObject |/**/collide::rqtObstacle));
			SFeelParam	feel_params(this, &item, vis_threshold);
			// check cache
			if (item.Cache.result && item.Cache.similar(P, D, f))
			{
				// similar with previous query
				feel_params.vis = item.Cache_vis;
				//					Log("cache 0");
			}
			else
			{
				float _u, _v, _range;
				if (CDB::TestRayTri(P, D, item.Cache.verts, _u, _v, _range, false) && (_range > 0 && _range < f))
				{
					feel_params.vis = 0.f;
					//						Log("cache 1");
				}
				else
				{
					// cache outdated. real query.
					VERIFY(!fis_zero(RD.dir.magnitude()));

					if (g_pGameLevel->ObjectSpace.RayQuery(RQR, RD, feel_vision_callback, &feel_params, nullptr, nullptr))
					{
						item.Cache_vis = feel_params.vis;
						item.Cache.set(P, D, f, TRUE);
					}
					else
					{
						//							feel_params.vis	= 0.f;
						//							item.Cache_vis	= feel_params.vis	;
						item.Cache.set(P, D, f, FALSE);
					}
					//						Log("query");
				}
			}
			g_SpatialSpace->q_ray(r_spatial, 0, STYPE_VISIBLEFORAI, P, D, f);

			RD.flags = CDB::OPT_ONLYFIRST;

			bool collision_found = false;

			for (ISpatial* S : r_spatial)
			{
				CObject* object = S->dcast_CObject();

				if (!object || object->getDestroy())
					continue;

				if (object == m_owner)
					continue;

				if (object == item.O)
					continue;

				if (object == item.O->ParentHolder())
					continue;

				RQR.r_clear();
				if (object && object->collidable.model && !object->collidable.model->_RayQuery(RD, RQR))
					continue;

				collision_found = true;
				break;
			}

			if (collision_found)
				feel_params.vis = 0.f;

			if (feel_params.vis < feel_params.vis_threshold)
			{
				// INVISIBLE, choose next point
				item.fuzzy -= fuzzy_update_novis * dt;
				clamp(item.fuzzy, -.5f, 1.f);
				TRY
				item.cp_LP = item.O->get_new_local_point_on_mesh(item.bone_id);
				CATCH
			}
			else
			{
				// VISIBLE
				item.fuzzy += fuzzy_update_vis * dt;
				clamp(item.fuzzy, -.5f, 1.f);
			}
		}
		else
		{
			// VISIBLE, 'cause near
			item.fuzzy += fuzzy_update_vis * dt;
			clamp(item.fuzzy, -.5f, 1.f);
		}
	}
}