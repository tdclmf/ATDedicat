#include "stdafx.h"
#include "level.h"
#include "xrServerMapSync.h"
#include "../xrCore/stream_reader.h"
#include "MainMenu.h"
#include "string_table.h"
#include "../xrEngine/xr_ioconsole.h"

void CLevel::CalculateLevelCrc32()
{
	Msg("* calculating checksum of level.geom");
	IReader* geom = FS.r_open("$level$", "level.geom");
	R_ASSERT2(geom, "failed to open level.geom file");
	map_data.m_level_geom_crc32 = crc32(geom->pointer(), 512);
	FS.r_close(geom);
}

bool CLevel::synchronize_map_data()
{
	if (!OnClient() && !IsDemoSave())
	{
		deny_m_spawn = FALSE;
		map_data.m_map_sync_received = true;
		return synchronize_client();
	}

	Msg("* synchronizing map data...");

	map_data.CheckToSendMapSync();

	ClientReceive();

	if (map_data.m_wait_map_time >= 1000 && !map_data.m_map_sync_received && !IsDemoPlay())
	{
		Msg("Wait map data time out: reconnecting...");
		MakeReconnect();
		g_loading_events.erase(++g_loading_events.begin(), g_loading_events.end());
		return true;
	}

	if (!map_data.m_map_sync_received)
	{
		Sleep(5);
		++map_data.m_wait_map_time;
		return false;
	}

	if (map_data.IsInvalidMapOrVersion())
	{
		Msg("! Incorect map or version");
		connected_to_server = FALSE;
		OnSessionTerminate("ui_st_conn_version_differs");
		g_loading_events.erase(++g_loading_events.begin(), g_loading_events.end());
		return true;
	}

	if (map_data.IsInvalidClientChecksum())
	{
		connected_to_server = FALSE;
		OnSessionTerminate("ui_st_conn_version_differs");
		return false;
	}
	return synchronize_client();
}

bool CLevel::synchronize_client()
{
	//---------------------------------------------------------------------------
	if (!sended_request_connection_data)
	{
		NET_Packet P;
		P.w_begin(M_CLIENT_REQUEST_CONNECTION_DATA);

		Send(P, net_flags(TRUE, TRUE, TRUE, TRUE));
		sended_request_connection_data = true;
	}
	//---------------------------------------------------------------------------
	if (game_configured)
	{
		deny_m_spawn = FALSE;
		return true;
	}

	Msg("--- Waiting for server configuration...");

	if (Server)
	{
		ClientReceive();
		Server->Update();
	} // if OnClient ClientReceive method called in upper invokation
	//Sleep(5); 
	return game_configured;
}


void LevelMapSyncData::CheckToSendMapSync()
{
	if (!m_sended_map_name_request)
	{
		NET_Packet P;
		P.w_begin(M_SV_MAP_NAME);
		P.w_stringZ(m_name);
		P.w_stringZ(m_map_version);
		P.w_u32(m_level_geom_crc32);
		Level().Send(P, net_flags(TRUE, TRUE, TRUE, TRUE));
		m_sended_map_name_request = true;
		invalid_geom_checksum = false;
		m_map_sync_received = false;
		m_wait_map_time = 0;
	}
}

void LevelMapSyncData::ReceiveServerMapSync(NET_Packet& P)
{
	m_map_sync_received = true;
	MapSyncResponse server_resp = (MapSyncResponse)P.r_u8();
	if (server_resp == InvalidChecksum)
		invalid_geom_checksum = true;
	else if (server_resp == YouHaveOtherMap)
		invalid_map_or_version = true;
}
