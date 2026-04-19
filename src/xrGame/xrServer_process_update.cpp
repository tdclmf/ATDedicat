#include "stdafx.h"
#include "xrServer.h"
#include "xrServer_Objects.h"

int g_Dump_Update_Read = 0;

void xrServer::Process_update(NET_Packet& P, ClientID sender)
{
	xrClientData* CL = ID_to_client(sender);
	R_ASSERT2(CL, "Process_update client not found");

#ifndef MASTER_GOLD
	if (g_Dump_Update_Read) Msg("---- UPDATE_Read --- ");
#endif // #ifndef MASTER_GOLD

	R_ASSERT(CL->flags.bLocal);
	// while has information
	while (!P.r_eof())
	{
		// find entity
		u16 ID;
		u8 size;

		P.r_u16(ID);
		P.r_u8(size);
		u32 _pos = P.r_tell();

		if (size > P.r_elapsed())
		{
			Msg("! [NET_SAFE] Process_update malformed chunk: initiator=0x%08x id=%u size=%u elapsed=%u pos=%u",
			    CL->ID.value(), ID, size, P.r_elapsed(), _pos);
			break;
		}

		CSE_Abstract* E = ID_to_entity(ID);

		if (E)
		{
			//Msg				("sv_import: %d '%s'",E->ID,E->name_replace());
			E->net_Ready = TRUE;
			const u32 chunk_end = _pos + size;
			bool update_read_ok = true;
#if defined(_WIN32)
			__try
			{
				E->UPDATE_Read(P);
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				update_read_ok = false;
			}
#else
			E->UPDATE_Read(P);
#endif

			if (g_Dump_Update_Read) Msg("* %s : %d - %d", E->name(), size, P.r_tell() - _pos);

			if (!update_read_ok)
			{
				string16 tmp;
				CLSID2TEXT(E->m_tClassID, tmp);
				Msg("! [NET_SAFE] Process_update exception while reading chunk '%s'; initiator=0x%08x, objectID=%d, size=%d, pos=%d",
				    tmp, CL->ID.value(), E->ID, size, _pos);

				P.r_seek(chunk_end);
				continue;
			}

			if ((P.r_tell() - _pos) != size)
			{
				string16 tmp;
				CLSID2TEXT(E->m_tClassID, tmp);

				Msg("! Error: Beer from the creator of '%s'; initiator: 0x%08x, r_tell() = %d, pos = %d, objectID = %d, size = %d\nAre you sure that the save is from this modpack?",
				            tmp, CL->ID.value(), P.r_tell(), _pos, E->ID, size);

				// Resync packet reader to the declared end of this chunk to avoid parser drift and crashes.
				P.r_seek(chunk_end);
			}
		}
		else
			P.r_advance(size);
	}
#ifndef MASTER_GOLD
	if (g_Dump_Update_Read) Msg("-------------------- ");
#endif // #ifndef MASTER_GOLD
}

void xrServer::Process_save(NET_Packet& P, ClientID sender)
{
	xrClientData* CL = ID_to_client(sender);
	R_ASSERT2(CL, "Process_save client not found");
	CL->net_Ready = TRUE;

	R_ASSERT(CL->flags.bLocal);
	// while has information
	while (!P.r_eof())
	{
		// find entity
		u16 ID;
		u16 size;

		P.r_u16(ID);
		P.r_u16(size);
		s32 _pos_start = P.r_tell();
		CSE_Abstract* E = ID_to_entity(ID);

		if (E)
		{
			E->net_Ready = TRUE;
			E->load(P);
		}
		else
			P.r_advance(size);
		s32 _pos_end = P.r_tell();
		s32 _size = size;
		if (_size != (_pos_end - _pos_start))
		{
			Msg("! load/save mismatch, object: '%s'", E ? E->name_replace() : "unknown");
			s32 _rollback = _pos_start + _size;
			P.r_seek(_rollback);
		}
	}
}
