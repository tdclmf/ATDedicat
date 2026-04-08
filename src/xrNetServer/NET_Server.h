#pragma once

#include "ip_filter.h"
#include "NET_Common.h"
#include "NET_Shared.h"
#include "NET_Compressor.h"
#include "NET_Stats.h"
#include "NET_PlayersMonitor.h"

struct XRNETSERVER_API ip_address
{
    union
    {
        struct
        {
            u8 a1;
            u8 a2;
            u8 a3;
            u8 a4;
        };
        u32 data;
    } m_data;

    void set(LPCSTR src_string);
    xr_string to_string() const;

    bool operator ==(const ip_address& other) const
    {
        return (m_data.data == other.m_data.data) ||
            ((m_data.a1 == other.m_data.a1) &&
                (m_data.a2 == other.m_data.a2) &&
                (m_data.a3 == other.m_data.a3) &&
                (m_data.a4 == 0));
    }
};

class IPureServer;
class XRNETSERVER_API IClient : public MultipacketSender
{
public:
    struct Flags
    {
        u32 bLocal : 1;
        u32 bConnected : 1;
        u32 bReconnect : 1;
        u32 bVerified : 1;
    };

    IClient(CTimer* timer);
    virtual ~IClient() {};

    IClientStatistic stats;

    ClientID ID;
    string128 m_guid;
    shared_str name;
    shared_str pass;

    Flags flags; // local/host/normal
    u32 dwTime_LastUpdate;

    ip_address m_cAddress;
    DWORD m_dwPort;
    u32 process_id;

    IPureServer* server;

private:
    virtual void _SendTo_LL(const void* data, u32 size, u32 flags, u32 timeout);
};

IC bool operator==(IClient const* pClient, ClientID const& ID) { return pClient->ID == ID; }

class XRNETSERVER_API IBannedClient
{
public:
    ip_address HAddr;
    time_t BanTime;

    IBannedClient()
    {
        HAddr.m_data.data = 0;
        BanTime = 0;
    };

    void Load(CInifile& ini, const shared_str& sect);
    void Save(CInifile& ini);
    xr_string BannedTimeTo() const;
};

//==============================================================================

struct ClientIdSearchPredicate
{
    ClientID clientId;

    ClientIdSearchPredicate(ClientID clientIdToSearch) :
        clientId(clientIdToSearch)
    {
    }

    inline bool operator()(IClient* client) const
    {
        return client->ID == clientId;
    }
};

class CServerInfo;

class XRNETSERVER_API IPureServer : public MultipacketReceiver
{
public:
    enum EConnect
    {
        ErrConnect,
        ErrMax,
        ErrNoError = ErrMax,
    };

    // Steamworks интерфейсы
    ISteamNetworkingSockets* SteamInterfaceP;
    HSteamNetPollGroup PoolGroopPlayers;
    HSteamListenSocket OpenSocket;

    // Состояние сервера
protected:
    shared_str connect_options;
    NET_Compressor net_Compressor;
    PlayersMonitor net_players;
    IClient* SV_Client;

    // Конфигурация
    u32 psNET_Port;
    u32 M_MaxPlayers;

    // Бан-лист и фильтры
    xr_vector<IBannedClient*> BannedAddresses;
    ip_filter m_ip_filter;

    // Синхронизация
    xrCriticalSection csMessage;

    // Статистика
    IServerStatistic stats;
    CTimer* device_timer;
    BOOL m_bDedicated;
public:
    // Основные методы
    IPureServer(CTimer* timer, BOOL Dedicated = FALSE);
    virtual ~IPureServer();

    virtual EConnect Connect(LPCSTR session_name, GameDescriptionData& game_descr);
    virtual void Disconnect();

    // Управление сокетом
    bool OpenListenSocket(u32 port);
    void OnStateChange(SteamNetConnectionStatusChangedCallback_t* pInfo);

    // Отправка данных
    virtual void SendTo_LL(ClientID ID, void* data, u32 size, u32 dwFlags = k_nSteamNetworkingSend_Reliable, u32 dwTimeout = 0);
    virtual void SendTo_Buf(ClientID ID, void* data, u32 size, u32 dwFlags = k_nSteamNetworkingSend_Reliable, u32 dwTimeout = 0);
    virtual void Flush_Clients_Buffers();

    void SendTo(ClientID ID, NET_Packet& P,  u32 dwFlags = k_nSteamNetworkingSend_Reliable, u32 dwTimeout = 0);
    void SendBroadcast_LL(ClientID exclude, void* data, u32 size, u32 dwFlags = k_nSteamNetworkingSend_Reliable);
    virtual void SendBroadcast(ClientID exclude, NET_Packet& P, u32 dwFlags = k_nSteamNetworkingSend_Reliable);

    // Статистика и мониторинг
    SteamNetConnectionRealTimeStatus_t GetStatus(HSteamNetConnection clientID);
    u64 GetStatusRemain(HSteamNetConnection clientID);
    const IServerStatistic* GetStatistic() { return &stats; }
    void ClearStatistic();
    void UpdateClientStatistic(IClient* C);

    // Обработка сообщений
    virtual u32 OnMessage(NET_Packet& P, ClientID sender);
    virtual void OnCL_Connected(IClient* C);
    virtual void OnCL_Disconnected(IClient* C);
    virtual bool OnCL_QueryHost() { return true; }

    // Управление клиентами
    virtual IClient* new_client(SClientConnectData* cl_data) = 0;
    virtual IClient* client_Create() = 0;
    virtual void client_Replicate() = 0;
    virtual void client_Destroy(IClient* C) = 0;

    // Поиск клиентов
    IClient* ID_to_client(ClientID ID, bool ScanAll = false);
    IClient* GetClientByID(const ClientID& clientId)
    {
        return net_players.GetFoundClient(ClientIdSearchPredicate(clientId));
    }

    // Проверка подключения
    BOOL HasBandwidth(IClient* C);
    bool GetClientAddress(ClientID ID, ip_address& Address, DWORD* pPort = NULL);
    bool GetClientAddress(SteamNetConnectionInfo_t* pClientAddress,
        ip_address& Address, DWORD* pPort = NULL);

    // Управление подключениями
    virtual bool DisconnectClient(IClient* C, LPCSTR Reason);
    virtual bool DisconnectAddress(const ip_address& Address, LPCSTR reason);

    // Бан-система
    IBannedClient* GetBannedClient(const ip_address& Address);
    virtual void BanClient(IClient* C, u32 BanTime);
    virtual void BanAddress(const ip_address& Address, u32 BanTime);
    virtual void UnBanAddress(const ip_address& Address);
    void Print_Banned_Addreses();
    void UpdateBannedList();
    void BannedList_Save();
    void BannedList_Load();
    void IpList_Load();
    void IpList_Unload();
    IC LPCSTR GetBannedListName() { return "banned_list_ip.ltx"; }

    // Прием данных
    void ReceivePackets();

    // Утилиты
    int GetPort() { return psNET_Port; }
    u32 GetClientsCount() { return net_players.ClientsCount(); }
    IClient* GetServerClient() { return SV_Client; }
    const shared_str& GetConnectOptions() const { return connect_options; }

    // Шаблонные методы для итерации по клиентам
    template <typename SearchPredicate>
    IClient* FindClient(SearchPredicate const& predicate)
    {
        return net_players.GetFoundClient(predicate);
    }

    template <typename ActionFunctor>
    void ForEachClientDo(ActionFunctor& action)
    {
        net_players.ForEachClientDo(action);
    }

    template <typename SenderFunctor>
    void ForEachClientDoSender(SenderFunctor& action)
    {
        csMessage.Enter();
        net_players.ForEachClientDo(action);
        csMessage.Leave();
    }

    // Проверка IP
    bool IsPlayerIPDenied(u32 ip_address) { return !m_ip_filter.is_ip_present(ip_address); };

    // Виртуальные методы для переопределения
    virtual void Assign_ServerType(string512& res) {}
    virtual void GetServerInfo(CServerInfo* si) {}

protected:
    // Константы для обработки соединений
    enum DisconnectReasons
    {
        EUnknownReason = 0,
        EPlayerBanned,
        EServerShutdown,
        EInvalidPassword,
        ESessionFull
    };

private:
    virtual void _Receive(const void* data, u32 data_size, u32 param);
};

XRNETSERVER_API extern IPureServer* sptr;