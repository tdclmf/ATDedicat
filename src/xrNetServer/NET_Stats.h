#pragma once
class XRNETSERVER_API IServerStatistic
{
public:
	u32 bytes_out, bytes_out_real;
	u32 bytes_in, bytes_in_real;
	u32 dwBytesSended;
	u32 dwSendTime;
	u32 dwBytesPerSec;
public:
	void clear()
	{
		bytes_out = bytes_out_real = 0;
		bytes_in = bytes_in_real = 0;
		dwBytesSended = 0;
		dwSendTime = 0;
		dwBytesPerSec = 0;
	}
};

class XRNETSERVER_API IClientStatistic
{
private:
	float qualityLocal = 0;
	float qualityRemote = 0;
	float packetsInPerSec = 0;
	float packetsOutPerSec = 0;
	u32 mps_recive, mps_receive_base;
	u32 mps_send, mps_send_base;
	u32 dwBaseTime;

	s64 queueTime = 0;
	s32 sendRateBytesPerSecond = 0;
	s32 pendingReliable = 0;
	s32 pendingUnreliable = 0;
	s32 sentUnackedReliable = 0;
public:
	u32 dwRoundTripLatencyMS = 0;
	u32 dwThroughputBPS = 0;
	u32 dwPeakThroughputBPS = 0;
	u32 dwPacketsDropped = 0;
	u32 dwPacketsRetried = 0;

	u32 dwTimesBlocked = 0;
	u32 dwBytesSended = 0;
	u32 dwBytesSendedPerSec = 0;
	u32 dwBytesReceived = 0;
	u32 dwBytesReceivedPerSec = 0;
	CTimer* device_timer;
public:
	IClientStatistic(CTimer* timer) : device_timer(timer) {
		ZeroMemory(this, sizeof(*this));
		dwBaseTime = timer->GetElapsed_ms();
	}
	~IClientStatistic() {}
	void Update(SteamNetConnectionRealTimeStatus_t& status) {
		u32 time_global = device_timer->GetElapsed_ms();
		if (time_global - dwBaseTime >= 999)
		{
			dwBaseTime = time_global;
			dwBytesSendedPerSec = dwBytesSended; // from other place
			dwBytesSended = 0;
			dwBytesReceivedPerSec = dwBytesReceived; // from other place
			dwBytesReceived = 0;
		}

		dwRoundTripLatencyMS = status.m_nPing;
		qualityLocal = status.m_flConnectionQualityLocal;
		qualityRemote = status.m_flConnectionQualityRemote;
		packetsInPerSec = status.m_flInPacketsPerSec;
		packetsOutPerSec = status.m_flOutPacketsPerSec;
		queueTime = status.m_usecQueueTime;
		sendRateBytesPerSecond = status.m_nSendRateBytesPerSecond;
		pendingReliable = status.m_cbPendingReliable;
		pendingUnreliable = status.m_cbPendingUnreliable;
		sentUnackedReliable = status.m_cbSentUnackedReliable;
	}

	IC void Clear() {
		CTimer* timer = device_timer;
		ZeroMemory(this, sizeof(*this));
		device_timer = timer;
		dwBaseTime = timer->GetElapsed_ms();
	}

	IC u32 getPing() { return dwRoundTripLatencyMS; }
	IC u32 getBPS() { return dwThroughputBPS; }
	IC u32 getPeakBPS() { return dwPeakThroughputBPS; }
	IC u32 getDroppedCount() { return dwPacketsDropped; }
	IC u32 getRetriedCount() { return dwPacketsRetried; }
	IC u32 getMPS_Receive() { return mps_recive; }
	IC u32 getMPS_Send() { return mps_send; }
	IC u32 getReceivedPerSec() { return dwBytesReceivedPerSec; }
	IC u32 getSendedPerSec() { return dwBytesSendedPerSec; }

	IC float getQualityLocal() { return qualityLocal; }
	IC float getQualityRemote() { return qualityRemote; }
	IC float getPacketsInPerSec() { return packetsInPerSec; }
	IC float getPacketsOutPerSec() { return packetsOutPerSec; }

	IC s64 getQueueTime() { return queueTime; }
	IC s32 getSendRateBytesPerSecond() { return sendRateBytesPerSecond; }
	IC s32 getPendingReliable() { return pendingReliable; }
	IC s32 getPendingUnreliable() { return pendingUnreliable; }
	IC s32 getSentUnackedReliable() { return sentUnackedReliable; }
};