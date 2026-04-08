#pragma once

#include "NET_Common.h"
#include "NET_Shared.h"
#include "NET_Compressor.h"
#include "NET_Stats.h"

struct ip_address;
class XRNETSERVER_API INetQueue
{
private:
    xrCriticalSection cs;
    xr_deque<NET_Packet*> ready;
    xr_vector<NET_Packet*> unused;

public:
    INetQueue();
    ~INetQueue();

    NET_Packet* Create();
    NET_Packet* Create(const NET_Packet& _other);
    NET_Packet* Retrieve();  // Исправлено: Retreive -> Retrieve
    void Release();
    void Lock() { cs.Enter(); }
    void Unlock() { cs.Leave(); }
};

//==============================================================================

class XRNETSERVER_API IPureClient : public MultipacketReceiver, public MultipacketSender
{
public:
    enum ConnectionState
    {
        EnmConnectionFails,
        EnmConnectionWait1,
        EnmConnectionWait2,
        EnmConnectionCompleted
    };

    IPureClient(CTimer* tm);
    virtual ~IPureClient();

    // Основные методы соединения
    BOOL Connect(LPCSTR server_name);
    void Disconnect();
    void net_Syncronize();
    BOOL net_IsSyncronised() { return net_Syncronised; }

    // Управление соединением
    BOOL CreateConnection(LPCSTR connectionIP);
    bool GetServerAddress(ip_address& pAddress, DWORD* pPort);

    // Состояние соединения
    BOOL net_isCompleted_Connect() { return net_Connected == EnmConnectionCompleted; }
    BOOL net_isFails_Connect() { return net_Connected == EnmConnectionFails; }
    BOOL net_isCompleted_Sync() { return net_Syncronised; }
    BOOL net_isDisconnected() { return net_Disconnected; }

    // Очередь сообщений
    void StartProcessQueue() { net_Queue.Lock(); }
    NET_Packet* net_msg_Retrieve() { return net_Queue.Retrieve(); }
    void net_msg_Release() { net_Queue.Release(); }
    void EndProcessQueue() { net_Queue.Unlock(); }

    // Отправка данных
    virtual void Send(NET_Packet& P, u32 dwFlags = k_nSteamNetworkingSend_Reliable, u32 dwTimeout = 0);
    virtual void Flush_Send_Buffer() { FlushSendBuffer(0); }
    virtual void SendTo_LL(void* data, u32 size, u32 dwFlags = k_nSteamNetworkingSend_Reliable, u32 dwTimeout = 0);

    // Получение данных
    void ReceivePackets();

    // Управление полосой пропускания и статистика
    BOOL net_HasBandwidth();
    void ClearStatistic();
    IClientStatistic& GetStatistic() { return net_Statistic; }
    void UpdateStatistic();

    // Время и синхронизация
    u32 timeServer() { return device_timer->GetElapsed_ms() + net_TimeDelta + net_TimeDelta_User; }
    u32 timeServer_Async() { return device_timer->GetElapsed_ms() + net_TimeDelta + net_TimeDelta_User; }
    u32 timeServer_Delta() { return net_TimeDelta; }
    void timeServer_UserDelta(s32 d) { net_TimeDelta_User = d; }
    void timeServer_Correct(u32 sv_time, u32 cl_time);

    // Информация о сессии
    GameDescriptionData const& get_net_DescriptionData() const { return m_game_description; }
    LPCSTR net_SessionName() { return m_game_description.map_name; }
    bool has_SessionName() { return xr_strlen(m_game_description.map_name) > 1; }

    // Steamworks специфичные методы
    void OnStateChange(SteamNetConnectionStatusChangedCallback_t* pInfo);
    SteamNetConnectionRealTimeStatus_t GetStatus();
    u64 GetStatusRemain();

    // Виртуальные обработчики событий
    virtual void OnMessage(void* data, u32 size);
    virtual void OnInvalidHost() {}
    virtual void OnInvalidPassword() {}
    virtual void OnSessionFull() {}
    virtual void OnConnectRejected() {}
    virtual void OnSessionTerminate(LPCSTR reason) {}

    // Идентификация
    ClientID const& GetClientID() { return net_ClientID; }
    void SetClientID(ClientID const& local_client) { net_ClientID = local_client; }

    // Утилиты
    virtual LPCSTR GetMsgId2Name(u16 ID) { return ""; }
    virtual bool TestLoadBEClient() { return false; }

protected:
    GameDescriptionData m_game_description;
    CTimer* device_timer;

    // Steamworks интерфейсы
    ISteamNetworkingSockets* SteamInterfaceP;
    HSteamNetConnection OpenConnection;

    // Сетевое состояние
    ConnectionState net_Connected;
    BOOL net_Syncronised;
    BOOL net_Disconnected;

    // Компоненты
    NET_Compressor net_Compressor;
    INetQueue net_Queue;
    IClientStatistic net_Statistic;
    xrCriticalSection net_csEnumeration;

    // Временные параметры
    u32 net_Time_LastUpdate;
    s32 net_TimeDelta;
    s32 net_TimeDelta_Calculated;
    s32 net_TimeDelta_User;

    ClientID net_ClientID;

private:
    // Внутренние методы синхронизации
    void Sync_Thread();
    void Sync_Average();

    // Внутренняя обработка сообщений (реализация абстрактного метода)
    void _Receive(const void* data, u32 data_size, u32 param);
    void _SendTo_LL(const void* data, u32 size, u32 flags, u32 timeout);
    friend void sync_thread(void* PTR); // хз почему но работает так.
}; 

XRNETSERVER_API extern IPureClient* cptr;