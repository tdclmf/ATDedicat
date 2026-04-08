// NET_Compressor.cpp: implementation of the NET_Compressor class.
//////////////////////////////////////////////////////////////////////
#include "stdafx.h"
#pragma hdrstop

#include "NET_Common.h"
#include "NET_Compressor.h"
#include <boost/crc.hpp>
XRNETSERVER_API BOOL g_net_compressor_enabled = FALSE;
XRNETSERVER_API BOOL g_net_compressor_gather_stats = FALSE;
const u32 MIN_COMPRESSION_SIZE = 36;
const u32 SAFETY_OVERHEAD = 64;
IC bool IsCompressionEnabled() { return g_net_compressor_enabled; }

#if NET_USE_COMPRESSION
static FILE* CompressionDump = NULL;
static FILE* RawTrafficDump = NULL;
// Сохраняем оригинальные макросы, но добавляем проверки
#if NET_USE_LZO_COMPRESSION
#define ENCODE    rtc9_compress
#define DECODE    rtc9_decompress
static const char* COMPRESSOR_NAME = "LZO";
#else
#include "../xrCore/ppmd_compressor.h"
#define ENCODE    ppmd_compress
#define DECODE    ppmd_decompress
static const char* COMPRESSOR_NAME = "PPMd";
#endif

#endif // NET_USE_COMPRESSION

//////////////////////////////////////////////////////////////////////
// Construction/Destruction
//////////////////////////////////////////////////////////////////////

NET_Compressor::NET_Compressor()
#ifdef PROFILE_CRITICAL_SECTIONS
    : CS(MUTEX_PROFILE_ID(NET_Compressor))
#endif // PROFILE_CRITICAL_SECTIONS
{
}

NET_Compressor::~NET_Compressor()
{
    // Закрываем файлы если они открыты
    if (CompressionDump)
    {
        fclose(CompressionDump);
        CompressionDump = nullptr;
    }
    if (RawTrafficDump)
    {
        fclose(RawTrafficDump);
        RawTrafficDump = nullptr;
    }
}

// Вспомогательный метод для определения необходимости сжатия
bool NET_Compressor::ShouldCompress(u32 count) const
{
    return (count > MIN_COMPRESSION_SIZE);
}

u16 NET_Compressor::compressed_size(const u32& count)
{
#if NET_USE_COMPRESSION

#if NET_USE_LZO_COMPRESSION
    u32 result = rtc_csize(count) + 1;
#else
    u32 result = SAFETY_OVERHEAD + (count / 8 + 1) * 10;
#endif // NET_USE_LZO_COMPRESSION

    R_ASSERT(result <= u32(u16(-1)));

    return static_cast<u16>(result);

#else
    return static_cast<u16>(count);
#endif // NET_USE_COMPRESSION
}

u16 NET_Compressor::Compress(BYTE* dest, const u32& dest_size, BYTE* src, const u32& count)
{
    VERIFY(dest);
    VERIFY(src);
    VERIFY(count);
    R_ASSERT(dest_size >= compressed_size(count));

    // Собираем статистику
    SCompressorStats::SStatPacket* packet_stats = nullptr;
    const bool should_collect_stats = g_net_compressor_gather_stats;
    const bool can_compress = ShouldCompress(count);

    if (should_collect_stats && can_compress)
    {
        packet_stats = m_stats.get(count);
        packet_stats->hit_count += 1;
        m_stats.total_uncompressed_bytes += count;
    }

#if !NET_USE_COMPRESSION
    // Без сжатия
    CopyMemory(dest, src, count);
    return static_cast<u16>(count);
#else
    u32 final_size = count;
    u32 offset = 1;

#if NET_USE_COMPRESSION_CRC
    offset += sizeof(u32);
#endif // NET_USE_COMPRESSION_CRC

    bool compression_performed = false;

    if (can_compress && IsCompressionEnabled())
    {
        CS.Enter();
        const u32 compressed_data_size = ENCODE(dest + offset, dest_size - offset, src, count);
        CS.Leave();

        if (compressed_data_size < count)
        {
            final_size = offset + compressed_data_size;
            compression_performed = true;

            if (should_collect_stats)
            {
                m_stats.total_compressed_bytes += final_size;
            }
        }
    }

    if (compression_performed)
    {
        *dest = NET_TAG_COMPRESSED;

#if NET_USE_COMPRESSION_CRC
        boost::crc_32_type crc_calculator;
        crc_calculator.process_block(dest + offset, dest + final_size);
        u32 crc = crc_calculator.checksum();
        *reinterpret_cast<u32*>(dest + 1) = crc;
#endif // NET_USE_COMPRESSION_CRC

#if NET_DUMP_COMPRESSION
        if (!CompressionDump)
        {
            CompressionDump = fopen("net-compression.log", "w+b");
        }

        if (CompressionDump)
        {
            const float ratio = 100.0f * static_cast<float>(final_size) / static_cast<float>(count);
            fprintf(CompressionDump, "%s compress %2.0f%% %u->%u\r\n",
                COMPRESSOR_NAME, ratio, count, final_size);
        }
#endif // NET_DUMP_COMPRESSION
    }
    else
    {
        // Сжатие не выгодно или отключено
        if (should_collect_stats && can_compress && packet_stats)
        {
            packet_stats->unlucky_attempts += 1;
        }

        *dest = NET_TAG_NONCOMPRESSED;
        final_size = count + 1;
        CopyMemory(dest + 1, src, count);
    }

    if (should_collect_stats && can_compress && packet_stats)
    {
        packet_stats->compressed_size += final_size;
    }

    return static_cast<u16>(final_size);
#endif // NET_USE_COMPRESSION
}

u16 NET_Compressor::Decompress(BYTE* dest, const u32& dest_size, BYTE* src, const u32& count)
{
    VERIFY(dest);
    VERIFY(src);
    VERIFY(count);

#if !NET_USE_COMPRESSION
    CopyMemory(dest, src, count);
    return static_cast<u16>(count);
#else
    if (*src != NET_TAG_COMPRESSED)
    {
        // Несжатые данные
        if (count > 0)
        {
            const u32 data_size = count - 1;
            CopyMemory(dest, src + 1, data_size);
            return static_cast<u16>(data_size);
        }
        return 0;
    }

    u32 offset = 1;

#if NET_USE_COMPRESSION_CRC
    offset += sizeof(u32);

    // Проверка CRC
    boost::crc_32_type crc_calculator;
    crc_calculator.process_block(src + offset, src + count);
    u32 calculated_crc = crc_calculator.checksum();
    u32 stored_crc = *reinterpret_cast<const u32*>(src + 1);

    R_ASSERT2(calculated_crc == stored_crc,
        make_string("CRC mismatch! Calculated: 0x%08X, Stored: 0x%08X",
            calculated_crc, stored_crc));
#endif // NET_USE_COMPRESSION_CRC

    CS.Enter();
    const u32 uncompressed_size = DECODE(dest, dest_size, src + offset, count - offset);
    CS.Leave();

    return static_cast<u16>(uncompressed_size);
#endif // !NET_USE_COMPRESSION
}

// Новые вспомогательные методы
void NET_Compressor::ResetStats()
{
    CS.Enter();
    m_stats = SCompressorStats(); // Сбрасываем к начальному состоянию
    CS.Leave();
}

float NET_Compressor::GetCompressionRatio() const
{
    if (m_stats.total_uncompressed_bytes == 0)
        return 0.0f;

    return static_cast<float>(m_stats.total_compressed_bytes) /
        static_cast<float>(m_stats.total_uncompressed_bytes);
}

u32 NET_Compressor::GetTotalProcessedBytes() const
{
    return m_stats.total_uncompressed_bytes;
}

void NET_Compressor::DumpStats(bool brief)
{
    CS.Enter();

    const auto& packets = m_stats.m_packets;

    Msg("---------NET_Compressor::DumpStats-----------");
    Msg("Active=[%s]", g_net_compressor_enabled ? "yes" : "no");
    Msg("Uncompressed total: [%u bytes]", m_stats.total_uncompressed_bytes);
    Msg("Compressed total:   [%u bytes]", m_stats.total_compressed_bytes);
    Msg("Compression ratio:  [%.2f%%]", GetCompressionRatio() * 100.0f);

    u32 total_hits = 0;
    u32 unlucky_hits = 0;

    for (const auto& pair : packets)
    {
        const auto& stats = pair.second;
        total_hits += stats.hit_count;
        unlucky_hits += stats.unlucky_attempts;

        if (!brief)
        {
            const float avg_compressed = (stats.hit_count > 0) ?
                static_cast<float>(stats.compressed_size) / static_cast<float>(stats.hit_count) : 0.0f;

            Msg("Size[%6u] Count[%6u] Unlucky[%4u] AvgCompressed[%6.0f]",
                pair.first, stats.hit_count, stats.unlucky_attempts, avg_compressed);
        }
    }

    Msg("Total packets:   [%u]", total_hits);
    Msg("Unlucky attempts:[%u] (%.1f%%)", unlucky_hits,
        total_hits > 0 ? (100.0f * unlucky_hits / total_hits) : 0.0f);

    CS.Leave();
}