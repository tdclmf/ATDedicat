#include "stdafx.h"
#include "xrserver.h"
#include "game_sv_single.h"
#include "game_sv_deathmatch.h"
#include "game_sv_teamdeathmatch.h"
#include "game_sv_artefacthunt.h"
#include "xrMessages.h"
#include "game_cl_artefacthunt.h"
#include "game_cl_single.h"
#include "MainMenu.h"
#include "../xrEngine/x_ray.h"
#include "file_transfer.h"
#include "screenshot_server.h"
#include "../xrNetServer/NET_AuthCheck.h"
#pragma warning(push)
#pragma warning(disable:4995)
#include <malloc.h>
#pragma warning(pop)

LPCSTR xrServer::get_map_download_url(LPCSTR level_name, LPCSTR level_version)
{
	R_ASSERT(level_name && level_version);
	LPCSTR ret_url = "";
	CInifile* level_ini = pApp->GetArchiveHeader(level_name, level_version);
	if (!level_ini)
	{
		Msg("! Warning: level [%s][%s] has not header ltx", level_name, level_version);
		return ret_url;
	}

	ret_url = level_ini->r_string_wb("header", "link").c_str();
	if (!ret_url)
		ret_url = "";

	return ret_url;
}

xrServer::EConnect xrServer::Connect(shared_str& session_name, GameDescriptionData& game_descr)
{
#ifdef DEBUG
	Msg						("* sv_Connect: %s",	*session_name);
#endif

	// Parse options and create game
	if (0 == strchr(*session_name, '/'))
		return ErrConnect;

	string1024 options;
	R_ASSERT2(xr_strlen(session_name) <= sizeof(options), "session_name too BIIIGGG!!!");
	xr_strcpy(options, strchr(*session_name, '/') + 1);

	// Parse game type
	string1024 type;
	R_ASSERT2(xr_strlen(options) <= sizeof(type), "session_name too BIIIGGG!!!");
	xr_strcpy(type, options);
	if (strchr(type, '/')) *strchr(type, '/') = 0;
	game = NULL;

	CLASS_ID clsid = game_GameState::getCLASS_ID(type, true);
	game = smart_cast<game_sv_GameState*>(NEW_INSTANCE(clsid));

	// Options
	if (0 == game) return ErrConnect;
	//	game->type				= type_id;
	m_file_transfers = xr_new<file_transfer::server_site>();
	initialize_screenshot_proxies();
	LoadServerInfo();

	Msg("* Created server_game %s", game->type_name());

	ZeroMemory(&game_descr, sizeof(game_descr));
	xr_strcpy(game_descr.map_name, game->level_name(session_name.c_str()).c_str());
	xr_strcpy(game_descr.map_version, game_sv_GameState::parse_level_version(session_name.c_str()).c_str());
	xr_strcpy(game_descr.download_url, get_map_download_url(game_descr.map_name, game_descr.map_version));

	game->Create(session_name);

	return IPureServer::Connect(*session_name, game_descr);
}


IClient* xrServer::new_client(SClientConnectData* cl_data)
{
	IClient* CL = client_Find_Get(cl_data->clientID);
	VERIFY(CL);

	// copy entity
	CL->ID = cl_data->clientID;
	CL->process_id = cl_data->process_id;
	CL->name = cl_data->name; //only for offline mode
	CL->pass._set(cl_data->pass);

	NET_Packet P;
	P.B.count = 0;
	P.r_pos = 0;

	game->AddDelayedEvent(P, GAME_EVENT_CREATE_CLIENT, 0, CL->ID);

	Msg("* Adding new client! [%u]", CL->ID.value());
	return CL;
}

void xrServer::AttachNewClient(IClient* CL)
{
	MSYS_CONFIG msgConfig;
	msgConfig.sign1 = 0x12071980;
	msgConfig.sign2 = 0x26111975;

	// Authoritative map sync payload for client startup.
	// Prefer currently loaded level data over startup connect options.
	shared_str map_name = Level().GetMapData().m_name;
	shared_str map_version = Level().GetMapData().m_map_version;

	if (!map_name.size() || (0 == xr_strcmp(*map_name, "all")))
		map_name = level_name(GetConnectOptions());
	if (!map_name.size() || (0 == xr_strcmp(*map_name, "all")))
		map_name = Level().name();
	if (!map_version.size())
		map_version = level_version(GetConnectOptions());

	shared_str download_url = get_map_download_url(*map_name, *map_version);
	Msg("* [NET_PROXY][MAP] AttachNewClient payload [%s][%s]", *map_name, *map_version);

	strncpy(msgConfig.mdata.map_name, *map_name, sizeof(msgConfig.mdata.map_name));
	msgConfig.mdata.map_name[sizeof(msgConfig.mdata.map_name) - 1] = 0;
	strncpy(msgConfig.mdata.map_version, *map_version, sizeof(msgConfig.mdata.map_version));
	msgConfig.mdata.map_version[sizeof(msgConfig.mdata.map_version) - 1] = 0;
	strncpy(msgConfig.mdata.download_url, *download_url, sizeof(msgConfig.mdata.download_url));
	msgConfig.mdata.download_url[sizeof(msgConfig.mdata.download_url) - 1] = 0;


	SendTo_LL(CL->ID, &msgConfig, sizeof(msgConfig), net_flags(TRUE, TRUE, TRUE, TRUE));
	Server_Client_Check(CL);

	// gen message
	Check_GameSpy_CDKey_Success(CL);

	CL->m_guid[0] = 0;
}

void xrServer::RequestClientDigest(IClient* CL)
{
	if (CL == GetServerClient())
	{
		CL->flags.bVerified = TRUE;
		SendConnectResult(CL, 1, 0, "we are host");
		return;
	}

	xrClientData* tmp_client = smart_cast<xrClientData*>(CL);
	VERIFY(tmp_client);
	PerformSecretKeysSync(tmp_client);

	NET_Packet P;
	P.w_begin(M_SV_DIGEST);
	SendTo(CL->ID, P);
}

#define NET_BANNED_STR	"Player banned by server!"

void xrServer::ProcessClientDigest(xrClientData* xrCL, NET_Packet* P)
{
	R_ASSERT(xrCL);
	IClient* tmp_client = static_cast<IClient*>(xrCL);
	game_sv_mp* server_game = smart_cast<game_sv_mp*>(game);
	P->r_stringZ(xrCL->m_cdkey_digest);
	shared_str admin_name;
	if (server_game && server_game->IsPlayerBanned(xrCL->m_cdkey_digest.c_str(), admin_name))
	{
		R_ASSERT2(tmp_client != GetServerClient(), "can't disconnect server client");
		Msg("--- Client [%s] tried to connect - rejecting connection (he is banned by %s) ...",
		    tmp_client->m_cAddress.to_string().c_str(),
		    admin_name.size() ? admin_name.c_str() : "Server");
		xr_string message_to_user = "mp_you_have_been_banned_by ";
		if (admin_name.size()) message_to_user += *admin_name;
		SendConnectResult(tmp_client, 0, ecr_have_been_banned, (char*)message_to_user.c_str());
		return;
	}
	GetPooledState(xrCL);
	PerformSecretKeysSync(xrCL);
	xrCL->flags.bVerified = TRUE;
	SendConnectResult(xrCL, 1, 0, "Client connect accepted");
}
