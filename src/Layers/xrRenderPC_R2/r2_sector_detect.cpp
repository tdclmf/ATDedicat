#include "stdafx.h"
#include "r2.h"

int CRender::translateSector(IRender_Sector* pSector)
{
	if (!pSector)
		return -1;

	for (u32 i = 0; i < Sectors.size(); ++i)
	{
		if (Sectors[i] == pSector)
			return i;
	}

	FATAL("Sector was not found!");
	NODEFAULT;

#ifdef DEBUG
	return			(-1);
#endif // #ifdef DEBUG
}
static thread_local xrXRC xrc;
IRender_Sector* CRender::detectSector(const Fvector& P)
{
	IRender_Sector* S = NULL;
	Fvector dir;
	xrc.ray_options(CDB::OPT_ONLYNEAREST);

	dir.set(0, -1, 0);
	S = detectSector(P, dir);
	if (NULL == S)
	{
		dir.set(0, 1, 0);
		S = detectSector(P, dir);
	}
	return S;
}

IRender_Sector* CRender::detectSector(const Fvector& P, Fvector& dir)
{
	// Portals model
	int id1 = -1;
	float range1 = 500.f;
	if (rmPortals)
	{
		xrc.ray_query(rmPortals, P, dir, range1);
		if (xrc.r_count())
		{
			CDB::RESULT* RP1 = xrc.r_begin();
			id1 = RP1->id;
			range1 = RP1->range;
		}
	}

	// Geometry model
	int id2 = -1;
	float range2 = range1;
	xrc.ray_query(g_pGameLevel->ObjectSpace.GetStaticModel(), P, dir, range2);
	if (xrc.r_count())
	{
		CDB::RESULT* RP2 = xrc.r_begin();
		id2 = RP2->id;
		range2 = RP2->range;
	}

	// Select ID
	int ID;
	if (id1 >= 0)
	{
		if (id2 >= 0) ID = (range1 <= range2 + EPS) ? id1 : id2; // both was found
		else ID = id1; // only id1 found
	}
	else if (id2 >= 0) ID = id2; // only id2 found
	else return 0;

	if (ID == id1)
	{
		// Take sector, facing to our point from portal
		CDB::TRI* pTri = rmPortals->get_tris() + ID;
		if (pTri->dummy < Portals.size())
		{
			if (CPortal* pPortal = (CPortal*)Portals[pTri->dummy])
				return pPortal->getSectorFacing(P);
		}
	}
	else
	{
		// Take triangle at ID and use it's Sector
		CDB::TRI* pTri = g_pGameLevel->ObjectSpace.GetStaticTris() + ID;
		if (pTri->sector < Sectors.size())
		{
			if (IRender_Sector* sector = getSector(pTri->sector))
				return sector;
		}
	}
	return 0;
}
