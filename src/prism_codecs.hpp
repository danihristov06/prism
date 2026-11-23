#ifndef PRISM_CODECS_HPP
#define PRISM_CODECS_HPP

#include <vector>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <mutex>
#include <memory>
#include <atomic>
#include <iostream>
#include <omp.h>
#include <stdexcept>
#include "prism_buf.hpp"

extern "C" {
#include "libbsc/libbsc.h"
#include "LzmaEnc.h"
#include "LzmaDec.h"
#include "Alloc.h"
#include "Bcj2.h"
#include "Bra.h"
#include "Lzma2Enc.h"
#include "Lzma2Dec.h"
#include "Ppmd7.h"
}

namespace prism {
namespace codecs {

inline bool g_fast_mode = false;
inline bool g_ultra_mode = false;
inline int g_comp_level = 5;
inline std::atomic<bool> g_emit_progress{false};
inline std::atomic<uint64_t> g_prog_total_bytes{0};
inline std::atomic<uint64_t> g_prog_done_bytes{0};
inline std::atomic<bool> g_prog_track_payload{false};
inline thread_local int t_prog_suppress_depth{0};

struct SuppressProgress {
    bool in_parallel;
    bool prev;
    SuppressProgress() : in_parallel(omp_in_parallel() != 0), prev(false) {
        if (in_parallel) {
            ++t_prog_suppress_depth;
        } else {
            prev = g_prog_track_payload.exchange(false, std::memory_order_relaxed);
        }
    }
    ~SuppressProgress() {
        if (in_parallel) {
            --t_prog_suppress_depth;
        } else {
            g_prog_track_payload.store(prev, std::memory_order_relaxed);
        }
    }
    SuppressProgress(const SuppressProgress&) = delete;
    SuppressProgress& operator=(const SuppressProgress&) = delete;
};

inline void reset_payload_progress(uint64_t total_bytes) {
    g_prog_done_bytes.store(0, std::memory_order_relaxed);
    g_prog_total_bytes.store(total_bytes, std::memory_order_relaxed);
    g_prog_track_payload.store(total_bytes > 0, std::memory_order_relaxed);
}

struct ChunkBuf {
    std::unique_ptr<uint8_t[]> ptr;
    size_t size = 0;
};

inline void bsc_init_once() {
    static std::once_flag flag;
    std::call_once(flag, []() {
        bsc_init(LIBBSC_FEATURE_FASTMODE | LIBBSC_FEATURE_MULTITHREADING);
    });
}

inline void emit_chunk_progress(size_t chunk_len, double prog_scale = 1.0) {
    if (chunk_len == 0 ||
        prog_scale <= 0.0 ||
        t_prog_suppress_depth > 0 ||
        !g_emit_progress.load(std::memory_order_relaxed) ||
        !g_prog_track_payload.load(std::memory_order_relaxed)) {
        return;
    }
    size_t reported = (prog_scale == 1.0)
        ? chunk_len
        : std::max<size_t>(1, static_cast<size_t>(static_cast<double>(chunk_len) * prog_scale));
    #pragma omp critical(prism_progress)
    {
        uint64_t total = g_prog_total_bytes.load(std::memory_order_relaxed);
        if (total > 0) {
            uint64_t done = g_prog_done_bytes.fetch_add(static_cast<uint64_t>(reported), std::memory_order_relaxed)
                            + static_cast<uint64_t>(reported);
            uint64_t clamped_done = std::min(done, total);
            int global_pct = 20 + static_cast<int>((clamped_done * 75ULL) / total);
            if (global_pct > 95) global_pct = 95;
            std::cout << "[PROGRESS] " << global_pct << " Compressing "
                      << clamped_done << " " << total << "\n" << std::flush;
        }
    }
}

// BSC Parallel Compression / Decompression with default QLFC_FAST (3)
inline ByteVec bsc_parallel_compress(const uint8_t* src, size_t len, int coder = 3, int lzp_hash = 15, int lzp_min = 72, int num_chunks_arg = 0, double prog_scale = 1.0) {
    bsc_init_once();
    if (g_fast_mode && lzp_hash > 0) {
        lzp_hash = 12;
        lzp_min = 96;
    }
    if (g_comp_level >= 9 && coder == LIBBSC_CODER_QLFC_FAST) {
        coder = LIBBSC_CODER_QLFC_ADAPTIVE;
        if (lzp_hash > 0) {
            lzp_hash = 17;
            lzp_min = 32;
        }
    } else if (g_comp_level >= 5 && coder == LIBBSC_CODER_QLFC_FAST) {
        coder = LIBBSC_CODER_QLFC_STATIC;
        if (lzp_hash > 0) {
            lzp_hash = 16;
            lzp_min = 48;
        }
    }
    if (len == 0) {
        ByteVec header(16, 0);
        const char* magic = "PBSC";
        std::memcpy(header.data(), magic, 4);
        uint64_t orig_size = 0;
        uint32_t num_chunks = 0;
        std::memcpy(header.data() + 4, &orig_size, 8);
        std::memcpy(header.data() + 12, &num_chunks, 4);
        return header;
    }

    int num_chunks = num_chunks_arg;
    if (num_chunks <= 0) {
        if (len < 65536) {
            num_chunks = 1;
        } else if (g_comp_level >= 9 && !g_fast_mode) {
            size_t target_block = 48ULL * 1024 * 1024;
            num_chunks = std::max(1, (int)((len + target_block - 1) / target_block));
        } else if (len <= 8 * 1024 * 1024) {
            num_chunks = std::min(12, std::max(2, (int)((len + 524287) / 524288)));
        } else if (len <= 36 * 1024 * 1024) {
            num_chunks = (g_comp_level >= 5 && !g_fast_mode) ? 6 : 12;
        } else if (!g_fast_mode) {
            size_t target_block = (g_comp_level >= 7)
                ? (16ULL * 1024 * 1024)
                : ((g_comp_level >= 5)
                    ? (24ULL * 1024 * 1024)
                    : (8ULL * 1024 * 1024));
            int raw_chunks = (int)((len + target_block - 1) / target_block);
            num_chunks = std::max(6, ((raw_chunks + 5) / 6) * 6);
        } else {
            int raw_chunks = (int)((len + (3ULL * 1024 * 1024 - 1)) / (3ULL * 1024 * 1024));
            num_chunks = ((raw_chunks + 5) / 6) * 6;
        }
    }
    if (num_chunks < 1) num_chunks = 1;

    std::vector<size_t> chunk_sizes(num_chunks);
    std::vector<size_t> chunk_offsets(num_chunks);
    size_t base_chunk = len / num_chunks;
    size_t rem = len % num_chunks;
    size_t cur_off = 0;
    for (int i = 0; i < num_chunks; ++i) {
        chunk_sizes[i] = base_chunk + (i < (int)rem ? 1 : 0);
        chunk_offsets[i] = cur_off;
        cur_off += chunk_sizes[i];
    }

    std::vector<ChunkBuf> comp_chunks(num_chunks);
    std::vector<uint32_t> raw_len_vec(num_chunks);

    for (int i = 0; i < num_chunks; ++i) {
        raw_len_vec[i] = (uint32_t)chunk_sizes[i];
    }

    int features = (num_chunks > 1 || omp_in_parallel()) ? LIBBSC_FEATURE_FASTMODE : (LIBBSC_FEATURE_FASTMODE | LIBBSC_FEATURE_MULTITHREADING);

    #pragma omp parallel for schedule(dynamic, 1)
    for (int i = 0; i < num_chunks; ++i) {
        size_t chunk_off = chunk_offsets[i];
        size_t c_len = chunk_sizes[i];
        size_t cap = c_len + LIBBSC_HEADER_SIZE + 64;

        std::unique_ptr<uint8_t[]> out_buf(new uint8_t[cap]);
        int res = bsc_compress(src + chunk_off, out_buf.get(), (int)c_len, lzp_hash, lzp_min, LIBBSC_BLOCKSORTER_BWT, coder, features);
        if (res > 0) {
            comp_chunks[i].ptr = std::move(out_buf);
            comp_chunks[i].size = (size_t)res;
        } else {
            int s_res = bsc_store(src + chunk_off, out_buf.get(), (int)c_len, features);
            if (s_res > 0) {
                comp_chunks[i].ptr = std::move(out_buf);
                comp_chunks[i].size = (size_t)s_res;
            } else {
                std::memcpy(out_buf.get(), src + chunk_off, c_len);
                comp_chunks[i].ptr = std::move(out_buf);
                comp_chunks[i].size = c_len;
            }
        }
        emit_chunk_progress(c_len, prog_scale);
    }

    size_t total_header_size = 4 + 8 + 4 + num_chunks * 8;
    size_t total_payload_size = 0;
    for (int i = 0; i < num_chunks; ++i) total_payload_size += comp_chunks[i].size;

    ByteVec container(total_header_size + total_payload_size);
    uint8_t* ptr = container.data();
    std::memcpy(ptr, "PBSC", 4); ptr += 4;
    uint64_t orig_size = (uint64_t)len;
    std::memcpy(ptr, &orig_size, 8); ptr += 8;
    uint32_t n_chunks_u32 = (uint32_t)num_chunks;
    std::memcpy(ptr, &n_chunks_u32, 4); ptr += 4;

    for (int i = 0; i < num_chunks; ++i) {
        uint32_t r_l = raw_len_vec[i];
        uint32_t c_l = (uint32_t)comp_chunks[i].size;
        std::memcpy(ptr, &r_l, 4); ptr += 4;
        std::memcpy(ptr, &c_l, 4); ptr += 4;
    }

    for (int i = 0; i < num_chunks; ++i) {
        std::memcpy(ptr, comp_chunks[i].ptr.get(), comp_chunks[i].size);
        ptr += comp_chunks[i].size;
    }

    return container;
}

inline ByteVec bsc_parallel_decompress(const uint8_t* src, size_t len) {
    bsc_init_once();
    if (len < 16) throw std::runtime_error("Invalid BSC container: too short");
    if (std::memcmp(src, "PBSC", 4) != 0) throw std::runtime_error("Invalid BSC container magic");

    uint64_t orig_size = 0;
    std::memcpy(&orig_size, src + 4, 8);
    uint32_t num_chunks = 0;
    std::memcpy(&num_chunks, src + 12, 4);

    if (orig_size == 0) return ByteVec();

    size_t header_size = 4 + 8 + 4 + (size_t)num_chunks * 8;
    if (len < header_size) throw std::runtime_error("Invalid BSC container header size");

    const uint8_t* meta_ptr = src + 16;
    const uint8_t* payload_ptr = src + header_size;

    std::vector<uint32_t> raw_lens(num_chunks);
    std::vector<uint32_t> comp_lens(num_chunks);
    std::vector<size_t> payload_offsets(num_chunks);
    std::vector<size_t> raw_offsets(num_chunks);

    size_t current_payload_off = 0;
    size_t current_raw_off = 0;
    for (uint32_t i = 0; i < num_chunks; ++i) {
        std::memcpy(&raw_lens[i], meta_ptr, 4); meta_ptr += 4;
        std::memcpy(&comp_lens[i], meta_ptr, 4); meta_ptr += 4;
        payload_offsets[i] = current_payload_off;
        current_payload_off += comp_lens[i];
        raw_offsets[i] = current_raw_off;
        current_raw_off += raw_lens[i];
    }

    if (header_size + current_payload_off > len) throw std::runtime_error("Invalid BSC container payload truncated");

    ByteVec out(orig_size);
    int features = (num_chunks > 1 || omp_in_parallel()) ? LIBBSC_FEATURE_FASTMODE : (LIBBSC_FEATURE_FASTMODE | LIBBSC_FEATURE_MULTITHREADING);

    bool failed = false;
    #pragma omp parallel for schedule(dynamic, 1)
    for (int i = 0; i < (int)num_chunks; ++i) {
        if (failed) continue;
        size_t out_off = raw_offsets[i];

        const uint8_t* c_src = payload_ptr + payload_offsets[i];
        size_t c_len = comp_lens[i];
        size_t r_len = raw_lens[i];

        if (c_len == r_len) {
            std::memcpy(out.data() + out_off, c_src, r_len);
        } else {
            int res = bsc_decompress(c_src, (int)c_len, out.data() + out_off, (int)r_len, features);
            if (res != LIBBSC_NO_ERROR) {
                #pragma omp atomic write
                failed = true;
            }
        }
    }

    if (failed) throw std::runtime_error("BSC parallel decompression failed");
    return out;
}

// LZMA Parallel Compression / Decompression with extended signature
inline ByteVec lzma_parallel_compress(const uint8_t* src, size_t len, int dict_mb = 8, int fb = 64, int mc = 16, int num_chunks_arg = 0, int lc = 3, int lp = 0, int pb = 2, int level = 5, double prog_scale = 1.0) {
    if (len == 0) {
        ByteVec header(16, 0);
        std::memcpy(header.data(), "PLZM", 4);
        return header;
    }
    if (g_fast_mode) {
        dict_mb = std::min(dict_mb, 2);
        fb = std::min(fb, 16);
        mc = std::min(mc, 8);
        level = std::min(level, 3);
    }

    int num_chunks = num_chunks_arg;
    if (num_chunks <= 0) {
        if (g_fast_mode && len >= 4ULL * 1024 * 1024) {
            num_chunks = std::min(12, std::max(6, (int)((len + 1572863) / 1572864)));
        } else if (len <= 12 * 1024 * 1024) {
            num_chunks = 6;
        } else {
            num_chunks = std::min(12, std::max(6, (int)((len + 4194303) / 4194304)));
        }
    }
    if (num_chunks < 1) num_chunks = 1;

    std::vector<size_t> chunk_sizes(num_chunks);
    std::vector<size_t> chunk_offsets(num_chunks);
    size_t base_chunk = len / num_chunks;
    size_t rem = len % num_chunks;
    size_t cur_off = 0;
    for (int i = 0; i < num_chunks; ++i) {
        chunk_sizes[i] = base_chunk + (i < (int)rem ? 1 : 0);
        chunk_offsets[i] = cur_off;
        cur_off += chunk_sizes[i];
    }

    std::vector<ChunkBuf> comp_chunks(num_chunks);
    std::vector<uint32_t> raw_len_vec(num_chunks);

    #pragma omp parallel for schedule(dynamic, 1)
    for (int i = 0; i < num_chunks; ++i) {
        size_t chunk_off = chunk_offsets[i];
        size_t c_len = chunk_sizes[i];
        raw_len_vec[i] = (uint32_t)c_len;

        CLzmaEncProps props;
        LzmaEncProps_Init(&props);
        props.level = level;
        props.lc = lc;
        props.lp = lp;
        props.pb = pb;
        props.dictSize = std::min<uint32_t>(dict_mb * 1024 * 1024, std::max<uint32_t>(65536, (uint32_t)c_len));
        props.fb = fb;
        props.btMode = g_fast_mode ? 0 : 1;
        props.numHashBytes = 4;
        props.mc = mc;
        props.writeEndMark = 0;
        props.numThreads = 1;

        SizeT dest_len = c_len + 128;
        std::unique_ptr<uint8_t[]> chunk_out(new uint8_t[dest_len + 5]);
        Byte props_encoded[5];
        SizeT props_size = 5;

        SRes res = LzmaEncode(chunk_out.get() + 5, &dest_len, src + chunk_off, (SizeT)c_len, &props, props_encoded, &props_size, 0, NULL, &g_Alloc, &g_Alloc);

        if (res == SZ_OK && (dest_len + 5) < c_len) {
            std::memcpy(chunk_out.get(), props_encoded, 5);
            comp_chunks[i].ptr = std::move(chunk_out);
            comp_chunks[i].size = 5 + (size_t)dest_len;
        } else {
            std::memcpy(chunk_out.get(), src + chunk_off, c_len);
            comp_chunks[i].ptr = std::move(chunk_out);
            comp_chunks[i].size = c_len;
        }
        emit_chunk_progress(c_len, prog_scale);
    }

    size_t total_header_size = 4 + 8 + 4 + num_chunks * 8;
    size_t total_payload_size = 0;
    for (int i = 0; i < num_chunks; ++i) total_payload_size += comp_chunks[i].size;

    ByteVec container(total_header_size + total_payload_size);
    uint8_t* ptr = container.data();
    std::memcpy(ptr, "PLZM", 4); ptr += 4;
    uint64_t orig_size = (uint64_t)len;
    std::memcpy(ptr, &orig_size, 8); ptr += 8;
    uint32_t n_chunks_u32 = (uint32_t)num_chunks;
    std::memcpy(ptr, &n_chunks_u32, 4); ptr += 4;

    for (int i = 0; i < num_chunks; ++i) {
        uint32_t r_l = raw_len_vec[i];
        uint32_t c_l = (uint32_t)comp_chunks[i].size;
        std::memcpy(ptr, &r_l, 4); ptr += 4;
        std::memcpy(ptr, &c_l, 4); ptr += 4;
    }

    for (int i = 0; i < num_chunks; ++i) {
        std::memcpy(ptr, comp_chunks[i].ptr.get(), comp_chunks[i].size);
        ptr += comp_chunks[i].size;
    }

    return container;
}

inline ByteVec lzma_parallel_decompress(const uint8_t* src, size_t len) {
    if (len < 16) throw std::runtime_error("Invalid LZMA container: too short");
    if (std::memcmp(src, "PLZM", 4) != 0) throw std::runtime_error("Invalid LZMA container magic");

    uint64_t orig_size = 0;
    std::memcpy(&orig_size, src + 4, 8);
    uint32_t num_chunks = 0;
    std::memcpy(&num_chunks, src + 12, 4);

    if (orig_size == 0) return ByteVec();

    size_t header_size = 4 + 8 + 4 + (size_t)num_chunks * 8;
    if (len < header_size) throw std::runtime_error("Invalid LZMA container header size");

    const uint8_t* meta_ptr = src + 16;
    const uint8_t* payload_ptr = src + header_size;

    std::vector<uint32_t> raw_lens(num_chunks);
    std::vector<uint32_t> comp_lens(num_chunks);
    std::vector<size_t> payload_offsets(num_chunks);
    std::vector<size_t> raw_offsets(num_chunks);

    size_t current_payload_off = 0;
    size_t current_raw_off = 0;
    for (uint32_t i = 0; i < num_chunks; ++i) {
        std::memcpy(&raw_lens[i], meta_ptr, 4); meta_ptr += 4;
        std::memcpy(&comp_lens[i], meta_ptr, 4); meta_ptr += 4;
        payload_offsets[i] = current_payload_off;
        raw_offsets[i] = current_raw_off;
        current_payload_off += comp_lens[i];
        current_raw_off += raw_lens[i];
    }

    if (header_size + current_payload_off > len) throw std::runtime_error("Invalid LZMA container payload truncated");

    ByteVec out(orig_size);
    bool failed = false;

    #pragma omp parallel for schedule(dynamic, 1)
    for (int i = 0; i < (int)num_chunks; ++i) {
        if (failed) continue;
        size_t out_off = raw_offsets[i];

        const uint8_t* c_src = payload_ptr + payload_offsets[i];
        size_t c_len = comp_lens[i];
        size_t r_len = raw_lens[i];

        if (c_len == r_len) {
            std::memcpy(out.data() + out_off, c_src, r_len);
        } else {
            if (c_len < 5) {
                #pragma omp atomic write
                failed = true;
                continue;
            }
            SizeT dest_len = r_len;
            SizeT src_len_lz = c_len - 5;
            ELzmaStatus status;
            SRes res = LzmaDecode(out.data() + out_off, &dest_len, c_src + 5, &src_len_lz, c_src, 5, LZMA_FINISH_ANY, &status, &g_Alloc);
            if (res != SZ_OK || dest_len != r_len) {
                #pragma omp atomic write
                failed = true;
            }
        }
    }

    if (failed) throw std::runtime_error("LZMA parallel decompression failed");
    return out;
}

// Parallel LZMA2 ("PLZ2"): large independent solid LZMA2 blocks, each with large dictionary.
// Container: [4B "PLZ2"][8B orig][4B n] + n*(4B raw_len,4B comp_len,1B prop) + payloads. prop 0xFF => raw stored.
inline ByteVec lzma2_parallel_compress(const uint8_t* src, size_t len, int dict_mb = 16, int fb = 64, int mc = 24, int num_chunks_arg = 0, int lc = 3, int lp = 0, int pb = 2, int level = 6, double prog_scale = 1.0) {
    if (len == 0) {
        ByteVec header(16, 0);
        std::memcpy(header.data(), "PLZ2", 4);
        return header;
    }
    if (g_fast_mode) {
        dict_mb = std::min(dict_mb, 2);
        fb = std::min(fb, 16);
        mc = std::min(mc, 8);
        level = std::min(level, 3);
    }
    int max_t = std::max(1, omp_get_max_threads());
    bool nested = (omp_in_parallel() != 0);
    int inner_threads = (nested || max_t < 4 || len < 4ULL * 1024 * 1024 || num_chunks_arg >= max_t) ? 1 : 2;
    int outer_threads = nested ? 1 : std::max(1, max_t / inner_threads);
    if (dict_mb >= 16) outer_threads = std::min(outer_threads, 8);
    if (g_comp_level >= 9 && !g_fast_mode) {
        dict_mb = 128;
        fb = 273;
        mc = 256;
        level = 9;
        outer_threads = std::min(outer_threads, 2);
    } else if (g_comp_level >= 7 && !g_fast_mode) {
        dict_mb = 32;
        fb = 128;
        mc = 64;
        level = 8;
        outer_threads = std::min(outer_threads, 4);
    }

    int num_chunks = num_chunks_arg;
    if (num_chunks <= 0 && g_comp_level >= 9 && !g_fast_mode) {
        size_t tc = 128ULL * 1024 * 1024;
        num_chunks = std::max(1, (int)((len + tc - 1) / tc));
    } else if (num_chunks <= 0 && g_comp_level >= 7 && !g_fast_mode) {
        size_t tc = 48ULL * 1024 * 1024;
        num_chunks = (len < 4ULL * 1024 * 1024) ? 1 : std::max(2, (int)((len + tc - 1) / tc));
    }
    if (num_chunks <= 0) {
        if (len < 256ULL * 1024) {
            num_chunks = 1;
        } else if (len <= 16ULL * 1024 * 1024) {
            num_chunks = std::min(outer_threads, std::max(2, (int)((len + 4194303) / 4194304)));
        } else {
            size_t target_chunk = 24ULL * 1024 * 1024;
            int raw_chunks = (int)((len + target_chunk - 1) / target_chunk);
            num_chunks = std::max(outer_threads, raw_chunks);
            if (outer_threads > 1 && num_chunks > outer_threads) {
                num_chunks = ((num_chunks + outer_threads - 1) / outer_threads) * outer_threads;
            }
        }
    }
    if (num_chunks < 1) num_chunks = 1;

    std::vector<size_t> chunk_sizes(num_chunks);
    std::vector<size_t> chunk_offsets(num_chunks);
    size_t base_chunk = len / num_chunks;
    size_t rem = len % num_chunks;
    size_t cur_off = 0;
    for (int i = 0; i < num_chunks; ++i) {
        chunk_sizes[i] = base_chunk + (i < (int)rem ? 1 : 0);
        chunk_offsets[i] = cur_off;
        cur_off += chunk_sizes[i];
    }

    std::vector<ChunkBuf> comp_chunks(num_chunks);
    std::vector<uint8_t> prop_bytes(num_chunks, 0xFF);
    std::atomic<bool> enc_failed{false};

    #pragma omp parallel for num_threads(outer_threads) schedule(dynamic, 1)
    for (int i = 0; i < num_chunks; ++i) {
        size_t chunk_off = chunk_offsets[i];
        size_t c_len = chunk_sizes[i];
        size_t out_cap = c_len + (c_len >> 10) + 4096;
        std::unique_ptr<uint8_t[]> chunk_out(new uint8_t[out_cap]);
        size_t out_sz = 0;
        bool ok = false;
        uint8_t prop_byte = 0xFF;

        int best_lc = lc, best_lp = lp, best_pb = pb;
        bool use_bsc_chunk = false;

        if (!g_fast_mode && c_len >= 2ULL * 1024 * 1024) {
            constexpr size_t SL = 32768;
            constexpr size_t SAMPLE_SZ = SL * 3;
            uint8_t sample_buf[SAMPLE_SZ];
            const size_t offs[3] = { c_len * 15 / 100, c_len * 50 / 100, c_len * 85 / 100 };
            for (int s = 0; s < 3; ++s) {
                size_t so = std::min(offs[s], c_len - SL);
                std::memcpy(sample_buf + s * SL, src + chunk_off + so, SL);
            }

            static constexpr int kCtx[4][3] = {
                {3, 0, 2},
                {1, 2, 2},
                {0, 2, 2},
                {1, 1, 1},
            };
            size_t best_lz_sz = SAMPLE_SZ + 1024;
            uint8_t probe_out[SAMPLE_SZ + LIBBSC_HEADER_SIZE + 128];

            for (int ci = 0; ci < 4; ++ci) {
                CLzmaEncProps p;
                LzmaEncProps_Init(&p);
                p.level = 1;
                p.lc = kCtx[ci][0];
                p.lp = kCtx[ci][1];
                p.pb = kCtx[ci][2];
                p.dictSize = 128 * 1024;
                p.fb = 16;
                p.btMode = 0;
                p.numHashBytes = 4;
                p.mc = 8;
                p.writeEndMark = 0;
                p.numThreads = 1;
                SizeT dl = SAMPLE_SZ + 128;
                Byte pe[5];
                SizeT ps = 5;
                if (LzmaEncode(probe_out, &dl, sample_buf, SAMPLE_SZ, &p, pe, &ps, 0, NULL, &g_Alloc, &g_Alloc) == SZ_OK) {
                    if (static_cast<size_t>(dl) < best_lz_sz) {
                        best_lz_sz = static_cast<size_t>(dl);
                        best_lc = kCtx[ci][0];
                        best_lp = kCtx[ci][1];
                        best_pb = kCtx[ci][2];
                    }
                }
            }

            bsc_init_once();
            int bsc_r = bsc_compress(sample_buf, probe_out, static_cast<int>(SAMPLE_SZ), 15, 72, LIBBSC_BLOCKSORTER_BWT, LIBBSC_CODER_QLFC_FAST, LIBBSC_FEATURE_FASTMODE);
            if (bsc_r > 0 && static_cast<size_t>(bsc_r) * 100ULL <= best_lz_sz * 97ULL) {
                use_bsc_chunk = true;
            }
        }

        if (use_bsc_chunk) {
            bsc_init_once();
            int coder = (g_comp_level >= 9) ? LIBBSC_CODER_QLFC_ADAPTIVE : LIBBSC_CODER_QLFC_STATIC;
            int lzp_h = (g_comp_level >= 9) ? 17 : 16;
            int lzp_m = (g_comp_level >= 9) ? 32 : 48;
            int bres = bsc_compress(src + chunk_off, chunk_out.get(), static_cast<int>(c_len), lzp_h, lzp_m, LIBBSC_BLOCKSORTER_BWT, coder, LIBBSC_FEATURE_FASTMODE);
            if (bres > 0 && static_cast<size_t>(bres) < c_len) {
                out_sz = static_cast<size_t>(bres);
                prop_byte = 0xFE;
                ok = true;
            }
        }

        if (!ok) {
            CLzma2EncHandle enc = Lzma2Enc_Create(&g_Alloc, &g_BigAlloc);
            if (enc) {
                CLzma2EncProps props;
                Lzma2EncProps_Init(&props);
                props.lzmaProps.level = level;
                props.lzmaProps.lc = best_lc;
                props.lzmaProps.lp = best_lp;
                props.lzmaProps.pb = best_pb;
                props.lzmaProps.dictSize = std::min<uint32_t>((uint32_t)dict_mb * 1024U * 1024U, std::max<uint32_t>(65536U, (uint32_t)c_len));
                props.lzmaProps.fb = fb;
                props.lzmaProps.btMode = 1;
                props.lzmaProps.numHashBytes = 4;
                props.lzmaProps.mc = mc;
                props.lzmaProps.writeEndMark = 0;
                props.lzmaProps.numThreads = inner_threads;
                props.blockSize = LZMA2_ENC_PROPS_BLOCK_SIZE_SOLID;
                props.numBlockThreads_Max = 1;
                props.numTotalThreads = inner_threads;
                if (Lzma2Enc_SetProps(enc, &props) == SZ_OK) {
                    Lzma2Enc_SetDataSize(enc, (UInt64)c_len);
                    prop_byte = Lzma2Enc_WriteProperties(enc);
                    out_sz = out_cap;
                    SRes res = Lzma2Enc_Encode2(enc, NULL, chunk_out.get(), &out_sz, NULL, src + chunk_off, c_len, NULL);
                    ok = (res == SZ_OK && out_sz < c_len);
                }
                Lzma2Enc_Destroy(enc);
            }
        }
        if (ok) {
            comp_chunks[i].size = out_sz;
            prop_bytes[i] = prop_byte;
        } else {
            std::memcpy(chunk_out.get(), src + chunk_off, c_len);
            comp_chunks[i].size = c_len;
            prop_bytes[i] = 0xFF;
        }
        comp_chunks[i].ptr = std::move(chunk_out);
        emit_chunk_progress(c_len, prog_scale);
    }

    size_t total_header_size = 16 + (size_t)num_chunks * 9;
    size_t total_payload_size = 0;
    for (int i = 0; i < num_chunks; ++i) total_payload_size += comp_chunks[i].size;

    ByteVec container(total_header_size + total_payload_size);
    uint8_t* ptr = container.data();
    std::memcpy(ptr, "PLZ2", 4); ptr += 4;
    uint64_t orig_size = (uint64_t)len;
    std::memcpy(ptr, &orig_size, 8); ptr += 8;
    uint32_t n_u32 = (uint32_t)num_chunks;
    std::memcpy(ptr, &n_u32, 4); ptr += 4;
    for (int i = 0; i < num_chunks; ++i) {
        uint32_t r_l = (uint32_t)chunk_sizes[i];
        uint32_t c_l = (uint32_t)comp_chunks[i].size;
        std::memcpy(ptr, &r_l, 4); ptr += 4;
        std::memcpy(ptr, &c_l, 4); ptr += 4;
        *ptr++ = prop_bytes[i];
    }
    for (int i = 0; i < num_chunks; ++i) {
        std::memcpy(ptr, comp_chunks[i].ptr.get(), comp_chunks[i].size);
        ptr += comp_chunks[i].size;
    }
    return container;
}

inline ByteVec lzma2_parallel_decompress(const uint8_t* src, size_t len) {
    if (len < 16) throw std::runtime_error("Invalid LZMA2 container: too short");
    if (std::memcmp(src, "PLZ2", 4) != 0) throw std::runtime_error("Invalid LZMA2 container magic");
    uint64_t orig_size = 0;
    std::memcpy(&orig_size, src + 4, 8);
    uint32_t num_chunks = 0;
    std::memcpy(&num_chunks, src + 12, 4);
    if (orig_size == 0) return ByteVec();

    size_t header_size = 16 + (size_t)num_chunks * 9;
    if (len < header_size) throw std::runtime_error("Invalid LZMA2 container header size");

    std::vector<uint32_t> raw_lens(num_chunks), comp_lens(num_chunks);
    std::vector<uint8_t> props(num_chunks);
    std::vector<size_t> payload_offsets(num_chunks), raw_offsets(num_chunks);
    const uint8_t* meta = src + 16;
    size_t cp = 0, cr = 0;
    for (uint32_t i = 0; i < num_chunks; ++i) {
        std::memcpy(&raw_lens[i], meta, 4); meta += 4;
        std::memcpy(&comp_lens[i], meta, 4); meta += 4;
        props[i] = *meta++;
        payload_offsets[i] = cp;
        raw_offsets[i] = cr;
        cp += comp_lens[i];
        cr += raw_lens[i];
    }
    if (cr != orig_size) throw std::runtime_error("Invalid LZMA2 container size sum");
    if (header_size + cp > len) throw std::runtime_error("Invalid LZMA2 container payload truncated");
    const uint8_t* payload_ptr = src + header_size;

    ByteVec out(orig_size);
    std::atomic<bool> failed{false};
    #pragma omp parallel for schedule(dynamic, 1)
    for (int i = 0; i < (int)num_chunks; ++i) {
        if (failed.load(std::memory_order_relaxed)) continue;
        uint8_t* dst = out.data() + raw_offsets[i];
        const uint8_t* c_src = payload_ptr + payload_offsets[i];
        size_t c_len = comp_lens[i];
        size_t r_len = raw_lens[i];
        if (props[i] == 0xFF) {
            if (c_len != r_len) { failed = true; continue; }
            std::memcpy(dst, c_src, r_len);
        } else if (props[i] == 0xFE) {
            bsc_init_once();
            if (bsc_decompress(c_src, static_cast<int>(c_len), dst, static_cast<int>(r_len), LIBBSC_FEATURE_FASTMODE) != LIBBSC_NO_ERROR) {
                failed = true;
            }
        } else {
            SizeT dest_len = r_len;
            SizeT src_len_lz = c_len;
            ELzmaStatus status;
            SRes res = Lzma2Decode(dst, &dest_len, c_src, &src_len_lz, props[i], LZMA_FINISH_END, &status, &g_Alloc);
            if (res != SZ_OK || dest_len != r_len) failed = true;
        }
    }
    if (failed) throw std::runtime_error("LZMA2 parallel decompression failed");
    return out;
}

inline ByteVec lrm_lzma2_decompress(const uint8_t* src, size_t len);
inline ByteVec macho_lrm_lzma2_decompress(const uint8_t* src, size_t len);
inline ByteVec ppmd_parallel_decompress(const uint8_t* src, size_t len);

inline ByteVec lzma_auto_decompress(const uint8_t* src, size_t len) {
    if (len >= 4 && std::memcmp(src, "PMCH", 4) == 0) return macho_lrm_lzma2_decompress(src, len);
    if (len >= 4 && std::memcmp(src, "PLRM", 4) == 0) return lrm_lzma2_decompress(src, len);
    if (len >= 4 && std::memcmp(src, "PPMD", 4) == 0) return ppmd_parallel_decompress(src, len);
    if (len >= 4 && std::memcmp(src, "PLZ2", 4) == 0) return lzma2_parallel_decompress(src, len);
    if (len >= 4 && std::memcmp(src, "PBSC", 4) == 0) return bsc_parallel_decompress(src, len);
    return lzma_parallel_decompress(src, len);
}

struct PpmdMemOut {
    IByteOut vt;
    uint8_t* dst;
    size_t pos;
    size_t cap;
    bool overflow;
    static void Write(const IByteOut* p, Byte b) {
        PpmdMemOut* self = reinterpret_cast<PpmdMemOut*>(const_cast<IByteOut*>(p));
        if (self->pos < self->cap) {
            self->dst[self->pos++] = b;
        } else {
            self->overflow = true;
        }
    }
};

struct PpmdMemIn {
    IByteIn vt;
    const uint8_t* src;
    size_t pos;
    size_t len;
    static Byte Read(const IByteIn* p) {
        PpmdMemIn* self = reinterpret_cast<PpmdMemIn*>(const_cast<IByteIn*>(p));
        if (self->pos < self->len) return self->src[self->pos++];
        return 0;
    }
};

inline ByteVec ppmd_parallel_compress(const uint8_t* src, size_t len, int order = 8, uint32_t mem_mb = 16, int num_chunks_arg = 0, double prog_scale = 1.0) {
    if (len == 0) {
        ByteVec header(21, 0);
        std::memcpy(header.data(), "PPMD", 4);
        return header;
    }
    if (g_comp_level >= 9) {
        order = 12;
        mem_mb = 64;
    } else if (g_comp_level >= 7) {
        order = 10;
        mem_mb = 32;
    } else {
        order = 8;
        mem_mb = 16;
    }
    uint32_t mem_bytes = mem_mb * 1024U * 1024U;
    if (len < mem_bytes && len >= 1024ULL * 1024) {
        mem_bytes = std::max<uint32_t>(2U * 1024U * 1024U, static_cast<uint32_t>(len));
    }

    int num_chunks = num_chunks_arg;
    if (num_chunks <= 0) {
        if (len < 256ULL * 1024) {
            num_chunks = 1;
        } else if (g_comp_level >= 9) {
            size_t tc = 48ULL * 1024 * 1024;
            num_chunks = std::max(1, static_cast<int>((len + tc - 1) / tc));
        } else {
            size_t tc = 12ULL * 1024 * 1024;
            int raw_c = static_cast<int>((len + tc - 1) / tc);
            num_chunks = std::min(12, std::max(6, raw_c));
            if (len < 6ULL * 1024 * 1024) num_chunks = std::max(2, static_cast<int>((len + 1048575) / 1048576));
        }
    }
    if (num_chunks < 1) num_chunks = 1;

    std::vector<size_t> chunk_sizes(num_chunks), chunk_offsets(num_chunks);
    size_t base_chunk = len / num_chunks, rem = len % num_chunks, cur_off = 0;
    for (int i = 0; i < num_chunks; ++i) {
        chunk_sizes[i] = base_chunk + (i < static_cast<int>(rem) ? 1 : 0);
        chunk_offsets[i] = cur_off;
        cur_off += chunk_sizes[i];
    }

    std::vector<ChunkBuf> comp_chunks(num_chunks);
    #pragma omp parallel for schedule(dynamic, 1)
    for (int i = 0; i < num_chunks; ++i) {
        size_t c_len = chunk_sizes[i];
        const uint8_t* c_src = src + chunk_offsets[i];
        std::unique_ptr<uint8_t[]> out_buf(new uint8_t[c_len + 64]);

        CPpmd7 ppmd;
        Ppmd7_Construct(&ppmd);
        bool ok = false;
        if (Ppmd7_Alloc(&ppmd, mem_bytes, &g_Alloc)) {
            Ppmd7_Init(&ppmd, static_cast<unsigned>(order));
            PpmdMemOut mout;
            mout.vt.Write = &PpmdMemOut::Write;
            mout.dst = out_buf.get();
            mout.pos = 0;
            mout.cap = c_len;
            mout.overflow = false;
            ppmd.rc.enc.Stream = &mout.vt;
            Ppmd7z_Init_RangeEnc(&ppmd);
            Ppmd7z_EncodeSymbols(&ppmd, c_src, c_src + c_len);
            Ppmd7z_Flush_RangeEnc(&ppmd);
            Ppmd7_Free(&ppmd, &g_Alloc);
            if (!mout.overflow && mout.pos < c_len) {
                comp_chunks[i].size = mout.pos;
                ok = true;
            }
        }
        if (!ok) {
            std::memcpy(out_buf.get(), c_src, c_len);
            comp_chunks[i].size = c_len;
        }
        comp_chunks[i].ptr = std::move(out_buf);
        emit_chunk_progress(c_len, prog_scale);
    }

    size_t hdr_sz = 21 + static_cast<size_t>(num_chunks) * 8;
    size_t pay_sz = 0;
    for (int i = 0; i < num_chunks; ++i) pay_sz += comp_chunks[i].size;
    ByteVec container(hdr_sz + pay_sz);
    uint8_t* ptr = container.data();
    std::memcpy(ptr, "PPMD", 4); ptr += 4;
    uint64_t orig_sz = static_cast<uint64_t>(len);
    std::memcpy(ptr, &orig_sz, 8); ptr += 8;
    uint32_t n_u32 = static_cast<uint32_t>(num_chunks);
    std::memcpy(ptr, &n_u32, 4); ptr += 4;
    *ptr++ = static_cast<uint8_t>(order);
    std::memcpy(ptr, &mem_bytes, 4); ptr += 4;
    for (int i = 0; i < num_chunks; ++i) {
        uint32_t rl = static_cast<uint32_t>(chunk_sizes[i]);
        uint32_t cl = static_cast<uint32_t>(comp_chunks[i].size);
        std::memcpy(ptr, &rl, 4); ptr += 4;
        std::memcpy(ptr, &cl, 4); ptr += 4;
    }
    for (int i = 0; i < num_chunks; ++i) {
        std::memcpy(ptr, comp_chunks[i].ptr.get(), comp_chunks[i].size);
        ptr += comp_chunks[i].size;
    }
    return container;
}

inline ByteVec ppmd_parallel_decompress(const uint8_t* src, size_t len) {
    if (len < 21 || std::memcmp(src, "PPMD", 4) != 0) {
        throw std::runtime_error("Invalid PPMD container");
    }
    uint64_t orig_size = 0;
    std::memcpy(&orig_size, src + 4, 8);
    if (orig_size == 0) return ByteVec();
    uint32_t num_chunks = 0;
    std::memcpy(&num_chunks, src + 12, 4);
    uint8_t order = src[16];
    uint32_t mem_bytes = 0;
    std::memcpy(&mem_bytes, src + 17, 4);

    size_t hdr_sz = 21 + static_cast<size_t>(num_chunks) * 8;
    if (len < hdr_sz) throw std::runtime_error("Invalid PPMD header truncated");

    std::vector<uint32_t> raw_lens(num_chunks), comp_lens(num_chunks);
    std::vector<size_t> pay_offs(num_chunks), raw_offs(num_chunks);
    const uint8_t* meta = src + 21;
    size_t cp = 0, cr = 0;
    for (uint32_t i = 0; i < num_chunks; ++i) {
        std::memcpy(&raw_lens[i], meta, 4); meta += 4;
        std::memcpy(&comp_lens[i], meta, 4); meta += 4;
        pay_offs[i] = cp;
        raw_offs[i] = cr;
        cp += comp_lens[i];
        cr += raw_lens[i];
    }
    if (cr != orig_size || hdr_sz + cp > len) throw std::runtime_error("Invalid PPMD payload");
    const uint8_t* pay_ptr = src + hdr_sz;

    ByteVec out(orig_size);
    std::atomic<bool> failed{false};
    #pragma omp parallel for schedule(dynamic, 1)
    for (int i = 0; i < static_cast<int>(num_chunks); ++i) {
        if (failed.load(std::memory_order_relaxed)) continue;
        uint8_t* dst = out.data() + raw_offs[i];
        const uint8_t* c_src = pay_ptr + pay_offs[i];
        size_t c_len = comp_lens[i];
        size_t r_len = raw_lens[i];
        if (c_len == r_len) {
            std::memcpy(dst, c_src, r_len);
        } else {
            CPpmd7 ppmd;
            Ppmd7_Construct(&ppmd);
            if (!Ppmd7_Alloc(&ppmd, mem_bytes, &g_Alloc)) {
                failed = true;
                continue;
            }
            Ppmd7_Init(&ppmd, static_cast<unsigned>(order));
            PpmdMemIn min_s;
            min_s.vt.Read = &PpmdMemIn::Read;
            min_s.src = c_src;
            min_s.pos = 0;
            min_s.len = c_len;
            ppmd.rc.dec.Stream = &min_s.vt;
            if (!Ppmd7z_RangeDec_Init(&ppmd.rc.dec)) {
                Ppmd7_Free(&ppmd, &g_Alloc);
                failed = true;
                continue;
            }
            for (size_t j = 0; j < r_len; ++j) {
                int sym = Ppmd7z_DecodeSymbol(&ppmd);
                if (sym < 0) {
                    failed = true;
                    break;
                }
                dst[j] = static_cast<uint8_t>(sym);
            }
            Ppmd7_Free(&ppmd, &g_Alloc);
        }
    }
    if (failed) throw std::runtime_error("PPMD parallel decompression failed");
    return out;
}

inline ByteVec lrm_ppmd_compress(const uint8_t* src, size_t len, double prog_scale = 1.0) {
    if (!g_fast_mode && len >= 4ULL * 1024 * 1024) {
        int mode = g_comp_level >= 9 ? 3 : (g_comp_level >= 7 ? 1 : 0);
        auto lrm_res = precond::lrm_encode(src, len, mode);
        if (lrm_res.applied) {
            ByteVec tok_comp;
            {
                SuppressProgress sp;
                tok_comp = bsc_parallel_compress(lrm_res.tokens.data(), lrm_res.tokens.size(), 3, 15, 72, 0);
            }
            double lit_scale = lrm_res.literals.empty()
                ? prog_scale
                : (prog_scale * static_cast<double>(len) / static_cast<double>(lrm_res.literals.size()));
            ByteVec lit_comp = ppmd_parallel_compress(lrm_res.literals.data(), lrm_res.literals.size(), 8, 16, 0, lit_scale);

            ByteVec container;
            container.reserve(24 + tok_comp.size() + lit_comp.size());
            container.insert(container.end(), {'P', 'L', 'R', 'M'});
            uint64_t orig_sz = static_cast<uint64_t>(len);
            container.insert(container.end(), reinterpret_cast<const uint8_t*>(&orig_sz), reinterpret_cast<const uint8_t*>(&orig_sz) + 8);
            uint32_t tok_sz = static_cast<uint32_t>(tok_comp.size());
            container.insert(container.end(), reinterpret_cast<const uint8_t*>(&tok_sz), reinterpret_cast<const uint8_t*>(&tok_sz) + 4);
            container.insert(container.end(), tok_comp.begin(), tok_comp.end());
            uint64_t lit_sz = static_cast<uint64_t>(lrm_res.literals.size());
            container.insert(container.end(), reinterpret_cast<const uint8_t*>(&lit_sz), reinterpret_cast<const uint8_t*>(&lit_sz) + 8);
            container.insert(container.end(), lit_comp.begin(), lit_comp.end());
            return container;
        }
    }
    return ppmd_parallel_compress(src, len, 8, 16, 0, prog_scale);
}

inline ByteVec lrm_bsc_compress(const uint8_t* src, size_t len, int coder = 3, int lzp_hash = 15, int lzp_min = 72, int num_chunks_arg = 0, double prog_scale = 1.0) {
    if (len >= 4ULL * 1024 * 1024) {
        int mode = g_fast_mode ? 2 : (g_comp_level >= 9 ? 3 : (g_comp_level >= 7 ? 1 : 0));
        size_t far_d = (g_comp_level >= 9) ? (32ULL * 1024 * 1024) : (4ULL * 1024 * 1024);
        size_t min_loc = (g_comp_level >= 9) ? 192 : 64;
        auto lrm_res = precond::lrm_encode(src, len, mode, far_d, min_loc);
        if (lrm_res.applied) {
            ByteVec tok_comp;
            {
                SuppressProgress sp;
                tok_comp = bsc_parallel_compress(lrm_res.tokens.data(), lrm_res.tokens.size(), 3, 15, 72, 0);
            }
            double lit_scale = lrm_res.literals.empty()
                ? prog_scale
                : (prog_scale * static_cast<double>(len) / static_cast<double>(lrm_res.literals.size()));
            ByteVec lit_comp = bsc_parallel_compress(
                lrm_res.literals.data(), lrm_res.literals.size(),
                coder, lzp_hash, lzp_min, num_chunks_arg, lit_scale);

            ByteVec container;
            container.reserve(24 + tok_comp.size() + lit_comp.size());
            container.insert(container.end(), {'P', 'L', 'R', 'M'});
            uint64_t orig_sz = static_cast<uint64_t>(len);
            container.insert(container.end(), reinterpret_cast<const uint8_t*>(&orig_sz), reinterpret_cast<const uint8_t*>(&orig_sz) + 8);
            uint32_t tok_sz = static_cast<uint32_t>(tok_comp.size());
            container.insert(container.end(), reinterpret_cast<const uint8_t*>(&tok_sz), reinterpret_cast<const uint8_t*>(&tok_sz) + 4);
            container.insert(container.end(), tok_comp.begin(), tok_comp.end());
            uint64_t lit_sz = static_cast<uint64_t>(lrm_res.literals.size());
            container.insert(container.end(), reinterpret_cast<const uint8_t*>(&lit_sz), reinterpret_cast<const uint8_t*>(&lit_sz) + 8);
            container.insert(container.end(), lit_comp.begin(), lit_comp.end());
            return container;
        }
    }
    return bsc_parallel_compress(src, len, coder, lzp_hash, lzp_min, num_chunks_arg, prog_scale);
}

inline ByteVec lrm_lzma2_compress(const uint8_t* src, size_t len, int dict_mb = 16, int fb = 64, int mc = 24, int lc = 3, int lp = 0, int pb = 2, int level = 6, double prog_scale = 1.0) {
    if (!g_fast_mode && len >= 4ULL * 1024 * 1024) {
        struct MachoSecRange {
            uint64_t off;
            uint64_t sz;
            uint8_t arch; // 1=ARM64, 2=X86_64
        };
        std::vector<MachoSecRange> sec_ranges;
        // Scan for 64-bit LE Mach-O (0xFEEDFACF -> CF FA ED FE) and 64-bit LE ELF (0x7F 'E' 'L' 'F')
        for (size_t p = 0; p + 64 <= len; ) {
            const uint8_t* m_hit = static_cast<const uint8_t*>(std::memchr(src + p, 0xCF, (len - 32) - p + 1));
            const uint8_t* e_hit = static_cast<const uint8_t*>(std::memchr(src + p, 0x7F, (len - 64) - p + 1));
            const uint8_t* hit = nullptr;
            if (m_hit && e_hit) hit = std::min(m_hit, e_hit);
            else hit = m_hit ? m_hit : e_hit;
            if (!hit) break;
            size_t pos = static_cast<size_t>(hit - src);
            if (src[pos] == 0x7F && pos + 64 <= len &&
                src[pos + 1] == 'E' && src[pos + 2] == 'L' && src[pos + 3] == 'F' &&
                src[pos + 4] == 2 && src[pos + 5] == 1) { // ELF64 LE
                uint16_t e_machine = 0, e_phentsize = 0, e_phnum = 0;
                uint64_t e_phoff = 0;
                std::memcpy(&e_machine, src + pos + 18, 2);
                std::memcpy(&e_phoff, src + pos + 32, 8);
                std::memcpy(&e_phentsize, src + pos + 54, 2);
                std::memcpy(&e_phnum, src + pos + 56, 2);
                uint8_t arch = (e_machine == 183) ? 1 : ((e_machine == 62) ? 2 : 0);
                if (arch != 0 && e_phnum > 0 && e_phnum <= 128 && e_phentsize >= 56 &&
                    e_phoff >= 64 && pos + e_phoff + static_cast<size_t>(e_phnum) * e_phentsize <= len) {
                    size_t max_seg_end = pos + 64;
                    for (uint16_t pi = 0; pi < e_phnum; ++pi) {
                        size_t ph = pos + static_cast<size_t>(e_phoff) + static_cast<size_t>(pi) * e_phentsize;
                        uint32_t p_type = 0, p_flags = 0;
                        uint64_t p_offset = 0, p_filesz = 0;
                        std::memcpy(&p_type, src + ph, 4);
                        std::memcpy(&p_flags, src + ph + 4, 4);
                        std::memcpy(&p_offset, src + ph + 8, 8);
                        std::memcpy(&p_filesz, src + ph + 32, 8);
                        if (p_type == 1 && (p_flags & 1) != 0 && p_filesz >= 64 &&
                            p_offset >= 64 && pos + p_offset + p_filesz <= len) {
                            uint64_t abs_off = static_cast<uint64_t>(pos + p_offset);
                            uint64_t fsz = (arch == 1) ? (p_filesz & ~3ULL) : p_filesz;
                            if (sec_ranges.empty() || abs_off >= sec_ranges.back().off + sec_ranges.back().sz) {
                                sec_ranges.push_back(MachoSecRange{abs_off, fsz, arch});
                                if (abs_off + p_filesz > max_seg_end) {
                                    max_seg_end = static_cast<size_t>(abs_off + p_filesz);
                                }
                            }
                        }
                    }
                    if (max_seg_end > pos + 64) {
                        p = max_seg_end;
                        continue;
                    }
                }
            }
            if (src[pos] == 0xCF && pos + 32 <= len &&
                src[pos + 1] == 0xFA && src[pos + 2] == 0xED && src[pos + 3] == 0xFE) {
                uint32_t cputype = 0, ncmds = 0, sizeofcmds = 0;
                std::memcpy(&cputype, src + pos + 4, 4);
                std::memcpy(&ncmds, src + pos + 16, 4);
                std::memcpy(&sizeofcmds, src + pos + 20, 4);
                uint8_t arch = (cputype == 0x0100000Cu) ? 1 : ((cputype == 0x01000007u) ? 2 : 0);
                if (arch != 0 && ncmds > 0 && ncmds <= 4096 &&
                    sizeofcmds >= 8 && pos + 32 + sizeofcmds <= len) {
                    size_t cmd_off = pos + 32;
                    size_t cmd_end = cmd_off + sizeofcmds;
                    size_t max_sec_end = pos + 32 + sizeofcmds;
                    for (uint32_t ci = 0; ci < ncmds && cmd_off + 8 <= cmd_end; ++ci) {
                        uint32_t cmd = 0, cmdsize = 0;
                        std::memcpy(&cmd, src + cmd_off, 4);
                        std::memcpy(&cmdsize, src + cmd_off + 4, 4);
                        if (cmdsize < 8 || cmd_off + cmdsize > cmd_end) break;
                        if (cmd == 0x19u && cmdsize >= 72) { // LC_SEGMENT_64
                            uint32_t nsects = 0;
                            std::memcpy(&nsects, src + cmd_off + 64, 4);
                            if (72ULL + static_cast<uint64_t>(nsects) * 80ULL <= cmdsize) {
                                for (uint32_t si = 0; si < nsects; ++si) {
                                    size_t sh = cmd_off + 72 + static_cast<size_t>(si) * 80;
                                    if (std::memcmp(src + sh, "__text\0", 7) == 0 &&
                                        std::memcmp(src + sh + 16, "__TEXT\0", 7) == 0) {
                                        uint64_t sec_sz = 0;
                                        uint32_t sec_off = 0;
                                        std::memcpy(&sec_sz, src + sh + 40, 8);
                                        std::memcpy(&sec_off, src + sh + 48, 4);
                                        if (sec_sz >= 64 && sec_off >= 32 + sizeofcmds &&
                                            pos + sec_off + sec_sz <= len) {
                                            uint64_t abs_off = static_cast<uint64_t>(pos + sec_off);
                                            uint64_t fsz = (arch == 1) ? (sec_sz & ~3ULL) : sec_sz;
                                            if (sec_ranges.empty() || abs_off >= sec_ranges.back().off + sec_ranges.back().sz) {
                                                sec_ranges.push_back(MachoSecRange{abs_off, fsz, arch});
                                                if (abs_off + sec_sz > max_sec_end) {
                                                    max_sec_end = static_cast<size_t>(abs_off + sec_sz);
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                        cmd_off += cmdsize;
                    }
                    if (max_sec_end > pos + 32) {
                        p = max_sec_end;
                        continue;
                    }
                }
            }
            p = pos + 1;
        }
        if (!sec_ranges.empty()) {
            ByteVec filtered(src, src + len);
            for (const auto& r : sec_ranges) {
                if (r.arch == 1 && r.sz >= 4) {
                    z7_BranchConv_ARM64_Enc(filtered.data() + r.off, static_cast<SizeT>(r.sz), 0);
                } else if (r.arch == 2 && r.sz >= 16) {
                    UInt32 st = Z7_BRANCH_CONV_ST_X86_STATE_INIT_VAL;
                    z7_BranchConvSt_X86_Enc(filtered.data() + r.off, static_cast<SizeT>(r.sz), 0, &st);
                }
            }
            ByteVec inner;
            size_t far_d = (g_comp_level >= 9) ? (32ULL * 1024 * 1024) : (4ULL * 1024 * 1024);
            size_t min_loc = (g_comp_level >= 9) ? 192 : 64;
            auto lrm_res = precond::lrm_encode(filtered.data(), filtered.size(), (g_comp_level >= 9) ? 3 : ((g_comp_level >= 7) ? 1 : 0), far_d, min_loc);
            if (lrm_res.applied) {
                ByteVec tok_comp;
                {
                    SuppressProgress sp;
                    tok_comp = bsc_parallel_compress(lrm_res.tokens.data(), lrm_res.tokens.size(), 3, 15, 72, 0);
                }
                int max_t = std::max(1, omp_get_max_threads());
                int lit_chunks = (g_comp_level >= 9)
                    ? std::max(1, (int)((lrm_res.literals.size() + 134217727) / 134217728))
                    : ((g_comp_level >= 7)
                        ? std::min(6, std::max(2, (int)((lrm_res.literals.size() + 25165823) / 25165824)))
                        : ((g_comp_level >= 5)
                            ? std::min(4, std::max(2, (int)((lrm_res.literals.size() + 33554431) / 33554432)))
                            : std::min(max_t, std::max(6, (int)((lrm_res.literals.size() + 4194303) / 4194304)))));
                int lit_dict = (g_comp_level <= 4) ? std::min(dict_mb, 8) : ((g_comp_level == 5) ? std::max(dict_mb, 64) : dict_mb);
                int lit_fb   = (g_comp_level <= 4) ? std::min(fb, 32) : ((g_comp_level == 5) ? std::max(fb, 96) : fb);
                int lit_mc   = (g_comp_level <= 4) ? std::min(mc, 16) : ((g_comp_level == 5) ? std::max(mc, 32) : mc);
                int lit_lvl  = (g_comp_level <= 4) ? std::min(level, 5) : level;
                double lit_scale = lrm_res.literals.empty()
                    ? prog_scale
                    : (prog_scale * static_cast<double>(len) / static_cast<double>(lrm_res.literals.size()));
                ByteVec lit_comp = lzma2_parallel_compress(
                    lrm_res.literals.data(), lrm_res.literals.size(),
                    lit_dict, lit_fb, lit_mc, lit_chunks, lc, lp, pb, lit_lvl, lit_scale);

                inner.reserve(24 + tok_comp.size() + lit_comp.size());
                inner.insert(inner.end(), {'P', 'L', 'R', 'M'});
                uint64_t orig_sz = static_cast<uint64_t>(len);
                inner.insert(inner.end(), reinterpret_cast<const uint8_t*>(&orig_sz), reinterpret_cast<const uint8_t*>(&orig_sz) + 8);
                uint32_t tok_sz = static_cast<uint32_t>(tok_comp.size());
                inner.insert(inner.end(), reinterpret_cast<const uint8_t*>(&tok_sz), reinterpret_cast<const uint8_t*>(&tok_sz) + 4);
                inner.insert(inner.end(), tok_comp.begin(), tok_comp.end());
                uint64_t lit_sz = static_cast<uint64_t>(lrm_res.literals.size());
                inner.insert(inner.end(), reinterpret_cast<const uint8_t*>(&lit_sz), reinterpret_cast<const uint8_t*>(&lit_sz) + 8);
                inner.insert(inner.end(), lit_comp.begin(), lit_comp.end());
            } else {
                inner = lzma2_parallel_compress(filtered.data(), filtered.size(), dict_mb, fb, mc, 0, lc, lp, pb, level, prog_scale);
            }
            uint32_t n_sec = static_cast<uint32_t>(sec_ranges.size());
            ByteVec pmch;
            pmch.reserve(8 + static_cast<size_t>(n_sec) * 17 + inner.size());
            pmch.insert(pmch.end(), {'P', 'M', 'C', 'H'});
            pmch.insert(pmch.end(), reinterpret_cast<const uint8_t*>(&n_sec), reinterpret_cast<const uint8_t*>(&n_sec) + 4);
            for (const auto& r : sec_ranges) {
                pmch.insert(pmch.end(), reinterpret_cast<const uint8_t*>(&r.off), reinterpret_cast<const uint8_t*>(&r.off) + 8);
                pmch.insert(pmch.end(), reinterpret_cast<const uint8_t*>(&r.sz), reinterpret_cast<const uint8_t*>(&r.sz) + 8);
                pmch.push_back(r.arch);
            }
            pmch.insert(pmch.end(), inner.begin(), inner.end());
            return pmch;
        }
    }
    if (!g_fast_mode && len >= 4ULL * 1024 * 1024) {
        size_t far_d = (g_comp_level >= 9) ? (32ULL * 1024 * 1024) : (4ULL * 1024 * 1024);
        size_t min_loc = (g_comp_level >= 9) ? 192 : 64;
        auto lrm_res = precond::lrm_encode(src, len, (g_comp_level >= 9) ? 3 : ((g_comp_level >= 7) ? 1 : 0), far_d, min_loc);
        if (lrm_res.applied) {
            ByteVec tok_comp;
            {
                SuppressProgress sp;
                tok_comp = bsc_parallel_compress(lrm_res.tokens.data(), lrm_res.tokens.size(), 3, 15, 72, 0);
            }
            int max_t = std::max(1, omp_get_max_threads());
            int lit_chunks = (g_comp_level >= 9)
                ? std::max(1, (int)((lrm_res.literals.size() + 134217727) / 134217728))
                : ((g_comp_level >= 7)
                    ? std::min(6, std::max(2, (int)((lrm_res.literals.size() + 25165823) / 25165824)))
                    : ((g_comp_level >= 5)
                        ? std::min(4, std::max(2, (int)((lrm_res.literals.size() + 33554431) / 33554432)))
                        : std::min(max_t, std::max(6, (int)((lrm_res.literals.size() + 4194303) / 4194304)))));
            int lit_dict = (g_comp_level <= 4) ? std::min(dict_mb, 8) : ((g_comp_level == 5) ? std::max(dict_mb, 64) : dict_mb);
            int lit_fb   = (g_comp_level <= 4) ? std::min(fb, 32) : ((g_comp_level == 5) ? std::max(fb, 96) : fb);
            int lit_mc   = (g_comp_level <= 4) ? std::min(mc, 16) : ((g_comp_level == 5) ? std::max(mc, 32) : mc);
            int lit_lvl  = (g_comp_level <= 4) ? std::min(level, 5) : level;
            double lit_scale = lrm_res.literals.empty()
                ? prog_scale
                : (prog_scale * static_cast<double>(len) / static_cast<double>(lrm_res.literals.size()));
            ByteVec lit_comp = lzma2_parallel_compress(
                lrm_res.literals.data(), lrm_res.literals.size(),
                lit_dict, lit_fb, lit_mc, lit_chunks, lc, lp, pb, lit_lvl, lit_scale);

            ByteVec container;
            container.reserve(24 + tok_comp.size() + lit_comp.size());
            container.insert(container.end(), {'P', 'L', 'R', 'M'});
            uint64_t orig_sz = static_cast<uint64_t>(len);
            container.insert(container.end(), reinterpret_cast<const uint8_t*>(&orig_sz), reinterpret_cast<const uint8_t*>(&orig_sz) + 8);
            uint32_t tok_sz = static_cast<uint32_t>(tok_comp.size());
            container.insert(container.end(), reinterpret_cast<const uint8_t*>(&tok_sz), reinterpret_cast<const uint8_t*>(&tok_sz) + 4);
            container.insert(container.end(), tok_comp.begin(), tok_comp.end());
            uint64_t lit_sz = static_cast<uint64_t>(lrm_res.literals.size());
            container.insert(container.end(), reinterpret_cast<const uint8_t*>(&lit_sz), reinterpret_cast<const uint8_t*>(&lit_sz) + 8);
            container.insert(container.end(), lit_comp.begin(), lit_comp.end());
            return container;
        }
    }
    return lzma2_parallel_compress(src, len, dict_mb, fb, mc, 0, lc, lp, pb, level, prog_scale);
}

inline ByteVec macho_lrm_lzma2_decompress(const uint8_t* src, size_t len) {
    if (len < 8 || std::memcmp(src, "PMCH", 4) != 0) {
        throw std::runtime_error("Invalid PMCH container");
    }
    uint32_t n_sec = 0;
    std::memcpy(&n_sec, src + 4, 4);
    size_t hdr_sz = 8ULL + static_cast<size_t>(n_sec) * 17ULL;
    if (hdr_sz > len) {
        throw std::runtime_error("Invalid PMCH header truncated");
    }
    ByteVec out = lzma_auto_decompress(src + hdr_sz, len - hdr_sz);
    for (uint32_t i = 0; i < n_sec; ++i) {
        size_t rec = 8 + static_cast<size_t>(i) * 17;
        uint64_t off = 0, sz = 0;
        std::memcpy(&off, src + rec, 8);
        std::memcpy(&sz, src + rec + 8, 8);
        uint8_t arch = src[rec + 16];
        if (off + sz > out.size()) {
            throw std::runtime_error("Invalid PMCH section range");
        }
        if (arch == 1 && sz >= 4) {
            z7_BranchConv_ARM64_Dec(out.data() + off, static_cast<SizeT>(sz), 0);
        } else if (arch == 2 && sz >= 16) {
            UInt32 st = Z7_BRANCH_CONV_ST_X86_STATE_INIT_VAL;
            z7_BranchConvSt_X86_Dec(out.data() + off, static_cast<SizeT>(sz), 0, &st);
        }
    }
    return out;
}

inline ByteVec lrm_lzma2_decompress(const uint8_t* src, size_t len) {
    if (len < 24 || std::memcmp(src, "PLRM", 4) != 0) {
        throw std::runtime_error("Invalid PLRM container");
    }
    uint64_t orig_size = 0;
    std::memcpy(&orig_size, src + 4, 8);
    uint32_t tok_sz = 0;
    std::memcpy(&tok_sz, src + 12, 4);
    if (16ULL + tok_sz + 8ULL > len) {
        throw std::runtime_error("Invalid PLRM container truncated");
    }
    ByteVec tokens = bsc_parallel_decompress(src + 16, tok_sz);
    uint64_t lit_orig = 0;
    std::memcpy(&lit_orig, src + 16 + tok_sz, 8);
    ByteVec literals = lzma_auto_decompress(src + 24 + tok_sz, len - 24 - tok_sz);
    if (literals.size() != lit_orig) {
        throw std::runtime_error("PLRM literal size mismatch");
    }
    ByteVec out;
    if (!precond::lrm_decode(literals.data(), literals.size(), tokens.data(), tokens.size(), orig_size, out)) {
        throw std::runtime_error("PLRM lrm_decode failed");
    }
    return out;
}

// Router: decide whether LZMA2 beats BWT for a binary stream (multi-slice probe).
inline bool should_use_lzma2_for_binary(const uint8_t* data, size_t len) {
    if (g_fast_mode || !data || len < 2ULL * 1024 * 1024) return false;
    const size_t slice = 128 * 1024;
    const size_t positions[3] = { len * 15 / 100, len * 50 / 100, len * 85 / 100 };
    size_t printable = 0, checked = 0;
    for (int s = 0; s < 3; ++s) {
        size_t off = std::min(positions[s], len - slice);
        const uint8_t* p = data + off;
        for (size_t i = 0; i < 4096; ++i) {
            uint8_t c = p[i];
            if ((c >= 32 && c <= 126) || c == 9 || c == 10 || c == 13) ++printable;
        }
        checked += 4096;
    }
    if ((double)printable / (double)checked >= 0.78) return false;

    const size_t sample_len = slice * 3;
    std::unique_ptr<uint8_t[]> sample(new uint8_t[sample_len]);
    for (int s = 0; s < 3; ++s) {
        size_t off = std::min(positions[s], len - slice);
        std::memcpy(sample.get() + s * slice, data + off, slice);
    }

    bsc_init_once();
    size_t bsc_sz = sample_len;
    {
        std::unique_ptr<uint8_t[]> out(new uint8_t[sample_len + LIBBSC_HEADER_SIZE + 64]);
        int r = bsc_compress(sample.get(), out.get(), (int)sample_len, 15, 72, LIBBSC_BLOCKSORTER_BWT, 3, LIBBSC_FEATURE_FASTMODE);
        if (r > 0) bsc_sz = (size_t)r;
    }
    size_t lz_sz = sample_len;
    {
        CLzmaEncProps lp;
        LzmaEncProps_Init(&lp);
        lp.level = 3;
        lp.dictSize = 256 * 1024;
        lp.fb = 32;
        lp.mc = 16;
        lp.btMode = 1;
        lp.numHashBytes = 4;
        lp.numThreads = 1;
        lp.writeEndMark = 0;
        SizeT dl = sample_len + 128;
        std::unique_ptr<uint8_t[]> out(new uint8_t[dl]);
        Byte pe[5];
        SizeT ps = 5;
        SRes r = LzmaEncode(out.get(), &dl, sample.get(), (SizeT)sample_len, &lp, pe, &ps, 0, NULL, &g_Alloc, &g_Alloc);
        if (r == SZ_OK) lz_sz = (size_t)dl;
    }
    const size_t incompressible = sample_len * 985 / 1000;
    if (bsc_sz >= incompressible && lz_sz >= incompressible) return true;
    if (len >= 16ULL * 1024 * 1024) return lz_sz * 100ULL <= bsc_sz * (g_ultra_mode ? 108ULL : 102ULL);
    return lz_sz * 100ULL <= bsc_sz * 98ULL;
}

// X86 Executable Compression / Decompression via BCJ2 + LZMA/BSC
inline ByteVec exec_x86_compress(const uint8_t* src, size_t len, int dict_mb = 4, int fb = 64, int mc = 16, double prog_scale = 1.0) {
    if (len < 64) {
        return lzma_parallel_compress(src, len, dict_mb, fb, mc, 0, 3, 0, 2, 5, prog_scale);
    }

    CBcj2Enc enc;
    Bcj2Enc_Init(&enc);
    Bcj2Enc_SET_FileSize(&enc, len);
    enc.relatLimit = BCJ2_ENC_RELAT_LIMIT_DEFAULT;
    enc.fileIp64 = 0;
    enc.ip64 = 0;
    enc.src = src;
    enc.srcLim = src + len;

    size_t buf_main_size = len + 64;
    size_t buf_call_size = len + 64;
    size_t buf_jump_size = len + 64;
    size_t buf_rc_size   = len / 2 + 1024;

    std::unique_ptr<Byte[]> buf_main(new Byte[buf_main_size]);
    std::unique_ptr<Byte[]> buf_call(new Byte[buf_call_size]);
    std::unique_ptr<Byte[]> buf_jump(new Byte[buf_jump_size]);
    std::unique_ptr<Byte[]> buf_rc(new Byte[buf_rc_size]);

    enc.bufs[BCJ2_STREAM_MAIN] = buf_main.get();
    enc.lims[BCJ2_STREAM_MAIN] = buf_main.get() + buf_main_size;
    enc.bufs[BCJ2_STREAM_CALL] = buf_call.get();
    enc.lims[BCJ2_STREAM_CALL] = buf_call.get() + buf_call_size;
    enc.bufs[BCJ2_STREAM_JUMP] = buf_jump.get();
    enc.lims[BCJ2_STREAM_JUMP] = buf_jump.get() + buf_jump_size;
    enc.bufs[BCJ2_STREAM_RC]   = buf_rc.get();
    enc.lims[BCJ2_STREAM_RC]   = buf_rc.get() + buf_rc_size;

    enc.finishMode = BCJ2_ENC_FINISH_MODE_END_STREAM;
    Bcj2Enc_Encode(&enc);
    if (enc.state != BCJ2_ENC_STATE_FINISHED) {
        return lzma_parallel_compress(src, len, dict_mb, fb, mc, 0, 3, 0, 2, 5, prog_scale);
    }

    size_t out_main_len = (size_t)(enc.bufs[BCJ2_STREAM_MAIN] - buf_main.get());
    size_t out_call_len = (size_t)(enc.bufs[BCJ2_STREAM_CALL] - buf_call.get());
    size_t out_jump_len = (size_t)(enc.bufs[BCJ2_STREAM_JUMP] - buf_jump.get());
    size_t out_rc_len   = (size_t)(enc.bufs[BCJ2_STREAM_RC]   - buf_rc.get());

    double main_scale = (out_main_len > 0)
        ? (prog_scale * static_cast<double>(len) / static_cast<double>(out_main_len))
        : prog_scale;
    ByteVec c_main = g_fast_mode
        ? lzma_parallel_compress(buf_main.get(), out_main_len, 8, 64, 16, (out_main_len > 2 * 1024 * 1024 ? 4 : 2), 3, 0, 2, 5, main_scale)
        : lrm_lzma2_compress(buf_main.get(), out_main_len, 16, 64, 24, 3, 0, 2, 6, main_scale);
    ByteVec c_call, c_jump, c_rc;
    {
        SuppressProgress sp;
        c_call = bsc_parallel_compress(buf_call.get(), out_call_len, 3, 0, 0, 1);
        c_jump = bsc_parallel_compress(buf_jump.get(), out_jump_len, 3, 0, 0, 1);
        c_rc   = bsc_parallel_compress(buf_rc.get(), out_rc_len, 3, 0, 0, 1);
    }

    size_t header_size = 4 + 8 + 16;
    size_t total_size = header_size + c_main.size() + c_call.size() + c_jump.size() + c_rc.size();

    ByteVec container(total_size);
    uint8_t* ptr = container.data();
    std::memcpy(ptr, "P86C", 4); ptr += 4;
    uint64_t orig_sz = (uint64_t)len;
    std::memcpy(ptr, &orig_sz, 8); ptr += 8;

    uint32_t szs[4] = { (uint32_t)c_main.size(), (uint32_t)c_call.size(), (uint32_t)c_jump.size(), (uint32_t)c_rc.size() };
    std::memcpy(ptr, szs, 16); ptr += 16;

    std::memcpy(ptr, c_main.data(), c_main.size()); ptr += c_main.size();
    std::memcpy(ptr, c_call.data(), c_call.size()); ptr += c_call.size();
    std::memcpy(ptr, c_jump.data(), c_jump.size()); ptr += c_jump.size();
    std::memcpy(ptr, c_rc.data(), c_rc.size()); ptr += c_rc.size();

    return container;
}

inline ByteVec exec_x86_decompress(const uint8_t* src, size_t len) {
    if (len >= 4 && std::memcmp(src, "PLZM", 4) == 0) {
        return lzma_parallel_decompress(src, len);
    }
    if (len >= 4 && std::memcmp(src, "PLZ2", 4) == 0) {
        return lzma2_parallel_decompress(src, len);
    }
    if (len < 28) throw std::runtime_error("Invalid X86 container: too short");
    if (std::memcmp(src, "P86C", 4) != 0) throw std::runtime_error("Invalid X86 container magic");

    uint64_t orig_size = 0;
    std::memcpy(&orig_size, src + 4, 8);

    uint32_t szs[4];
    std::memcpy(szs, src + 12, 16);

    size_t header_size = 28;
    if (header_size + szs[0] + szs[1] + szs[2] + szs[3] > len) throw std::runtime_error("Invalid X86 container truncated");

    const uint8_t* ptr = src + header_size;
    ByteVec main_buf = lzma_auto_decompress(ptr, szs[0]); ptr += szs[0];
    ByteVec call_buf, jump_buf, rc_buf;
    if (!omp_in_parallel()) {
        #pragma omp parallel sections
        {
            #pragma omp section
            { call_buf = bsc_parallel_decompress(ptr, szs[1]); }
            #pragma omp section
            { jump_buf = bsc_parallel_decompress(ptr + szs[1], szs[2]); }
            #pragma omp section
            { rc_buf   = bsc_parallel_decompress(ptr + szs[1] + szs[2], szs[3]); }
        }
    } else {
        call_buf = bsc_parallel_decompress(ptr, szs[1]);
        jump_buf = bsc_parallel_decompress(ptr + szs[1], szs[2]);
        rc_buf   = bsc_parallel_decompress(ptr + szs[1] + szs[2], szs[3]);
    }

    ByteVec out(orig_size);
    CBcj2Dec dec;
    Bcj2Dec_Init(&dec);
    dec.bufs[BCJ2_STREAM_MAIN] = main_buf.data();
    dec.lims[BCJ2_STREAM_MAIN] = main_buf.data() + main_buf.size();
    dec.bufs[BCJ2_STREAM_CALL] = call_buf.data();
    dec.lims[BCJ2_STREAM_CALL] = call_buf.data() + call_buf.size();
    dec.bufs[BCJ2_STREAM_JUMP] = jump_buf.data();
    dec.lims[BCJ2_STREAM_JUMP] = jump_buf.data() + jump_buf.size();
    dec.bufs[BCJ2_STREAM_RC]   = rc_buf.data();
    dec.lims[BCJ2_STREAM_RC]   = rc_buf.data() + rc_buf.size();
    dec.dest = out.data();
    dec.destLim = out.data() + orig_size;

    SRes d_res = Bcj2Dec_Decode(&dec);
    if (d_res != SZ_OK) throw std::runtime_error("Bcj2Dec_Decode failed");

    return out;
}

// SPARC Executable Compression / Decompression via Bra SPARC + LZMA
inline ByteVec exec_sparc_compress(const uint8_t* src, size_t len, int dict_mb = 4, int fb = 64, int mc = 16, double prog_scale = 1.0) {
    ByteVec buf(src, src + len);
    size_t aligned_len = len & ~size_t(3);
    if (aligned_len > 0) {
        z7_BranchConv_SPARC_Enc(buf.data(), aligned_len, 0);
    }
    ByteVec lz_comp = lzma_parallel_compress(buf.data(), buf.size(), dict_mb, fb, mc, 0, 3, 0, 2, 5, prog_scale);

    ByteVec container(4 + lz_comp.size());
    std::memcpy(container.data(), "PSPC", 4);
    std::memcpy(container.data() + 4, lz_comp.data(), lz_comp.size());
    return container;
}

inline ByteVec exec_arm64_compress(const uint8_t* src, size_t len, int dict_mb = 16, int fb = 64, int mc = 24, double prog_scale = 1.0) {
    ByteVec buf(src, src + len);
    size_t aligned_len = len & ~size_t(3);
    if (aligned_len > 0) {
        z7_BranchConv_ARM64_Enc(buf.data(), aligned_len, 0);
    }
    ByteVec lz_comp = lrm_lzma2_compress(buf.data(), buf.size(), dict_mb, fb, mc, 3, 0, 2, 6, prog_scale);

    ByteVec container(4 + lz_comp.size());
    std::memcpy(container.data(), "PA64", 4);
    std::memcpy(container.data() + 4, lz_comp.data(), lz_comp.size());
    return container;
}

inline ByteVec exec_sparc_decompress(const uint8_t* src, size_t len) {
    if (len < 4) throw std::runtime_error("Invalid SPARC/ARM64 container: too short");
    if (std::memcmp(src, "PA64", 4) == 0) {
        ByteVec out = lzma_auto_decompress(src + 4, len - 4);
        size_t aligned_len = out.size() & ~size_t(3);
        if (aligned_len > 0) {
            z7_BranchConv_ARM64_Dec(out.data(), aligned_len, 0);
        }
        return out;
    }
    if (std::memcmp(src, "PSPC", 4) != 0) throw std::runtime_error("Invalid SPARC container magic");

    ByteVec out = lzma_parallel_decompress(src + 4, len - 4);
    size_t aligned_len = out.size() & ~size_t(3);
    if (aligned_len > 0) {
        z7_BranchConv_SPARC_Dec(out.data(), aligned_len, 0);
    }
    return out;
}

} // namespace codecs
} // namespace prism

#endif // PRISM_CODECS_HPP
