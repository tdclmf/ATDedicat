#include "stdafx.h"
#include "Level.h"
#include "xrServer.h"
#include "xrServerMapSync.h"

void xrServer::OnProcessClientMapData(NET_Packet& P, ClientID const& clientID)
{
	Msg("--- Sending map verify response to client %u", clientID);

	shared_str client_map_name;
	shared_str client_map_version;
	P.r_stringZ(client_map_name);
	P.r_stringZ(client_map_version);
	u32 client_geom_crc32 = P.r_u32();

	shared_str server_map_name = Level().GetMapData().m_name;
	shared_str server_map_version = Level().GetMapData().m_map_version;
	if (!server_map_name.size() || (0 == xr_strcmp(*server_map_name, "all")))
		server_map_name = level_name(GetConnectOptions());
	if (!server_map_version.size())
		server_map_version = level_version(GetConnectOptions());
	shared_str server_download_url = get_map_download_url(*server_map_name, *server_map_version);
	u32 server_map_crc = Level().GetMapData().m_level_geom_crc32;
	Msg("* [NET_PROXY][MAP] Authoritative map for sync [%s][%s]", *server_map_name, *server_map_version);

	NET_Packet responseP;
	responseP.w_begin(M_SV_MAP_NAME);
	if (server_map_name != client_map_name || server_map_version != client_map_version)
	{
		responseP.w_u8(YouHaveOtherMap);
		Msg("--- Client [%u] has incorrect map version Name [%s != %s] | Version [%s != %s]", clientID.value(),
			*server_map_name, *client_map_name, *server_map_version, *client_map_version);
	}
	else if (server_map_crc != client_geom_crc32) {
		responseP.w_u8(InvalidChecksum);
		Msg("--- Client [%u] has incorrect map crc [%u != %u]", clientID.value(), 
			server_map_crc, client_geom_crc32);
	}
	else {
		responseP.w_u8(SuccessSync);
		Msg("* Client [%u] has same map data [%s == %s] | [%s == %s] | [%u == %u]", clientID.value(),
			*server_map_name, *client_map_name, 
			*server_map_version, *client_map_version,
			server_map_crc, client_geom_crc32);
	}
	// Append authoritative map info so clients can trust server values.
	responseP.w_stringZ(server_map_name);
	responseP.w_stringZ(server_map_version);
	responseP.w_stringZ(server_download_url);
	responseP.w_u32(server_map_crc);

	SendTo(clientID, responseP, net_flags(TRUE, TRUE));
}
