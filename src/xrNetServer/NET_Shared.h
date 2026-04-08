#pragma once
#ifdef XR_NETSERVER_EXPORTS
#define XRNETSERVER_API __declspec(dllexport)
#else
#define XRNETSERVER_API __declspec(dllimport)

#ifndef _EDITOR
#pragma comment(lib, "xrNetServer" )
#endif
#endif

#include "../xrCore/net_utils.h"
#include "NET_Messages.h"
#include "NET_Server.h"
#include "NET_Client.h"
#include "NET_Stats.h"
#include "NET_Compressor.h"

XRNETSERVER_API extern ClientID BroadcastCID;
XRNETSERVER_API extern Flags32 psNET_Flags;
XRNETSERVER_API extern int psNET_ClientUpdate;
XRNETSERVER_API extern int psNET_ClientPending;
XRNETSERVER_API extern char psNET_Name[];
XRNETSERVER_API extern int psNET_ServerUpdate;
XRNETSERVER_API extern int psNET_ServerPending;
XRNETSERVER_API extern BOOL psNET_direct_connect;
XRNETSERVER_API extern int psNET_GuaranteedPacketMode;


IC u32 TimeGlobal(CTimer* timer) { return timer->GetElapsed_ms(); }
IC u32 TimerAsync(CTimer* timer) { return TimeGlobal(timer); }

enum enmDisconnectReason
{
	EUnknownReason = k_ESteamNetConnectionEnd_App_Min + 100,
	EDetailedReason = k_ESteamNetConnectionEnd_App_Min + 200,
	EInvalidPassword = k_ESteamNetConnectionEnd_App_Min + 300,
	ESessionFull = k_ESteamNetConnectionEnd_App_Min + 400,
	EPlayerBanned = k_ESteamNetConnectionEnd_App_Min + 500,
	EServerShutdown = k_ESteamNetConnectionEnd_App_Min + 600
};

enum
{
	NETFLAG_MINIMIZEUPDATES = (1 << 0),
	NETFLAG_DBG_DUMPSIZE = (1 << 1),
	NETFLAG_LOG_SV_PACKETS = (1 << 2),
	NETFLAG_LOG_CL_PACKETS = (1 << 3),
};