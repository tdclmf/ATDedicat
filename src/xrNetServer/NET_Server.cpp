#include "stdafx.h"
#include "NET_Common.h"
#include "NET_Server.h"
#include "NET_Log.h"
#include "NET_Compressor.h"
#include "NET_Shared.h"
#include "NET_Stats.h"


static INetLog* pSvNetLog = NULL;

XRNETSERVER_API int psNET_ServerUpdate = 30; // FPS
XRNETSERVER_API int psNET_ServerPending = 3;

XRNETSERVER_API ClientID BroadcastCID(0xffffffff);
XRNETSERVER_API IPureServer* sptr;

namespace
{
struct RemoteConnectedClientsCounter
{
    u32 count;

    RemoteConnectedClientsCounter() : count(0) {}

    void operator()(IClient* client)
    {
        if (!client)
            return;
        if (!client->flags.bConnected)
            return;
        if (client->flags.bLocal)
            return;
        ++count;
    }
};
} // namespace

// Реализация методов ip_address
void ip_address::set(LPCSTR src_string)
{
    u32 buff[4];
    int cnt = sscanf(src_string, "%d.%d.%d.%d", &buff[0], &buff[1], &buff[2], &buff[3]);
    if (cnt == 4)
    {
        m_data.a1 = u8(buff[0] & 0xff);
        m_data.a2 = u8(buff[1] & 0xff);
        m_data.a3 = u8(buff[2] & 0xff);
        m_data.a4 = u8(buff[3] & 0xff);
    }
    else
    {
        Msg("! Bad ipAddress format [%s]", src_string);
        m_data.data = 0;
    }
}

xr_string ip_address::to_string() const
{
    string128 res;
    xr_sprintf(res, sizeof(res), "%d.%d.%d.%d", m_data.a1, m_data.a2, m_data.a3, m_data.a4);
    return res;
}

// Реализация IBannedClient
void IBannedClient::Load(CInifile& ini, const shared_str& sect)
{
    HAddr.set(sect.c_str());

    tm _tm_banned;
    const shared_str& time_to = ini.r_string(sect, "time_to");
    int res_t = sscanf(time_to.c_str(),
        "%02d.%02d.%d_%02d:%02d:%02d",
        &_tm_banned.tm_mday,
        &_tm_banned.tm_mon,
        &_tm_banned.tm_year,
        &_tm_banned.tm_hour,
        &_tm_banned.tm_min,
        &_tm_banned.tm_sec);
    VERIFY(res_t == 6);

    _tm_banned.tm_mon -= 1;
    _tm_banned.tm_year -= 1900;
    BanTime = mktime(&_tm_banned);

    Msg("- loaded banned client %s to %s", HAddr.to_string().c_str(), BannedTimeTo().c_str());
}

void IBannedClient::Save(CInifile& ini)
{
    ini.w_string(HAddr.to_string().c_str(), "time_to", BannedTimeTo().c_str());
}

xr_string IBannedClient::BannedTimeTo() const
{
    string256 res;
    tm* _tm_banned;
    _tm_banned = _localtime64(&BanTime);
    xr_sprintf(res, sizeof(res),
        "%02d.%02d.%d_%02d:%02d:%02d",
        _tm_banned->tm_mday,
        _tm_banned->tm_mon + 1,
        _tm_banned->tm_year + 1900,
        _tm_banned->tm_hour,
        _tm_banned->tm_min,
        _tm_banned->tm_sec);

    return res;
}

// Реализация IClient
IClient::IClient(CTimer* timer)
    : stats(timer), server(NULL)
{
    dwTime_LastUpdate = 0;
    flags.bLocal = FALSE;
    flags.bConnected = FALSE;
    flags.bReconnect = FALSE;
    flags.bVerified = TRUE;
}

void IClient::_SendTo_LL(const void* data, u32 size, u32 flags, u32 timeout)
{
    R_ASSERT(server);
    server->IPureServer::SendTo_LL(ID, const_cast<void*>(data), size, flags, timeout);
}

// Реализация IPureServer
IPureServer::IPureServer(CTimer* timer, BOOL Dedicated)
    : m_bDedicated(Dedicated)
#ifdef PROFILE_CRITICAL_SECTIONS
    , csMessage(MUTEX_PROFILE_ID(IPureServer::csMessage))
#endif
{
    device_timer = timer;
    stats.clear();
    stats.dwSendTime = TimeGlobal(device_timer);

    SV_Client = NULL;
    SteamInterfaceP = nullptr;
    OpenSocket = k_HSteamListenSocket_Invalid;
    PoolGroopPlayers = k_HSteamNetPollGroup_Invalid;

    pSvNetLog = NULL;
    sptr = this;
}

IPureServer::~IPureServer()
{
    for (u32 it = 0; it < BannedAddresses.size(); it++)
        xr_delete(BannedAddresses[it]);
    BannedAddresses.clear();

    SV_Client = NULL;
    xr_delete(pSvNetLog);
    Disconnect();
}

IPureServer::EConnect IPureServer::Connect(LPCSTR options, GameDescriptionData& game_descr)
{
    connect_options = options;
    M_MaxPlayers = 32;

    string4096 session_name;
    string64 password_str = "";

    xr_strcpy(session_name, options);
    if (strchr(session_name, '/'))
        *strchr(session_name, '/') = 0;

    if (strstr(options, "psw="))
    {
        const char* PSW = strstr(options, "psw=") + 4;
        if (strchr(PSW, '/'))
            strncpy_s(password_str, PSW, strchr(PSW, '/') - PSW);
        else
            strncpy_s(password_str, PSW, 63);
    }

    if (strstr(options, "maxplayers="))
    {
        const char* sMaxPlayers = strstr(options, "maxplayers=") + 11;
        string64 tmpStr = "";
        if (strchr(sMaxPlayers, '/'))
            strncpy_s(tmpStr, sMaxPlayers, strchr(sMaxPlayers, '/') - sMaxPlayers);
        else
            strncpy_s(tmpStr, sMaxPlayers, 63);
        M_MaxPlayers = atol(tmpStr);
    }

    if (m_bDedicated)
        clamp(M_MaxPlayers, 1U, 32U);
    else
        clamp(M_MaxPlayers, 2U, 32U);

    psNET_Port = START_PORT_LAN_SV;
    if (strstr(options, "portsv="))
    {
        const char* ServerPort = strstr(options, "portsv=") + 7;
        string64 tmpStr = "";
        if (strchr(ServerPort, '/'))
            strncpy_s(tmpStr, ServerPort, strchr(ServerPort, '/') - ServerPort);
        else
            strncpy_s(tmpStr, ServerPort, 63);
        psNET_Port = atol(tmpStr);
    }

    clamp(psNET_Port, u32(START_PORT), u32(END_PORT));

    if (!OpenListenSocket(psNET_Port))
        return ErrConnect;

    Msg("* Server started on port %u, max players: %u", psNET_Port, M_MaxPlayers);

    // Загружаем фильтры
    BannedList_Load();
    IpList_Load();
    return ErrNoError;
}

u32 IPureServer::GetRemoteConnectedClientsCount()
{
    RemoteConnectedClientsCounter counter;
    net_players.ForEachClientDo(counter);
    return counter.count;
}

void IPureServer::Disconnect()
{
    if (OpenSocket != k_HSteamListenSocket_Invalid)
    {
        SteamInterfaceP->CloseListenSocket(OpenSocket);
        Msg("--- [SteamworksServer] Listen socket closed");
    }

    if (PoolGroopPlayers != k_HSteamNetPollGroup_Invalid)
        SteamInterfaceP->DestroyPollGroup(PoolGroopPlayers);

    // Завершаем Steamworks
    if (SteamInterfaceP)
    {
        GameNetworkingSockets_Kill();
        SteamInterfaceP = nullptr;
    }

    OpenSocket = k_HSteamListenSocket_Invalid;
    PoolGroopPlayers = k_HSteamNetPollGroup_Invalid;

    // Сохраняем и выгружаем фильтры
    BannedList_Save();
    IpList_Unload();

    Msg("* Server disconnected");
}

// Callback для Steamworks
void ServerCallbackCall(SteamNetConnectionStatusChangedCallback_t* pInfo)
{
    if (sptr && pInfo)
        sptr->OnStateChange(pInfo);
}

bool IPureServer::OpenListenSocket(u32 port)
{
    if (OpenSocket != k_HSteamListenSocket_Invalid)
        return true;

    if (!SteamInterfaceP)
    {
        SteamDatagramErrMsg errMsg;
        SteamNetworkingIdentity identity;
        identity.Clear();
        identity.SetLocalHost();

        if (!GameNetworkingSockets_Init(&identity, errMsg))
        {
            Msg("! [SteamworksServer] GameNetworkingSockets_Init failed: %s", errMsg);
            return false;
        }
        SteamInterfaceP = SteamNetworkingSockets();
    }

    SteamNetworkingIPAddr bindServerAddress;
    bindServerAddress.Clear();
    bindServerAddress.m_port = port;

    // Настройка callback'ов
    SteamNetworkingConfigValue_t opt;
    opt.SetPtr(k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged, (void*)ServerCallbackCall);

    // Настройка параметров сети
    SteamNetworkingUtils()->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_SendBufferSize, 1024 * 1024 * 24);
    SteamNetworkingUtils()->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_TimeoutConnected, 5000);
    SteamNetworkingUtils()->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_TimeoutInitial, 15000);

    // Создаем слушающий сокет
    OpenSocket = SteamInterfaceP->CreateListenSocketIP(bindServerAddress, 1, &opt);
    if (OpenSocket == k_HSteamListenSocket_Invalid)
    {
        Msg("! [SteamworksServer] Failed to create listen socket on port %u", port);
        return false;
    }

    // Создаем группу для опроса подключений
    PoolGroopPlayers = SteamInterfaceP->CreatePollGroup();
    if (PoolGroopPlayers == k_HSteamNetPollGroup_Invalid)
    {
        Msg("! [SteamworksServer] Failed to create poll group");
        SteamInterfaceP->CloseListenSocket(OpenSocket);
        OpenSocket = k_HSteamListenSocket_Invalid;
        return false;
    }

    Msg("--- [SteamworksServer] Server listening on port %u", port);
    return true;
}

void IPureServer::OnStateChange(SteamNetConnectionStatusChangedCallback_t* pInfo)
{
    if (!pInfo)
        return;

    switch (pInfo->m_info.m_eState)
    {
    case k_ESteamNetworkingConnectionState_Connecting:
    {
        Msg("--- [SteamworksServer] Connection request from [%u]", pInfo->m_hConn);

        // Проверяем не заполнен ли сервер
        const u32 clients_for_limit = m_bDedicated ? GetRemoteConnectedClientsCount() : GetClientsCount();
        if (clients_for_limit >= M_MaxPlayers)
        {
            SteamInterfaceP->CloseConnection(pInfo->m_hConn, ESessionFull, "Server full", false);
            Msg("! [SteamworksServer] Connection rejected: server full (%u/%u, total clients=%u, dedicated=%d)",
                clients_for_limit, M_MaxPlayers, GetClientsCount(), m_bDedicated ? 1 : 0);
            break;
        }

        // Проверяем IP в бане
        ip_address clientAddr;
        DWORD clientPort;
        if (GetClientAddress(&pInfo->m_info, clientAddr, &clientPort))
        {
            if (GetBannedClient(clientAddr))
            {
                SteamInterfaceP->CloseConnection(pInfo->m_hConn, EPlayerBanned, "Player banned", false);
                Msg("! [SteamworksServer] Connection rejected: player banned");
                break;
            }

            if (IsPlayerIPDenied(clientAddr.m_data.data))
            {
                SteamInterfaceP->CloseConnection(pInfo->m_hConn, EUnknownReason, "IP not allowed", false);
                Msg("! [SteamworksServer] Connection rejected: IP not allowed");
                break;
            }
        }

        // Принимаем соединение
        if (SteamInterfaceP->AcceptConnection(pInfo->m_hConn) != k_EResultOK)
        {
            Msg("! [SteamworksServer] Failed to accept connection");
            SteamInterfaceP->CloseConnection(pInfo->m_hConn, EUnknownReason, "Accept failed", false);
            break;
        }

        // Добавляем в группу опроса
        if (!SteamInterfaceP->SetConnectionPollGroup(pInfo->m_hConn, PoolGroopPlayers))
        {
            Msg("! [SteamworksServer] Failed to add connection to poll group");
            SteamInterfaceP->CloseConnection(pInfo->m_hConn, EUnknownReason, "Poll group failed", false);
            break;
        }

        Msg("--- [SteamworksServer] Client %u accepted", pInfo->m_hConn);
        break;
    }

    case k_ESteamNetworkingConnectionState_Connected:
    {
        Msg("--- [SteamworksServer] Client [%u] connected", pInfo->m_hConn);

        // werasik2aa todo set name
        SClientConnectData cl_data;
        strncpy(cl_data.name, "somepcname", sizeof(cl_data.name));
        cl_data.clientID.set(pInfo->m_hConn);
        new_client(&cl_data);
        break;
    }

    case k_ESteamNetworkingConnectionState_ClosedByPeer:
    case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
    {
        Msg("--- [SteamworksServer] Client [%u] disconnected", pInfo->m_hConn);

        // Находим и удаляем клиента
        IClient* tmp_client = GetClientByID(ClientID(pInfo->m_hConn));
        if (tmp_client)
        {
            tmp_client->flags.bConnected = FALSE;
            tmp_client->flags.bReconnect = FALSE;
            OnCL_Disconnected(tmp_client);
            client_Destroy(tmp_client);
        }

        // Закрываем соединение
        SteamInterfaceP->CloseConnection(pInfo->m_hConn, 0, "Client disconnected", false);
        break;
    }

    default:
        break;
    }
}

void IPureServer::SendTo_LL(ClientID ID, void* data, u32 size, u32 dwFlags, u32 dwTimeout)
{
    if (!SteamInterfaceP || ID.value() == 0)
        return;

    if (psNET_Flags.test(NETFLAG_LOG_SV_PACKETS))
    {
        if (!pSvNetLog)
            pSvNetLog = xr_new<INetLog>("logs\\net_sv_log.log", TimeGlobal(device_timer));
        if (pSvNetLog)
            pSvNetLog->LogData(TimeGlobal(device_timer), data, size);
    }

    if (ID.value() == OpenSocket)
    {
        Msg("== [SteamworksServer] Sending data to self client %u: %d", ID.value(), size);
        NET_Packet d;
        d.w(data, size);
        OnMessage(d, ID);
        return;
    }

    EResult result = SteamInterfaceP->SendMessageToConnection(ID.value(), data, size, dwFlags, nullptr);
    if (result != k_EResultOK)
        Msg("! [SteamworksServer] Failed to send data to client %u: %d", ID.value(), result);
}

void IPureServer::SendTo_Buf(ClientID id, void* data, u32 size, u32 dwFlags, u32 dwTimeout)
{
    IClient* tmp_client = GetClientByID(id);
    if (!tmp_client)
    {
        Msg("! [SteamworksServer] Client %u not found for SendTo_Buf", id.value());
        return;
    }

    tmp_client->MultipacketSender::SendPacket(data, size, dwFlags, dwTimeout);
}

void IPureServer::SendTo(ClientID ID, NET_Packet& P, u32 dwFlags, u32 dwTimeout)
{
    SendTo_LL(ID, P.B.data, P.B.count, dwFlags, dwTimeout);
}

void IPureServer::Flush_Clients_Buffers()
{
#if NET_LOG_PACKETS
    Msg("#flush server send-buf");
#endif

    struct LocalSenderFunctor
    {
        static void FlushBuffer(IClient* client)
        {
            client->MultipacketSender::FlushSendBuffer(0);
        }
    };

    net_players.ForEachClientDo(LocalSenderFunctor::FlushBuffer);
}

void IPureServer::SendBroadcast_LL(ClientID exclude, void* data, u32 size, u32 dwFlags)
{
    struct ClientExcluderPredicate
    {
        ClientID id_to_exclude;
        ClientExcluderPredicate(ClientID exclude) : id_to_exclude(exclude) {}
        bool operator()(IClient* client)
        {
            if (client->ID == id_to_exclude)
                return false;
            if (!client->flags.bConnected)
                return false;
            return true;
        }
    };

    struct ClientSenderFunctor
    {
        IPureServer* m_owner;
        void* m_data;
        u32 m_size;
        u32 m_dwFlags;

        ClientSenderFunctor(IPureServer* owner, void* data, u32 size, u32 dwFlags) :
            m_owner(owner), m_data(data), m_size(size), m_dwFlags(dwFlags) {
        }

        void operator()(IClient* client)
        {
            m_owner->SendTo_LL(client->ID, m_data, m_size, m_dwFlags);
        }
    };

    ClientSenderFunctor temp_functor(this, data, size, dwFlags);
    net_players.ForFoundClientsDo(ClientExcluderPredicate(exclude), temp_functor);
}

void IPureServer::SendBroadcast(ClientID exclude, NET_Packet& P, u32 dwFlags)
{
    SendBroadcast_LL(exclude, P.B.data, P.B.count, dwFlags);
}

u32 IPureServer::OnMessage(NET_Packet& P, ClientID sender) { return 0; }

void IPureServer::OnCL_Connected(IClient* CL)
{
    Msg("* Player 0x%08x connected", CL->ID.value());
}

void IPureServer::OnCL_Disconnected(IClient* CL)
{
    Msg("* Player 0x%08x disconnected", CL->ID.value());
}

BOOL IPureServer::HasBandwidth(IClient* C)
{
    u32 dwTime = TimeGlobal(device_timer);
    u32 dwInterval = 0;

    if (psNET_ServerUpdate != 0)
        dwInterval = 1000 / psNET_ServerUpdate;
    if (psNET_Flags.test(NETFLAG_MINIMIZEUPDATES))
        dwInterval = 1000;

    if (psNET_ServerUpdate != 0 && (dwTime - C->dwTime_LastUpdate) > dwInterval)
    {
        DWORD dwPending = GetStatusRemain(C->ID.value());
        if (dwPending > u32(psNET_ServerPending))
        {
            C->stats.dwTimesBlocked++;
            return FALSE;
        }

        UpdateClientStatistic(C);
        C->dwTime_LastUpdate = dwTime;
        return TRUE;
    }

    return FALSE;
}

void IPureServer::UpdateClientStatistic(IClient* C)
{
    if (!C->stats.device_timer)
        C->stats.device_timer = device_timer;
    C->stats.Update(GetStatus(C->ID.value()));
}

SteamNetConnectionRealTimeStatus_t IPureServer::GetStatus(HSteamNetConnection clientID)
{
    SteamNetConnectionRealTimeStatus_t status = {};
    if (SteamInterfaceP && clientID != k_HSteamNetConnection_Invalid)
        SteamInterfaceP->GetConnectionRealTimeStatus(clientID, &status, 0, nullptr);
    return status;
}

u64 IPureServer::GetStatusRemain(HSteamNetConnection clientID)
{
    SteamNetConnectionRealTimeStatus_t status = GetStatus(clientID);
    return status.m_cbPendingReliable + status.m_cbPendingUnreliable;
}

void IPureServer::ClearStatistic()
{
    stats.clear();

    struct StatsClearFunctor
    {
        static void Clear(IClient* client) { client->stats.Clear(); }
    };

    net_players.ForEachClientDo(StatsClearFunctor::Clear);
}

bool IPureServer::GetClientAddress(SteamNetConnectionInfo_t* pClientAddress,
    ip_address& Address, DWORD* pPort)
{
    if (!pClientAddress)
        return false;

    string64 ip_str;
    pClientAddress->m_addrRemote.ToString(ip_str, sizeof(string64), false);
    Address.set(ip_str);

    if (pPort)
        *pPort = pClientAddress->m_addrRemote.m_port;

    return true;
}

bool IPureServer::GetClientAddress(ClientID ID, ip_address& Address, DWORD* pPort)
{
    if (!SteamInterfaceP || ID.value() == 0)
        return false;

    SteamNetConnectionInfo_t info;
    SteamInterfaceP->GetConnectionInfo(ID.value(), &info);

    return GetClientAddress(&info, Address, pPort);
}

// Методы приема данных
void IPureServer::ReceivePackets()
{
    if (!SteamInterfaceP || PoolGroopPlayers == k_HSteamNetPollGroup_Invalid)
        return;

    SteamNetworkingMessage_t* messages[256];
    int numMessages = SteamInterfaceP->ReceiveMessagesOnPollGroup(PoolGroopPlayers, messages, 256);

    for (int i = 0; i < numMessages; i++)
    {
        SteamNetworkingMessage_t* msg = messages[i];

        // Обработка системных сообщений (пинг)
        MSYS_PING* m_ping = (MSYS_PING*)msg->m_pData;
        if (msg->m_cbSize == sizeof(MSYS_PING) && m_ping->sign1 == 0x12071980 && m_ping->sign2 == 0x26111975)
        {
            // Отвечаем на пинг
            m_ping->dwTime_Server = TimerAsync(device_timer);
            IPureServer::SendTo_Buf(ClientID(msg->m_conn), msg->m_pData, msg->m_cbSize, net_flags(FALSE, FALSE, TRUE, TRUE));
        }
        else
            MultipacketReceiver::ReceivePacket(msg->m_pData, msg->m_cbSize, msg->m_conn);

        msg->Release();
    }

    // Обработка callback'ов
    SteamInterfaceP->RunCallbacks();
}

void IPureServer::_Receive(const void* data, u32 data_size, u32 param)
{
    if (data_size >= NET_PacketSizeLimit)
    {
        Msg("! too large packet size[%d] received, DoS attack?", data_size);
        return;
    }

    NET_Packet packet;
    ClientID id;

    id.set(param);
    packet.construct(data, data_size);

    csMessage.Enter();

    // Логирование
    if (psNET_Flags.test(NETFLAG_LOG_SV_PACKETS))
    {
        if (!pSvNetLog)
            pSvNetLog = xr_new<INetLog>("logs\\net_sv_log.log", TimeGlobal(device_timer));
        if (pSvNetLog)
            pSvNetLog->LogPacket(TimeGlobal(device_timer), &packet, TRUE);
    }

    // Обработка сообщения
    u32 result = OnMessage(packet, id);

    csMessage.Leave();

    if (result)
        SendBroadcast(id, packet, result);
}

IClient* IPureServer::ID_to_client(ClientID ID, bool ScanAll)
{
    if (0 == ID.value()) return NULL;
    IClient* ret_client = GetClientByID(ID);
    if (ret_client || !ScanAll)
        return ret_client;

    return NULL;
}

bool IPureServer::DisconnectAddress(const ip_address& Address, LPCSTR reason)
{
    u32 players_count = net_players.ClientsCount();
    buffer_vector<IClient*> PlayersToDisconnect(
        _malloca(players_count * sizeof(IClient*)),
        players_count
    );
    struct ToDisconnectFillerFunctor
    {
        IPureServer* m_owner;
        buffer_vector<IClient*>* dest;
        ip_address const* address_to_disconnect;

        ToDisconnectFillerFunctor(IPureServer* owner, buffer_vector<IClient*>* dest_disconnect,
            ip_address const* address) :
            m_owner(owner), dest(dest_disconnect), address_to_disconnect(address)
        {
        }

        void operator()(IClient* client)
        {
            ip_address tmp_address;
            m_owner->GetClientAddress(client->ID, tmp_address);
            if (*address_to_disconnect == tmp_address)
                dest->push_back(client);
        }
    };
    ToDisconnectFillerFunctor tmp_functor(this, &PlayersToDisconnect, &Address);
    net_players.ForEachClientDo(tmp_functor);

    buffer_vector<IClient*>::iterator it = PlayersToDisconnect.begin();
    buffer_vector<IClient*>::iterator it_e = PlayersToDisconnect.end();

    for (; it != it_e; ++it)
        DisconnectClient(*it, reason);
    return true;
}

bool IPureServer::DisconnectClient(IClient* C, LPCSTR Reason)
{
    if (!C || !SteamInterfaceP) return false;

    SteamInterfaceP->CloseConnection(C->ID.value(), 0, Reason, false);
    return true;
}

IBannedClient* IPureServer::GetBannedClient(const ip_address& Address)
{
    for (u32 it = 0; it < BannedAddresses.size(); it++)
    {
        IBannedClient* pBClient = BannedAddresses[it];
        if (pBClient->HAddr == Address)
            return pBClient;
    }
    return NULL;
}

void IPureServer::BanAddress(const ip_address& Address, u32 BanTimeSec)
{
    if (GetBannedClient(Address))
    {
        Msg("Already banned\n");
        return;
    };

    IBannedClient* pNewClient = xr_new<IBannedClient>();
    pNewClient->HAddr = Address;
    time(&pNewClient->BanTime);
    pNewClient->BanTime += BanTimeSec;
    if (pNewClient)
    {
        BannedAddresses.push_back(pNewClient);
        BannedList_Save();
    }
}

void IPureServer::UnBanAddress(const ip_address& Address)
{
    if (!GetBannedClient(Address))
    {
        Msg("! Can't find address %s in ban list.", Address.to_string().c_str());
        return;
    };

    for (u32 it = 0; it < BannedAddresses.size(); it++)
    {
        IBannedClient* pBClient = BannedAddresses[it];
        if (pBClient->HAddr == Address)
        {
            xr_delete(BannedAddresses[it]);
            BannedAddresses.erase(BannedAddresses.begin() + it);
            Msg("Unbanning %s", Address.to_string().c_str());
            BannedList_Save();
            break;
        }
    };
}

void IPureServer::BanClient(IClient* C, u32 BanTime)
{
    ip_address ClAddress;
    GetClientAddress(C->ID, ClAddress);
    BanAddress(ClAddress, BanTime);
}

void IPureServer::Print_Banned_Addreses()
{
    Msg("- ----banned ip list begin-------");
    for (u32 i = 0; i < BannedAddresses.size(); i++)
    {
        IBannedClient* pBClient = BannedAddresses[i];
        Msg("- %s to %s", pBClient->HAddr.to_string().c_str(), pBClient->BannedTimeTo().c_str());
    }
    Msg("- ----banned ip list end-------");
}

void IPureServer::BannedList_Save()
{
    string_path temp;
    FS.update_path(temp, "$app_data_root$", GetBannedListName());

    CInifile ini(temp, FALSE, FALSE, TRUE);

    for (u32 it = 0; it < BannedAddresses.size(); it++)
    {
        IBannedClient* cl = BannedAddresses[it];
        cl->Save(ini);
    };
}

void IPureServer::BannedList_Load()
{
    string_path temp;
    FS.update_path(temp, "$app_data_root$", GetBannedListName());

    CInifile ini(temp);

    CInifile::RootIt it = ini.sections().begin();
    CInifile::RootIt it_e = ini.sections().end();

    for (; it != it_e; ++it)
    {
        const shared_str& sect_name = (*it)->Name;
        IBannedClient* Cl = xr_new<IBannedClient>();
        Cl->Load(ini, sect_name);
        BannedAddresses.push_back(Cl);
    }
}

void IPureServer::IpList_Load()
{
    Msg("* Initializing IP filter.");
    m_ip_filter.load();
}

void IPureServer::IpList_Unload()
{
    Msg("* Deinitializing IP filter.");
    m_ip_filter.unload();
}

void IPureServer::UpdateBannedList()
{
    if (!BannedAddresses.size()) return;
    std::sort(BannedAddresses.begin(), BannedAddresses.end(), 
        [](IBannedClient* C1, IBannedClient* C2) {
            return C1->BanTime > C2->BanTime;
        }
    );
    time_t T;
    time(&T);

    IBannedClient* Cl = BannedAddresses.back();
    if (Cl->BanTime < T)
    {
        ip_address Address = Cl->HAddr;
        UnBanAddress(Address);
    }
}
