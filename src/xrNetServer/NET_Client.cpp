#include "stdafx.h"
#include "NET_Common.h"
#include "NET_Client.h"
#include "NET_Log.h"
#include "NET_Compressor.h"
#include "NET_Shared.h"

static INetLog* pClNetLog = NULL;
XRNETSERVER_API IPureClient* cptr = NULL;
XRNETSERVER_API Flags32 psNET_Flags = { 0 };
XRNETSERVER_API int psNET_ClientUpdate = 30;
XRNETSERVER_API int psNET_ClientPending = 2;
XRNETSERVER_API char psNET_Name[32] = "Player";

// Вспомогательная структура для синхронизации
const u32 syncQueueSize = 512;
class XRNETSERVER_API syncQueue
{
    u32 table[syncQueueSize];
    u32 write;
    u32 count;
public:
    syncQueue() { clear(); }

    IC void push(u32 value)
    {
        table[write++] = value;
        if (write == syncQueueSize) write = 0;
        if (count <= syncQueueSize) count++;
    }

    IC u32* begin() { return table; }
    IC u32* end() { return table + count; }
    IC u32 size() { return count; }
    IC void clear()
    {
        write = 0;
        count = 0;
    }
} net_DeltaArray;

// Реализация INetQueue
INetQueue::INetQueue()
#ifdef PROFILE_CRITICAL_SECTIONS
    : cs(MUTEX_PROFILE_ID(INetQueue))
#endif
{
    unused.reserve(128);
    for (int i = 0; i < 16; i++)
        unused.push_back(xr_new<NET_Packet>());
}

INetQueue::~INetQueue()
{
    cs.Enter();
    for (u32 it = 0; it < unused.size(); it++) xr_delete(unused[it]);
    for (u32 it = 0; it < ready.size(); it++) xr_delete(ready[it]);
    cs.Leave();
}

static u64 LastTimeCreate = 0;

NET_Packet* INetQueue::Create()
{
    NET_Packet* P = 0;
    cs.Enter();

    if (unused.empty())
    {
        ready.push_back(xr_new<NET_Packet>());
        P = ready.back();
        LastTimeCreate = GetTickCount64();
    }
    else
    {
        ready.push_back(unused.back());
        unused.pop_back();
        P = ready.back();
    }

    cs.Leave();
    return P;
}

NET_Packet* INetQueue::Create(const NET_Packet& _other)
{
    NET_Packet* P = 0;
    cs.Enter();

    if (unused.empty())
    {
        ready.push_back(xr_new<NET_Packet>());
        P = ready.back();
        LastTimeCreate = GetTickCount64();
    }
    else
    {
        ready.push_back(unused.back());
        unused.pop_back();
        P = ready.back();
    }

    CopyMemory(P, &_other, sizeof(NET_Packet));
    cs.Leave();
    return P;
}

NET_Packet* INetQueue::Retrieve()
{
    NET_Packet* P = 0;
    cs.Enter();

    if (!ready.empty())
        P = ready.front();
    else
    {
        u32 tmp_time = GetTickCount64() - 60000;
        u32 size = unused.size();
        if ((LastTimeCreate < tmp_time) && (size > 32))
        {
            xr_delete(unused.back());
            unused.pop_back();
        }
    }

    cs.Leave();
    return P;
}

void INetQueue::Release()
{
    cs.Enter();

    VERIFY(!ready.empty());
    u32 tmp_time = GetTickCount() - 60000;
    u32 size = unused.size();
    ready.front()->B.count = 0;

    if ((LastTimeCreate < tmp_time) && (size > 32))
        xr_delete(ready.front());
    else
        unused.push_back(ready.front());

    ready.pop_front();
    cs.Leave();
}

// Реализация IPureClient
IPureClient::IPureClient(CTimer* timer)
    : net_Statistic(timer)
#ifdef PROFILE_CRITICAL_SECTIONS
    , net_csEnumeration(MUTEX_PROFILE_ID(IPureClient::net_csEnumeration))
#endif
{
    device_timer = timer;
    net_TimeDelta_User = 0;
    net_Time_LastUpdate = 0;
    net_TimeDelta = 0;
    net_TimeDelta_Calculated = 0;

    SteamInterfaceP = nullptr;
    OpenConnection = k_HSteamNetConnection_Invalid;

    net_Connected = EnmConnectionFails;
    net_Syncronised = FALSE;
    net_Disconnected = FALSE;

    cptr = this;
    pClNetLog = NULL;
}

IPureClient::~IPureClient()
{
    xr_delete(pClNetLog);
    Disconnect();

    if (SteamInterfaceP)
    {
        GameNetworkingSockets_Kill();
        SteamInterfaceP = nullptr;
    }
}

void ClientCallbackCall(SteamNetConnectionStatusChangedCallback_t* pInfo)
{
    if (cptr && pInfo)
        cptr->OnStateChange(pInfo);
}

BOOL IPureClient::CreateConnection(LPCSTR connectionIP)
{
    if (OpenConnection != k_HSteamNetConnection_Invalid)
        return true;

    if (!SteamInterfaceP)
    {
        SteamDatagramErrMsg errMsg;
        if (!GameNetworkingSockets_Init(nullptr, errMsg))
        {
            Msg("! [SteamworksClient] GameNetworkingSockets_Init failed: %s", errMsg);
            return false;
        }
        SteamInterfaceP = SteamNetworkingSockets();
    }

    SteamNetworkingIPAddr serverAddr;
    if (!serverAddr.ParseString(connectionIP))
    {
        Msg("! [SteamworksClient] Failed to parse address: %s", connectionIP);
        return false;
    }

    Msg("--- [SteamworksClient] Connecting to [%s]", connectionIP);

    SteamNetworkingConfigValue_t opt;
    opt.SetPtr(k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged, (void*)ClientCallbackCall);

    SteamNetworkingUtils()->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_SendBufferSize, 1024 * 1024 * 24);
    SteamNetworkingUtils()->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_TimeoutConnected, 5000);
    SteamNetworkingUtils()->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_TimeoutInitial, 15000);

    OpenConnection = SteamInterfaceP->ConnectByIPAddress(serverAddr, 1, &opt);
    if (OpenConnection == k_HSteamNetConnection_Invalid)
    {
        Msg("! [SteamworksClient] Failed to create connection");
        return false;
    }

    Msg("--- [SteamworksClient] Connection created");
    return true;
}

BOOL IPureClient::Connect(LPCSTR options)
{
    R_ASSERT(options);
    net_Disconnected = FALSE;
    net_TimeDelta = 0;

    string256 server_name = "";
    if (strchr(options, '/'))
        strncpy_s(server_name, options, strchr(options, '/') - options);
    if (strchr(server_name, '/')) *strchr(server_name, '/') = 0;

    string64 password_str = "";
    if (strstr(options, "psw="))
    {
        const char* PSW = strstr(options, "psw=") + 4;
        if (strchr(PSW, '/'))
            strncpy_s(password_str, PSW, strchr(PSW, '/') - PSW);
        else
            xr_strcpy(password_str, PSW);
    }

    string64 user_name_str = "";
    if (strstr(options, "name="))
    {
        const char* NM = strstr(options, "name=") + 5;
        if (strchr(NM, '/'))
            strncpy_s(user_name_str, NM, strchr(NM, '/') - NM);
        else
            xr_strcpy(user_name_str, NM);
    }

    int psNET_Port = START_PORT_LAN_SV;
    if (strstr(options, "port="))
    {
        string64 portstr;
        xr_strcpy(portstr, strstr(options, "port=") + 5);
        if (strchr(portstr, '/')) *strchr(portstr, '/') = 0;
        psNET_Port = atol(portstr);
        clamp(psNET_Port, int(START_PORT), int(END_PORT));
    }

    Msg("* Client connecting to port %d", psNET_Port);

    net_Connected = EnmConnectionWait1;
    net_Syncronised = FALSE;
    net_Disconnected = FALSE;

    std::string connectionString = !stricmp(server_name, "localhost") ? "127.0.0.1" : server_name;
    if (connectionString.find(":") == std::string::npos)
        connectionString += ":" + std::to_string(psNET_Port);

    if (!CreateConnection(connectionString.c_str()))
        return FALSE;

    // Ждем подключения таймаут задается глобально в стим параметрах...
    while (net_Connected == EnmConnectionWait1 && !net_Disconnected)
        SteamInterfaceP->RunCallbacks();

    return net_Connected == EnmConnectionWait2 || net_Connected == EnmConnectionCompleted;
}

void IPureClient::Disconnect()
{
    if (SteamInterfaceP && OpenConnection != k_HSteamNetConnection_Invalid)
    {
        SteamInterfaceP->CloseConnection(OpenConnection, 0, "Client disconnect", false);
        Msg("--- [SteamworksClient] Disconnected from server");
    }

    OpenConnection = k_HSteamNetConnection_Invalid;
    net_Connected = EnmConnectionFails;
    net_Syncronised = FALSE;
    net_Disconnected = TRUE;
}

void IPureClient::OnStateChange(SteamNetConnectionStatusChangedCallback_t* pInfo)
{
    if (!pInfo || pInfo->m_hConn != OpenConnection)
        return;

    switch (pInfo->m_info.m_eState)
    {
    case k_ESteamNetworkingConnectionState_ClosedByPeer:
    case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
    {
        if (pInfo->m_info.m_eState == k_ESteamNetworkingConnectionState_ClosedByPeer)
            Msg("![SteamworksClient] ClosedByPeer");
        else
            Msg("![SteamworksClient] ProblemDetectedLocally");

        net_Disconnected = TRUE;
        net_Connected = EnmConnectionFails;
        OnConnectRejected();
        if (pInfo->m_info.m_szEndDebug)
            OnSessionTerminate(pInfo->m_info.m_szEndDebug);
        Disconnect();
        break;
    }
    case k_ESteamNetworkingConnectionState_Connected:
        Msg("--- [SteamworksClient] Connected to server. Awaiting data...");
        net_Connected = EnmConnectionWait2;
        break;

    case k_ESteamNetworkingConnectionState_Connecting:
        Msg("--- [SteamworksClient] Connecting...");
        break;
    default:
        break;
    }
}

void IPureClient::SendTo_LL(void* data, u32 size, u32 dwFlags, u32 dwTimeout)
{
    if (net_Disconnected || !SteamInterfaceP || OpenConnection == k_HSteamNetConnection_Invalid)
        return;

    if (psNET_Flags.test(NETFLAG_LOG_CL_PACKETS))
    {
        if (!pClNetLog)
            pClNetLog = xr_new<INetLog>("logs\\net_cl_log.log", timeServer());
        if (pClNetLog)
            pClNetLog->LogData(timeServer(), data, size);
    }

    EResult result = SteamInterfaceP->SendMessageToConnection(OpenConnection, data, size, dwFlags, nullptr);
    if (result != k_EResultOK)
        Msg("![SteamworksClient] Failed to send packet: %d", result);
}

void IPureClient::_SendTo_LL(const void* data, u32 size, u32 flags, u32 timeout)
{
    SendTo_LL(const_cast<void*>(data), size, flags, timeout);
}

void IPureClient::Send(NET_Packet& packet, u32 dwFlags, u32 dwTimeout)
{
    SendTo_LL(packet.B.data, packet.B.count, dwFlags, dwTimeout);
}

void IPureClient::_Receive(const void* data, u32 data_size, u32 /*param*/)
{
    MSYS_PING* Header = (MSYS_PING*)data;
    net_Statistic.dwBytesReceived += data_size;

    if (data_size >= 2 * sizeof(u32) && Header->sign1 == 0x12071980 && Header->sign2 == 0x26111975)
    {
        if (data_size == sizeof(MSYS_PING))
        {
            u32 time = TimerAsync(device_timer);
            u32 ping = time - Header->dwTime_ClientSend;
            u32 delta = Header->dwTime_Server + ping / 2 - time;
            net_DeltaArray.push(delta);
            Sync_Average();
        }
        else if (data_size == sizeof(MSYS_CONFIG)) {
            net_Connected = EnmConnectionCompleted;
            MSYS_CONFIG* cfgm = (MSYS_CONFIG*)data;
            m_game_description = cfgm->mdata;
        }
        else
            Msg("! Unknown system message");
        return;
    }
    else if (net_Connected == EnmConnectionCompleted)
    {
        if (psNET_Flags.test(NETFLAG_LOG_CL_PACKETS))
        {
            if (!pClNetLog)
                pClNetLog = xr_new<INetLog>("logs\\net_cl_log.log", timeServer());
            if (pClNetLog)
                pClNetLog->LogData(timeServer(), (void*)data, data_size, TRUE);
        }

        OnMessage((void*)data, data_size);
    }
}

void IPureClient::OnMessage(void* data, u32 size)
{
    net_Queue.Lock();
    NET_Packet* P = net_Queue.Create();

    P->construct(data, size);
    P->timeReceive = timeServer_Async();

    u16 m_type;
    P->r_begin(m_type);
    net_Queue.Unlock();
}

void IPureClient::ReceivePackets()
{
    if (!SteamInterfaceP || OpenConnection == k_HSteamNetConnection_Invalid)
        return;

    SteamNetworkingMessage_t* messages[256];
    int numMessages = SteamInterfaceP->ReceiveMessagesOnConnection(OpenConnection, messages, 256);

    for (int i = 0; i < numMessages; i++)
    {
        SteamNetworkingMessage_t* msg = messages[i];
        ReceivePacket(msg->m_pData, msg->m_cbSize, msg->m_nFlags);
        msg->Release();
    }

    SteamInterfaceP->RunCallbacks();
}

BOOL IPureClient::net_HasBandwidth()
{
    u32 dwTime = TimeGlobal(device_timer);
    u32 dwInterval = 0;

    if (net_Disconnected)
        return FALSE;

    if (psNET_ClientUpdate != 0)
        dwInterval = 1000 / psNET_ClientUpdate;
    if (psNET_Flags.test(NETFLAG_MINIMIZEUPDATES))
        dwInterval = 1000;

    if (0 != psNET_ClientUpdate && (dwTime - net_Time_LastUpdate) > dwInterval)
    {
        DWORD dwPending = GetStatusRemain();
        if (dwPending > u32(psNET_ClientPending))
        {
            net_Statistic.dwTimesBlocked++;
            return FALSE;
        }

        UpdateStatistic();
        net_Time_LastUpdate = dwTime;
        return TRUE;
    }

    return FALSE;
}

void IPureClient::UpdateStatistic()
{
    if (!net_Statistic.device_timer)
        net_Statistic.device_timer = device_timer;
    net_Statistic.Update(GetStatus());
}

SteamNetConnectionRealTimeStatus_t IPureClient::GetStatus()
{
    SteamNetConnectionRealTimeStatus_t status = {};
    if (SteamInterfaceP && OpenConnection != k_HSteamNetConnection_Invalid)
        SteamInterfaceP->GetConnectionRealTimeStatus(OpenConnection, &status, 0, nullptr);
    return status;
}

u64 IPureClient::GetStatusRemain()
{
    SteamNetConnectionRealTimeStatus_t status = GetStatus();
    return status.m_cbPendingReliable + status.m_cbPendingUnreliable;
}

void IPureClient::timeServer_Correct(u32 sv_time, u32 cl_time)
{
    u32 ping = net_Statistic.getPing();
    u32 delta = sv_time + ping / 2 - cl_time;
    net_DeltaArray.push(delta);
    Sync_Average();
}

void IPureClient::Sync_Thread()
{
    // Очищаем массив дельт
    net_DeltaArray.clear();

    // Проверяем подключение
    if (!SteamInterfaceP || OpenConnection == k_HSteamNetConnection_Invalid || net_Disconnected)
    {
        net_Syncronised = FALSE;
        return;
    }

    // Start while reading useless!
    while (!net_Disconnected)
    {
        ReceivePackets();
        // Проверяем, есть ли ожидающие отправки пакеты
        u64 dwPending = GetStatusRemain();
        if (dwPending > 0)
        {
            Sleep(10);
            continue;
        }
        break;
    }

    // Переменные
    u32 old_size = net_DeltaArray.size();
    u32 timeBegin = TimerAsync(device_timer);
    u8 NeedSyncSamples = 32;

    // Формируем и отправляем первое пинг-сообщение
    MSYS_PING clPing;
    clPing.sign1 = 0x12071980;
    clPing.sign2 = 0x26111975;
    clPing.dwTime_ClientSend = TimerAsync(device_timer);
    clPing.dwTime_Server = 0; // Заполняется сервером
    SendTo_LL(&clPing, sizeof(clPing), k_nSteamNetworkingSend_Reliable, 0);

    // Ожидаем ответ и повторяем
    while (!net_Disconnected && TimerAsync(device_timer) - timeBegin < 5000)
    {
        // Проверяем, получили ли достаточно данных для синхронизации
        if (net_DeltaArray.size() >= NeedSyncSamples) // werasik2aa syncSamples = 256
        {
            Sync_Average();
            net_Syncronised = TRUE;
            Msg("* CLIENT: Time synchronized. Delta: %d ms", net_TimeDelta);
            break;
        }
        if (net_DeltaArray.size() != old_size)
        {
            // Формируем пинг-сообщение
            clPing.dwTime_ClientSend = TimerAsync(device_timer);

            // Отправляем пинг
            SendTo_LL(&clPing, sizeof(clPing), k_nSteamNetworkingSend_Reliable, 0);
            Msg("! Sync thread %u/%u", net_DeltaArray.size(), NeedSyncSamples);

            // Для ожидания... Чтобы не отсылало сто раз туда пока не придет ответ
            old_size = net_DeltaArray.size();
        }
        ReceivePackets();
    }

    if (!net_Syncronised)
        Msg("! [SteamworksClient] Failed to synchronize! [%u] sync samples was received! Bad network?", net_DeltaArray.size());
}

void IPureClient::Sync_Average()
{
    s64 summary_delta = 0;
    s32 size = net_DeltaArray.size();

    if (size == 0)
        return;

    u32* I = net_DeltaArray.begin();
    u32* E = I + size;
    for (; I != E; I++)
        summary_delta += *((s32*)I);

    s64 frac = s64(summary_delta) % s64(size);
    if (frac < 0)
        frac = -frac;

    summary_delta /= s64(size);
    if (frac > s64(size / 2))
        summary_delta += (summary_delta < 0) ? -1 : 1;

    net_TimeDelta_Calculated = s32(summary_delta);
    net_TimeDelta = (net_TimeDelta * 5 + net_TimeDelta_Calculated) / 6;
}

void sync_thread(void* PTR)
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    SetThreadDescription(GetCurrentThread(), L"Multiplayer sync thread");
    IPureClient* C = (IPureClient*)PTR;
    if (C) C->Sync_Thread();
}

void IPureClient::net_Syncronize()
{
    net_Syncronised = FALSE;
    net_DeltaArray.clear();
    thread_spawn(sync_thread, "network-time-sync", 0, this);
}

void IPureClient::ClearStatistic()
{
    net_Statistic.Clear();
}

bool IPureClient::GetServerAddress(ip_address& pAddress, DWORD* pPort)
{
    *pPort = 0;
    if (!SteamInterfaceP || OpenConnection == k_HSteamNetConnection_Invalid)
        return false;

    SteamNetConnectionInfo_t info;
    SteamInterfaceP->GetConnectionInfo(OpenConnection, &info);
    *pPort = info.m_addrRemote.m_port;
    string64 sucka;
    info.m_addrRemote.ToString(sucka, sizeof(sucka), false);
    pAddress.set(sucka);
    return true;
}