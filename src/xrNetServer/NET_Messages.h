#pragma once
#include "SteamWorksSDK/include/steamnetworkingsockets.h"
#include "SteamWorksSDK/include/isteamnetworkingutils.h"

IC u32 net_flags(BOOL bReliable = FALSE, BOOL bSequental = TRUE, BOOL bHighPriority = FALSE, BOOL bSendImmediately = FALSE)
{
    if (bReliable)
    {
        if (bSendImmediately)
            return k_nSteamNetworkingSend_ReliableNoNagle | k_nSteamNetworkingSend_NoDelay;
        if (bHighPriority)
            return k_nSteamNetworkingSend_ReliableNoNagle;
        return k_nSteamNetworkingSend_Reliable;
    }
    if (bSendImmediately)
        return k_nSteamNetworkingSend_UnreliableNoDelay;
    if (bHighPriority)
        return k_nSteamNetworkingSend_UnreliableNoNagle;
    return k_nSteamNetworkingSend_Unreliable;
}

struct GameDescriptionData
{
    string128 map_name;
    string128 map_version;
    string512 download_url;
};

struct MSYS_CONFIG
{
	u32 sign1; // 0x12071980;
	u32 sign2; // 0x26111975;
    GameDescriptionData mdata;
};

struct MSYS_PING
{
	u32 sign1; // 0x12071980;
	u32 sign2; // 0x26111975;
	u32 dwTime_ClientSend;
	u32 dwTime_Server;
	u32 dwTime_ClientReceive;
};

struct SClientConnectData
{
    ClientID clientID;
    string64 name;
    string64 pass;
    u32 process_id;

    SClientConnectData()
    {
        name[0] = pass[0] = 0;
        process_id = 0;
    }
};