#if !defined(AFX_NET_COMPRESSOR_H__21E1ED1C_BF92_4BF0_94A8_18A27486EBFD__INCLUDED_)
#define AFX_NET_COMPRESSOR_H__21E1ED1C_BF92_4BF0_94A8_18A27486EBFD__INCLUDED_
#pragma once

#include "../xrcore/xrSyncronize.h"

class XRNETSERVER_API NET_Compressor
{
private:
    struct SCompressorStats
    {
        u32 total_uncompressed_bytes;
        u32 total_compressed_bytes;

        struct SStatPacket
        {
            u32 hit_count;
            u32 unlucky_attempts;
            u32 compressed_size;

            SStatPacket() : hit_count(0), unlucky_attempts(0), compressed_size(0)
            {
            }
        };

        xr_map<u32, SStatPacket> m_packets;

        SCompressorStats() : total_uncompressed_bytes(0), total_compressed_bytes(0)
        {
        }

        SStatPacket* get(u32 size)
        {
            // Используем insert_or_assign для гарантированного наличия записи
            return &(m_packets[size]);
        }
    };

    xrCriticalSection CS;
    SCompressorStats m_stats;

    // Вспомогательные методы (можно сделать private)
    bool ShouldCompress(u32 count) const;
    void UpdateStats(u32 original_size, u32 compressed_size,
        SCompressorStats::SStatPacket* stats, bool compressed);

public:
    NET_Compressor();
    ~NET_Compressor();

    // Сохраняем оригинальные имена
    u16 compressed_size(const u32& count);
    u16 Compress(BYTE* dest, const u32& dest_size, BYTE* src, const u32& count);
    u16 Decompress(BYTE* dest, const u32& dest_size, BYTE* src, const u32& count);
    void DumpStats(bool brief);

    // Новые методы для расширения функциональности
    void ResetStats();
    float GetCompressionRatio() const;
    u32 GetTotalProcessedBytes() const;
};

#endif // !defined(AFX_NET_COMPRESSOR_H__21E1ED1C_BF92_4BF0_94A8_18A27486EBFD__INCLUDED_)