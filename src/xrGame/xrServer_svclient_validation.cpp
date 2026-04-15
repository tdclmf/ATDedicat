#include "stdafx.h"
#include "xrServer_svclient_validation.h"
#include "GameObject.h"
#include "Level.h"

extern ENGINE_API bool g_dedicated_server;

bool should_validate_object_on_svclient()
{
	// Dedicated+single bridge can legitimately miss local mirror objects
	// for remote clients while server entities are still valid.
	return !(g_dedicated_server && IsGameTypeSingle());
}

bool is_object_valid_on_svclient(u16 id_entity)
{
	if (!should_validate_object_on_svclient())
		return true;

	CObject* tmp_obj = Level().Objects.net_Find(id_entity);
	if (!tmp_obj)
		return false;

	CGameObject* tmp_gobj = smart_cast<CGameObject*>(tmp_obj);
	if (!tmp_gobj)
		return false;

	if (tmp_obj->getDestroy())
		return false;

	if (tmp_gobj->object_removed())
		return false;

	return true;
};
