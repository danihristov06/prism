#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <memory>
#include <unordered_map>
#include <vector>
#include <omp.h>
#ifdef __AVX2__
#include <immintrin.h>
#endif
#include "zlib.h"
#include "prism_buf.hpp"

namespace prism::precond {

struct CdcResult {
    bool applied = false;
    uint64_t orig_size = 0;
    uint32_t num_unique = 0;
    uint32_t num_refs = 0;
    ByteVec unique_payload;
    ByteVec chunk_table;
};

struct LrmResult {
    bool applied = false;
    uint64_t orig_size = 0;
    ByteVec literals;
    ByteVec tokens;
};

struct StrideResult {
    bool applied = false;
    uint8_t stride = 1;
    uint64_t delta_mask = 0;
    uint64_t orig_size = 0;
    ByteVec data;
};

struct ZipPrecompResult {
    bool applied = false;
    uint32_t num_entries = 0;
    uint64_t orig_size = 0;
    ByteVec transformed;
    ByteVec patch_meta;
};

enum TarBucketId : uint8_t {
    BUCKET_TEXT = 0,
    BUCKET_STRIDE = 1,
    BUCKET_EXEC_X86 = 2,
    BUCKET_EXEC_SPARC = 3,
    BUCKET_BINARY = 4,
    BUCKET_TAR_ARCHIVE = 5,
    BUCKET_COUNT = 6
};

struct TarDemuxResult {
    bool applied = false;
    uint64_t orig_size = 0;
    ByteVec meta_stream;
    ByteVec buckets[BUCKET_COUNT];
};

enum ExecType : uint8_t {
    EXEC_NONE = 0,
    EXEC_X86 = 1,
    EXEC_SPARC = 2,
    EXEC_ARM64 = 3
};

// Helper: read/write little/big endian
inline uint16_t read_le16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}
inline uint16_t read_be16(const uint8_t* p) {
    return (static_cast<uint16_t>(p[0]) << 8) | static_cast<uint16_t>(p[1]);
}
inline uint32_t read_le32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}
inline uint64_t read_le64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) {
        v = (v << 8) | p[i];
    }
    return v;
}
inline void write_le64(uint8_t* p, uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        p[i] = static_cast<uint8_t>(v & 0xFF);
        v >>= 8;
    }
}

inline size_t count_printable_ascii(const uint8_t* data, size_t len) {
    size_t count = 0;
    size_t i = 0;
#ifdef __AVX2__
    const __m256i v31  = _mm256_set1_epi8(31);
    const __m256i v127 = _mm256_set1_epi8(127);
    const __m256i v9   = _mm256_set1_epi8(9);
    const __m256i v10  = _mm256_set1_epi8(10);
    const __m256i v13  = _mm256_set1_epi8(13);
    for (; i + 32 <= len; i += 32) {
        __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(data + i));
        __m256i ge32 = _mm256_cmpgt_epi8(v, v31);
        __m256i le126 = _mm256_cmpgt_epi8(v127, v);
        __m256i mask = _mm256_and_si256(ge32, le126);
        mask = _mm256_or_si256(mask, _mm256_cmpeq_epi8(v, v9));
        mask = _mm256_or_si256(mask, _mm256_cmpeq_epi8(v, v10));
        mask = _mm256_or_si256(mask, _mm256_cmpeq_epi8(v, v13));
        count += static_cast<size_t>(__builtin_popcount(static_cast<uint32_t>(_mm256_movemask_epi8(mask))));
    }
#endif
    for (; i < len; ++i) {
        uint8_t c = data[i];
        if ((c >= 32 && c <= 126) || c == 9 || c == 10 || c == 13) {
            count++;
        }
    }
    return count;
}

inline ExecType detect_executable(const uint8_t* data, size_t len) {
    if (!data || len < 64) return EXEC_NONE;

    // Check Windows PE
    if (data[0] == 'M' && data[1] == 'Z') {
        if (len >= 64) {
            uint32_t e_lfanew = read_le32(data + 60);
            if (e_lfanew > 0 && e_lfanew + 6 <= len) {
                if (data[e_lfanew] == 'P' && data[e_lfanew + 1] == 'E' &&
                    data[e_lfanew + 2] == 0 && data[e_lfanew + 3] == 0) {
                    if (e_lfanew + 6 <= len) {
                        uint16_t machine = read_le16(data + e_lfanew + 4);
                        if (machine == 0x014c || machine == 0x8664) {
                            if (e_lfanew + 22 <= len) {
                                uint16_t opt_sz = read_le16(data + e_lfanew + 20);
                                if (opt_sz >= 8 && e_lfanew + 32 <= len) {
                                    uint32_t size_of_code = read_le32(data + e_lfanew + 28);
                                    if (size_of_code < 16384) return EXEC_NONE;
                                }
                            }
                            return EXEC_X86;
                        }
                    }
                }
            }
        }
    }

    // Check ELF
    if (len >= 52 && data[0] == 0x7F && data[1] == 'E' && data[2] == 'L' && data[3] == 'F') {
        uint8_t ei_data = data[5];  // 1 = LE, 2 = BE
        if (len >= 20) {
            uint16_t e_machine = (ei_data == 2) ? read_be16(data + 18) : read_le16(data + 18);
            if (ei_data == 2) {
                // Big-endian SPARC
                if (e_machine == 2 || e_machine == 43 || e_machine == 18) {
                    return EXEC_SPARC;
                }
            } else if (ei_data == 1) {
                // Little-endian x86 / x64 / ARM64
                if (e_machine == 3 || e_machine == 62) {
                    return EXEC_X86;
                }
                if (e_machine == 183) {
                    return EXEC_ARM64;
                }
            }
        }
    }

    return EXEC_NONE;
}

// SplitMix64 PRNG for deterministic Gear table generation
inline uint64_t splitmix64_next(uint64_t& state) {
    state += 0x9e3779b97f4a7c15ULL;
    uint64_t z = state;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

inline void generate_gear_table(uint64_t gear[256]) {
    uint64_t seed = 0x12345678abcdef00ULL;
    for (int i = 0; i < 256; ++i) {
        gear[i] = splitmix64_next(seed);
    }
}

// Fast 64-bit hash for CDC chunk deduplication (8 bytes at a time)
inline uint64_t hash_bytes(const uint8_t* p, size_t len) {
    uint64_t h = 0x9e3779b97f4a7c15ULL ^ (static_cast<uint64_t>(len) * 0xbf58476d1ce4e5b9ULL);
    size_t i = 0;
    for (; i + 8 <= len; i += 8) {
        uint64_t w;
        std::memcpy(&w, p + i, 8);
        h ^= w * 0xbf58476d1ce4e5b9ULL;
        h = (h ^ (h >> 30)) * 0x94d049bb133111ebULL;
    }
    if (i < len) {
        uint64_t tail = 0;
        std::memcpy(&tail, p + i, len - i);
        h ^= tail * 0xbf58476d1ce4e5b9ULL;
        h = (h ^ (h >> 30)) * 0x94d049bb133111ebULL;
    }
    h ^= static_cast<uint64_t>(len);
    h = (h ^ (h >> 30)) * 0xbf58476d1ce4e5b9ULL;
    h = (h ^ (h >> 27)) * 0x94d049bb133111ebULL;
    return h ^ (h >> 31);
}

inline CdcResult cdc_encode(const uint8_t* data, size_t len) {
    CdcResult res;
    res.orig_size = len;
    if (!data || len < 2048) {
        res.applied = false;
        return res;
    }

    static const std::array<uint64_t, 256> gear = []() {
        std::array<uint64_t, 256> t{};
        generate_gear_table(t.data());
        return t;
    }();

    const size_t MIN_CHUNK = 512;
    const size_t MAX_CHUNK = 8192;
    const uint64_t TARGET_MASK = 0x7FF; // 2 KiB avg

    std::vector<uint32_t> chunk_lengths;
    std::vector<const uint8_t*> chunk_ptrs;
    const size_t est_chunks = len / 1536 + 64;
    chunk_lengths.reserve(est_chunks);
    chunk_ptrs.reserve(est_chunks);

    size_t start = 0;
    while (start < len) {
        size_t end = std::min(start + MIN_CHUNK, len);
        uint64_t h = 0;
        while (end < len && (end - start < MAX_CHUNK)) {
            h = (h << 1) + gear[data[end]];
            end++;
            if ((h & TARGET_MASK) == 0) {
                break;
            }
        }
        size_t chunk_len = end - start;
        chunk_lengths.push_back(static_cast<uint32_t>(chunk_len));
        chunk_ptrs.push_back(data + start);
        start = end;
    }

    struct UniqueChunkInfo {
        size_t payload_off;
        uint32_t len;
        uint32_t id;
        uint64_t hash;
        int32_t next;
    };

    const size_t num_chunks = chunk_lengths.size();
    const size_t min_ht = std::max<size_t>(64, num_chunks * 2);
    size_t ht_size = 64;
    while (ht_size < min_ht) {
        ht_size <<= 1;
    }
    const size_t ht_mask = ht_size - 1;

    std::vector<UniqueChunkInfo> unique_registry;
    unique_registry.reserve(num_chunks);
    std::vector<int32_t> buckets(ht_size, -1);

    res.unique_payload.clear();
    res.unique_payload.reserve(len);
    res.chunk_table.resize(num_chunks * 4);
    uint32_t next_unique_id = 0;

    for (size_t i = 0; i < num_chunks; ++i) {
        uint32_t clen = chunk_lengths[i];
        const uint8_t* cptr = chunk_ptrs[i];
        uint64_t h = hash_bytes(cptr, clen);
        size_t b = static_cast<size_t>(h) & ht_mask;

        bool duplicated = false;
        uint32_t dup_id = 0;

        for (int32_t idx = buckets[b]; idx != -1; idx = unique_registry[idx].next) {
            const auto& uinfo = unique_registry[idx];
            if (uinfo.hash == h && uinfo.len == clen &&
                std::memcmp(cptr, res.unique_payload.data() + uinfo.payload_off, clen) == 0) {
                duplicated = true;
                dup_id = uinfo.id;
                break;
            }
        }

        if (duplicated) {
            uint32_t tag = 0x80000000U | dup_id;
            std::memcpy(res.chunk_table.data() + i * 4, &tag, 4);
            res.num_refs++;
        } else {
            uint32_t new_id = next_unique_id++;
            size_t payload_off = res.unique_payload.size();
            res.unique_payload.insert(res.unique_payload.end(), cptr, cptr + clen);

            int32_t reg_idx = static_cast<int32_t>(unique_registry.size());
            unique_registry.push_back(UniqueChunkInfo{payload_off, clen, new_id, h, buckets[b]});
            buckets[b] = reg_idx;

            uint32_t tag = clen;
            std::memcpy(res.chunk_table.data() + i * 4, &tag, 4);
            res.num_unique++;
        }
    }

    size_t compressed_est = res.unique_payload.size() + res.chunk_table.size();
    res.applied = (compressed_est < len * 92 / 100);
    return res;
}

inline bool cdc_decode(const uint8_t* unique_payload, size_t up_len,
                const uint8_t* chunk_table, size_t ct_len,
                uint64_t orig_size, ByteVec& out) {
    const size_t out_size = static_cast<size_t>(orig_size);
    out.resize(out_size);

    std::vector<uint64_t> chunk_offsets;
    std::vector<uint32_t> chunk_lengths;
    chunk_offsets.reserve(ct_len / 4);
    chunk_lengths.reserve(ct_len / 4);

    size_t up_cursor = 0;
    size_t ct_cursor = 0;
    size_t out_cursor = 0;

    while (ct_cursor + 4 <= ct_len) {
        uint32_t tag = read_le32(chunk_table + ct_cursor);
        ct_cursor += 4;

        if ((tag & 0x80000000U) == 0) {
            uint32_t clen = tag;
            if (clen > up_len - up_cursor || clen > out_size - out_cursor) return false;
            chunk_offsets.push_back(up_cursor);
            chunk_lengths.push_back(clen);

            if (clen > 0) {
                std::memcpy(out.data() + out_cursor, unique_payload + up_cursor, clen);
            }
            up_cursor += clen;
            out_cursor += clen;
        } else {
            uint32_t ref_id = tag & 0x7FFFFFFF;
            if (ref_id >= chunk_offsets.size()) return false;
            uint64_t off = chunk_offsets[ref_id];
            uint32_t clen = chunk_lengths[ref_id];
            if (off > up_len || clen > up_len - off || clen > out_size - out_cursor) return false;

            if (clen > 0) {
                std::memcpy(out.data() + out_cursor, unique_payload + off, clen);
            }
            out_cursor += clen;
        }
    }

    return out_cursor == out_size;
}

inline void write_leb128(ByteVec& out, uint64_t v) {
    while (v >= 0x80) {
        out.push_back(static_cast<uint8_t>((v & 0x7F) | 0x80));
        v >>= 7;
    }
    out.push_back(static_cast<uint8_t>(v));
}

inline bool read_leb128(const uint8_t* buf, size_t len, size_t& pos, uint64_t& val) {
    if (pos < len) {
        uint8_t b0 = buf[pos];
        if (b0 < 0x80u) {
            val = b0;
            pos += 1;
            return true;
        }
        if (pos + 1 < len) {
            uint8_t b1 = buf[pos + 1];
            if (b1 < 0x80u) {
                val = static_cast<uint64_t>(b0 & 0x7Fu) | (static_cast<uint64_t>(b1) << 7);
                pos += 2;
                return true;
            }
        }
    }
    val = 0;
    unsigned shift = 0;
    while (pos < len) {
        uint8_t b = buf[pos++];
        val |= static_cast<uint64_t>(b & 0x7F) << shift;
        if ((b & 0x80) == 0) return true;
        shift += 7;
        if (shift >= 64) return false;
    }
    return false;
}

inline LrmResult lrm_encode(const uint8_t* data, size_t len, int mode = 0, size_t far_dist = 4ULL * 1024 * 1024, size_t min_match_local = 64) {
    LrmResult res;
    res.orig_size = len;
    if (!data || len < 4ULL * 1024 * 1024) {
        res.applied = false;
        return res;
    }

    static const std::array<uint64_t, 256> gear = []() {
        std::array<uint64_t, 256> t{};
        generate_gear_table(t.data());
        return t;
    }();

    const size_t HT_BITS = (mode == 3) ? 21 : ((mode == 2) ? 19 : 20);
    const size_t HT_SIZE = 1ULL << HT_BITS;
    const uint64_t HT_MASK = HT_SIZE - 1;
    const uint64_t SAMPLE_MASK = (mode == 1 || mode == 3) ? 0x0FULL : ((mode == 2) ? 0x3FULL : 0x1FULL);
    const size_t MIN_MATCH_LOCAL = min_match_local;
    const size_t MIN_MATCH_FAR = 32;
    const size_t MIN_MATCH = MIN_MATCH_FAR;
    const size_t FAR_DIST = far_dist;
    const size_t MAX_DIST = 512ULL * 1024 * 1024;
    const size_t MIN_DIST = 2048;

    struct LrmSlot {
        uint32_t p[4] = {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};
    };
    std::vector<LrmSlot> ht(HT_SIZE);
    res.literals.reserve(len / 2);
    res.tokens.reserve(len / 64);

    size_t anchor = 0;
    size_t pos = 0;
    uint64_t h = 0;

    // Warm up 32-byte rolling hash
    for (size_t i = 0; i < 32 && i < len; ++i) {
        h = (h << 1) + gear[data[i]];
    }
    pos = 32;

    while (pos + MIN_MATCH <= len) {
        h = (h << 1) + gear[data[pos]];
        if ((h & SAMPLE_MASK) == 0) {
            size_t slot_idx = static_cast<size_t>((h >> 5) & HT_MASK);
            LrmSlot& sl = ht[slot_idx];
            uint32_t cands[4] = { sl.p[0], sl.p[1], sl.p[2], sl.p[3] };
            sl.p[3] = sl.p[2];
            sl.p[2] = sl.p[1];
            sl.p[1] = sl.p[0];
            sl.p[0] = static_cast<uint32_t>(pos);

            size_t best_mlen = 0;
            size_t best_m_start = 0, best_p_start = 0, best_m_end = 0;

            const int max_ci = (mode >= 3) ? 4 : 2;
            for (int ci = 0; ci < max_ci; ++ci) {
                uint32_t prev_u32 = cands[ci];
                if (prev_u32 == 0xFFFFFFFFu) continue;
                size_t prev = static_cast<size_t>(prev_u32);
                size_t dist0 = pos - prev;
                if (prev >= pos || dist0 < MIN_DIST || dist0 > MAX_DIST ||
                    prev + 8 > len || pos + 8 > len ||
                    std::memcmp(data + prev, data + pos, 8) != 0) {
                    continue;
                }
                size_t m_start = pos;
                size_t p_start = prev;
                while (m_start > anchor && p_start > 0 && data[m_start - 1] == data[p_start - 1]) {
                    --m_start;
                    --p_start;
                }
                size_t m_end = pos + 8;
                size_t p_end = prev + 8;
#ifdef __AVX2__
                while (m_end + 32 <= len) {
                    __m256i va = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(data + p_end));
                    __m256i vb = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(data + m_end));
                    uint32_t eq = static_cast<uint32_t>(_mm256_movemask_epi8(_mm256_cmpeq_epi8(va, vb)));
                    if (eq != 0xFFFFFFFFu) {
                        size_t adv = static_cast<size_t>(__builtin_ctz(~eq));
                        m_end += adv;
                        p_end += adv;
                        goto match_done;
                    }
                    m_end += 32;
                    p_end += 32;
                }
#endif
                while (m_end + 8 <= len) {
                    uint64_t a, b;
                    std::memcpy(&a, data + p_end, 8);
                    std::memcpy(&b, data + m_end, 8);
                    if (a != b) {
                        size_t adv = static_cast<size_t>(__builtin_ctzll(a ^ b) >> 3);
                        m_end += adv;
                        p_end += adv;
                        break;
                    }
                    m_end += 8;
                    p_end += 8;
                }
                while (m_end < len && data[p_end] == data[m_end]) {
                    ++m_end;
                    ++p_end;
                }
#ifdef __AVX2__
                match_done:;
#endif
                size_t mlen = m_end - m_start;
                size_t req = (dist0 >= FAR_DIST) ? MIN_MATCH_FAR : MIN_MATCH_LOCAL;
                if (mlen >= req && mlen > best_mlen) {
                    best_mlen = mlen;
                    best_m_start = m_start;
                    best_p_start = p_start;
                    best_m_end = m_end;
                }
            }

            if (best_mlen > 0) {
                size_t lit_len = best_m_start - anchor;
                size_t dist = best_m_start - best_p_start;
                write_leb128(res.tokens, static_cast<uint64_t>(lit_len));
                write_leb128(res.tokens, static_cast<uint64_t>(dist));
                write_leb128(res.tokens, static_cast<uint64_t>(best_mlen));
                if (lit_len > 0) {
                    res.literals.insert(res.literals.end(), data + anchor, data + best_m_start);
                }
                anchor = best_m_end;
                pos = best_m_end;
                h = 0;
                size_t warm_start = (pos >= 32) ? (pos - 32) : 0;
                for (size_t wi = warm_start; wi < pos; ++wi) {
                    h = (h << 1) + gear[data[wi]];
                }
                continue;
            }
        }
        ++pos;
    }

    if (anchor < len) {
        res.literals.insert(res.literals.end(), data + anchor, data + len);
    }

    res.applied = (res.literals.size() + res.tokens.size() < (len * 96) / 100);
    return res;
}

inline bool lrm_decode(const uint8_t* literals, size_t lit_len,
                       const uint8_t* tokens, size_t tok_len,
                       uint64_t orig_size, ByteVec& out) {
    out.resize(static_cast<size_t>(orig_size));
    size_t lit_pos = 0;
    size_t tok_pos = 0;
    size_t out_pos = 0;

    while (tok_pos < tok_len) {
        uint64_t l_len = 0, dist = 0, m_len = 0;
        if (!read_leb128(tokens, tok_len, tok_pos, l_len)) return false;
        if (!read_leb128(tokens, tok_len, tok_pos, dist)) return false;
        if (!read_leb128(tokens, tok_len, tok_pos, m_len)) return false;

        if (l_len > lit_len - lit_pos || l_len > orig_size - out_pos) return false;
        if (l_len > 0) {
            std::memcpy(out.data() + out_pos, literals + lit_pos, static_cast<size_t>(l_len));
            lit_pos += static_cast<size_t>(l_len);
            out_pos += static_cast<size_t>(l_len);
        }

        if (dist == 0 || dist > out_pos || m_len > orig_size - out_pos) return false;
        if (m_len > 0) {
            if (dist >= m_len) {
                std::memcpy(out.data() + out_pos, out.data() + out_pos - static_cast<size_t>(dist), static_cast<size_t>(m_len));
            } else {
                uint8_t* dst = out.data() + out_pos;
                size_t d = static_cast<size_t>(dist);
                size_t ml = static_cast<size_t>(m_len);
                std::memcpy(dst, dst - d, d);
                size_t copied = d;
                while (copied < ml) {
                    size_t step = std::min(copied, ml - copied);
                    std::memcpy(dst + copied, dst, step);
                    copied += step;
                }
            }
            out_pos += static_cast<size_t>(m_len);
        }
    }

    size_t rem = static_cast<size_t>(orig_size) - out_pos;
    if (rem > lit_len - lit_pos) return false;
    if (rem > 0) {
        std::memcpy(out.data() + out_pos, literals + lit_pos, rem);
        lit_pos += rem;
        out_pos += rem;
    }

    return out_pos == orig_size && lit_pos == lit_len;
}

// Stride implementation with Fast 64-Bit Record Primary-Key Delta
inline uint8_t best_record64_stride(const uint8_t* data, size_t len) {
    if (!data || len < 256) return 0;
    const ByteVec strides = {16, 20, 24, 28, 32, 40, 48, 64};
    uint8_t best_s = 0;
    double max_min_gain = 0.4999; // threshold >= 0.50

    for (uint8_t S : strides) {
        if (len <= (size_t)S * 160) continue;
        size_t records = (len - S) / S;
        if (records < 128) continue;

        size_t win_starts[3] = {
            S + records / 4 * S,
            S + records / 2 * S,
            S + (records * 3) / 4 * S
        };
        for (int w = 0; w < 3; ++w) {
            if (win_starts[w] + 128 * S > len) {
                win_starts[w] = len > 128 * S ? len - 128 * S : S;
            }
        }

        double gains[3];
        bool valid = true;
        for (int w = 0; w < 3; ++w) {
            size_t start_r = win_starts[w] / S;
            if (start_r + 128 > records) start_r = records > 128 ? records - 128 : 0;

            size_t raw_top0_count = 0;
            size_t dif_top0_count = 0;
            size_t total = 0;

            for (size_t r = 1; r < 128 && (start_r + r) < records; ++r) {
                size_t off = S + (start_r + r) * S;
                if (off + 8 > len) break;
                uint64_t val = read_le64(data + off);
                uint64_t prev_val = read_le64(data + off - S);
                uint64_t diff = val - prev_val;

                if ((val >> 48) == 0 || (val >> 48) == 0xFFFF) {
                    raw_top0_count++;
                }
                if ((diff >> 48) == 0 || (diff >> 48) == 0xFFFF) {
                    dif_top0_count++;
                }
                total++;
            }

            if (total == 0) { valid = false; break; }
            double raw_frac = static_cast<double>(raw_top0_count) / total;
            double dif_frac = static_cast<double>(dif_top0_count) / total;
            gains[w] = dif_frac - raw_frac;
        }

        if (!valid) continue;
        double min_gain = std::min({gains[0], gains[1], gains[2]});
        if (min_gain >= 0.50 && min_gain > max_min_gain) {
            max_min_gain = min_gain;
            best_s = S;
        }
    }
    return best_s;
}

inline bool has_beneficial_stride(const uint8_t* data, size_t len) {
    return best_record64_stride(data, len) > 0;
}

inline StrideResult stride_encode(const uint8_t* data, size_t len) {
    StrideResult res;
    res.orig_size = len;
    if (!data || len < 4096) {
        res.applied = false;
        return res;
    }

    uint8_t best_s = best_record64_stride(data, len);
    if (best_s == 0) {
        res.applied = false;
        return res;
    }

    size_t S = best_s;
    size_t records = (len - S) / S;
    if (records < 2) {
        res.applied = false;
        return res;
    }

    uint8_t qword_mask = 1u;
    uint16_t dword_mask = 0u;
    uint64_t byte_mask = 0ULL;

    size_t win_starts[3] = {
        S + records / 4 * S,
        S + records / 2 * S,
        S + (records * 3) / 4 * S
    };
    for (int w = 0; w < 3; ++w) {
        if (win_starts[w] + 128 * S > len) {
            win_starts[w] = len > 128 * S ? len - 128 * S : S;
        }
    }

    // 1. Check additional 8-byte aligned lanes for 64-bit delta
    size_t n_qwords = S / 8;
    for (size_t q = 1; q < n_qwords && q < 8; ++q) {
        size_t off_in_rec = q * 8;
        size_t raw_top = 0, dif_top = 0, total = 0;
        for (int w = 0; w < 3; ++w) {
            size_t start_r = win_starts[w] / S;
            if (start_r + 128 > records) start_r = records > 128 ? records - 128 : 0;
            for (size_t r = 1; r < 128 && (start_r + r) < records; ++r) {
                size_t off = S + (start_r + r) * S + off_in_rec;
                if (off + 8 > len) break;
                uint64_t val = read_le64(data + off);
                uint64_t prev_val = read_le64(data + off - S);
                uint64_t diff = val - prev_val;
                if ((val >> 48) == 0 || (val >> 48) == 0xFFFF) raw_top++;
                if ((diff >> 48) == 0 || (diff >> 48) == 0xFFFF) dif_top++;
                total++;
            }
        }
        if (total > 0 && dif_top > raw_top &&
            static_cast<double>(dif_top - raw_top) / static_cast<double>(total) >= 0.40) {
            qword_mask |= static_cast<uint8_t>(1u << q);
        }
    }

    // 2. Check 4-byte aligned lanes not covered by qword_mask for 32-bit delta
    size_t n_dwords = S / 4;
    for (size_t d = 2; d < n_dwords && d < 16; ++d) {
        if ((qword_mask >> (d / 2)) & 1u) continue;
        size_t off_in_rec = d * 4;
        size_t raw_top = 0, dif_top = 0, total = 0;
        for (int w = 0; w < 3; ++w) {
            size_t start_r = win_starts[w] / S;
            if (start_r + 128 > records) start_r = records > 128 ? records - 128 : 0;
            for (size_t r = 1; r < 128 && (start_r + r) < records; ++r) {
                size_t off = S + (start_r + r) * S + off_in_rec;
                if (off + 4 > len) break;
                uint32_t val = read_le32(data + off);
                uint32_t prev_val = read_le32(data + off - S);
                uint32_t diff = val - prev_val;
                if ((val >> 16) == 0 || (val >> 16) == 0xFFFF) raw_top++;
                if ((diff >> 16) == 0 || (diff >> 16) == 0xFFFF) dif_top++;
                total++;
            }
        }
        if (total > 0 && dif_top > raw_top &&
            static_cast<double>(dif_top - raw_top) / static_cast<double>(total) >= 0.30) {
            dword_mask |= static_cast<uint16_t>(1u << d);
        }
    }

    // 3. Check remaining bytes for 8-bit byte delta
    if (S > 8 && S <= 64) {
        size_t raw_small[64] = {0};
        size_t dif_small[64] = {0};
        size_t total = 0;
        for (int w = 0; w < 3; ++w) {
            size_t start_r = win_starts[w] / S;
            if (start_r + 128 > records) start_r = records > 128 ? records - 128 : 0;
            for (size_t r = 1; r < 128 && (start_r + r) < records; ++r) {
                size_t off = S + (start_r + r) * S;
                if (off + S > len) break;
                const uint8_t* cur = data + off;
                const uint8_t* prev = data + off - S;
                for (size_t b = 8; b < S; ++b) {
                    if (((qword_mask >> (b / 8)) & 1u) || ((dword_mask >> (b / 4)) & 1u)) continue;
                    uint8_t v = cur[b];
                    uint8_t d = static_cast<uint8_t>(v - prev[b]);
                    if (v <= 15 || v >= 241) raw_small[b]++;
                    if (d <= 15 || d >= 241) dif_small[b]++;
                }
                total++;
            }
        }
        if (total > 0) {
            for (size_t b = 8; b < S; ++b) {
                if (dif_small[b] > raw_small[b] &&
                    static_cast<double>(dif_small[b] - raw_small[b]) / static_cast<double>(total) >= 0.15) {
                    byte_mask |= (1ULL << b);
                }
            }
        }
    }

    res.applied = true;
    res.stride = static_cast<uint8_t>(S | 0x80u);
    res.delta_mask = static_cast<uint64_t>(qword_mask) |
                     (static_cast<uint64_t>(dword_mask) << 8) |
                     (1ULL << 24);
    res.data.resize(len + 8);
    write_le64(res.data.data(), byte_mask);

    uint8_t* out_base = res.data.data() + 8;
    size_t head_copy = std::min<size_t>(2 * S, len);
    std::memcpy(out_base, data, head_copy);

    constexpr size_t BLOCK_RECS = 4096;
    std::vector<uint8_t> block_tmp(BLOCK_RECS * S);
    size_t r_start = 1;
    while (r_start < records) {
        size_t r_count = std::min(BLOCK_RECS, records - r_start);
        for (size_t r = 0; r < r_count; ++r) {
            size_t rec_off = S + (r_start + r) * S;
            const uint8_t* src = data + rec_off;
            const uint8_t* prev_src = data + rec_off - S;
            uint8_t* dst = block_tmp.data() + r * S;
            std::memcpy(dst, src, S);
            for (size_t q = 0; q < n_qwords && q < 8; ++q) {
                if ((qword_mask >> q) & 1u) {
                    write_le64(dst + q * 8, read_le64(src + q * 8) - read_le64(prev_src + q * 8));
                }
            }
            for (size_t d = 2; d < n_dwords && d < 16; ++d) {
                if ((dword_mask >> d) & 1u) {
                    uint32_t diff32 = read_le32(src + d * 4) - read_le32(prev_src + d * 4);
                    std::memcpy(dst + d * 4, &diff32, 4);
                }
            }
            if (byte_mask) {
                uint64_t m = byte_mask;
                while (m) {
                    int b = __builtin_ctzll(m);
                    m &= m - 1;
                    dst[b] = static_cast<uint8_t>(src[b] - prev_src[b]);
                }
            }
        }
        uint8_t* block_out = out_base + S + r_start * S;
        for (size_t col = 0; col < S; ++col) {
            uint8_t* col_dst = block_out + col * r_count;
            const uint8_t* col_src = block_tmp.data() + col;
            for (size_t r = 0; r < r_count; ++r) {
                col_dst[r] = col_src[r * S];
            }
        }
        r_start += r_count;
    }

    size_t encoded_end = S + records * S;
    if (encoded_end < len) {
        std::memcpy(out_base + encoded_end, data + encoded_end, len - encoded_end);
    }

    return res;
}

inline bool stride_decode(const uint8_t* in, size_t in_len, uint64_t orig_size,
                   uint8_t stride, uint64_t delta_mask, ByteVec& out) {
    if (!in) return false;
    if (stride & 0x80u) {
        size_t S = static_cast<size_t>(stride & 0x7Fu);
        if (S < 16 || in_len != orig_size + 8) return false;
        uint64_t byte_mask = read_le64(in);
        const uint8_t* p_in = in + 8;
        out.resize(orig_size);

        size_t records = (orig_size - S) / S;
        size_t head_copy = std::min<size_t>(2 * S, static_cast<size_t>(orig_size));
        std::memcpy(out.data(), p_in, head_copy);
        if (records < 2) {
            if (orig_size > head_copy) {
                std::memcpy(out.data() + head_copy, p_in + head_copy, orig_size - head_copy);
            }
            return true;
        }

        uint8_t qword_mask = static_cast<uint8_t>(delta_mask & 0xFFu);
        uint16_t dword_mask = static_cast<uint16_t>((delta_mask >> 8) & 0xFFFFu);
        size_t n_qwords = S / 8;
        size_t n_dwords = S / 4;

        constexpr size_t BLOCK_RECS = 4096;
        size_t num_blocks = (records - 1 + BLOCK_RECS - 1) / BLOCK_RECS;
        #pragma omp parallel for schedule(static) if(num_blocks >= 4)
        for (int bi = 0; bi < static_cast<int>(num_blocks); ++bi) {
            size_t r_start = 1 + static_cast<size_t>(bi) * BLOCK_RECS;
            size_t r_count = std::min(BLOCK_RECS, records - r_start);
            const uint8_t* block_in = p_in + S + r_start * S;
            uint8_t* block_out = out.data() + S + r_start * S;
            for (size_t col = 0; col < S; ++col) {
                const uint8_t* col_src = block_in + col * r_count;
                uint8_t* col_dst = block_out + col;
                for (size_t r = 0; r < r_count; ++r) {
                    col_dst[r * S] = col_src[r];
                }
            }
        }

        for (size_t r = 1; r < records; ++r) {
            size_t rec_off = S + r * S;
            uint8_t* out_rec = out.data() + rec_off;
            const uint8_t* prev_rec = out_rec - S;
            for (size_t q = 0; q < n_qwords && q < 8; ++q) {
                if ((qword_mask >> q) & 1u) {
                    write_le64(out_rec + q * 8, read_le64(prev_rec + q * 8) + read_le64(out_rec + q * 8));
                }
            }
            for (size_t d = 2; d < n_dwords && d < 16; ++d) {
                if ((dword_mask >> d) & 1u) {
                    uint32_t cur32 = read_le32(prev_rec + d * 4) + read_le32(out_rec + d * 4);
                    std::memcpy(out_rec + d * 4, &cur32, 4);
                }
            }
            if (byte_mask) {
                uint64_t m = byte_mask;
                while (m) {
                    int b = __builtin_ctzll(m);
                    m &= m - 1;
                    out_rec[b] = static_cast<uint8_t>(out_rec[b] + prev_rec[b]);
                }
            }
        }

        size_t encoded_end = S + records * S;
        if (encoded_end < orig_size) {
            std::memcpy(out.data() + encoded_end, p_in + encoded_end, orig_size - encoded_end);
        }
        return true;
    }

    if (stride < 16 || in_len != orig_size) return false;
    out.resize(orig_size);

    size_t S = stride;
    size_t records = (orig_size - S) / S;

    size_t head_copy = std::min<size_t>(2 * S, static_cast<size_t>(orig_size));
    std::memcpy(out.data(), in, head_copy);

    if (records < 2) {
        if (orig_size > head_copy) {
            std::memcpy(out.data() + head_copy, in + head_copy, orig_size - head_copy);
        }
        return true;
    }

    const uint64_t valid_bits = (S < 64) ? ((1ULL << S) - 1ULL) : ~0ULL;
    const uint64_t extra = (delta_mask & valid_bits) & ~0xFFULL;
    uint64_t prev = read_le64(in + S);

#ifdef __AVX2__
    if (S == 32) {
        alignas(32) uint8_t mask_bytes[32];
        for (size_t b = 0; b < 32; ++b) {
            mask_bytes[b] = (b >= 8 && ((extra >> b) & 1ULL)) ? 0xFF : 0x00;
        }
        const __m256i vmask = _mm256_load_si256(reinterpret_cast<const __m256i*>(mask_bytes));
        __m256i v_prev = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(out.data() + 32));
        uint64_t prev_q = prev;

        for (size_t r = 1; r < records; ++r) {
            size_t rec_off = 32 + r * 32;
            if (rec_off + 32 > orig_size) break;
            const uint8_t* in_rec = in + rec_off;
            uint8_t* out_rec = out.data() + rec_off;

            __m256i v_in = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(in_rec));
            __m256i v_out = _mm256_add_epi8(v_in, _mm256_and_si256(v_prev, vmask));
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(out_rec), v_out);
            prev_q += read_le64(in_rec);
            write_le64(out_rec, prev_q);
            v_prev = v_out;
        }
    } else if (S == 64) {
        alignas(32) uint8_t mask_bytes[64];
        for (size_t b = 0; b < 64; ++b) {
            mask_bytes[b] = (b >= 8 && ((extra >> b) & 1ULL)) ? 0xFF : 0x00;
        }
        const __m256i vmask0 = _mm256_load_si256(reinterpret_cast<const __m256i*>(mask_bytes));
        const __m256i vmask1 = _mm256_load_si256(reinterpret_cast<const __m256i*>(mask_bytes + 32));
        __m256i v_prev0 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(out.data() + 64));
        __m256i v_prev1 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(out.data() + 64 + 32));
        uint64_t prev_q = prev;

        for (size_t r = 1; r < records; ++r) {
            size_t rec_off = 64 + r * 64;
            if (rec_off + 64 > orig_size) break;
            const uint8_t* in_rec = in + rec_off;
            uint8_t* out_rec = out.data() + rec_off;

            __m256i v_in0 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(in_rec));
            __m256i v_out0 = _mm256_add_epi8(v_in0, _mm256_and_si256(v_prev0, vmask0));
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(out_rec), v_out0);

            __m256i v_in1 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(in_rec + 32));
            __m256i v_out1 = _mm256_add_epi8(v_in1, _mm256_and_si256(v_prev1, vmask1));
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(out_rec + 32), v_out1);

            prev_q += read_le64(in_rec);
            write_le64(out_rec, prev_q);
            v_prev0 = v_out0;
            v_prev1 = v_out1;
        }
    } else
#endif
    for (size_t r = 1; r < records; ++r) {
        size_t rec_off = S + r * S;
        if (rec_off + S > orig_size) break;
        const uint8_t* in_rec = in + rec_off;
        uint8_t* out_rec = out.data() + rec_off;

        uint64_t diff = read_le64(in_rec);
        uint64_t cur = prev + diff;
        write_le64(out_rec, cur);
        prev = cur;

        if (S > 8) {
            std::memcpy(out_rec + 8, in_rec + 8, S - 8);
            if (extra) {
                const uint8_t* prev_out_rec = out.data() + rec_off - S;
                uint64_t m = extra;
                while (m) {
                    int b = __builtin_ctzll(m);
                    m &= m - 1;
                    out_rec[b] = static_cast<uint8_t>(in_rec[b] + prev_out_rec[b]);
                }
            }
        }
    }

    size_t encoded_end = S + records * S;
    if (encoded_end < orig_size) {
        std::memcpy(out.data() + encoded_end, in + encoded_end, orig_size - encoded_end);
    }

    return true;
}

// Zip precomp implementation with OpenMP parallelization & level 6 focus
struct ZipPatch {
    uint64_t orig_comp_off;
    uint32_t orig_comp_len;
    uint32_t uncomp_len;
    uint8_t level;
};

inline ZipPrecompResult zip_precomp_encode(const uint8_t* data, size_t len, bool all_levels = false) {
    ZipPrecompResult res;
    res.orig_size = len;
    if (!data || len < 64) {
        res.applied = false;
        return res;
    }

    struct Candidate {
        size_t data_off;
        uint32_t csize;
        uint32_t usize;
        uint32_t crc;
        ByteVec pre_inflated;
    };

    std::vector<Candidate> candidates;
    size_t pos = 0;
#ifdef __AVX2__
    const __m256i vP = _mm256_set1_epi8('P');
    const __m256i vGz = _mm256_set1_epi8(0x1F);
#endif
    while (pos + 18 <= len) {
#ifdef __AVX2__
        while (pos + 32 <= len) {
            __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(data + pos));
            __m256i m = _mm256_or_si256(_mm256_cmpeq_epi8(v, vP), _mm256_cmpeq_epi8(v, vGz));
            uint32_t mask = static_cast<uint32_t>(_mm256_movemask_epi8(m));
            if (mask != 0) {
                pos += static_cast<size_t>(__builtin_ctz(mask));
                break;
            }
            pos += 32;
        }
        if (pos + 18 > len) break;
#endif
        if (pos + 30 <= len && data[pos] == 'P' && data[pos + 1] == 'K' && data[pos + 2] == 3 && data[pos + 3] == 4) {
            uint16_t flags = read_le16(data + pos + 6);
            uint16_t method = read_le16(data + pos + 8);
            uint32_t crc = read_le32(data + pos + 14);
            uint32_t csize = read_le32(data + pos + 18);
            uint32_t usize = read_le32(data + pos + 22);
            uint16_t fnlen = read_le16(data + pos + 26);
            uint16_t exlen = read_le16(data + pos + 28);

            size_t data_off = pos + 30 + fnlen + exlen;

            if (method == 8 && (flags & 8) == 0 && csize >= 16 && usize > csize &&
                usize <= 16 * 1024 * 1024 && data_off + csize <= len) {
                candidates.push_back(Candidate{data_off, csize, usize, crc, {}});
                pos = data_off + csize; // non-overlapping jump
                continue;
            }
        } else if (data[pos] == 0x1F && data[pos + 1] == 0x8B && data[pos + 2] == 0x08) {
            uint8_t flg = data[pos + 3];
            uint8_t xfl = data[pos + 8];
            uint8_t os = data[pos + 9];
            if ((flg & 0xE0) == 0 && (xfl == 0 || xfl == 2 || xfl == 4) && (os <= 19 || os == 255)) {
                size_t hpos = pos + 10;
                bool hdr_ok = true;
                if (flg & 4) { // FEXTRA
                    if (hpos + 2 > len) hdr_ok = false;
                    else { uint16_t xlen = read_le16(data + hpos); hpos += 2 + xlen; }
                }
                if (hdr_ok && (flg & 8)) { // FNAME
                    while (hpos < len && data[hpos] != 0) ++hpos;
                    if (hpos >= len) hdr_ok = false; else ++hpos;
                }
                if (hdr_ok && (flg & 16)) { // FCOMMENT
                    while (hpos < len && data[hpos] != 0) ++hpos;
                    if (hpos >= len) hdr_ok = false; else ++hpos;
                }
                if (hdr_ok && (flg & 2)) { // FHCRC
                    hpos += 2;
                }
                if (hdr_ok && hpos + 10 <= len) {
                    z_stream zs;
                    std::memset(&zs, 0, sizeof(zs));
                    if (inflateInit2(&zs, -15) == Z_OK) {
                        uint8_t probe[4096];
                        zs.next_in = const_cast<Bytef*>(data + hpos);
                        zs.avail_in = static_cast<uInt>(std::min<size_t>(len - hpos, 0x7FFFFFFFu));
                        zs.next_out = probe;
                        zs.avail_out = sizeof(probe);
                        int ret = inflate(&zs, Z_NO_FLUSH);
                        if (ret == Z_OK || ret == Z_STREAM_END) {
                            ByteVec tmp;
                            size_t first_out = sizeof(probe) - zs.avail_out;
                            if (first_out > 0) tmp.insert(tmp.end(), probe, probe + first_out);
                            while (ret == Z_OK && tmp.size() < 128ULL * 1024 * 1024) {
                                size_t old_sz = tmp.size();
                                tmp.resize(old_sz + 65536);
                                zs.next_out = tmp.data() + old_sz;
                                zs.avail_out = 65536;
                                ret = inflate(&zs, Z_NO_FLUSH);
                                tmp.resize(old_sz + (65536 - zs.avail_out));
                            }
                            if (ret == Z_STREAM_END && zs.total_in >= 64 && hpos + zs.total_in + 8 <= len) {
                                uint32_t gz_crc = read_le32(data + hpos + zs.total_in);
                                uint32_t gz_isz = read_le32(data + hpos + zs.total_in + 4);
                                if (gz_isz == static_cast<uint32_t>(tmp.size()) &&
                                    crc32(0L, tmp.data(), static_cast<uInt>(tmp.size())) == gz_crc) {
                                    uint32_t csz = static_cast<uint32_t>(zs.total_in);
                                    uint32_t usz = static_cast<uint32_t>(tmp.size());
                                    inflateEnd(&zs);
                                    candidates.push_back(Candidate{hpos, csz, usz, gz_crc, std::move(tmp)});
                                    pos = hpos + csz + 8;
                                    continue;
                                }
                            }
                        }
                        inflateEnd(&zs);
                    }
                }
            }
        }
        pos++;
    }

    if (candidates.empty()) {
        res.applied = false;
        return res;
    }

    struct VerifiedPatch {
        size_t data_off;
        uint32_t csize;
        uint32_t usize;
        uint8_t level;
        ByteVec raw_buf;
        bool valid;
    };

    std::vector<VerifiedPatch> verified(candidates.size());

    #pragma omp parallel for schedule(dynamic)
    for (int i = 0; i < static_cast<int>(candidates.size()); ++i) {
        auto& cand = candidates[i];
        verified[i].data_off = cand.data_off;
        verified[i].csize = cand.csize;
        verified[i].usize = cand.usize;
        verified[i].level = 6;
        verified[i].valid = false;

        ByteVec raw_buf;
        bool inflated_ok = false;
        if (!cand.pre_inflated.empty()) {
            raw_buf = std::move(cand.pre_inflated);
            inflated_ok = (raw_buf.size() == cand.usize);
        } else {
            raw_buf.resize(cand.usize);
            z_stream zs;
            std::memset(&zs, 0, sizeof(zs));
            if (inflateInit2(&zs, -15) == Z_OK) {
                zs.next_in = const_cast<Bytef*>(data + cand.data_off);
                zs.avail_in = static_cast<uInt>(cand.csize);
                zs.next_out = raw_buf.data();
                zs.avail_out = static_cast<uInt>(cand.usize);

                int ret = inflate(&zs, Z_FINISH);
                inflateEnd(&zs);
                inflated_ok = (ret == Z_STREAM_END && zs.total_out == cand.usize && zs.total_in == cand.csize &&
                               crc32(0L, raw_buf.data(), static_cast<uInt>(cand.usize)) == cand.crc);
            }
        }

        if (inflated_ok) {
            static constexpr int kLevels[9] = {6, 9, 8, 5, 1, 7, 4, 3, 2};
            int n_levels = all_levels ? 9 : 5;
            size_t redef_cap = cand.usize + 1024;
            std::unique_ptr<uint8_t[]> redef_buf(new uint8_t[redef_cap]);
            for (int li = 0; li < n_levels; ++li) {
                int lvl = kLevels[li];
                z_stream ds;
                std::memset(&ds, 0, sizeof(ds));
                if (deflateInit2(&ds, lvl, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) == Z_OK) {
                    ds.next_in = raw_buf.data();
                    ds.avail_in = static_cast<uInt>(cand.usize);
                    ds.next_out = redef_buf.get();
                    ds.avail_out = static_cast<uInt>(redef_cap);

                    int dret = deflate(&ds, Z_FINISH);
                    uInt redef_len = ds.total_out;
                    deflateEnd(&ds);

                    if (dret == Z_STREAM_END && redef_len == cand.csize &&
                        std::memcmp(redef_buf.get(), data + cand.data_off, cand.csize) == 0) {
                        verified[i].level = static_cast<uint8_t>(lvl);
                        verified[i].raw_buf = std::move(raw_buf);
                        verified[i].valid = true;
                        break;
                    }
                }
            }
        }
    }

    std::vector<ZipPatch> patches;
    ByteVec transformed;
    size_t prev_cursor = 0;

    for (const auto& v : verified) {
        if (!v.valid) continue;
        transformed.insert(transformed.end(), data + prev_cursor, data + v.data_off);
        transformed.insert(transformed.end(), v.raw_buf.begin(), v.raw_buf.end());

        patches.push_back(ZipPatch{static_cast<uint64_t>(v.data_off), v.csize, v.usize, v.level});
        prev_cursor = v.data_off + v.csize;
    }

    if (patches.empty()) {
        res.applied = false;
        return res;
    }

    transformed.insert(transformed.end(), data + prev_cursor, data + len);

    res.applied = true;
    res.num_entries = static_cast<uint32_t>(patches.size());
    res.transformed = std::move(transformed);

    uint32_t num_p = static_cast<uint32_t>(patches.size());
    res.patch_meta.resize(4 + patches.size() * sizeof(ZipPatch));
    std::memcpy(res.patch_meta.data(), &num_p, 4);
    std::memcpy(res.patch_meta.data() + 4, patches.data(), patches.size() * sizeof(ZipPatch));

    return res;
}

inline bool zip_precomp_decode(const uint8_t* transformed, size_t tr_len,
                        const uint8_t* patch_meta, size_t pm_len,
                        uint64_t orig_size, ByteVec& out) {
    out.clear();
    out.resize(static_cast<size_t>(orig_size));

    if (!patch_meta || pm_len < 4) return false;

    uint32_t num_patches = read_le32(patch_meta);
    if (pm_len < 4 + num_patches * sizeof(ZipPatch)) return false;

    std::vector<ZipPatch> patches(num_patches);
    std::memcpy(patches.data(), patch_meta + 4, num_patches * sizeof(ZipPatch));

    size_t tr_cursor = 0;
    size_t prev_orig_off = 0;

    struct PatchDecodeInfo {
        size_t tr_raw_src;
        size_t uncomp_len;
        uint64_t orig_comp_off;
        uint32_t orig_comp_len;
        uint8_t level;
    };

    std::vector<PatchDecodeInfo> dec_infos(num_patches);

    for (uint32_t i = 0; i < num_patches; ++i) {
        const auto& p = patches[i];
        if (p.orig_comp_off < prev_orig_off) return false;

        size_t gap_len = p.orig_comp_off - prev_orig_off;
        if (tr_cursor + gap_len > tr_len) return false;

        if (gap_len > 0) {
            std::memcpy(out.data() + prev_orig_off, transformed + tr_cursor, gap_len);
        }
        tr_cursor += gap_len;

        if (tr_cursor + p.uncomp_len > tr_len) return false;
        dec_infos[i] = PatchDecodeInfo{tr_cursor, p.uncomp_len, p.orig_comp_off, p.orig_comp_len, p.level};
        tr_cursor += p.uncomp_len;
        prev_orig_off = p.orig_comp_off + p.orig_comp_len;
    }

    if (tr_cursor < tr_len) {
        size_t tail_len = tr_len - tr_cursor;
        if (prev_orig_off + tail_len > orig_size) return false;
        std::memcpy(out.data() + prev_orig_off, transformed + tr_cursor, tail_len);
    }

    bool decode_ok = true;

    std::vector<int> order(num_patches);
    for (uint32_t i = 0; i < num_patches; ++i) order[i] = static_cast<int>(i);
    std::sort(order.begin(), order.end(), [&](int a, int b) {
        return dec_infos[a].uncomp_len > dec_infos[b].uncomp_len;
    });

    #pragma omp parallel for schedule(dynamic, 1)
    for (int idx = 0; idx < static_cast<int>(num_patches); ++idx) {
        const auto& info = dec_infos[order[idx]];
        z_stream ds;
        std::memset(&ds, 0, sizeof(ds));
        if (deflateInit2(&ds, info.level, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) == Z_OK) {
            ds.next_in = const_cast<Bytef*>(transformed + info.tr_raw_src);
            ds.avail_in = static_cast<uInt>(info.uncomp_len);
            ds.next_out = out.data() + info.orig_comp_off;
            ds.avail_out = static_cast<uInt>(info.orig_comp_len);

            int dret = deflate(&ds, Z_FINISH);
            uInt redef_len = ds.total_out;
            deflateEnd(&ds);

            if (dret != Z_STREAM_END || redef_len != info.orig_comp_len) {
                #pragma omp atomic write
                decode_ok = false;
            }
        } else {
            #pragma omp atomic write
            decode_ok = false;
        }
    }

    return decode_ok && (out.size() == orig_size);
}

// Tar demux implementation
inline uint64_t parse_octal(const uint8_t* p, size_t n) {
    if (n > 0 && (p[0] & 0x80)) {
        uint64_t val = 0;
        for (size_t i = 1; i < n; ++i) {
            val = (val << 8) + p[i];
        }
        return val;
    }
    size_t i = 0;
    while (i < n && (p[i] == ' ' || p[i] == 0)) ++i;
    uint64_t val = 0;
    for (; i < n; ++i) {
        uint8_t c = p[i];
        if (c < '0' || c > '7') break;
        val = val * 8 + (c - '0');
    }
    return val;
}

inline bool tar_check_header(const uint8_t* h) {
    bool has_ustar = (std::memcmp(h + 257, "ustar", 5) == 0 ||
                      std::memcmp(h + 257, "ustar\0", 6) == 0 ||
                      std::memcmp(h + 257, "ustar  ", 7) == 0);
    bool is_v7_zero = true;
    for (int i = 257; i < 263; ++i) {
        if (h[i] != 0) { is_v7_zero = false; break; }
    }

    if (!has_ustar && !is_v7_zero) {
        return false;
    }

#ifdef __AVX2__
    const __m256i vzero = _mm256_setzero_si256();
    __m256i vor = vzero;
    __m256i vsum = vzero;
    for (int i = 0; i < 512; i += 32) {
        __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(h + i));
        vor = _mm256_or_si256(vor, v);
        vsum = _mm256_add_epi64(vsum, _mm256_sad_epu8(v, vzero));
    }
    if (_mm256_testz_si256(vor, vor)) return false;

    alignas(32) uint64_t lanes[4];
    _mm256_store_si256(reinterpret_cast<__m256i*>(lanes), vsum);
    unsigned int sum = static_cast<unsigned int>(lanes[0] + lanes[1] + lanes[2] + lanes[3]);
    for (int i = 148; i < 156; ++i) {
        sum -= h[i];
    }
    sum += 8u * static_cast<unsigned int>(' ');
#else
    bool all_zero = true;
    for (int i = 0; i < 512; ++i) {
        if (h[i] != 0) { all_zero = false; break; }
    }
    if (all_zero) return false;

    unsigned int sum = 0;
    for (int i = 0; i < 512; ++i) {
        if (i >= 148 && i < 156) {
            sum += ' ';
        } else {
            sum += h[i];
        }
    }
#endif
    unsigned int stored_sum = static_cast<unsigned int>(parse_octal(h + 148, 8));

    return sum == stored_sum;
}

enum TarOpcode : uint8_t {
    OP_RAW_CHUNK = 1,
    OP_BUCKET_ENTRY = 2
};

// Forward declaration for recursion
inline bool demux_tar_span(const uint8_t* buf, size_t span_len,
                           ByteVec& meta_stream,
                           ByteVec buckets[BUCKET_COUNT]);

inline TarDemuxResult tar_demux_encode(const uint8_t* data, size_t len) {
    TarDemuxResult res;
    res.orig_size = len;
    if (!data || len < 512) {
        res.applied = false;
        return res;
    }

    if (len >= 1024 * 1024) {
        res.meta_stream.reserve(len / 32);
        res.buckets[BUCKET_TEXT].reserve(len / 2);
        res.buckets[BUCKET_BINARY].reserve(len / 2);
    }

    bool valid_outer = demux_tar_span(data, len, res.meta_stream, res.buckets);
    if (!valid_outer) {
        res.applied = false;
        return res;
    }

    res.applied = true;
    return res;
}

inline bool has_embedded_zip(const uint8_t* data, size_t len) {
    if (!data || len < 64) return false;
    size_t i = 0;
    while (i + 30 <= len) {
        const void* hit = std::memchr(data + i, 'P', (len - 30) - i + 1);
        if (!hit) break;
        i = static_cast<size_t>(static_cast<const uint8_t*>(hit) - data);
        if (data[i + 1] == 'K' && data[i + 2] == 3 && data[i + 3] == 4) {
            uint16_t method = read_le16(data + i + 8);
            uint32_t csize = read_le32(data + i + 18);
            uint32_t usize = read_le32(data + i + 22);
            if (method == 8 && csize >= 16 && usize > csize && usize <= 16 * 1024 * 1024) {
                return true;
            }
        }
        i++;
    }
    return false;
}

inline bool demux_tar_span(const uint8_t* buf, size_t span_len,
                           ByteVec& meta_stream,
                           ByteVec buckets[BUCKET_COUNT]) {
    size_t pos = 0;
    size_t raw_chunk_start = 0;
    size_t raw_chunk_len = 0;

    auto flush_raw = [&]() {
        if (raw_chunk_len > 0) {
            uint8_t op = OP_RAW_CHUNK;
            uint32_t clen = static_cast<uint32_t>(raw_chunk_len);
            meta_stream.push_back(op);
            meta_stream.insert(meta_stream.end(),
                               reinterpret_cast<const uint8_t*>(&clen),
                               reinterpret_cast<const uint8_t*>(&clen) + 4);
            meta_stream.insert(meta_stream.end(), buf + raw_chunk_start, buf + raw_chunk_start + clen);
            raw_chunk_len = 0;
        }
    };

    while (pos + 512 <= span_len) {
        bool all_zero = true;
        for (size_t i = 0; i < 512; ++i) {
            if (buf[pos + i] != 0) {
                all_zero = false;
                break;
            }
        }
        if (all_zero) {
            flush_raw();
            uint8_t op = OP_RAW_CHUNK;
            uint32_t clen = static_cast<uint32_t>(span_len - pos);
            meta_stream.push_back(op);
            meta_stream.insert(meta_stream.end(),
                               reinterpret_cast<const uint8_t*>(&clen),
                               reinterpret_cast<const uint8_t*>(&clen) + 4);
            meta_stream.insert(meta_stream.end(), buf + pos, buf + span_len);
            return true;
        }

        if (!tar_check_header(buf + pos)) {
            return false;
        }

        uint64_t file_size = parse_octal(buf + pos + 124, 12);
        uint8_t typeflag = buf[pos + 156];

        size_t header_pos = pos;
        size_t padded_size = ((file_size + 511) / 512) * 512;
        size_t entry_total_len = 512 + padded_size;

        if (pos + entry_total_len > span_len) {
            return false;
        }

        if (raw_chunk_len == 0) {
            raw_chunk_start = header_pos;
        }
        raw_chunk_len += 512;

        flush_raw();

        const uint8_t* file_data = buf + header_pos + 512;

        if ((typeflag == 0 || typeflag == '0') && file_size > 0) {
            bool is_nested_tar = (file_size >= 65536 && (file_size % 512) == 0 && tar_check_header(file_data) && has_embedded_zip(file_data, file_size));

            if (is_nested_tar) {
                // Route directly to BUCKET_TAR_ARCHIVE without recursing
                TarBucketId b_id = BUCKET_TAR_ARCHIVE;
                uint32_t b_idx = static_cast<uint32_t>(b_id);
                uint32_t entry_len = static_cast<uint32_t>(file_size);

                uint8_t op = OP_BUCKET_ENTRY;
                meta_stream.push_back(op);
                meta_stream.push_back(b_id);
                meta_stream.insert(meta_stream.end(),
                                   reinterpret_cast<const uint8_t*>(&entry_len),
                                   reinterpret_cast<const uint8_t*>(&entry_len) + 4);

                buckets[b_idx].insert(buckets[b_idx].end(), file_data, file_data + file_size);
            } else {
                TarBucketId b_id = BUCKET_BINARY;
                ExecType ex = detect_executable(file_data, file_size);
                if (ex == EXEC_SPARC) {
                    b_id = BUCKET_EXEC_SPARC;
                } else if (ex == EXEC_X86) {
                    b_id = BUCKET_EXEC_X86;
                } else {
                    size_t sample_sz = std::min<size_t>(file_size, 8192);
                    size_t ascii_c = count_printable_ascii(file_data, sample_sz);
                    if (static_cast<double>(ascii_c) / sample_sz >= 0.78) {
                        b_id = BUCKET_TEXT;
                    } else if ((file_size >= 4 && file_data[0] == 'P' && file_data[1] == 'K' && file_data[2] == 3 && file_data[3] == 4) ||
                               (file_size >= 18 && file_data[0] == 0x1F && file_data[1] == 0x8B && file_data[2] == 0x08)) {
                        b_id = BUCKET_TAR_ARCHIVE;
                    } else if (has_beneficial_stride(file_data, file_size)) {
                        b_id = BUCKET_STRIDE;
                    }
                }

                uint32_t b_idx = static_cast<uint32_t>(b_id);
                uint32_t entry_len = static_cast<uint32_t>(file_size);

                uint8_t op = OP_BUCKET_ENTRY;
                meta_stream.push_back(op);
                meta_stream.push_back(b_id);
                meta_stream.insert(meta_stream.end(),
                                   reinterpret_cast<const uint8_t*>(&entry_len),
                                   reinterpret_cast<const uint8_t*>(&entry_len) + 4);

                buckets[b_idx].insert(buckets[b_idx].end(), file_data, file_data + file_size);
            }

            if (padded_size > file_size) {
                size_t pad_len = padded_size - file_size;
                if (raw_chunk_len == 0) raw_chunk_start = header_pos + 512 + file_size;
                raw_chunk_len += pad_len;
            }
        } else {
            if (file_size > 0) {
                if (raw_chunk_len == 0) raw_chunk_start = header_pos + 512;
                raw_chunk_len += padded_size;
            }
        }

        pos += entry_total_len;
    }

    if (pos < span_len) {
        if (raw_chunk_len == 0) raw_chunk_start = pos;
        raw_chunk_len += (span_len - pos);
    }
    flush_raw();
    return true;
}

inline bool tar_demux_decode(const uint8_t* meta, size_t meta_len,
                      const ByteVec buckets[BUCKET_COUNT],
                      uint64_t orig_size, ByteVec& out) {
    out.clear();
    out.resize(static_cast<size_t>(orig_size));

    size_t bucket_cursors[BUCKET_COUNT] = {0};
    size_t meta_cursor = 0;

    size_t out_cursor = 0;

    while (meta_cursor < meta_len) {
        uint8_t op = meta[meta_cursor++];
        if (op == OP_RAW_CHUNK) {
            if (meta_cursor + 4 > meta_len) return false;
            uint32_t clen = read_le32(meta + meta_cursor);
            meta_cursor += 4;
            if (meta_cursor + clen > meta_len || out_cursor + clen > orig_size) return false;

            std::memcpy(out.data() + out_cursor, meta + meta_cursor, clen);
            meta_cursor += clen;
            out_cursor += clen;
        } else if (op == OP_BUCKET_ENTRY) {
            if (meta_cursor + 5 > meta_len) return false;
            uint8_t b_id = meta[meta_cursor++];
            uint32_t entry_len = read_le32(meta + meta_cursor);
            meta_cursor += 4;

            if (b_id >= BUCKET_COUNT) return false;
            if (bucket_cursors[b_id] + entry_len > buckets[b_id].size() || out_cursor + entry_len > orig_size) return false;

            std::memcpy(out.data() + out_cursor, buckets[b_id].data() + bucket_cursors[b_id], entry_len);
            bucket_cursors[b_id] += entry_len;
            out_cursor += entry_len;
        } else {
            return false;
        }
    }

    return out_cursor == orig_size;
}

} // namespace prism::precond
