#if !defined _INCDEF_NETCOMMON_H_
#define _INCDEF_NETCOMMON_H_
#pragma once

#define START_PORT_LAN_CL 1234
#define START_PORT_LAN_SV 1235
#define START_PORT 0
#define END_PORT 65535

#define NET_MERGE_PACKETS               1
#define NET_TAG_MERGED                  0xE1
#define NET_TAG_NONMERGED               0xE0

#define NET_USE_COMPRESSION             1
#define NET_TAG_COMPRESSED              0xC1
#define NET_TAG_NONCOMPRESSED           0xC0

#define NET_USE_LZO_COMPRESSION         1
#define NET_USE_COMPRESSION_CRC         1

#define NET_LOG_PACKETS                 0
#define NET_LOG_COMPRESSION             0
#define NET_DUMP_COMPRESSION            0


#define NET_GUARANTEEDPACKET_DEFAULT    0
#define NET_GUARANTEEDPACKET_IGNORE     1
#define NET_GUARANTEEDPACKET_SEPARATE   2

struct Buffer
{
	Buffer() : last_flags(0) { buffer.B.count = 0; }

	NET_Packet buffer;
	u32 last_flags;
};

class MultipacketSender
{
public:
	MultipacketSender() {}
	virtual ~MultipacketSender() { }

	void SendPacket(const void* packet_data, u32 packet_sz, u32 flags, u32 timeout);
	void FlushSendBuffer(u32 timeout);
	virtual void _SendTo_LL(const void* data, u32 size, u32 flags, u32 timeout) =0;
	void _FlushSendBuffer(u32 timeout, Buffer* buf);
	Buffer _buf;
	Buffer _gbuf;
	xrCriticalSection _buf_cs;
};


//==============================================================================

class MultipacketReceiver
{
public:
	MultipacketReceiver() {}
	virtual ~MultipacketReceiver() {}
	virtual void ReceivePacket(const void* packet_data, u32 packet_sz, u32 param = 0);
	virtual void _Receive(const void* data, u32 data_size, u32 param) =0;
};


//==============================================================================
#endif // _INCDEF_NETCOMMON_H_
