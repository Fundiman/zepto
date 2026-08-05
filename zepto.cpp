#include "zepto.h"
#include <fstream>
#include <cstring>
#include <algorithm>
#include <numeric>
#include <bit>
#include <iostream>
#include <sstream>
#include <cctype>
#include <filesystem>
#include <set>
#include <map>
#include <unordered_map>
#include <zstd.h>
namespace fs = std::filesystem;

namespace zepto {

// CRC32C with hardware support (SSE 4.2) or fallback table
static uint32_t table[256];
static bool table_init = false;

static void init_crc32c() {
    if (table_init) return;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t crc = i;
        for (int j = 0; j < 8; j++)
            crc = (crc >> 1) ^ (crc & 1 ? 0x82F63B78 : 0);
        table[i] = crc;
    }
    table_init = true;
}

// ---- LZ4 Block Compression (fast hash-chain implementation) ----

static std::vector<uint8_t> lz4_compress(const uint8_t* src, size_t src_size) {
    if (src_size == 0) return {};
    std::vector<uint8_t> out;
    out.reserve(src_size + src_size / 4 + 8);

    // Reserve 4 bytes for uncompressed size, patch later
    out.resize(4);

    constexpr int HASH_BITS = 12;
    constexpr int HASH_SIZE = 1 << HASH_BITS;
    constexpr int MAX_DIST = 65535;
    std::vector<int32_t> ht(HASH_SIZE, -1);
    auto hfn = [](const uint8_t* p) -> int {
        uint32_t v; memcpy(&v, p, 3);
        return (int)((v * 0x1E35A7BDu) >> (32 - HASH_BITS)) & (HASH_SIZE - 1);
    };

    size_t ip = 0;
    while (ip < src_size) {
        // Find match
        int best_len = 0, best_off = 0;
        if (ip + 3 < src_size) {
            int h = hfn(src + ip);
            int32_t ch = ht[h];
            if (ch >= 0 && ip - (size_t)ch <= MAX_DIST) {
                size_t mx = src_size - ip;
                if (mx > 18) mx = 18;
                int len = 0;
                while ((size_t)len < mx && src[ch + len] == src[ip + len]) len++;
                if (len >= 4) { best_len = len; best_off = (int)(ip - (size_t)ch); }
            }
            ht[h] = (int32_t)ip;
        }

        if (best_len >= 4) {
            // Emit match token with 0 literals
            int match_len = best_len - 4;
            out.push_back((uint8_t)match_len);
            out.push_back((uint8_t)(best_off));
            out.push_back((uint8_t)(best_off >> 8));
            for (size_t j = 1; j < (size_t)best_len && ip + j + 3 < src_size; j++) {
                ht[hfn(src + ip + j)] = (int32_t)(ip + j);
            }
            ip += best_len;
        } else {
            // Literal run
            size_t lit_start = ip;
            int lit_count = 0;
            while (ip < src_size && lit_count < 15) {
                if (ip + 3 < src_size) {
                    int h = hfn(src + ip);
                    int32_t ch = ht[h];
                    if (ch >= 0 && (int32_t)ip != ch && ip - (size_t)ch <= MAX_DIST) {
                        size_t mx = src_size - ip;
                        if (mx > 18) mx = 18;
                        int len = 0;
                        while ((size_t)len < mx && src[ch + len] == src[ip + len]) len++;
                        if (len >= 4) break;
                    }
                    ht[h] = (int32_t)ip;
                }
                lit_count++;
                ip++;
            }
            out.push_back((uint8_t)(lit_count << 4));
            out.insert(out.end(), src + lit_start, src + lit_start + lit_count);
        }
    }

    // Patch uncompressed size
    out[0] = (uint8_t)(src_size);
    out[1] = (uint8_t)(src_size >> 8);
    out[2] = (uint8_t)(src_size >> 16);
    out[3] = (uint8_t)(src_size >> 24);
    return out;
}

static std::vector<uint8_t> lz4_decompress(const uint8_t* src, size_t src_size, size_t max_out = 0) {
    if (src_size < 4) return {};
    uint32_t uncomp_size = 0;
    memcpy(&uncomp_size, src, 4);
    if (max_out > 0 && uncomp_size > max_out) return {};
    std::vector<uint8_t> out;
    out.reserve(uncomp_size);
    size_t ip = 4;
    while (ip < src_size && out.size() < uncomp_size) {
        uint8_t token = src[ip++];
        int lit_len = (token >> 4) & 0x0F;
        int match_len = token & 0x0F;
        if (ip + lit_len > src_size) break;
        out.insert(out.end(), src + ip, src + ip + lit_len);
        ip += lit_len;
        if (match_len > 0 || (lit_len == 0 && out.size() < uncomp_size)) {
            if (ip + 2 > src_size) break;
            uint16_t offset = (uint16_t)src[ip] | ((uint16_t)src[ip + 1] << 8);
            ip += 2;
            match_len += 4;
            if (offset == 0 || offset > out.size()) break;
            size_t match_pos = out.size() - offset;
            for (int i = 0; i < match_len && out.size() < uncomp_size; i++) {
                out.push_back(out[match_pos + i]);
            }
        }
    }
    out.resize(uncomp_size);
    return out;
}

// ---- ZSTD block compression (via libzstd) ----

static constexpr int ZSTD_LEVEL = 1;

static std::vector<uint8_t> zstd_compress(const uint8_t* src, size_t src_size) {
    if (src_size == 0) return {};
    size_t bound = ZSTD_compressBound(src_size);
    std::vector<uint8_t> out(bound);
    size_t n = ZSTD_compress(out.data(), bound, src, src_size, ZSTD_LEVEL);
    if (ZSTD_isError(n)) return {};
    out.resize(n);
    return out;
}

static std::vector<uint8_t> zstd_decompress(const uint8_t* src, size_t src_size, size_t max_out = 0) {
    if (src_size == 0) return {};
    size_t content = ZSTD_getFrameContentSize(src, src_size);
    if (content == ZSTD_CONTENTSIZE_ERROR || content == ZSTD_CONTENTSIZE_UNKNOWN) return {};
    if (max_out > 0 && content > max_out) return {};
    std::vector<uint8_t> out(content);
    size_t n = ZSTD_decompress(out.data(), out.size(), src, src_size);
    if (ZSTD_isError(n)) return {};
    out.resize(n);
    return out;
}

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
static bool has_sse42() {
#if defined(_MSC_VER)
    int info[4] = {};
    __cpuidex(info, 1, 0);
    return (info[2] >> 20) & 1;
#else
    return __builtin_cpu_supports("sse4.2");
#endif
}

static uint32_t crc32c_hw(const void* buf, size_t len, uint32_t crc = 0) {
    const auto* p = static_cast<const uint8_t*>(buf);
    while (len >= 8) {
        crc = (uint32_t)_mm_crc32_u64(crc, *reinterpret_cast<const uint64_t*>(p));
        p += 8; len -= 8;
    }
    if (len >= 4) {
        crc = _mm_crc32_u32(crc, *reinterpret_cast<const uint32_t*>(p));
        p += 4; len -= 4;
    }
    if (len >= 2) {
        crc = _mm_crc32_u16(crc, *reinterpret_cast<const uint16_t*>(p));
        p += 2; len -= 2;
    }
    if (len) crc = _mm_crc32_u8(crc, *p);
    return crc;
}
#endif

static uint32_t crc32c_sw(const void* buf, size_t len, uint32_t crc = 0) {
    init_crc32c();
    crc = ~crc;
    auto* p = static_cast<const uint8_t*>(buf);
    while (len--) crc = table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

static uint32_t crc32c(const void* buf, size_t len, uint32_t crc = 0) {
#if defined(__x86_64__) || defined(_M_X64)
    static bool hw = has_sse42();
    return hw ? crc32c_hw(buf, len, crc) : crc32c_sw(buf, len);
#else
    return crc32c_sw(buf, len);
#endif
}

template<typename T>
static void write_le(std::ofstream& os, T val) {
    if constexpr (std::endian::native == std::endian::big) {
        if constexpr (sizeof(T) == 2) val = __builtin_bswap16(val);
        else if constexpr (sizeof(T) == 4) val = __builtin_bswap32(val);
        else if constexpr (sizeof(T) == 8) val = __builtin_bswap64(val);
    }
    os.write(reinterpret_cast<const char*>(&val), sizeof(T));
}

template<typename T>
static T read_le(std::ifstream& is) {
    T val;
    is.read(reinterpret_cast<char*>(&val), sizeof(T));
    if constexpr (std::endian::native == std::endian::big) {
        if constexpr (sizeof(T) == 2) val = __builtin_bswap16(val);
        else if constexpr (sizeof(T) == 4) val = __builtin_bswap32(val);
        else if constexpr (sizeof(T) == 8) val = __builtin_bswap64(val);
    }
    return val;
}

// ---- Bit packing helpers ----

static void pack_bits(const int64_t* values, size_t n, int64_t min_val, uint8_t bits, std::vector<uint8_t>& out) {
    if (n == 0 || bits == 0) return;
    uint64_t mask = (bits == 64) ? ~0ULL : ((1ULL << bits) - 1);
    uint64_t buf = 0;
    int bits_in_buf = 0;
    for (size_t i = 0; i < n; i++) {
        uint64_t v = static_cast<uint64_t>(values[i] - min_val) & mask;
        buf |= v << bits_in_buf;
        bits_in_buf += bits;
        while (bits_in_buf >= 8) {
            out.push_back(static_cast<uint8_t>(buf & 0xFF));
            buf >>= 8;
            bits_in_buf -= 8;
        }
    }
    if (bits_in_buf > 0) {
        out.push_back(static_cast<uint8_t>(buf & 0xFF));
    }
}

static void unpack_bits(int64_t* values, size_t n, int64_t min_val, uint8_t bits, const uint8_t* data) {
    if (n == 0 || bits == 0) return;
    uint64_t mask = (bits == 64) ? ~0ULL : ((1ULL << bits) - 1);
    uint64_t buf = 0;
    int bits_in_buf = 0;
    size_t byte_pos = 0;
    for (size_t i = 0; i < n; i++) {
        while (bits_in_buf < bits) {
            buf |= static_cast<uint64_t>(data[byte_pos++]) << bits_in_buf;
            bits_in_buf += 8;
        }
        values[i] = static_cast<int64_t>(buf & mask) + min_val;
        buf >>= bits;
        bits_in_buf -= bits;
    }
}

// ---- Reed-Solomon Error Correction (GF(256), QR-code style) ----

static constexpr int RS_BLOCK_K = 223;
static constexpr int RS_BLOCK_P = 32;

namespace {

struct GF256 {
    uint8_t exp[512];
    uint8_t log[256];
    GF256() {
        uint8_t v = 1;
        for (int i = 0; i < 255; i++) {
            exp[i] = v;
            log[v] = (uint8_t)i;
            v = (uint8_t)((v << 1) ^ (v & 0x80 ? 0x11D : 0));
        }
        for (int i = 255; i < 512; i++) exp[i] = exp[i - 255];
        exp[255] = exp[0]; log[0] = 0;
    }
    uint8_t mul(uint8_t a, uint8_t b) const {
        if (a == 0 || b == 0) return 0;
        return exp[(uint16_t)log[a] + (uint16_t)log[b]];
    }
    uint8_t inv(uint8_t a) const { return a == 0 ? 0 : exp[255 - log[a]]; }
    uint8_t add(uint8_t a, uint8_t b) const { return a ^ b; }
};

static const GF256 gf;

static std::vector<uint8_t> rs_gen_poly(int p) {
    std::vector<uint8_t> g(p + 1, 0);
    g[0] = 1;
    for (int i = 0; i < p; i++) {
        uint8_t root = gf.exp[i];
        for (int j = p; j > 0; j--)
            g[j] = gf.add(g[j - 1], gf.mul(g[j], root));
        g[0] = gf.mul(g[0], root);
    }
    return g;
}

static void rs_encode_block(const uint8_t* data, int k, int p, const uint8_t* g, uint8_t* parity) {
    std::vector<uint8_t> poly(k + p, 0);
    for (int i = 0; i < k; i++) poly[p + i] = data[i];
    for (int i = k + p - 1; i >= p; i--) {
        uint8_t coeff = poly[i];
        if (coeff == 0) continue;
        for (int j = 0; j <= p; j++)
            poly[i - p + j] ^= gf.mul(coeff, g[j]);
    }
    for (int i = 0; i < p; i++) parity[i] = poly[i];
}

static int rs_decode_block(uint8_t* cw, int k, int p, const uint8_t* g) {
    int n = k + p;
    uint8_t S[256]; bool all_zero = true;
    for (int i = 0; i < p; i++) {
        // C(alpha^i) = sum_{j=0}^{n-1} cw[j] * alpha^(i*j)
        // Horner from highest degree (n-1) down to 0
        uint8_t a = gf.exp[i];
        uint8_t val = 0;
        for (int j = n - 1; j >= 0; j--)
            val = gf.add(gf.mul(val, a), cw[j]);
        S[i] = val;
        if (val) all_zero = false;
    }
    if (all_zero) return 0;

    uint8_t C[256] = {0}, B[256] = {0};
    C[0] = 1; B[0] = 1;
    int L = 0, m = 1; uint8_t b = 1;
    for (int r = 0; r < p; r++) {
        uint8_t d = S[r];
        for (int i = 1; i <= L && i <= r; i++) d ^= gf.mul(C[i], S[r - i]);
        if (d == 0) { m++; continue; }
        uint8_t fb = gf.mul(d, gf.inv(b));
        if (2 * L <= r) {
            uint8_t T[256]; memcpy(T, C, sizeof(T));
            for (int j = m; j < 256; j++) C[j] ^= gf.mul(fb, B[j - m]);
            L = r + 1 - L; memcpy(B, T, sizeof(T)); b = d; m = 1;
        } else {
            for (int j = m; j < 256; j++) C[j] ^= gf.mul(fb, B[j - m]);
            m++;
        }
    }

    int degC = 0;
    for (int i = 0; i < 256; i++) if (C[i]) degC = i;
    if (degC > p / 2) return -1;

    int n_errors = 0, error_pos[256];
    for (int j = 0; j < n; j++) {
        uint8_t x = gf.exp[255 - j];
        uint8_t val = 0, xpow = 1;
        for (int i = 0; i <= degC; i++) { val ^= gf.mul(C[i], xpow); xpow = gf.mul(xpow, x); }
        if (val == 0) { error_pos[n_errors++] = j; }
    }
    if (n_errors != degC) return -1;

    uint8_t Omega[256] = {0};
    for (int i = 0; i < p; i++) {
        if (S[i] == 0) continue;
        for (int j = 0; j <= degC && i + j < p; j++) Omega[i + j] ^= gf.mul(S[i], C[j]);
    }

    uint8_t Cder[256] = {0};
    for (int i = 1; i <= degC; i += 2) Cder[i - 1] = C[i];

    for (int ei = 0; ei < n_errors; ei++) {
        int pos = error_pos[ei];
        uint8_t x_inv = gf.exp[(255 - pos) % 255];
        uint8_t Omega_val = 0, xpow = 1;
        for (int i = 0; i < p; i++) { Omega_val ^= gf.mul(Omega[i], xpow); xpow = gf.mul(xpow, x_inv); }
        uint8_t Cder_val = 0; xpow = 1;
        for (int i = 0; i < degC; i++) { Cder_val ^= gf.mul(Cder[i], xpow); xpow = gf.mul(xpow, x_inv); }
        if (Cder_val == 0) return -1;
        // Forney: e_pos = alpha^pos * Omega(alpha^(-pos)) / Lambda'(alpha^(-pos))
        cw[pos] ^= gf.mul(gf.exp[pos], gf.mul(Omega_val, gf.inv(Cder_val)));
    }
    return n_errors;
}

} // anonymous namespace

// ---- QR-code-style block interleaving ----
// Splits data into RS blocks, then interleaves bytes from all blocks (data + parity)
// so a burst error spreads evenly across all blocks.

constexpr size_t RS_TOTAL = RS_BLOCK_K + RS_BLOCK_P; // 255

static std::vector<uint8_t> rs_encode_interleaved(const uint8_t* data, size_t len) {
    if (len == 0) return {};
    auto gen = rs_gen_poly(RS_BLOCK_P);
    size_t nb = (len + RS_BLOCK_K - 1) / RS_BLOCK_K;
    std::vector<std::vector<uint8_t>> blocks(nb, std::vector<uint8_t>(RS_TOTAL, 0));
    for (size_t bi = 0; bi < nb; bi++) {
        size_t start = bi * RS_BLOCK_K;
        size_t count = (std::min)(len - start, (size_t)RS_BLOCK_K);
        // codeword = [parity(0..31)][data(0..222)] → cw[0] = coeff of x^0
        memcpy(blocks[bi].data() + RS_BLOCK_P, data + start, count);
        rs_encode_block(blocks[bi].data() + RS_BLOCK_P, RS_BLOCK_K, RS_BLOCK_P, gen.data(),
                        blocks[bi].data());
    }
    // Interleave: col-major output
    std::vector<uint8_t> out(nb * RS_TOTAL);
    for (int j = 0; j < RS_TOTAL; j++)
        for (size_t i = 0; i < nb; i++)
            out[j * nb + i] = blocks[i][j];
    return out;
}

static bool rs_decode_interleaved(uint8_t* out, size_t orig_len,
                                  const uint8_t* interleaved, size_t interleaved_len) {
    if (interleaved_len < RS_TOTAL) return false;
    size_t nb = interleaved_len / RS_TOTAL;
    if (nb * RS_TOTAL != interleaved_len) return false;
    auto gen = rs_gen_poly(RS_BLOCK_P);
    std::vector<uint8_t> cw(RS_TOTAL);
    bool ok = true;
    for (size_t bi = 0; bi < nb; bi++) {
        for (int j = 0; j < RS_TOTAL; j++)
            cw[j] = interleaved[j * nb + bi];
        int n = rs_decode_block(cw.data(), RS_BLOCK_K, RS_BLOCK_P, gen.data());
        if (n < 0) { ok = false; continue; }
        size_t start = bi * RS_BLOCK_K;
        size_t count = (std::min)(orig_len - start, (size_t)RS_BLOCK_K);
        memcpy(out + start, cw.data() + RS_BLOCK_P, count);
    }
    return ok;
}

// ---- Writer Implementation ----

struct Writer::Impl {
    std::ofstream file;
};

Writer::Writer(std::string path, size_t chunk_size, bool use_rs, Codec codec)
    : path_(std::move(path)), chunk_size_(chunk_size), use_rs_(use_rs), codec_(codec) {
    impl_ = std::make_unique<Impl>();
    impl_->file.open(path_, std::ios::binary);
    if (!impl_->file.is_open())
        throw std::runtime_error("zepto: cannot open " + path_);
    // VERSION 3 header: magic(4) + version(2) + num_columns(2) + total_rows(8) + num_chunks(4) + meta_size(4) + flags(4) = 28
    write_le<uint32_t>(impl_->file, MAGIC);
    write_le<uint16_t>(impl_->file, VERSION);
    write_le<uint16_t>(impl_->file, 0); // num_columns placeholder
    write_le<uint64_t>(impl_->file, 0); // total_rows placeholder
    write_le<uint32_t>(impl_->file, 0); // num_chunks placeholder
    write_le<uint32_t>(impl_->file, 0); // meta_size placeholder
    uint32_t flags = 0;
    if (use_rs_) flags |= FLAG_RS_ECC;
    if (codec_ == Codec::LZ4) flags |= FLAG_LZ4;
    if (codec_ == Codec::ZSTD) flags |= FLAG_ZSTD;
    write_le<uint32_t>(impl_->file, flags); // flags
}

Writer::~Writer() { close(); }

Writer::Writer(Writer&&) noexcept = default;
Writer& Writer::operator=(Writer&&) noexcept = default;

bool Writer::add_column(const std::string& name, ColumnType type, bool nullable, Encoding encoding) {
    if (closed_) return false;
    col_meta_.push_back({type, nullable, name, encoding});
    return true;
}

bool Writer::append_row(const std::vector<Value>& row) {
    if (closed_) return false;
    if (row.size() != col_meta_.size()) return false;
    if (!metadata_written_) {
        write_metadata();
    }
    if (pending_.empty()) {
        pending_.resize(col_meta_.size());
    }
    for (size_t i = 0; i < row.size(); i++) {
        pending_[i].push_back(row[i]);
        if (std::holds_alternative<std::monostate>(row[i])) continue;
        switch (col_meta_[i].type) {
            case ColumnType::I32: pending_bytes_ += 4; break;
            case ColumnType::I64: pending_bytes_ += 8; break;
            case ColumnType::F32: pending_bytes_ += 4; break;
            case ColumnType::F64: pending_bytes_ += 8; break;
            case ColumnType::STRING: pending_bytes_ += 4 + std::get<std::string>(row[i]).size(); break;
        }
    }
    total_rows_++;
    if (pending_bytes_ >= chunk_size_) flush_chunk();
    return true;
}

bool Writer::begin_batch() {
    if (closed_) return false;
    if (!metadata_written_) write_metadata();
    if (pending_.empty()) pending_.resize(col_meta_.size());
    return true;
}

bool Writer::push_value(size_t col_idx, Value val) {
    if (col_idx >= col_meta_.size()) return false;
    pending_[col_idx].push_back(std::move(val));
    auto& v = pending_[col_idx].back();
    if (!std::holds_alternative<std::monostate>(v)) {
        switch (col_meta_[col_idx].type) {
            case ColumnType::I32: pending_bytes_ += 4; break;
            case ColumnType::I64: pending_bytes_ += 8; break;
            case ColumnType::F32: pending_bytes_ += 4; break;
            case ColumnType::F64: pending_bytes_ += 8; break;
            case ColumnType::STRING: pending_bytes_ += 4 + std::get<std::string>(v).size(); break;
        }
    }
    return true;
}

bool Writer::end_batch() {
    total_rows_++;
    if (pending_bytes_ >= chunk_size_) flush_chunk();
    return true;
}

bool Writer::finish_batch(size_t num_rows) {
    total_rows_ += num_rows;
    if (pending_bytes_ >= chunk_size_) flush_chunk();
    return true;
}

bool Writer::push_col_i32(size_t col_idx, const int32_t* vals, size_t count) {
    if (col_idx >= col_meta_.size()) return false;
    auto& col = pending_[col_idx];
    col.reserve(col.size() + count);
    for (size_t i = 0; i < count; i++)
        col.push_back(vals[i]);
    pending_bytes_ += count * 4;
    return true;
}

bool Writer::push_col_i64(size_t col_idx, const int64_t* vals, size_t count) {
    if (col_idx >= col_meta_.size()) return false;
    auto& col = pending_[col_idx];
    col.reserve(col.size() + count);
    for (size_t i = 0; i < count; i++)
        col.push_back(vals[i]);
    pending_bytes_ += count * 8;
    return true;
}

bool Writer::push_col_f32(size_t col_idx, const float* vals, size_t count) {
    if (col_idx >= col_meta_.size()) return false;
    auto& col = pending_[col_idx];
    col.reserve(col.size() + count);
    for (size_t i = 0; i < count; i++)
        col.push_back(vals[i]);
    pending_bytes_ += count * 4;
    return true;
}

bool Writer::push_col_f64(size_t col_idx, const double* vals, size_t count) {
    if (col_idx >= col_meta_.size()) return false;
    auto& col = pending_[col_idx];
    col.reserve(col.size() + count);
    for (size_t i = 0; i < count; i++)
        col.push_back(vals[i]);
    pending_bytes_ += count * 8;
    return true;
}

bool Writer::push_col_str(size_t col_idx, const char* const* strs, const size_t* lens, size_t count) {
    if (col_idx >= col_meta_.size()) return false;
    auto& col = pending_[col_idx];
    col.reserve(col.size() + count);
    for (size_t i = 0; i < count; i++) {
        if (strs[i] == nullptr)
            col.push_back(std::monostate{});
        else {
            col.push_back(std::string(strs[i], lens[i]));
            pending_bytes_ += 4 + lens[i];
        }
    }
    return true;
}

void Writer::write_metadata() {
    auto& f = impl_->file;
    std::vector<uint8_t> meta_buf;
    auto w = [&](const void* d, size_t n) {
        auto* p = static_cast<const uint8_t*>(d);
        meta_buf.insert(meta_buf.end(), p, p + n);
    };
    auto wp = [&](const auto& v) { w(&v, sizeof(v)); };

    for (auto& cm : col_meta_) {
        wp((uint8_t)cm.type);
        uint8_t flags = cm.nullable ? COL_NULLABLE : 0;
        wp(flags);
        wp((uint16_t)cm.name.size());
        w(cm.name.data(), cm.name.size());
        wp((uint8_t)cm.encoding);
    }

    uint32_t meta_crc = crc32c(meta_buf.data(), meta_buf.size());
    uint32_t meta_size = (uint32_t)meta_buf.size() + 4;

    f.write(reinterpret_cast<const char*>(meta_buf.data()), (std::streamsize)meta_buf.size());
    write_le<uint32_t>(f, meta_crc);

    // update meta_size and num_columns in header (don't disturb file position)
    auto after_meta = f.tellp();
    f.seekp(6);
    write_le<uint16_t>(f, (uint16_t)col_meta_.size());
    f.seekp(20);
    write_le<uint32_t>(f, meta_size);
    f.seekp(after_meta);

    metadata_written_ = true;
}

void Writer::write_chunk(const std::vector<std::vector<Value>>& cols,
                         const std::vector<ColMeta>& meta,
                         uint32_t idx) {
    auto& f = impl_->file;
    std::vector<uint8_t> buf;

    auto w = [&](const void* d, size_t n) {
        auto* p = static_cast<const uint8_t*>(d);
        buf.insert(buf.end(), p, p + n);
    };
    auto wp = [&](const auto& v) { w(&v, sizeof(v)); };

    struct PageInfo { uint64_t offset; uint32_t comp_size, crc; ZoneMap zm; uint32_t validity_size; };
    std::vector<PageInfo> pages(cols.size());

    wp(MAGIC);
    wp((uint16_t)idx);
    wp((uint16_t)cols.size());
    wp((uint32_t)(cols.empty() ? 0 : (uint32_t)cols[0].size()));
    size_t dir_off_pos = buf.size();
    wp((uint32_t)0);

    auto write_validity = [&](const std::vector<Value>& col_data) -> std::vector<uint8_t> {
        std::vector<uint8_t> bm;
        if (col_data.empty()) return bm;
        size_t n = col_data.size();
        size_t bytes = (n + 7) / 8;
        bm.resize(bytes, 0);
        for (size_t i = 0; i < n; i++) {
            if (!is_null(col_data[i]))
                bm[i / 8] |= (uint8_t)(1 << (i % 8));
        }
        return bm;
    };

    for (size_t ci = 0; ci < cols.size(); ci++) {
        pages[ci].offset = buf.size();
        ZoneMap& zm = pages[ci].zm;
        auto& col_data = cols[ci];
        Encoding enc = meta[ci].encoding;
        bool nullable = meta[ci].nullable;

        if (nullable) {
            auto validity = write_validity(col_data);
            pages[ci].validity_size = (uint32_t)validity.size();
            w(validity.data(), validity.size());
        } else {
            pages[ci].validity_size = 0;
        }

        switch (meta[ci].type) {
        case ColumnType::I32:
            if (enc == Encoding::BIT_PACKED) {
                int32_t cmin = INT32_MAX, cmax = INT32_MIN;
                for (auto& v : col_data) {
                    if (is_null(v)) continue;
                    int32_t val = std::get<int32_t>(v);
                    if (val < cmin) cmin = val;
                    if (val > cmax) cmax = val;
                }
                int64_t range = (int64_t)cmax - (int64_t)cmin;
                uint8_t bits = range == 0 ? 1 : (uint8_t)(std::bit_width((uint64_t)range));
                if (bits > 32) bits = 32;
                wp(cmin);
                wp(bits);
                std::vector<int64_t> tmp;
                tmp.reserve(col_data.size());
                for (auto& v : col_data) {
                    if (!is_null(v)) tmp.push_back(std::get<int32_t>(v));
                }
                pack_bits(tmp.data(), tmp.size(), cmin, bits, buf);
                zm.has_min = true; zm.min_i64 = cmin;
                zm.has_max = true; zm.max_i64 = cmax;
            } else {
                for (auto& v : col_data) {
                    if (is_null(v)) continue;
                    int32_t val = std::get<int32_t>(v);
                    wp(val);
                    if (!zm.has_min || val < zm.min_i64) { zm.min_i64 = val; zm.has_min = true; }
                    if (!zm.has_max || val > zm.max_i64) { zm.max_i64 = val; zm.has_max = true; }
                }
            }
            break;
        case ColumnType::I64:
            if (enc == Encoding::BIT_PACKED) {
                int64_t cmin = INT64_MAX, cmax = INT64_MIN;
                for (auto& v : col_data) {
                    if (is_null(v)) continue;
                    int64_t val = std::get<int64_t>(v);
                    if (val < cmin) cmin = val;
                    if (val > cmax) cmax = val;
                }
                uint64_t range = (uint64_t)(cmax - cmin);
                uint8_t bits = range == 0 ? 1 : (uint8_t)(std::bit_width(range));
                if (bits > 64) bits = 64;
                wp(cmin);
                wp(bits);
                std::vector<int64_t> tmp;
                tmp.reserve(col_data.size());
                for (auto& v : col_data) {
                    if (!is_null(v)) tmp.push_back(std::get<int64_t>(v));
                }
                pack_bits(tmp.data(), tmp.size(), cmin, bits, buf);
                zm.has_min = true; zm.min_i64 = cmin;
                zm.has_max = true; zm.max_i64 = cmax;
            } else {
                for (auto& v : col_data) {
                    if (is_null(v)) continue;
                    int64_t val = std::get<int64_t>(v);
                    wp(val);
                    if (!zm.has_min || val < zm.min_i64) { zm.min_i64 = val; zm.has_min = true; }
                    if (!zm.has_max || val > zm.max_i64) { zm.max_i64 = val; zm.has_max = true; }
                }
            }
            break;
        case ColumnType::F32:
            for (auto& v : col_data) {
                if (is_null(v)) continue;
                float val = std::get<float>(v);
                wp(val);
                if (!zm.has_min || val < zm.min_f64) { zm.min_f64 = val; zm.has_min = true; }
                if (!zm.has_max || val > zm.max_f64) { zm.max_f64 = val; zm.has_max = true; }
            }
            break;
        case ColumnType::F64:
            for (auto& v : col_data) {
                if (is_null(v)) continue;
                double val = std::get<double>(v);
                wp(val);
                if (!zm.has_min || val < zm.min_f64) { zm.min_f64 = val; zm.has_min = true; }
                if (!zm.has_max || val > zm.max_f64) { zm.max_f64 = val; zm.has_max = true; }
            }
            break;
        case ColumnType::STRING:
            if (enc == Encoding::DICT) {
                std::vector<std::string> dict;
                std::unordered_map<std::string, uint32_t> dict_map;
                std::vector<uint32_t> indices;
                indices.reserve(col_data.size());
                for (auto& v : col_data) {
                    if (is_null(v)) { indices.push_back(UINT32_MAX); continue; }
                    const auto& s = std::get<std::string>(v);
                    auto it = dict_map.find(s);
                    uint32_t idx;
                    if (it == dict_map.end()) {
                        idx = (uint32_t)dict.size();
                        dict_map[s] = idx;
                        dict.push_back(s);
                    } else {
                        idx = it->second;
                    }
                    indices.push_back(idx);
                    if (!zm.has_min || s < zm.min_str) { zm.min_str = s; zm.has_min = true; }
                    if (!zm.has_max || s > zm.max_str) { zm.max_str = s; zm.has_max = true; }
                }
                wp((uint32_t)dict.size());
                for (auto& entry : dict) {
                    wp((uint32_t)entry.size());
                    w(entry.data(), entry.size());
                }
                uint8_t bits = dict.size() <= 1 ? 1 : (uint8_t)std::bit_width((uint64_t)dict.size() - 1);
                wp(bits);
                std::vector<int64_t> idx64(indices.begin(), indices.end());
                std::vector<uint8_t> packed;
                pack_bits(idx64.data(), idx64.size(), 0, bits, packed);
                w(packed.data(), packed.size());
            } else {
                for (auto& v : col_data) {
                    if (is_null(v)) continue;
                    const auto& s = std::get<std::string>(v);
                    wp((uint32_t)s.size());
                    w(s.data(), s.size());
                    if (!zm.has_min || s < zm.min_str) { zm.min_str = s; zm.has_min = true; }
                    if (!zm.has_max || s > zm.max_str) { zm.max_str = s; zm.has_max = true; }
                }
            }
            break;
        }
        pages[ci].comp_size = (uint32_t)(buf.size() - pages[ci].offset);
        pages[ci].crc = crc32c(buf.data() + pages[ci].offset, pages[ci].comp_size);
    }

    uint32_t dir_offset = (uint32_t)buf.size();
    for (size_t ci = 0; ci < cols.size(); ci++) {
        auto& pg = pages[ci];
        wp((uint64_t)pg.offset);
        wp(pg.comp_size);
        wp(pg.comp_size);
        wp(pg.crc);
        wp((uint8_t)meta[ci].type);
        wp((uint8_t)meta[ci].encoding);
        wp((uint32_t)cols[ci].size());
        wp(pg.validity_size);
        wp((uint8_t)(pg.zm.has_min ? 1 : 0));
        wp((uint8_t)(pg.zm.has_max ? 1 : 0));
        wp(pg.zm.min_i64);
        wp(pg.zm.max_i64);
        wp(pg.zm.min_f64);
        wp(pg.zm.max_f64);
        wp((uint32_t)pg.zm.min_str.size());
        w(pg.zm.min_str.data(), pg.zm.min_str.size());
        wp((uint32_t)pg.zm.max_str.size());
        w(pg.zm.max_str.data(), pg.zm.max_str.size());
    }

    memcpy(buf.data() + dir_off_pos, &dir_offset, 4);
    uint32_t chunk_crc = crc32c(buf.data(), buf.size());
    wp(chunk_crc);

    uint32_t raw_size = (uint32_t)buf.size();

    if (use_rs_) {
        // VERSION 2-compatible RS encoding
        auto interleaved = rs_encode_interleaved(buf.data(), buf.size());
        f.write(reinterpret_cast<const char*>(buf.data()), 4);
        write_le<uint32_t>(f, raw_size);
        f.write(reinterpret_cast<const char*>(interleaved.data()),
                (std::streamsize)interleaved.size());
    } else if (codec_ == Codec::LZ4) {
        // LZ4 compress the chunk buffer
        auto compressed = lz4_compress(buf.data(), buf.size());
        uint32_t comp_size = (uint32_t)compressed.size();
        f.write(reinterpret_cast<const char*>(buf.data()), 4); // MAGIC
        write_le<uint32_t>(f, raw_size);
        write_le<uint32_t>(f, comp_size);
        f.write(reinterpret_cast<const char*>(compressed.data()),
                (std::streamsize)compressed.size());
    } else if (codec_ == Codec::ZSTD) {
        // ZSTD compress the chunk buffer
        auto compressed = zstd_compress(buf.data(), buf.size());
        if (compressed.empty())
            throw std::runtime_error("zepto: zstd compress failed");
        uint32_t comp_size = (uint32_t)compressed.size();
        f.write(reinterpret_cast<const char*>(buf.data()), 4); // MAGIC
        write_le<uint32_t>(f, raw_size);
        write_le<uint32_t>(f, comp_size);
        f.write(reinterpret_cast<const char*>(compressed.data()),
                (std::streamsize)compressed.size());
    } else {
        // No RS, no compression: write raw chunk buffer
        f.write(reinterpret_cast<const char*>(buf.data()), 4); // MAGIC
        write_le<uint32_t>(f, raw_size);
        write_le<uint32_t>(f, raw_size); // comp_size = raw_size
        f.write(reinterpret_cast<const char*>(buf.data()), raw_size); // full chunk as payload
    }

    // update file header
    f.seekp(0);
    auto fw = [&](const auto& v) { f.write(reinterpret_cast<const char*>(&v), sizeof(v)); };
    fw(MAGIC);
    fw((uint16_t)VERSION);
    fw((uint16_t)meta.size());
    fw((uint64_t)total_rows_);
    fw((uint32_t)chunk_index_);
    // meta_size stays unchanged at offset 20
    f.seekp(0, std::ios::end);
}

void Writer::write_chunk_cols(const std::vector<WriteColArray>& cols,
                              const std::vector<ColMeta>& meta,
                              uint32_t idx) {
    auto& f = impl_->file;
    std::vector<uint8_t> buf;

    auto w = [&](const void* d, size_t n) {
        auto* p = static_cast<const uint8_t*>(d);
        buf.insert(buf.end(), p, p + n);
    };
    auto wp = [&](const auto& v) { w(&v, sizeof(v)); };

    struct PageInfo { uint64_t offset; uint32_t comp_size, crc; ZoneMap zm; uint32_t validity_size; };
    std::vector<PageInfo> pages(cols.size());

    wp(MAGIC);
    wp((uint16_t)idx);
    wp((uint16_t)cols.size());
    wp((uint32_t)(cols.empty() ? 0 : (uint32_t)cols[0].num_values));
    size_t dir_off_pos = buf.size();
    wp((uint32_t)0);

    for (size_t ci = 0; ci < cols.size(); ci++) {
        pages[ci].offset = buf.size();
        ZoneMap& zm = pages[ci].zm;
        auto& ca = cols[ci];
        Encoding enc = meta[ci].encoding;
        bool nullable = meta[ci].nullable;
        size_t nrows = ca.num_values;

        if (nullable && ca.validity) {
            size_t bmp_size = (nrows + 7) / 8;
            pages[ci].validity_size = (uint32_t)bmp_size;
            w(ca.validity, bmp_size);
        } else if (nullable) {
            size_t bmp_size = (nrows + 7) / 8;
            std::vector<uint8_t> all_valid(bmp_size, 0xFF);
            pages[ci].validity_size = (uint32_t)bmp_size;
            w(all_valid.data(), bmp_size);
        } else {
            pages[ci].validity_size = 0;
        }

        auto get_valid = [&](size_t i) -> bool {
            if (!ca.validity) return true;
            return (ca.validity[i / 8] >> (i % 8)) & 1;
        };

        switch (meta[ci].type) {
        case ColumnType::I32: {
            const int32_t* vals = reinterpret_cast<const int32_t*>(ca.data);
            if (enc == Encoding::BIT_PACKED) {
                int32_t cmin = INT32_MAX, cmax = INT32_MIN;
                for (size_t i = 0; i < nrows; i++) {
                    if (!get_valid(i)) continue;
                    if (vals[i] < cmin) cmin = vals[i];
                    if (vals[i] > cmax) cmax = vals[i];
                }
                int64_t range = (int64_t)cmax - (int64_t)cmin;
                uint8_t bits = range == 0 ? 1 : (uint8_t)(std::bit_width((uint64_t)range));
                if (bits > 32) bits = 32;
                wp(cmin);
                wp(bits);
                std::vector<int64_t> tmp;
                tmp.reserve(nrows);
                for (size_t i = 0; i < nrows; i++)
                    if (get_valid(i)) tmp.push_back(vals[i]);
                pack_bits(tmp.data(), tmp.size(), cmin, bits, buf);
                zm.has_min = true; zm.min_i64 = cmin;
                zm.has_max = true; zm.max_i64 = cmax;
            } else {
                for (size_t i = 0; i < nrows; i++) {
                    if (!get_valid(i)) continue;
                    int32_t val = vals[i];
                    wp(val);
                    if (!zm.has_min || val < zm.min_i64) { zm.min_i64 = val; zm.has_min = true; }
                    if (!zm.has_max || val > zm.max_i64) { zm.max_i64 = val; zm.has_max = true; }
                }
            }
            break;
        }
        case ColumnType::I64: {
            const int64_t* vals = reinterpret_cast<const int64_t*>(ca.data);
            if (enc == Encoding::BIT_PACKED) {
                int64_t cmin = INT64_MAX, cmax = INT64_MIN;
                for (size_t i = 0; i < nrows; i++) {
                    if (!get_valid(i)) continue;
                    if (vals[i] < cmin) cmin = vals[i];
                    if (vals[i] > cmax) cmax = vals[i];
                }
                uint64_t range = (uint64_t)(cmax - cmin);
                uint8_t bits = range == 0 ? 1 : (uint8_t)(std::bit_width(range));
                if (bits > 64) bits = 64;
                wp(cmin);
                wp(bits);
                std::vector<int64_t> tmp;
                tmp.reserve(nrows);
                for (size_t i = 0; i < nrows; i++)
                    if (get_valid(i)) tmp.push_back(vals[i]);
                pack_bits(tmp.data(), tmp.size(), cmin, bits, buf);
                zm.has_min = true; zm.min_i64 = cmin;
                zm.has_max = true; zm.max_i64 = cmax;
            } else {
                for (size_t i = 0; i < nrows; i++) {
                    if (!get_valid(i)) continue;
                    int64_t val = vals[i];
                    wp(val);
                    if (!zm.has_min || val < zm.min_i64) { zm.min_i64 = val; zm.has_min = true; }
                    if (!zm.has_max || val > zm.max_i64) { zm.max_i64 = val; zm.has_max = true; }
                }
            }
            break;
        }
        case ColumnType::F32: {
            const float* vals = reinterpret_cast<const float*>(ca.data);
            for (size_t i = 0; i < nrows; i++) {
                if (!get_valid(i)) continue;
                float val = vals[i];
                wp(val);
                if (!zm.has_min || val < zm.min_f64) { zm.min_f64 = val; zm.has_min = true; }
                if (!zm.has_max || val > zm.max_f64) { zm.max_f64 = val; zm.has_max = true; }
            }
            break;
        }
        case ColumnType::F64: {
            const double* vals = reinterpret_cast<const double*>(ca.data);
            for (size_t i = 0; i < nrows; i++) {
                if (!get_valid(i)) continue;
                double val = vals[i];
                wp(val);
                if (!zm.has_min || val < zm.min_f64) { zm.min_f64 = val; zm.has_min = true; }
                if (!zm.has_max || val > zm.max_f64) { zm.max_f64 = val; zm.has_max = true; }
            }
            break;
        }
        case ColumnType::STRING: {
            if (enc == Encoding::DICT) {
                std::vector<std::string> dict;
                std::unordered_map<std::string, uint32_t> dict_map;
                std::vector<uint32_t> indices(nrows, UINT32_MAX);
                const uint8_t* sptr = ca.data;

                for (size_t i = 0; i < nrows; i++) {
                    uint32_t slen;
                    memcpy(&slen, sptr, 4); sptr += 4;
                    if (get_valid(i)) {
                        std::string s((const char*)sptr, slen);
                        auto it = dict_map.find(s);
                        uint32_t dict_idx;
                        if (it == dict_map.end()) {
                            dict_idx = (uint32_t)dict.size();
                            dict_map[s] = dict_idx;
                            dict.push_back(std::move(s));
                        } else {
                            dict_idx = it->second;
                        }
                        indices[i] = dict_idx;

                        auto& zs = dict[dict_idx];
                        if (!zm.has_min || zs < zm.min_str) { zm.min_str = zs; zm.has_min = true; }
                        if (!zm.has_max || zs > zm.max_str) { zm.max_str = zs; zm.has_max = true; }
                    }
                    sptr += slen;
                }
                wp((uint32_t)dict.size());
                for (auto& entry : dict) {
                    wp((uint32_t)entry.size());
                    w(entry.data(), entry.size());
                }
                uint8_t bits = dict.size() <= 1 ? 1 : (uint8_t)std::bit_width((uint64_t)dict.size() - 1);
                wp(bits);
                std::vector<int64_t> idx64(indices.begin(), indices.end());
                std::vector<uint8_t> packed;
                pack_bits(idx64.data(), idx64.size(), 0, bits, packed);
                w(packed.data(), packed.size());
            } else {
                const uint8_t* sptr = ca.data;
                for (size_t i = 0; i < nrows; i++) {
                    uint32_t slen;
                    memcpy(&slen, sptr, 4); sptr += 4;
                    if (get_valid(i)) {
                        wp(slen);
                        w(sptr, slen);
                        std::string s((const char*)sptr, slen);
                        if (!zm.has_min || s < zm.min_str) { zm.min_str = s; zm.has_min = true; }
                        if (!zm.has_max || s > zm.max_str) { zm.max_str = s; zm.has_max = true; }
                    }
                    sptr += slen;
                }
            }
            break;
        }
        }
        pages[ci].comp_size = (uint32_t)(buf.size() - pages[ci].offset);
        pages[ci].crc = crc32c(buf.data() + pages[ci].offset, pages[ci].comp_size);
    }

    uint32_t dir_offset = (uint32_t)buf.size();
    for (size_t ci = 0; ci < cols.size(); ci++) {
        auto& pg = pages[ci];
        wp((uint64_t)pg.offset);
        wp(pg.comp_size);
        wp(pg.comp_size);
        wp(pg.crc);
        wp((uint8_t)meta[ci].type);
        wp((uint8_t)meta[ci].encoding);
        wp((uint32_t)cols[ci].num_values);
        wp(pg.validity_size);
        wp((uint8_t)(pg.zm.has_min ? 1 : 0));
        wp((uint8_t)(pg.zm.has_max ? 1 : 0));
        wp(pg.zm.min_i64);
        wp(pg.zm.max_i64);
        wp(pg.zm.min_f64);
        wp(pg.zm.max_f64);
        wp((uint32_t)pg.zm.min_str.size());
        w(pg.zm.min_str.data(), pg.zm.min_str.size());
        wp((uint32_t)pg.zm.max_str.size());
        w(pg.zm.max_str.data(), pg.zm.max_str.size());
    }

    memcpy(buf.data() + dir_off_pos, &dir_offset, 4);
    uint32_t chunk_crc = crc32c(buf.data(), buf.size());
    wp(chunk_crc);

    uint32_t raw_size = (uint32_t)buf.size();

    if (use_rs_) {
        auto interleaved = rs_encode_interleaved(buf.data(), buf.size());
        f.write(reinterpret_cast<const char*>(buf.data()), 4);
        write_le<uint32_t>(f, raw_size);
        f.write(reinterpret_cast<const char*>(interleaved.data()),
                (std::streamsize)interleaved.size());
    } else if (codec_ == Codec::LZ4) {
        auto compressed = lz4_compress(buf.data(), buf.size());
        uint32_t comp_size = (uint32_t)compressed.size();
        f.write(reinterpret_cast<const char*>(buf.data()), 4);
        write_le<uint32_t>(f, raw_size);
        write_le<uint32_t>(f, comp_size);
        f.write(reinterpret_cast<const char*>(compressed.data()),
                (std::streamsize)compressed.size());
    } else if (codec_ == Codec::ZSTD) {
        auto compressed = zstd_compress(buf.data(), buf.size());
        if (compressed.empty())
            throw std::runtime_error("zepto: zstd compress failed");
        uint32_t comp_size = (uint32_t)compressed.size();
        f.write(reinterpret_cast<const char*>(buf.data()), 4);
        write_le<uint32_t>(f, raw_size);
        write_le<uint32_t>(f, comp_size);
        f.write(reinterpret_cast<const char*>(compressed.data()),
                (std::streamsize)compressed.size());
    } else {
        f.write(reinterpret_cast<const char*>(buf.data()), 4);
        write_le<uint32_t>(f, raw_size);
        write_le<uint32_t>(f, raw_size);
        f.write(reinterpret_cast<const char*>(buf.data()), raw_size);
    }

    f.seekp(0);
    auto fw = [&](const auto& v) { f.write(reinterpret_cast<const char*>(&v), sizeof(v)); };
    fw(MAGIC);
    fw((uint16_t)VERSION);
    fw((uint16_t)meta.size());
    fw((uint64_t)total_rows_);
    fw((uint32_t)chunk_index_);
    f.seekp(0, std::ios::end);
}

bool Writer::write_columns(const std::vector<WriteColArray>& cols, int num_rows) {
    if (closed_) return false;
    if (!metadata_written_) write_metadata();
    if (cols.empty()) return false;
    total_rows_ += num_rows;
    uint32_t idx = chunk_index_++;
    write_chunk_cols(cols, col_meta_, idx);
    return true;
}

uint64_t Writer::flush_chunk() {
    if (pending_.empty() || pending_[0].empty()) return 0;
    write_chunk(pending_, col_meta_, chunk_index_++);
    for (auto& col : pending_) col.clear();
    pending_bytes_ = 0;
    return chunk_index_;
}

void Writer::close() {
    if (closed_) return;
    if (!metadata_written_ && !col_meta_.empty()) {
        write_metadata();
    }
    flush_chunk();
    impl_->file.close();
    closed_ = true;
}

// ---- Reader Implementation ----

struct Reader::Impl {
    std::ifstream file;
};

Reader::Reader(std::string path) : path_(std::move(path)) {
    impl_ = std::make_unique<Impl>();
}

Reader::~Reader() { close(); }
Reader::Reader(Reader&&) noexcept = default;
Reader& Reader::operator=(Reader&&) noexcept = default;

bool Reader::open() {
    impl_->file.open(path_, std::ios::binary);
    if (!impl_->file.is_open()) return false;
    return read_header();
}

void Reader::close() {
    if (impl_->file.is_open()) impl_->file.close();
    opened_ = false;
}

bool Reader::read_header() {
    auto& f = impl_->file;
    f.seekg(0);
    uint32_t magic = read_le<uint32_t>(f);
    uint16_t version = read_le<uint16_t>(f);
    uint16_t num_cols_hdr = read_le<uint16_t>(f);
    total_rows_ = read_le<uint64_t>(f);
    uint32_t num_chunks_hdr = read_le<uint32_t>(f);
    uint32_t meta_size = read_le<uint32_t>(f);
    if (magic != MAGIC || (version != 2 && version != 3)) return false;

    // VERSION 3 has an extra 4-byte flags field at offset 24
    // VERSION 2: RS-ECC was always on
    uint32_t file_flags = 0;
    if (version == 3) {
        file_flags = read_le<uint32_t>(f);
    }
    use_rs_ = (version == 2) ? true : ((file_flags & FLAG_RS_ECC) != 0);
    if (version == 2) {
        codec_ = Codec::NONE;
    } else if (file_flags & FLAG_ZSTD) {
        codec_ = Codec::ZSTD;
    } else if (file_flags & FLAG_LZ4) {
        codec_ = Codec::LZ4;
    } else {
        codec_ = Codec::NONE;
    }

    if (meta_size > 0) {
        size_t meta_data_size = (size_t)meta_size - 4;
        std::vector<uint8_t> meta_buf(meta_data_size);
        f.read(reinterpret_cast<char*>(meta_buf.data()), meta_data_size);
        if (!f) return false;
        uint32_t stored_crc = read_le<uint32_t>(f);
        uint32_t calc_crc = crc32c(meta_buf.data(), meta_data_size);
        if (calc_crc != stored_crc) return false;

        size_t off = 0;
        col_names_.clear();
        col_types_.clear();
        col_nullable_.clear();
        col_encoding_.clear();
        for (uint16_t i = 0; i < num_cols_hdr; i++) {
            if (off + 4 > meta_data_size) return false;
            ColumnType type = (ColumnType)meta_buf[off++];
            uint8_t flags = meta_buf[off++];
            if (off + 2 > meta_data_size) return false;
            uint16_t name_len;
            memcpy(&name_len, meta_buf.data() + off, 2); off += 2;
            if (off + name_len > meta_data_size) return false;
            std::string name((const char*)(meta_buf.data() + off), name_len);
            off += name_len;
            if (off + 1 > meta_data_size) return false;
            Encoding enc = (Encoding)meta_buf[off++];

            col_types_.push_back(type);
            col_nullable_.push_back((flags & COL_NULLABLE) != 0);
            col_names_.push_back(name);
            col_encoding_.push_back(enc);
        }
    }

    for (uint32_t ci = 0; ci < num_chunks_hdr; ci++) {
        ChunkMeta meta;
        if (!read_chunk_meta(meta)) return false;
        chunks_.push_back(std::move(meta));
    }
    opened_ = true;
    return true;
}

bool Reader::read_chunk_meta(ChunkMeta& meta) {
    auto& f = impl_->file;
    meta.chunk_offset = f.tellg();

    // Cap all allocations against the real file size so corrupt length
    // fields can't trigger huge allocations.
    f.seekg(0, std::ios::end);
    auto fsize = (uint64_t)f.tellg();
    f.seekg((std::streamoff)meta.chunk_offset);

    uint32_t magic = read_le<uint32_t>(f);
    if (magic != MAGIC) return false;

    uint32_t raw_size = read_le<uint32_t>(f);
    if (!f) return false;

    uint32_t comp_size;
    size_t header_skip; // bytes already read after chunk offset
    if (use_rs_) {
        size_t nb = (raw_size + RS_BLOCK_K - 1) / RS_BLOCK_K;
        comp_size = (uint32_t)(nb * RS_TOTAL);
        header_skip = 8; // MAGIC(4) + raw_size(4)
    } else {
        comp_size = read_le<uint32_t>(f);
        if (!f) return false;
        header_skip = 12; // MAGIC(4) + raw_size(4) + comp_size(4)
    }

    if (meta.chunk_offset + header_skip + (uint64_t)comp_size > fsize) return false;

    // Read the chunk payload into a buffer
    std::vector<uint8_t> payload(comp_size);
    f.read(reinterpret_cast<char*>(payload.data()), (std::streamsize)comp_size);
    if (!f) return false;

    // Decode to raw chunk buffer
    std::vector<uint8_t> raw;
    if (use_rs_) {
        size_t nb = (raw_size + RS_BLOCK_K - 1) / RS_BLOCK_K;
        size_t padded = nb * RS_BLOCK_K;
        raw.resize(padded);
        if (!rs_decode_interleaved(raw.data(), raw_size, payload.data(), payload.size()))
            return false;
        raw.resize(raw_size);
    } else if (codec_ == Codec::LZ4) {
        raw = lz4_decompress(payload.data(), payload.size(), raw_size);
        if (raw.size() != raw_size) return false;
    } else if (codec_ == Codec::ZSTD) {
        raw = zstd_decompress(payload.data(), payload.size(), raw_size);
        if (raw.size() != raw_size) return false;
    } else {
        raw = std::move(payload);
        if (raw_size > comp_size) return false;
        raw.resize(raw_size);
    }

    // Parse raw buffer
    if (raw.size() < 16) return false;
    memcpy(&magic, raw.data(), 4);
    if (magic != MAGIC) return false;

    meta.chunk_index = 0;
    memcpy(&meta.chunk_index, raw.data() + 4, 2);
    uint16_t num_cols = 0;
    memcpy(&num_cols, raw.data() + 6, 2);
    meta.num_rows = 0;
    memcpy(&meta.num_rows, raw.data() + 8, 4);
    uint32_t dir_offset = 0;
    memcpy(&dir_offset, raw.data() + 12, 4);

    if (dir_offset + 4 > raw.size()) return false;

    auto r32 = [&](size_t p) -> uint32_t { uint32_t v; memcpy(&v, raw.data() + p, 4); return v; };
    auto r64 = [&](size_t p) -> uint64_t { uint64_t v; memcpy(&v, raw.data() + p, 8); return v; };

    size_t dp = dir_offset;
    for (uint16_t i = 0; i < num_cols; i++) {
        if (dp + 64 > raw.size()) return false;
        PageHeader ph{};
        ph.offset = r64(dp); dp += 8;
        ph.compressed_size = r32(dp); dp += 4;
        ph.uncompressed_size = r32(dp); dp += 4;
        ph.crc32c = r32(dp); dp += 4;
        ph.type = (ColumnType)raw[dp++];
        ph.encoding = (Encoding)raw[dp++];
        ph.num_values = r32(dp); dp += 4;
        ph.validity_bitmap_size = r32(dp); dp += 4;

        ZoneMap zm{};
        zm.has_min = raw[dp++] != 0;
        zm.has_max = raw[dp++] != 0;
        zm.min_i64 = r64(dp); dp += 8;
        zm.max_i64 = r64(dp); dp += 8;
        zm.min_f64 = *reinterpret_cast<const double*>(raw.data() + dp); dp += 8;
        zm.max_f64 = *reinterpret_cast<const double*>(raw.data() + dp); dp += 8;
        uint32_t slen = r32(dp); dp += 4;
        if (dp + slen > raw.size()) return false;
        zm.min_str.assign((const char*)(raw.data() + dp), slen); dp += slen;
        slen = r32(dp); dp += 4;
        if (dp + slen > raw.size()) return false;
        zm.max_str.assign((const char*)(raw.data() + dp), slen); dp += slen;

        meta.pages.push_back(ph);
        meta.zone_maps.push_back(zm);
    }
    meta.num_pages = num_cols;

    meta.chunk_size = header_skip + comp_size;
    f.seekg(meta.chunk_offset + (std::streamoff)meta.chunk_size);
    return true;
}

bool Reader::verify_integrity() {
    auto& f = impl_->file;
    auto orig = f.tellg();
    f.clear();
    f.seekg(0, std::ios::end);
    auto size = f.tellg();
    if (size <= 0) return false;
    std::vector<uint8_t> buf((size_t)size);
    f.seekg(0);
    f.read(reinterpret_cast<char*>(buf.data()), size);

    auto r32 = [&](size_t p) -> uint32_t { uint32_t v; memcpy(&v, buf.data() + p, 4); return v; };

    bool ok = true;
    uint32_t meta_size = r32(20);
    size_t pos = 24 + (use_rs_ ? 0 : 4) + meta_size; // skip header + flags (v3 only) + meta
    if (pos >= (size_t)size) return false;

    while (pos + 8 <= (size_t)size) {
        size_t chunk_start = pos;
        if (r32(pos) != MAGIC) { ok = false; break; }

        uint32_t raw_size = r32(pos + 4);
        std::vector<uint8_t> raw;

        if (use_rs_) {
            size_t nb = (raw_size + RS_BLOCK_K - 1) / RS_BLOCK_K;
            size_t interleaved_len = nb * RS_TOTAL;
            size_t chunk_end = pos + 8 + interleaved_len;
            if (chunk_end > (size_t)size) { ok = false; break; }
            size_t padded = nb * RS_BLOCK_K;
            raw.resize(padded);
            if (!rs_decode_interleaved(raw.data(), raw_size, buf.data() + pos + 8, interleaved_len)) {
                ok = false; break;
            }
            raw.resize(raw_size);
            pos = chunk_end;
        } else {
            if (pos + 12 > (size_t)size) { ok = false; break; }
            uint32_t comp_size = r32(pos + 8);
            size_t chunk_end = pos + 12 + comp_size;
            if (chunk_end > (size_t)size) { ok = false; break; }
            if (codec_ == Codec::LZ4) {
                raw = lz4_decompress(buf.data() + pos + 12, comp_size, raw_size);
                if (raw.size() != raw_size) { ok = false; break; }
            } else if (codec_ == Codec::ZSTD) {
                raw = zstd_decompress(buf.data() + pos + 12, comp_size, raw_size);
                if (raw.size() != raw_size) { ok = false; break; }
            } else {
                raw.assign(buf.data() + pos + 12, buf.data() + pos + 12 + raw_size);
            }
            pos = chunk_end;
        }

        if (raw.size() < 16) { ok = false; break; }
        uint32_t stored_crc = 0;
        memcpy(&stored_crc, raw.data() + raw.size() - 4, 4);
        uint32_t calc_crc = crc32c(raw.data(), raw.size() - 4);
        if (calc_crc != stored_crc) { ok = false; break; }
    }

    f.seekg(orig);
    return ok;
}

std::vector<uint8_t> Reader::read_and_decode_chunk(size_t chunk_idx) {
    std::vector<uint8_t> empty;
    if (chunk_idx >= chunks_.size()) return empty;
    auto& meta = chunks_[chunk_idx];
    auto& f = impl_->file;

    f.seekg(0, std::ios::end);
    auto fsize = (uint64_t)f.tellg();
    f.seekg(meta.chunk_offset);
    uint32_t magic = read_le<uint32_t>(f);
    if (magic != MAGIC) return empty;
    uint32_t raw_size = read_le<uint32_t>(f);
    if (!f) return empty;

    if (use_rs_) {
        size_t nb = (raw_size + RS_BLOCK_K - 1) / RS_BLOCK_K;
        size_t interleaved_len = nb * RS_TOTAL;
        if (meta.chunk_offset + 8 + interleaved_len > fsize) return empty;
        std::vector<uint8_t> blob(interleaved_len);
        f.read(reinterpret_cast<char*>(blob.data()), (std::streamsize)blob.size());
        if (!f) return empty;
        size_t padded = nb * RS_BLOCK_K;
        std::vector<uint8_t> chunk_data(padded);
        rs_decode_interleaved(chunk_data.data(), raw_size, blob.data(), blob.size());
        chunk_data.resize(raw_size);
        return chunk_data;
    } else {
        uint32_t comp_size = read_le<uint32_t>(f);
        if (!f) return empty;
        if (meta.chunk_offset + 12 + (uint64_t)comp_size > fsize) return empty;
        std::vector<uint8_t> payload(comp_size);
        f.read(reinterpret_cast<char*>(payload.data()), (std::streamsize)comp_size);
        if (!f) return empty;
        if (codec_ == Codec::LZ4) {
            auto raw = lz4_decompress(payload.data(), payload.size(), raw_size);
            if (raw.size() != raw_size) return empty;
            return raw;
        } else if (codec_ == Codec::ZSTD) {
            auto raw = zstd_decompress(payload.data(), payload.size(), raw_size);
            if (raw.size() != raw_size) return empty;
            return raw;
        } else {
            if (raw_size > comp_size) return empty;
            payload.resize(raw_size);
            return payload;
        }
    }
}

std::vector<Row> Reader::read_chunk(size_t chunk_idx) {
    std::vector<Row> result;
    if (chunk_idx >= chunks_.size()) return result;
    auto& meta = chunks_[chunk_idx];

    auto chunk_data = read_and_decode_chunk(chunk_idx);
    if (chunk_data.empty() || chunk_data.size() < 16) return result;

    // Parse directory from de-interleaved buffer
    uint32_t dir_offset = 0;
    memcpy(&dir_offset, chunk_data.data() + 12, 4);

    struct PageDirEntry {
        uint64_t offset;
        uint32_t comp_size;
        uint32_t crc;
        ColumnType type;
        Encoding encoding;
        uint32_t num_vals;
        uint32_t validity_size;
    };

    std::vector<PageDirEntry> dir;
    size_t dp = dir_offset;
    for (uint32_t i = 0; i < meta.num_pages; i++) {
        PageDirEntry e{};
        if (dp + 64 > chunk_data.size()) return result;
        memcpy(&e.offset, chunk_data.data() + dp, 8); dp += 8;
        memcpy(&e.comp_size, chunk_data.data() + dp, 4); dp += 4;
        dp += 4; // uncompressed_size
        memcpy(&e.crc, chunk_data.data() + dp, 4); dp += 4;
        e.type = (ColumnType)chunk_data[dp++];
        e.encoding = (Encoding)chunk_data[dp++];
        memcpy(&e.num_vals, chunk_data.data() + dp, 4); dp += 4;
        memcpy(&e.validity_size, chunk_data.data() + dp, 4); dp += 4;
        dp += 2; // has_min, has_max
        dp += 8; // min_i64
        dp += 8; // max_i64
        dp += 8; // min_f64
        dp += 8; // max_f64
        uint32_t slen;
        memcpy(&slen, chunk_data.data() + dp, 4); dp += 4;
        dp += slen;
        memcpy(&slen, chunk_data.data() + dp, 4); dp += 4;
        dp += slen;
        dir.push_back(e);
    }

    // Verify chunk CRC (last 4 bytes of raw data)
    uint32_t stored_crc = 0;
    memcpy(&stored_crc, chunk_data.data() + chunk_data.size() - 4, 4);
    uint32_t calc_crc = crc32c(chunk_data.data(), chunk_data.size() - 4);
    if (calc_crc != stored_crc) return result;

    result.reserve(meta.num_rows);

    for (size_t ci = 0; ci < dir.size(); ci++) {
        auto& e = dir[ci];
        if (e.offset + e.comp_size > chunk_data.size()) return result;
        uint8_t* page_start = chunk_data.data() + e.offset;

        uint32_t calc = crc32c(page_start, e.comp_size);
        bool crc_ok = (calc == e.crc);

        size_t off = 0;

        // read validity bitmap
        std::vector<uint8_t> validity;
        if (e.validity_size > 0) {
            validity.assign(page_start, page_start + e.validity_size);
            off += e.validity_size;
        }

        if (result.empty()) {
            result.resize(meta.num_rows);
            for (auto& r : result) {
                r.columns.resize(dir.size());
                for (auto& c : r.columns) c = std::monostate{};
            }
        }

        if (!crc_ok) continue;

        bool dict_read = false;
        std::vector<std::string> dict;

        switch (e.type) {
        case ColumnType::I32:
            if (e.encoding == Encoding::BIT_PACKED) {
                int32_t min_val;
                memcpy(&min_val, page_start + off, 4); off += 4;
                uint8_t bits = page_start[off++];
                size_t non_null_count = 0;
                for (size_t i = 0; i < meta.num_rows; i++) {
                    bool valid = validity.empty() || ((validity[i / 8] >> (i % 8)) & 1);
                    if (valid) non_null_count++;
                }
                std::vector<int64_t> tmp(non_null_count);
                unpack_bits(tmp.data(), non_null_count, min_val, bits, page_start + off);
                size_t vi = 0;
                for (size_t i = 0; i < meta.num_rows; i++) {
                    bool valid = validity.empty() || ((validity[i / 8] >> (i % 8)) & 1);
                    if (valid) {
                        result[i].columns[ci] = (int32_t)tmp[vi++];
                    } else {
                        result[i].columns[ci] = std::monostate{};
                    }
                }
            } else {
                for (size_t i = 0; i < meta.num_rows; i++) {
                    bool valid = validity.empty() || ((validity[i / 8] >> (i % 8)) & 1);
                    if (valid) {
                        int32_t val;
                        memcpy(&val, page_start + off, 4); off += 4;
                        result[i].columns[ci] = val;
                    } else {
                        result[i].columns[ci] = std::monostate{};
                    }
                }
            }
            break;
        case ColumnType::I64:
            if (e.encoding == Encoding::BIT_PACKED) {
                int64_t min_val;
                memcpy(&min_val, page_start + off, 8); off += 8;
                uint8_t bits = page_start[off++];
                size_t non_null_count = 0;
                for (size_t i = 0; i < meta.num_rows; i++) {
                    bool valid = validity.empty() || ((validity[i / 8] >> (i % 8)) & 1);
                    if (valid) non_null_count++;
                }
                std::vector<int64_t> tmp(non_null_count);
                unpack_bits(tmp.data(), non_null_count, min_val, bits, page_start + off);
                size_t vi = 0;
                for (size_t i = 0; i < meta.num_rows; i++) {
                    bool valid = validity.empty() || ((validity[i / 8] >> (i % 8)) & 1);
                    if (valid) {
                        result[i].columns[ci] = tmp[vi++];
                    } else {
                        result[i].columns[ci] = std::monostate{};
                    }
                }
            } else {
                for (size_t i = 0; i < meta.num_rows; i++) {
                    bool valid = validity.empty() || ((validity[i / 8] >> (i % 8)) & 1);
                    if (valid) {
                        int64_t val;
                        memcpy(&val, page_start + off, 8); off += 8;
                        result[i].columns[ci] = val;
                    } else {
                        result[i].columns[ci] = std::monostate{};
                    }
                }
            }
            break;
        case ColumnType::F32:
            for (size_t i = 0; i < meta.num_rows; i++) {
                bool valid = validity.empty() || ((validity[i / 8] >> (i % 8)) & 1);
                if (valid) {
                    float val;
                    memcpy(&val, page_start + off, 4); off += 4;
                    result[i].columns[ci] = val;
                } else {
                    result[i].columns[ci] = std::monostate{};
                }
            }
            break;
        case ColumnType::F64:
            for (size_t i = 0; i < meta.num_rows; i++) {
                bool valid = validity.empty() || ((validity[i / 8] >> (i % 8)) & 1);
                if (valid) {
                    double val;
                    memcpy(&val, page_start + off, 8); off += 8;
                    result[i].columns[ci] = val;
                } else {
                    result[i].columns[ci] = std::monostate{};
                }
            }
            break;
        case ColumnType::STRING:
            if (e.encoding == Encoding::DICT) {
                if (!dict_read) {
                    uint32_t dict_size;
                    memcpy(&dict_size, page_start + off, 4); off += 4;
                    dict.reserve(dict_size);
                    for (uint32_t di = 0; di < dict_size; di++) {
                        uint32_t slen;
                        memcpy(&slen, page_start + off, 4); off += 4;
                        dict.emplace_back((const char*)(page_start + off), slen);
                        off += slen;
                    }
                    dict_read = true;
                }
                uint8_t bits = page_start[off++];
                std::vector<int64_t> tmp(meta.num_rows);
                unpack_bits(tmp.data(), meta.num_rows, 0, bits, page_start + off);
                off += (meta.num_rows * (size_t)bits + 7) / 8;
                for (size_t i = 0; i < meta.num_rows; i++) {
                    bool valid = validity.empty() || ((validity[i / 8] >> (i % 8)) & 1);
                    if (valid) {
                        uint32_t di = (uint32_t)tmp[i];
                        if (di < dict.size())
                            result[i].columns[ci] = dict[di];
                    } else {
                        result[i].columns[ci] = std::monostate{};
                    }
                }
            } else {
                for (size_t i = 0; i < meta.num_rows; i++) {
                    bool valid = validity.empty() || ((validity[i / 8] >> (i % 8)) & 1);
                    if (valid) {
                        uint32_t slen;
                        memcpy(&slen, page_start + off, 4); off += 4;
                        result[i].columns[ci] = std::string((const char*)(page_start + off), slen);
                        off += slen;
                    } else {
                        result[i].columns[ci] = std::monostate{};
                    }
                }
            }
            break;
        }
    }

    return result;
}

std::vector<Reader::ColumnArray> Reader::read_chunk_cols(
    size_t chunk_idx, const std::vector<size_t>& col_indices)
{
    std::vector<ColumnArray> result;
    if (chunk_idx >= chunks_.size()) return result;
    auto& meta = chunks_[chunk_idx];

    auto chunk_data = read_and_decode_chunk(chunk_idx);
    if (chunk_data.empty() || chunk_data.size() < 16) return result;

    uint32_t dir_offset = 0;
    memcpy(&dir_offset, chunk_data.data() + 12, 4);

    struct PageDirEntry {
        uint64_t offset;
        uint32_t comp_size;
        uint32_t crc;
        ColumnType type;
        Encoding encoding;
        uint32_t num_vals;
        uint32_t validity_size;
    };

    std::vector<PageDirEntry> dir;
    size_t dp = dir_offset;
    for (uint32_t i = 0; i < meta.num_pages; i++) {
        PageDirEntry e{};
        if (dp + 64 > chunk_data.size()) return result;
        memcpy(&e.offset, chunk_data.data() + dp, 8); dp += 8;
        memcpy(&e.comp_size, chunk_data.data() + dp, 4); dp += 4;
        dp += 4;
        memcpy(&e.crc, chunk_data.data() + dp, 4); dp += 4;
        e.type = (ColumnType)chunk_data[dp++];
        e.encoding = (Encoding)chunk_data[dp++];
        memcpy(&e.num_vals, chunk_data.data() + dp, 4); dp += 4;
        memcpy(&e.validity_size, chunk_data.data() + dp, 4); dp += 4;
        dp += 2;
        dp += 8;
        dp += 8;
        dp += 8;
        dp += 8;
        uint32_t slen;
        memcpy(&slen, chunk_data.data() + dp, 4); dp += 4;
        dp += slen;
        memcpy(&slen, chunk_data.data() + dp, 4); dp += 4;
        dp += slen;
        dir.push_back(e);
    }

    uint32_t stored_crc = 0;
    memcpy(&stored_crc, chunk_data.data() + chunk_data.size() - 4, 4);
    uint32_t calc_crc = crc32c(chunk_data.data(), chunk_data.size() - 4);
    if (calc_crc != stored_crc) return result;

    result.resize(dir.size());

    for (size_t ci = 0; ci < dir.size(); ci++) {
        if (!col_indices.empty()) {
            bool found = false;
            for (auto idx : col_indices) { if (idx == ci) { found = true; break; } }
            if (!found) continue;
        }

        auto& e = dir[ci];
        auto& ca = result[ci];
        ca.type = e.type;
        ca.num_values = meta.num_rows;

        if (e.offset + e.comp_size > chunk_data.size()) continue;
        uint8_t* page_start = chunk_data.data() + e.offset;

        uint32_t calc = crc32c(page_start, e.comp_size);
        if (calc != e.crc) continue;

        size_t off = 0;

        if (e.validity_size > 0) {
            ca.validity.assign(page_start, page_start + e.validity_size);
            off += e.validity_size;
        } else {
            ca.validity.resize((meta.num_rows + 7) / 8, 0xFF);
        }

        auto get_valid = [&](size_t i) -> bool {
            return ca.validity.empty() || ((ca.validity[i / 8] >> (i % 8)) & 1);
        };

        switch (e.type) {
        case ColumnType::I32: {
            ca.data.resize(meta.num_rows * sizeof(int32_t));
            auto* dst = reinterpret_cast<int32_t*>(ca.data.data());
            if (e.encoding == Encoding::BIT_PACKED) {
                int32_t min_val;
                memcpy(&min_val, page_start + off, 4); off += 4;
                uint8_t bits = page_start[off++];
                size_t non_null_count = 0;
                for (size_t i = 0; i < meta.num_rows; i++)
                    if (get_valid(i)) non_null_count++;
                std::vector<int64_t> tmp(non_null_count);
                unpack_bits(tmp.data(), non_null_count, min_val, bits, page_start + off);
                size_t vi = 0;
                for (size_t i = 0; i < meta.num_rows; i++)
                    dst[i] = get_valid(i) ? (int32_t)tmp[vi++] : 0;
            } else {
                for (size_t i = 0; i < meta.num_rows; i++) {
                    if (get_valid(i)) {
                        memcpy(&dst[i], page_start + off, 4); off += 4;
                    } else {
                        dst[i] = 0;
                    }
                }
            }
            break;
        }
        case ColumnType::I64: {
            ca.data.resize(meta.num_rows * sizeof(int64_t));
            auto* dst = reinterpret_cast<int64_t*>(ca.data.data());
            if (e.encoding == Encoding::BIT_PACKED) {
                int64_t min_val;
                memcpy(&min_val, page_start + off, 8); off += 8;
                uint8_t bits = page_start[off++];
                size_t non_null_count = 0;
                for (size_t i = 0; i < meta.num_rows; i++)
                    if (get_valid(i)) non_null_count++;
                std::vector<int64_t> tmp(non_null_count);
                unpack_bits(tmp.data(), non_null_count, min_val, bits, page_start + off);
                size_t vi = 0;
                for (size_t i = 0; i < meta.num_rows; i++)
                    dst[i] = get_valid(i) ? tmp[vi++] : 0;
            } else {
                for (size_t i = 0; i < meta.num_rows; i++) {
                    if (get_valid(i)) {
                        memcpy(&dst[i], page_start + off, 8); off += 8;
                    } else {
                        dst[i] = 0;
                    }
                }
            }
            break;
        }
        case ColumnType::F32: {
            ca.data.resize(meta.num_rows * sizeof(float));
            auto* dst = reinterpret_cast<float*>(ca.data.data());
            for (size_t i = 0; i < meta.num_rows; i++) {
                if (get_valid(i)) {
                    memcpy(&dst[i], page_start + off, 4); off += 4;
                } else {
                    dst[i] = 0;
                }
            }
            break;
        }
        case ColumnType::F64: {
            ca.data.resize(meta.num_rows * sizeof(double));
            auto* dst = reinterpret_cast<double*>(ca.data.data());
            for (size_t i = 0; i < meta.num_rows; i++) {
                if (get_valid(i)) {
                    memcpy(&dst[i], page_start + off, 8); off += 8;
                } else {
                    dst[i] = 0;
                }
            }
            break;
        }
        case ColumnType::STRING: {
            if (e.encoding == Encoding::DICT) {
                uint32_t dict_size;
                memcpy(&dict_size, page_start + off, 4); off += 4;
                std::vector<std::string> dict(dict_size);
                for (uint32_t di = 0; di < dict_size; di++) {
                    uint32_t slen;
                    memcpy(&slen, page_start + off, 4); off += 4;
                    dict[di].assign((const char*)(page_start + off), slen);
                    off += slen;
                }
                uint8_t bits = page_start[off++];
                std::vector<int64_t> tmp(meta.num_rows);
                unpack_bits(tmp.data(), meta.num_rows, 0, bits, page_start + off);
                off += (meta.num_rows * (size_t)bits + 7) / 8;
                for (size_t i = 0; i < meta.num_rows; i++) {
                    if (get_valid(i) && (uint64_t)tmp[i] < dict.size()) {
                        auto& s = dict[(uint32_t)tmp[i]];
                        uint32_t slen = (uint32_t)s.size();
                        ca.data.insert(ca.data.end(), reinterpret_cast<uint8_t*>(&slen), reinterpret_cast<uint8_t*>(&slen) + 4);
                        ca.data.insert(ca.data.end(), s.begin(), s.end());
                    } else {
                        uint32_t slen = 0;
                        ca.data.insert(ca.data.end(), reinterpret_cast<uint8_t*>(&slen), reinterpret_cast<uint8_t*>(&slen) + 4);
                    }
                }
            } else {
                for (size_t i = 0; i < meta.num_rows; i++) {
                    if (get_valid(i)) {
                        uint32_t slen;
                        memcpy(&slen, page_start + off, 4); off += 4;
                        ca.data.insert(ca.data.end(), reinterpret_cast<uint8_t*>(&slen), reinterpret_cast<uint8_t*>(&slen) + 4);
                        ca.data.insert(ca.data.end(), page_start + off, page_start + off + slen);
                        off += slen;
                    } else {
                        uint32_t slen = 0;
                        ca.data.insert(ca.data.end(), reinterpret_cast<uint8_t*>(&slen), reinterpret_cast<uint8_t*>(&slen) + 4);
                    }
                }
            }
            break;
        }
        }
    }

    return result;
}

ChunkMeta Reader::chunk_meta(size_t chunk_idx) const {
    if (chunk_idx < chunks_.size()) return chunks_[chunk_idx];
    return {};
}

QueryResult Reader::query(const Query& q) {
    QueryResult res;
    res.total_rows = 0;
    res.col_names = col_names_;
    res.col_types = col_types_;

    auto cols = q.select_columns.empty()
        ? std::vector<size_t>(col_names_.size())
        : q.select_columns;
    if (q.select_columns.empty()) {
        std::iota(cols.begin(), cols.end(), 0);
    }

    for (size_t ci = 0; ci < chunks_.size(); ci++) {
        auto& meta = chunks_[ci];

        bool skip = false;
        for (auto& pred : q.predicates) {
            if (pred.col_index >= meta.zone_maps.size()) continue;
            auto& zm = meta.zone_maps[pred.col_index];
            if (!zm.has_min && !zm.has_max) continue;

            auto check = [&](auto val, auto min, auto max) -> bool {
                switch (pred.op) {
                case Query::EQ: return val >= min && val <= max;
                case Query::NE: return true;
                case Query::GT: return max > val;
                case Query::GE: return max >= val;
                case Query::LT: return min < val;
                case Query::LE: return min <= val;
                default: return true;
                }
            };

            if (std::holds_alternative<int64_t>(pred.value)) {
                auto v = std::get<int64_t>(pred.value);
                if (!check(v, zm.min_i64, zm.max_i64)) { skip = true; break; }
            } else if (std::holds_alternative<double>(pred.value)) {
                auto v = std::get<double>(pred.value);
                if (!check(v, zm.min_f64, zm.max_f64)) { skip = true; break; }
            } else if (std::holds_alternative<std::string>(pred.value)) {
                auto& v = std::get<std::string>(pred.value);
                if (zm.has_min && zm.has_max) {
                    auto scheck = [&](const std::string& val, const std::string& mn, const std::string& mx) -> bool {
                        switch (pred.op) {
                        case Query::EQ: return val >= mn && val <= mx;
                        case Query::NE: return true;
                        case Query::GT: return mx > val;
                        case Query::GE: return mx >= val;
                        case Query::LT: return mn < val;
                        case Query::LE: return mn <= val;
                        default: return true;
                        }
                    };
                    if (!scheck(v, zm.min_str, zm.max_str)) { skip = true; break; }
                }
            } else if (std::holds_alternative<int32_t>(pred.value)) {
                auto v = (int64_t)std::get<int32_t>(pred.value);
                if (!check(v, zm.min_i64, zm.max_i64)) { skip = true; break; }
            }
        }
        if (skip) continue;

        auto chunk_rows = read_chunk(ci);
        for (auto& row : chunk_rows) {
            bool match = true;
            for (auto& pred : q.predicates) {
                if (pred.col_index >= row.columns.size()) continue;
                auto& val = row.columns[pred.col_index];

                if (is_null(val)) {
                    if (pred.op == Query::NE) continue;
                    match = false;
                    break;
                }

                auto cmp = [&](auto a, auto b) -> bool {
                    switch (pred.op) {
                    case Query::EQ: return a == b;
                    case Query::NE: return a != b;
                    case Query::GT: return a > b;
                    case Query::GE: return a >= b;
                    case Query::LT: return a < b;
                    case Query::LE: return a <= b;
                    default: return true;
                    }
                };

                bool ok = std::visit([&](auto&& v) -> bool {
                    using T = std::decay_t<decltype(v)>;
                    if constexpr (std::is_same_v<T, std::monostate>) {
                        return false;
                    } else if constexpr (std::is_same_v<T, int32_t>) {
                        if (auto* p = std::get_if<int64_t>(&pred.value)) return cmp((int64_t)v, *p);
                        if (auto* p = std::get_if<double>(&pred.value)) return cmp((double)v, *p);
                        return false;
                    } else if constexpr (std::is_same_v<T, int64_t>) {
                        if (auto* p = std::get_if<int64_t>(&pred.value)) return cmp(v, *p);
                        if (auto* p = std::get_if<double>(&pred.value)) return cmp((double)v, *p);
                        return false;
                    } else if constexpr (std::is_same_v<T, float>) {
                        if (auto* p = std::get_if<double>(&pred.value)) return cmp((double)v, *p);
                        if (auto* p = std::get_if<int64_t>(&pred.value)) return cmp(v, (float)*p);
                        return false;
                    } else if constexpr (std::is_same_v<T, double>) {
                        if (auto* p = std::get_if<double>(&pred.value)) return cmp(v, *p);
                        if (auto* p = std::get_if<int64_t>(&pred.value)) return cmp(v, (double)*p);
                        return false;
                    } else if constexpr (std::is_same_v<T, std::string>) {
                        if (auto* p = std::get_if<std::string>(&pred.value)) return cmp(v, *p);
                        return false;
                    }
                    return false;
                }, val);
                if (!ok) { match = false; break; }
            }
            if (match) {
                Row out;
                for (auto ci : cols) {
                    if (ci < row.columns.size())
                        out.columns.push_back(row.columns[ci]);
                }
                res.rows.push_back(std::move(out));
                res.total_rows++;
                if (q.limit > 0 && res.total_rows >= q.limit + q.offset) break;
            }
        }
        if (q.limit > 0 && res.total_rows >= q.limit + q.offset) break;
    }

    if (q.offset > 0 && q.offset < res.rows.size()) {
        res.rows.erase(res.rows.begin(), res.rows.begin() + q.offset);
        res.total_rows = res.rows.size();
    }
    if (q.limit > 0 && res.rows.size() > q.limit) {
        res.rows.resize(q.limit);
        res.total_rows = q.limit;
    }

    return res;
}

// ---- Row Serialization Helpers ----

static std::vector<uint8_t> row_to_bytes(const Row& r) {
    std::vector<uint8_t> buf;
    auto le = [&](auto v) {
        auto p = reinterpret_cast<const uint8_t*>(&v);
        buf.insert(buf.end(), p, p + sizeof(v));
    };
    uint32_t n = (uint32_t)r.columns.size();
    le(n);
    for (auto& v : r.columns) {
        uint8_t tag = (uint8_t)v.index();
        buf.push_back(tag);
        if (tag == 0) continue;
        std::visit([&](auto&& val) {
            using T = std::decay_t<decltype(val)>;
            if constexpr (std::is_same_v<T, std::monostate>) {}
            else if constexpr (std::is_same_v<T, std::string>) {
                uint32_t len = (uint32_t)val.size();
                le(len);
                buf.insert(buf.end(), val.begin(), val.end());
            } else {
                le(val);
            }
        }, v);
    }
    return buf;
}

static Row bytes_to_row(const uint8_t* data, size_t len) {
    Row r;
    size_t off = 0;
    if (off + 4 > len) return r;
    uint32_t n;
    memcpy(&n, data + off, 4); off += 4;
    r.columns.resize(n);
    for (uint32_t i = 0; i < n; i++) {
        if (off >= len) break;
        uint8_t tag = data[off++];
        switch (tag) {
        case 0: break;
        case 1: { int32_t v; if (off + 4 <= len) { memcpy(&v, data + off, 4); off += 4; r.columns[i] = v; } break; }
        case 2: { int64_t v; if (off + 8 <= len) { memcpy(&v, data + off, 8); off += 8; r.columns[i] = v; } break; }
        case 3: { float v; if (off + 4 <= len) { memcpy(&v, data + off, 4); off += 4; r.columns[i] = v; } break; }
        case 4: { double v; if (off + 8 <= len) { memcpy(&v, data + off, 8); off += 8; r.columns[i] = v; } break; }
        case 5: {
            uint32_t slen;
            if (off + 4 <= len) { memcpy(&slen, data + off, 4); off += 4; }
            if (off + slen <= len) { r.columns[i] = std::string((const char*)(data + off), slen); off += slen; }
            break;
        }
        }
    }
    return r;
}

// ---- Database Implementation ----

Database::~Database() { close(); }

bool Database::open(const std::string& dir) {
    dir_ = dir;
    wal_path_ = dir_ + "/wal";
    current_path_ = dir_ + "/data.zdb";
    snap_dir_ = dir_ + "/snapshots";

    std::error_code ec;
    fs::create_directories(dir_, ec);
    fs::create_directories(snap_dir_, ec);

    wal_stream_.open(wal_path_, std::ios::binary | std::ios::app);
    if (!wal_stream_) return false;
    wal_stream_.seekp(0, std::ios::end);

    recover();
    opened_ = true;
    return true;
}

void Database::close() {
    if (!opened_) return;
    if (dirty_) checkpoint();
    if (wal_stream_.is_open()) wal_stream_.close();
    opened_ = false;
}

bool Database::wal_append(uint8_t op, const uint8_t* payload, uint32_t plen) {
    uint64_t seq = ++wal_seq_;
    std::vector<uint8_t> hdr(17);
    memcpy(hdr.data() + 4, &seq, 8);
    hdr[12] = op;
    memcpy(hdr.data() + 13, &plen, 4);
    uint32_t crc = crc32c(hdr.data() + 4, 13);
    if (plen > 0) crc = crc32c(payload, plen, crc);
    memcpy(hdr.data(), &crc, 4);

    wal_stream_.write((const char*)hdr.data(), 17);
    if (plen > 0) wal_stream_.write((const char*)payload, plen);
    wal_stream_.flush();
    return (bool)wal_stream_;
}

bool Database::wal_append_checkpoint() {
    uint64_t cs = checkpoint_seq_;
    return wal_append((uint8_t)WalOp::CHECKPOINT, (const uint8_t*)&cs, 8);
}

bool Database::recover() {
    rows_.clear();
    schema_.clear();
    wal_seq_ = 0;
    checkpoint_seq_ = 0;

    // Load from checkpoint if exists
    std::ifstream test(current_path_, std::ios::binary);
    bool has_checkpoint = test.is_open();
    test.close();

    if (has_checkpoint) {
        Reader rdr(current_path_);
        if (rdr.open()) {
            for (size_t i = 0; i < rdr.column_types().size(); i++) {
                ColMeta cm;
                cm.type = rdr.column_types()[i];
                cm.nullable = rdr.column_nullable()[i];
                cm.name = rdr.column_names()[i];
                cm.encoding = rdr.column_encoding()[i];
                schema_.push_back(cm);
            }
            for (size_t ci = 0; ci < rdr.num_chunks(); ci++) {
                auto chunk = rdr.read_chunk(ci);
                rows_.insert(rows_.end(), chunk.begin(), chunk.end());
            }
        }
    }

    // Replay WAL
    std::ifstream wal_r(wal_path_, std::ios::binary);
    if (!wal_r) return true;

    uint64_t max_checkpoint_seq = 0;
    std::vector<std::tuple<uint64_t, uint8_t, std::vector<uint8_t>>> entries;

    wal_r.seekg(0, std::ios::end);
    size_t wal_size = (size_t)wal_r.tellg();
    wal_r.seekg(0);

    while ((size_t)wal_r.tellg() + 17 <= wal_size) {
        uint32_t crc, stored_crc;
        uint64_t seq;
        uint8_t op;
        uint32_t plen;

        wal_r.read((char*)&stored_crc, 4);
        wal_r.read((char*)&seq, 8);
        wal_r.read((char*)&op, 1);
        wal_r.read((char*)&plen, 4);

        if (!wal_r) break;

        std::vector<uint8_t> payload(plen);
        if (plen > 0) {
            wal_r.read((char*)payload.data(), plen);
            if (!wal_r) break;
        }

        crc = crc32c(&seq, 13);
        if (plen > 0) crc = crc32c(payload.data(), plen, crc);
        if (crc != stored_crc) break;

        entries.push_back({seq, op, std::move(payload)});

        if (op == (uint8_t)WalOp::CHECKPOINT && plen >= 8) {
            uint64_t cs;
            memcpy(&cs, payload.data(), 8);
            if (cs > max_checkpoint_seq) max_checkpoint_seq = cs;
        }
    }

    // Replay entries after last checkpoint
    for (auto& [seq, op, payload] : entries) {
        if (seq <= max_checkpoint_seq) {
            if (seq > wal_seq_) wal_seq_ = seq;
            continue;
        }
        if (seq > wal_seq_) wal_seq_ = seq;

        if (op == (uint8_t)WalOp::INSERT) {
            Row r = bytes_to_row(payload.data(), payload.size());
            rows_.push_back(r);
            dirty_ = true;
        } else if (op == (uint8_t)WalOp::DELETE && payload.size() >= 8) {
            uint64_t idx;
            memcpy(&idx, payload.data(), 8);
            if (idx < rows_.size()) { rows_.erase(rows_.begin() + idx); dirty_ = true; }
        } else if (op == (uint8_t)WalOp::UPDATE && payload.size() >= 8) {
            uint64_t idx;
            memcpy(&idx, payload.data(), 8);
            Row r = bytes_to_row(payload.data() + 8, payload.size() - 8);
            if (idx < rows_.size()) { rows_[idx] = r; dirty_ = true; }
        } else if (op == (uint8_t)WalOp::CHECKPOINT && payload.size() >= 8) {
            uint64_t cs;
            memcpy(&cs, payload.data(), 8);
            checkpoint_seq_ = cs;
            dirty_ = false;
        }
    }

    return true;
}

static Encoding pick_best_encoding(const std::vector<Value>& col_data, ColumnType type) {
    if (col_data.empty()) return Encoding::PLAIN;
    if (type == ColumnType::STRING) {
        // DICT if few unique values
        std::set<std::string> uniq;
        for (auto& v : col_data) {
            if (auto* s = std::get_if<std::string>(&v)) {
                uniq.insert(*s);
                if (uniq.size() > col_data.size() / 4) return Encoding::PLAIN; // too many unique
            }
        }
        return uniq.size() <= 1 ? Encoding::PLAIN : Encoding::DICT;
    }
    if (type == ColumnType::I32 || type == ColumnType::I64) {
        // BIT_PACKED if range is small
        int64_t lo = INT64_MAX, hi = INT64_MIN;
        int nulls = 0;
        for (auto& v : col_data) {
            if (is_null(v)) { nulls++; continue; }
            int64_t x = 0;
            if (auto* p = std::get_if<int32_t>(&v)) x = *p;
            else if (auto* p = std::get_if<int64_t>(&v)) x = *p;
            else continue;
            if (x < lo) lo = x;
            if (x > hi) hi = x;
        }
        uint64_t range = (uint64_t)(hi - lo);
        int bits = 0;
        if (range > 0) bits = (int)std::bit_width(range);
        // BIT_PACKED saves space if bits <= 24 (3 bytes vs 4/8)
        size_t non_null = col_data.size() - nulls;
        if (bits > 0 && bits <= 24 && non_null > 0) return Encoding::BIT_PACKED;
    }
    return Encoding::PLAIN;
}

bool Database::checkpoint() {
    if (!dirty_ && schema_.empty()) return true;

    // Auto-select best encodings
    std::vector<Encoding> best_encs(schema_.size());
    for (size_t ci = 0; ci < schema_.size(); ci++) {
        if (schema_[ci].encoding == Encoding::PLAIN) {
            // Only auto-select if user didn't explicitly set
            std::vector<Value> col_data;
            col_data.reserve(rows_.size());
            for (auto& row : rows_) {
                col_data.push_back(ci < row.columns.size() ? row.columns[ci] : Value{});
            }
            best_encs[ci] = pick_best_encoding(col_data, schema_[ci].type);
        } else {
            best_encs[ci] = schema_[ci].encoding;
        }
    }

    std::string tmp = current_path_ + ".tmp";

    {
        Writer wtr(tmp, DEFAULT_CHUNK_SIZE, false, Codec::NONE);
        for (size_t ci = 0; ci < schema_.size(); ci++) {
            wtr.add_column(schema_[ci].name, schema_[ci].type, schema_[ci].nullable, best_encs[ci]);
        }
        for (auto& row : rows_) {
            wtr.append_row(row.columns);
        }
        wtr.close();
    }

    std::ifstream tin(tmp, std::ios::binary);
    if (!tin) return false;
    tin.close();

    checkpoint_seq_ = wal_seq_;
    dirty_ = false;

    // Replace old file
    std::error_code ec;
    fs::rename(tmp, current_path_, ec);
    if (ec) return false;

    return wal_append_checkpoint();
}

bool Database::create_snapshot(const std::string& name) {
    if (!checkpoint()) return false;

    std::string snap_path = snap_dir_ + "/" + name + ".zdb";
    std::error_code ec;
    fs::copy_file(current_path_, snap_path, fs::copy_options::overwrite_existing, ec);
    return !ec;
}

bool Database::restore_snapshot(const std::string& name) {
    std::string snap_path = snap_dir_ + "/" + name + ".zdb";

    std::ifstream test(snap_path, std::ios::binary);
    if (!test) return false;
    test.close();

    wal_stream_.close();

    std::error_code ec;
    fs::remove(current_path_, ec);
    fs::copy_file(snap_path, current_path_, ec);
    if (ec) { wal_stream_.open(wal_path_, std::ios::binary | std::ios::app); return false; }

    // Remove WAL so recover() starts fresh
    fs::remove(wal_path_, ec);

    rows_.clear();
    schema_.clear();
    wal_seq_ = 0;
    checkpoint_seq_ = 0;
    dirty_ = false;

    bool ok = recover();

    // Reopen WAL for appending
    wal_stream_.open(wal_path_, std::ios::binary | std::ios::app);
    return ok && wal_stream_.is_open();
}

std::vector<std::string> Database::list_snapshots() {
    std::vector<std::string> names;
    try {
        for (auto& entry : fs::directory_iterator(snap_dir_)) {
            auto name = entry.path().stem().string();
            names.push_back(name);
        }
    } catch (...) {}
    std::sort(names.begin(), names.end());
    return names;
}

// ---- SQL execution ----

std::string Database::to_upper(const std::string& s) {
    std::string r = s;
    for (auto& c : r) c = (char)std::toupper((unsigned char)c);
    return r;
}

std::vector<std::string> Database::tokenize(const std::string& sql) {
    std::vector<std::string> tok;
    std::string cur;
    bool in_str = false;
    char str_q = 0;

    for (size_t i = 0; i < sql.size(); i++) {
        char c = sql[i];
        if (in_str) {
            if (c == str_q) {
                tok.push_back(std::move(cur)); cur.clear(); in_str = false;
            } else {
                cur += c;
            }
            continue;
        }
        if (c == '\'' || c == '"') {
            if (!cur.empty()) { tok.push_back(std::move(cur)); cur.clear(); }
            in_str = true; str_q = c;
            continue;
        }
        if (c == '(' || c == ')' || c == ',' || c == '*' || c == ';') {
            if (!cur.empty()) { tok.push_back(std::move(cur)); cur.clear(); }
            tok.push_back(std::string(1, c));
            continue;
        }
        if (c == '=' || c == '!' || c == '<' || c == '>') {
            if (!cur.empty()) { tok.push_back(std::move(cur)); cur.clear(); }
            if (c == '!' && i + 1 < sql.size() && sql[i+1] == '=') { tok.push_back("!="); i++; }
            else if (c == '<' && i + 1 < sql.size() && sql[i+1] == '=') { tok.push_back("<="); i++; }
            else if (c == '>' && i + 1 < sql.size() && sql[i+1] == '=') { tok.push_back(">="); i++; }
            else if (c == '<' && i + 1 < sql.size() && sql[i+1] == '>') { tok.push_back("<>"); i++; }
            else { tok.push_back(std::string(1, c)); }
            continue;
        }
        if (std::isspace((unsigned char)c)) {
            if (!cur.empty()) { tok.push_back(std::move(cur)); cur.clear(); }
            continue;
        }
        cur += c;
    }
    if (!cur.empty()) tok.push_back(std::move(cur));

    // Merge multi-word keywords (2-token)
    for (size_t i = 0; i + 1 < tok.size();) {
        auto up = [&](const std::string& s) { return to_upper(s); };
        if ((up(tok[i]) == "NOT" && up(tok[i+1]) == "LIKE") ||
            (up(tok[i]) == "ORDER" && up(tok[i+1]) == "BY") ||
            (up(tok[i]) == "GROUP" && up(tok[i+1]) == "BY") ||
            (up(tok[i]) == "IS" && up(tok[i+1]) == "NULL") ||
            (up(tok[i]) == "NOT" && up(tok[i+1]) == "IN")) {
            tok[i] = tok[i] + " " + tok[i+1];
            tok.erase(tok.begin() + i + 1);
        } else {
            i++;
        }
    }
    // Merge 3-token: IS NOT NULL
    for (size_t i = 0; i + 2 < tok.size();) {
        auto up = [&](const std::string& s) { return to_upper(s); };
        if (up(tok[i]) == "IS" && up(tok[i+1]) == "NOT" && up(tok[i+2]) == "NULL") {
            tok[i] = "IS NOT NULL";
            tok.erase(tok.begin() + i + 1);
            tok.erase(tok.begin() + i + 1);
        } else {
            i++;
        }
    }
    return tok;
}

Value Database::parse_value(const std::string& s) {
    if (s == "null" || s == "NULL") return std::monostate{};
    // Empty token is a quoted '' literal, not a number.
    if (!s.empty()) {
        // try int first
        char* end = nullptr;
        long long iv = std::strtoll(s.c_str(), &end, 10);
        if (end && *end == 0) {
            if (iv >= INT32_MIN && iv <= INT32_MAX) return (int32_t)iv;
            return (int64_t)iv;
        }
        // try float
        end = nullptr;
        double fv = std::strtod(s.c_str(), &end);
        if (end && *end == 0) return (double)fv;
    }
    // string
    return s;
}

Database::WhereClause Database::parse_where(const std::vector<std::string>& tok, size_t& pos) {
    WhereClause result;
    ConditionGroup current;
    while (pos < tok.size()) {
        std::string col = tok[pos++];
        if (pos >= tok.size()) break;
        std::string op_raw = tok[pos++];
        std::string op = to_upper(op_raw);

        bool handled = true;

        if (op == "IS NULL") {
            Condition c;
            c.col = resolve_col(col);
            c.op = "IS NULL";
            current.push_back(c);
        } else if (op == "IS NOT NULL") {
            Condition c;
            c.col = resolve_col(col);
            c.op = "IS NOT NULL";
            current.push_back(c);
        } else if (op == "IN" || op == "NOT IN") {
            if (pos >= tok.size() || tok[pos] != "(") break;
            pos++;
            Condition c;
            c.col = resolve_col(col);
            c.op = op;
            while (pos < tok.size() && tok[pos] != ")") {
                c.val_list.push_back(parse_value(tok[pos++]));
                if (pos < tok.size() && tok[pos] == ",") pos++;
            }
            if (pos < tok.size() && tok[pos] == ")") pos++;
            current.push_back(c);
        } else if (op == "BETWEEN") {
            if (pos >= tok.size()) break;
            Value lo = parse_value(tok[pos++]);
            if (pos >= tok.size() || to_upper(tok[pos]) != "AND") break;
            pos++;
            if (pos >= tok.size()) break;
            Value hi = parse_value(tok[pos++]);
            Condition c1, c2;
            c1.col = resolve_col(col); c1.op = ">="; c1.val = lo;
            c2.col = resolve_col(col); c2.op = "<="; c2.val = hi;
            current.push_back(c1);
            current.push_back(c2);
        } else {
            // Standard: op val
            if (pos >= tok.size()) break;
            handled = false;
            bool not_like = false;
            if (op == "NOT LIKE") {
                not_like = true;
                op = "LIKE";
            }
            {
                Value val = parse_value(tok[pos++]);
                Condition c;
                c.col = resolve_col(col);
                c.op = op;
                c.val = val;
                c.not_like = not_like;
                current.push_back(c);
            }
        }
        if (!handled && pos >= tok.size()) break;

        if (pos < tok.size()) {
            std::string kw = to_upper(tok[pos]);
            if (kw == "AND") { pos++; continue; }
            if (kw == "OR") {
                pos++;
                result.push_back(std::move(current));
                current.clear();
                continue;
            }
        }
        break;
    }
    if (!current.empty()) result.push_back(std::move(current));
    if (result.empty()) result.push_back({});
    return result;
}

size_t Database::resolve_col(const std::string& name) const {
    for (size_t i = 0; i < schema_.size(); i++)
        if (schema_[i].name == name) return i;
    return (size_t)-1;
}

bool Database::like_match(const std::string& s, const std::string& pat) const {
    size_t si = 0, pi = 0, star_si = (size_t)-1, star_pi = (size_t)-1;
    while (si < s.size()) {
        if (pi < pat.size() && (pat[pi] == s[si] || pat[pi] == '_')) { si++; pi++; }
        else if (pi < pat.size() && pat[pi] == '%') { star_si = si; star_pi = pi++; }
        else if (star_pi != (size_t)-1) { si = ++star_si; pi = star_pi + 1; }
        else return false;
    }
    while (pi < pat.size() && pat[pi] == '%') pi++;
    return pi == pat.size();
}

bool Database::eval(const Row& row, const Condition& c) const {
    if (c.col >= row.columns.size()) return false;
    auto& val = row.columns[c.col];

    if (c.op == "IS NULL") return is_null(val);
    if (c.op == "IS NOT NULL") return !is_null(val);

    if (c.op == "IN" || c.op == "NOT IN") {
        if (is_null(val)) return false;
        bool found = false;
        for (auto& v : c.val_list) {
            if (val == v) { found = true; break; }
        }
        return c.op == "IN" ? found : !found;
    }

    if (c.op == "LIKE") {
        if (is_null(val)) return false;
        auto* s = std::get_if<std::string>(&val);
        auto* p = std::get_if<std::string>(&c.val);
        if (!s || !p) return false;
        bool m = like_match(*s, *p);
        return c.not_like ? !m : m;
    }

    if (is_null(val)) return c.op == "!=" || c.op == "<>";
    auto cmp = [&](auto a, auto b) -> bool {
        if (c.op == "=") return a == b;
        if (c.op == "!=" || c.op == "<>") return a != b;
        if (c.op == "<") return a < b;
        if (c.op == ">") return a > b;
        if (c.op == "<=") return a <= b;
        if (c.op == ">=") return a >= b;
        return false;
    };
    bool ok = std::visit([&](auto&& v) -> bool {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, std::monostate>) return false;
        else if constexpr (std::is_same_v<T, int32_t>) {
            if (auto* p = std::get_if<int64_t>(&c.val)) return cmp((int64_t)v, *p);
            if (auto* p = std::get_if<double>(&c.val)) return cmp((double)v, *p);
            if (auto* p = std::get_if<int32_t>(&c.val)) return cmp((int64_t)v, (int64_t)*p);
            return false;
        } else if constexpr (std::is_same_v<T, int64_t>) {
            if (auto* p = std::get_if<int64_t>(&c.val)) return cmp(v, *p);
            if (auto* p = std::get_if<double>(&c.val)) return cmp((double)v, *p);
            if (auto* p = std::get_if<int32_t>(&c.val)) return cmp(v, (int64_t)*p);
            return false;
        } else if constexpr (std::is_same_v<T, float>) {
            if (auto* p = std::get_if<double>(&c.val)) return cmp((double)v, *p);
            if (auto* p = std::get_if<int64_t>(&c.val)) return cmp(v, (float)*p);
            if (auto* p = std::get_if<int32_t>(&c.val)) return cmp(v, (float)*p);
            return false;
        } else if constexpr (std::is_same_v<T, double>) {
            if (auto* p = std::get_if<double>(&c.val)) return cmp(v, *p);
            if (auto* p = std::get_if<int64_t>(&c.val)) return cmp(v, (double)*p);
            if (auto* p = std::get_if<int32_t>(&c.val)) return cmp(v, (double)*p);
            return false;
        } else if constexpr (std::is_same_v<T, std::string>) {
            if (auto* p = std::get_if<std::string>(&c.val)) return cmp(v, *p);
            return false;
        }
        return false;
    }, val);
    return ok;
}

bool Database::eval_where(const Row& row, const WhereClause& wc) const {
    if (wc.empty()) return true;
    for (auto& group : wc) {
        bool all_match = true;
        for (auto& c : group) {
            if (!eval(row, c)) { all_match = false; break; }
        }
        if (all_match) return true;
    }
    return false;
}

bool Database::exec(const std::string& sql) {
    if (!opened_) return false;
    auto tok = tokenize(sql);
    if (tok.empty()) return true;

    std::string cmd = to_upper(tok[0]);

    if (cmd == "CREATE") { size_t p = 1; exec_create(tok, p); return true; }
    if (cmd == "INSERT") { size_t p = 1; exec_insert(tok, p); return true; }
    if (cmd == "SELECT") {
        // Capture cout to populate query result members
        query_col_names_.clear();
        query_col_types_.clear();
        query_rows_.clear();
        std::ostringstream capture;
        auto* old = std::cout.rdbuf(capture.rdbuf());
        size_t p = 1;
        exec_select(tok, p);
        std::cout.rdbuf(old);
        // Parse captured output into structured results
        std::istringstream iss(capture.str());
        std::string line;
        // Read header line
        if (std::getline(iss, line)) {
            std::istringstream hdr(line);
            std::string col;
            while (std::getline(hdr, col, '|')) {
                // Trim whitespace
                size_t s = col.find_first_not_of(" \t");
                size_t e = col.find_last_not_of(" \t");
                query_col_names_.push_back((s == std::string::npos) ? "" : col.substr(s, e - s + 1));
                query_col_types_.push_back(ColumnType::STRING);
            }
        }
        // Skip separator line (--- or ---+---)
        if (std::getline(iss, line)) {}
        // Read data rows until "(N row(s))" or EOF
        while (std::getline(iss, line)) {
            // Check for row count footer: "  (N row(s))"
            if (line.find("row(s))") != std::string::npos) continue;
            if (line.empty()) continue;
            std::vector<std::string> row;
            std::istringstream rds(line);
            std::string val;
            while (std::getline(rds, val, '|')) {
                size_t s = val.find_first_not_of(" \t");
                size_t e = val.find_last_not_of(" \t");
                row.push_back((s == std::string::npos) ? "" : val.substr(s, e - s + 1));
            }
            if (!row.empty()) query_rows_.push_back(std::move(row));
        }
        return true;
    }
    if (cmd == "UPDATE") { size_t p = 1; exec_update(tok, p); return true; }
    if (cmd == "DELETE") { size_t p = 1; exec_delete(tok, p); return true; }
    if (cmd == "DROP") { size_t p = 1; exec_drop(tok, p); return true; }
    if (cmd == "ALTER") { size_t p = 1; exec_alter(tok, p); return true; }
    if (cmd == "BEGIN" || cmd == "START") {
        std::cout << "  (transactions not fully implemented, executing directly)\n";
        return true;
    }
    if (cmd == "COMMIT") {
        if (dirty_) checkpoint();
        std::cout << "  committed\n";
        return true;
    }
    if (cmd == "ROLLBACK") {
        // restore from last checkpoint
        rows_.clear();
        schema_.clear();
        wal_seq_ = 0;
        checkpoint_seq_ = 0;
        recover();
        dirty_ = false;
        std::cout << "  rolled back to last checkpoint\n";
        return true;
    }
    if (cmd[0] == '.') {
        if (cmd == ".CHECKPOINT") { checkpoint(); std::cout << "  checkpoint done\n"; return true; }
        if (cmd == ".SNAPSHOT") {
            if (tok.size() < 2) { std::cout << "  usage: .snapshot <name>\n"; return true; }
            if (create_snapshot(tok[1])) std::cout << "  snapshot '" << tok[1] << "' created\n";
            else std::cout << "  snapshot failed\n";
            return true;
        }
        if (cmd == ".SNAPSHOTS") {
            auto snaps = list_snapshots();
            if (snaps.empty()) std::cout << "  no snapshots\n";
            else for (auto& s : snaps) std::cout << "  " << s << "\n";
            return true;
        }
        if (cmd == ".RESTORE") {
            if (tok.size() < 2) { std::cout << "  usage: .restore <name>\n"; return true; }
            if (restore_snapshot(tok[1])) std::cout << "  restored '" << tok[1] << "'\n";
            else std::cout << "  restore failed\n";
            return true;
        }
        if (cmd == ".HELP") {
            std::cout << "  Commands:\n";
            std::cout << "    CREATE TABLE <name> (<col> <type> [NOT NULL] [DICT|BIT_PACKED], ...)\n";
            std::cout << "    INSERT INTO <name> VALUES (<val>, ...)\n";
            std::cout << "    SELECT [DISTINCT] *|<cols>|<agg>(<col>) FROM <name>\n";
            std::cout << "      [WHERE <cond> [AND|OR ...]] [GROUP BY <col>] [ORDER BY <col> [ASC|DESC]]\n";
            std::cout << "      [HAVING <cond>] [LIMIT n] [OFFSET n]\n";
            std::cout << "    UPDATE <name> SET <col>=<val>,... WHERE <cond>\n";
            std::cout << "    DELETE FROM <name> WHERE <cond>\n";
            std::cout << "    DROP TABLE <name>\n";
            std::cout << "    ALTER TABLE <name> ADD COLUMN <col> <type> [NOT NULL] [DICT|BIT_PACKED]\n";
            std::cout << "    ALTER TABLE <name> DROP COLUMN <col>\n";
            std::cout << "    BEGIN | COMMIT | ROLLBACK\n";
            std::cout << "    WHERE operators: = != <> < > <= >= LIKE NOT LIKE IS NULL IS NOT NULL IN NOT IN BETWEEN\n";
            std::cout << "    Scalar functions: UPPER(col) LOWER(col) LENGTH(col)\n";
            std::cout << "    Column aliases: col AS alias\n";
            std::cout << "    .checkpoint\n";
            std::cout << "    .snapshot <name>\n";
            std::cout << "    .snapshots\n";
            std::cout << "    .restore <name>\n";
            std::cout << "    .tables\n";
            std::cout << "    .schema\n";
            std::cout << "    .exit | .quit\n";
            return true;
        }
        if (cmd == ".TABLES") {
            if (schema_.empty()) std::cout << "  no tables\n";
            else std::cout << "  " << (!dir_.empty() ? fs::path(dir_).filename().string() : "zepto") << "\n";
            return true;
        }
        if (cmd == ".SCHEMA") {
            if (schema_.empty()) { std::cout << "  no table\n"; return true; }
            std::cout << "  CREATE TABLE x (\n";
            for (size_t i = 0; i < schema_.size(); i++) {
                auto& cm = schema_[i];
                const char* tn = "UNKNOWN";
                switch (cm.type) {
                    case ColumnType::I32: tn = "I32"; break;
                    case ColumnType::I64: tn = "I64"; break;
                    case ColumnType::F32: tn = "F32"; break;
                    case ColumnType::F64: tn = "F64"; break;
                    case ColumnType::STRING: tn = "STRING"; break;
                }
                std::cout << "    " << cm.name << " " << tn;
                if (!cm.nullable) std::cout << " NOT NULL";
                if (cm.encoding == Encoding::DICT) std::cout << " DICT";
                else if (cm.encoding == Encoding::BIT_PACKED) std::cout << " BIT_PACKED";
                std::cout << (i + 1 < schema_.size() ? "," : "") << "\n";
            }
            std::cout << "  )\n";
            return true;
        }
        if (cmd == ".EXIT" || cmd == ".QUIT") return false;
        std::cout << "  unknown command: " << sql << "\n";
        return true;
    }

    std::cout << "  unknown command: " << sql << "\n";
    return true;
}

void Database::exec_create(const std::vector<std::string>& tok, size_t& pos) {
    if (!schema_.empty()) { std::cout << "  table already exists\n"; return; }
    if (pos >= tok.size() || to_upper(tok[pos]) != "TABLE") { std::cout << "  syntax: CREATE TABLE name (...)\n"; return; }
    pos++;
    pos++; // table name (ignored, single table)
    if (pos >= tok.size() || tok[pos] != "(") { std::cout << "  expected '(\n"; return; }
    pos++;

    while (pos < tok.size() && tok[pos] != ")") {
        std::string col = tok[pos++];
        if (pos >= tok.size()) break;
        std::string type_str = to_upper(tok[pos++]);
        ColumnType ct;
        if (type_str == "I32" || type_str == "INT" || type_str == "INTEGER") ct = ColumnType::I32;
        else if (type_str == "I64" || type_str == "BIGINT") ct = ColumnType::I64;
        else if (type_str == "F32" || type_str == "FLOAT") ct = ColumnType::F32;
        else if (type_str == "F64" || type_str == "DOUBLE") ct = ColumnType::F64;
        else if (type_str == "STRING" || type_str == "TEXT" || type_str == "VARCHAR") ct = ColumnType::STRING;
        else { std::cout << "  unknown type: " << type_str << "\n"; return; }

        bool nullable = true;
        Encoding enc = Encoding::PLAIN;

        while (pos < tok.size() && tok[pos] != "," && tok[pos] != ")") {
            std::string kw = to_upper(tok[pos]);
            if (kw == "NOT" && pos + 1 < tok.size() && to_upper(tok[pos+1]) == "NULL") {
                nullable = false; pos += 2;
            } else if (kw == "NULL" || kw == "NULLABLE") {
                nullable = true; pos++;
            } else if (kw == "DICT") {
                enc = Encoding::DICT; pos++;
            } else if (kw == "BIT_PACKED") {
                enc = Encoding::BIT_PACKED; pos++;
            } else if (kw == "PLAIN") {
                enc = Encoding::PLAIN; pos++;
            } else break;
        }

        ColMeta cm;
        cm.name = col;
        cm.type = ct;
        cm.nullable = nullable;
        cm.encoding = enc;
        schema_.push_back(cm);

        if (pos < tok.size() && tok[pos] == ",") pos++;
    }
    if (pos < tok.size() && tok[pos] == ")") pos++;
    std::cout << "  table created (" << schema_.size() << " columns)\n";
}


static Value coerce_value(const Value& v, ColumnType target) {
    if (is_null(v)) return v;
    return std::visit([&](auto&& x) -> Value {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, std::string>) {
            if (target == ColumnType::STRING) return x;
            if (x.empty()) return Value{}; // '' into a numeric column is unparseable -> NULL
            // Parse the string as a number so it can live in a numeric column.
            char* end = nullptr;
            long long iv = std::strtoll(x.c_str(), &end, 10);
            if (end && *end == 0)
                return coerce_value((iv >= INT32_MIN && iv <= INT32_MAX) ? (int32_t)iv : (int64_t)iv, target);
            end = nullptr;
            double fv = std::strtod(x.c_str(), &end);
            if (end && *end == 0) return coerce_value(fv, target);
            return Value{}; // unparseable -> NULL
        } else if constexpr (std::is_arithmetic_v<T>) {
            double d = (double)x;
            switch (target) {
                case ColumnType::I32: return (int32_t)d;
                case ColumnType::I64: return (int64_t)d;
                case ColumnType::F32: return (float)d;
                case ColumnType::F64: return d;
                case ColumnType::STRING:
                    if constexpr (std::is_integral_v<T>) return std::to_string((long long)x);
                    else return std::to_string(d);
                default: return Value{};
            }
        } else {
            return Value{};
        }
    }, v);
}

void Database::exec_insert(const std::vector<std::string>& tok, size_t& pos) {
    // INSERT INTO name VALUES (v1, v2, ...)
    if (schema_.empty()) { std::cout << "  no table created yet\n"; return; }
    if (pos >= tok.size() || to_upper(tok[pos]) != "INTO") { std::cout << "  syntax: INSERT INTO ...\n"; return; }
    pos++;
    pos++; // table name
    if (pos >= tok.size() || to_upper(tok[pos]) != "VALUES") { std::cout << "  expected VALUES\n"; return; }
    pos++;
    if (pos >= tok.size() || tok[pos] != "(") { std::cout << "  expected '('\n"; return; }
    pos++;

    std::vector<Value> vals;
    while (pos < tok.size() && tok[pos] != ")") {
        vals.push_back(parse_value(tok[pos++]));
        if (pos < tok.size() && tok[pos] == ",") pos++;
    }
    if (pos < tok.size() && tok[pos] == ")") pos++;

    if (vals.size() != schema_.size()) { std::cout << "  wrong number of values\n"; return; }

    for (size_t i = 0; i < vals.size(); i++) vals[i] = coerce_value(vals[i], schema_[i].type);

    Row r;
    r.columns = vals;
    auto bytes = row_to_bytes(r);
    if (wal_append((uint8_t)WalOp::INSERT, bytes.data(), (uint32_t)bytes.size())) {
        rows_.push_back(r);
        dirty_ = true;
        std::cout << "  inserted (" << rows_.size() << " rows total)\n";
    }
}

void Database::exec_select(const std::vector<std::string>& tok, size_t& pos) {
    if (schema_.empty()) { std::cout << "  no table\n"; return; }

    // Parse DISTINCT
    bool distinct = false;
    if (pos < tok.size() && to_upper(tok[pos]) == "DISTINCT") { distinct = true; pos++; }

    // Parse select columns (may include aggregate functions)
    enum class ScalarFunc : uint8_t { NONE, UPPER, LOWER, LENGTH };
    struct SCol {
        size_t col = 0;
        bool is_star = false;
        AggFunc agg = AggFunc::NONE;
        ScalarFunc func = ScalarFunc::NONE;
        std::string alias;
    };
    std::vector<SCol> sel_cols;
    bool has_aggregates = false;

    auto parse_sel_col = [&]() -> bool {
        if (pos >= tok.size()) return false;
        std::string up = to_upper(tok[pos]);
        AggFunc af = AggFunc::NONE;
        ScalarFunc sf = ScalarFunc::NONE;

        // Check for aggregate functions
        if (up == "COUNT" || up == "SUM" || up == "AVG" || up == "MIN" || up == "MAX") {
            if (up == "COUNT") af = AggFunc::COUNT;
            else if (up == "SUM") af = AggFunc::SUM;
            else if (up == "AVG") af = AggFunc::AVG;
            else if (up == "MIN") af = AggFunc::MIN;
            else if (up == "MAX") af = AggFunc::MAX;
            pos++;
            if (pos >= tok.size() || tok[pos] != "(") return false;
            pos++;
            bool agg_distinct = false;
            if (pos < tok.size() && to_upper(tok[pos]) == "DISTINCT") { agg_distinct = true; pos++; }
            if (pos >= tok.size()) return false;
            if (tok[pos] == "*") {
                if (af != AggFunc::COUNT) return false; // SUM(*) etc invalid
                pos++;
                // Push a dummy column for COUNT(*)
                SCol sc; sc.col = 0; sc.agg = af; sc.is_star = true;
                sel_cols.push_back(sc);
            } else {
                std::string cn = tok[pos++];
                size_t ci = resolve_col(cn);
                if (ci == (size_t)-1) return false;
                SCol sc;
                sc.col = ci; sc.agg = af; sc.is_star = false;
                sel_cols.push_back(sc);
            }
            if (pos >= tok.size() || tok[pos] != ")") return false;
            pos++;
            has_aggregates = true;
            // optional AS alias
            if (pos < tok.size() && to_upper(tok[pos]) == "AS") { pos++; if (pos < tok.size()) sel_cols.back().alias = tok[pos++]; }
            return true;
        }
        // Check for scalar functions: UPPER, LOWER, LENGTH
        if ((up == "UPPER" || up == "LOWER" || up == "LENGTH") &&
            pos + 1 < tok.size() && tok[pos+1] == "(") {
            if (up == "UPPER") sf = ScalarFunc::UPPER;
            else if (up == "LOWER") sf = ScalarFunc::LOWER;
            else if (up == "LENGTH") sf = ScalarFunc::LENGTH;
            pos++;
            pos++; // (
            if (pos >= tok.size()) return false;
            std::string cn = tok[pos++];
            size_t ci = resolve_col(cn);
            if (ci == (size_t)-1) return false;
            if (pos >= tok.size() || tok[pos] != ")") return false;
            pos++;
            SCol sc;
            sc.col = ci; sc.func = sf; sc.is_star = false;
            sel_cols.push_back(sc);
            // optional AS alias
            if (pos < tok.size() && to_upper(tok[pos]) == "AS") { pos++; if (pos < tok.size()) sel_cols.back().alias = tok[pos++]; }
            return true;
        }
        // Regular column or *
        if (tok[pos] == "*") {
            pos++;
            SCol sc; sc.is_star = true; sc.agg = AggFunc::NONE;
            sel_cols.push_back(sc);
        } else {
            std::string cn = tok[pos++];
            size_t ci = resolve_col(cn);
            if (ci == (size_t)-1) return false;
            SCol sc; sc.col = ci; sc.agg = AggFunc::NONE; sc.is_star = false;
            sel_cols.push_back(sc);
            // optional AS alias
            if (pos < tok.size() && to_upper(tok[pos]) == "AS") { pos++; if (pos < tok.size()) sel_cols.back().alias = tok[pos++]; }
        }
        return true;
    };

    while (pos < tok.size() && to_upper(tok[pos]) != "FROM") {
        if (!parse_sel_col()) { std::cout << "  syntax error in SELECT\n"; return; }
        if (pos < tok.size() && tok[pos] == ",") pos++;
    }

    if (pos >= tok.size() || to_upper(tok[pos]) != "FROM") { std::cout << "  expected FROM\n"; return; }
    pos++;
    pos++; // table name

    // Expand * before WHERE (COUNT(*) stays as a single column)
    std::vector<SCol> expanded;
    for (auto& sc : sel_cols) {
        if (sc.is_star && sc.agg == AggFunc::NONE) {
            for (size_t i = 0; i < schema_.size(); i++) {
                SCol e; e.col = i; e.agg = sc.agg;
                expanded.push_back(e);
            }
        } else expanded.push_back(sc);
    }
    sel_cols.swap(expanded);

    auto apply_scalar = [](ScalarFunc func, const Value& val) -> Value {
        if (func == ScalarFunc::NONE || is_null(val)) return val;
        auto* s = std::get_if<std::string>(&val);
        if (!s) return val;
        std::string r = *s;
        switch (func) {
            case ScalarFunc::UPPER: for (auto& c : r) c = (char)std::toupper((unsigned char)c); return r;
            case ScalarFunc::LOWER: for (auto& c : r) c = (char)std::tolower((unsigned char)c); return r;
            case ScalarFunc::LENGTH: return (int64_t)r.size();
            default: return val;
        }
    };

    // WHERE
    WhereClause wc;
    if (pos < tok.size() && to_upper(tok[pos]) == "WHERE") { pos++; wc = parse_where(tok, pos); }

    // Helper to resolve column name, optionally checking sel_col aliases
    auto resolve_col_or_alias = [&](const std::string& cn) -> size_t {
        size_t ci = resolve_col(cn);
        if (ci != (size_t)-1) return ci;
        for (auto& sc : sel_cols) {
            if (sc.alias == cn) return sc.col;
        }
        return (size_t)-1;
    };

    // GROUP BY
    std::vector<size_t> group_cols;
    if (pos < tok.size() && to_upper(tok[pos]) == "GROUP BY") {
        pos++;
        while (pos < tok.size() && to_upper(tok[pos]) != "HAVING" && to_upper(tok[pos]) != "ORDER BY" && to_upper(tok[pos]) != "LIMIT" && to_upper(tok[pos]) != "OFFSET" && tok[pos] != ";") {
            std::string cn = tok[pos++];
            size_t ci = resolve_col_or_alias(cn);
            if (ci == (size_t)-1) { std::cout << "  unknown column: " << cn << "\n"; return; }
            group_cols.push_back(ci);
            if (pos < tok.size() && tok[pos] == ",") pos++;
        }
    }

    // HAVING (parse as AND-only for simplicity)
    WhereClause having_wc;
    if (pos < tok.size() && to_upper(tok[pos]) == "HAVING") { pos++; having_wc = parse_where(tok, pos); }

    // ORDER BY
    std::vector<OrderByItem> order_by;
    if (pos < tok.size() && to_upper(tok[pos]) == "ORDER BY") {
        pos++;
        while (pos < tok.size() && to_upper(tok[pos]) != "LIMIT" && to_upper(tok[pos]) != "OFFSET" && tok[pos] != ";") {
            std::string cn = tok[pos++];
            size_t ci = resolve_col_or_alias(cn);
            if (ci == (size_t)-1) { std::cout << "  unknown column: " << cn << "\n"; return; }
            bool asc = true;
            if (pos < tok.size() && to_upper(tok[pos]) == "ASC") { asc = true; pos++; }
            else if (pos < tok.size() && to_upper(tok[pos]) == "DESC") { asc = false; pos++; }
            OrderByItem ob; ob.col = ci; ob.asc = asc;
            order_by.push_back(ob);
            if (pos < tok.size() && tok[pos] == ",") pos++;
        }
    }

    // LIMIT / OFFSET
    size_t limit = 0, offset = 0;
    if (pos < tok.size() && to_upper(tok[pos]) == "LIMIT") {
        pos++;
        if (pos < tok.size()) limit = (size_t)std::stoll(tok[pos++]);
    }
    if (pos < tok.size() && to_upper(tok[pos]) == "OFFSET") {
        pos++;
        if (pos < tok.size()) offset = (size_t)std::stoll(tok[pos++]);
    }

    // Filter rows
    std::vector<size_t> filtered_idx;
    filtered_idx.reserve(rows_.size());
    for (size_t ri = 0; ri < rows_.size(); ri++) {
        if (eval_where(rows_[ri], wc)) filtered_idx.push_back(ri);
    }

    // --- Aggregates / GROUP BY ---
    if (has_aggregates || !group_cols.empty()) {
        struct AggState {
            int64_t count = 0;
            double sum = 0;
            double min_val = 0;
            double max_val = 0;
            bool has_min = false;
            bool has_max = false;
        };
        std::map<std::vector<Value>, AggState> groups;
        std::vector<std::vector<Value>> group_order;

        for (auto ri : filtered_idx) {
            auto& row = rows_[ri];
            std::vector<Value> gkey;
            if (!group_cols.empty()) {
                for (auto gc : group_cols) {
                    gkey.push_back(gc < row.columns.size() ? row.columns[gc] : Value{});
                }
            } else {
                gkey = {Value{}}; // single global group
            }

            auto it = groups.find(gkey);
            if (it == groups.end()) {
                group_order.push_back(gkey);
                it = groups.insert({gkey, AggState{}}).first;
            }
            auto& st = it->second;
            st.count++;

            for (auto& sc : sel_cols) {
                if (sc.agg == AggFunc::NONE) continue;
                double v = 0;
                if (sc.col < row.columns.size()) {
                    auto& cv = row.columns[sc.col];
                    std::visit([&](auto&& x) {
                        using T = std::decay_t<decltype(x)>;
                        if constexpr (!std::is_same_v<T, std::monostate> && !std::is_same_v<T, std::string>)
                            v = (double)x;
                    }, cv);
                }
                if (sc.agg == AggFunc::SUM || sc.agg == AggFunc::AVG) st.sum += v;
                if (sc.agg == AggFunc::MIN && (!st.has_min || v < st.min_val)) { st.min_val = v; st.has_min = true; }
                if (sc.agg == AggFunc::MAX && (!st.has_max || v > st.max_val)) { st.max_val = v; st.has_max = true; }
            }
        }

        // Print header
        for (size_t i = 0; i < sel_cols.size(); i++) {
            if (i > 0) std::cout << " | ";
            auto& sc = sel_cols[i];
            if (!sc.alias.empty()) {
                std::cout << sc.alias;
            } else if (sc.agg != AggFunc::NONE) {
                const char* an = "AGG";
                switch (sc.agg) {
                    case AggFunc::COUNT: an = "COUNT"; break;
                    case AggFunc::SUM:   an = "SUM";   break;
                    case AggFunc::AVG:   an = "AVG";   break;
                    case AggFunc::MIN:   an = "MIN";   break;
                    case AggFunc::MAX:   an = "MAX";   break;
                    default: break;
                }
                if (sc.is_star) std::cout << an << "(*)";
                else std::cout << an << "(" << schema_[sc.col].name << ")";
            } else if (sc.func != ScalarFunc::NONE) {
                const char* fn = "FN";
                switch (sc.func) {
                    case ScalarFunc::UPPER: fn = "UPPER"; break;
                    case ScalarFunc::LOWER: fn = "LOWER"; break;
                    case ScalarFunc::LENGTH: fn = "LENGTH"; break;
                    default: break;
                }
                std::cout << fn << "(" << schema_[sc.col].name << ")";
            } else {
                std::cout << schema_[sc.col].name;
            }
        }
        std::cout << "\n---\n";

        size_t printed = 0;
        for (auto& gk : group_order) {
            if (limit > 0 && printed >= limit + offset) break;
            auto& st = groups[gk];

            // HAVING: build a synthetic row with group key + aggregate values
            if (!having_wc.empty()) {
                Row having_row;
                having_row.columns.resize(schema_.size(), std::monostate{});
                for (size_t gi = 0; gi < group_cols.size(); gi++) {
                    if (gi < gk.size())
                        having_row.columns[group_cols[gi]] = gk[gi];
                }
                for (auto& sc : sel_cols) {
                    if (sc.agg == AggFunc::NONE) continue;
                    Value agg_val;
                    switch (sc.agg) {
                        case AggFunc::COUNT: agg_val = (int64_t)st.count; break;
                        case AggFunc::SUM:   agg_val = st.sum; break;
                        case AggFunc::AVG:   agg_val = st.count ? st.sum / st.count : 0.0; break;
                        case AggFunc::MIN:   agg_val = st.has_min ? st.min_val : 0.0; break;
                        case AggFunc::MAX:   agg_val = st.has_max ? st.max_val : 0.0; break;
                        default: break;
                    }
                    having_row.columns[sc.col] = agg_val;
                }
                if (!eval_where(having_row, having_wc)) continue;
            }

            if (offset > 0) { offset--; continue; }

            // Print group key columns then aggregates
            size_t col_i = 0;
            for (auto& sc : sel_cols) {
                if (col_i > 0) std::cout << " | ";
                col_i++;
                if (sc.agg != AggFunc::NONE) {
                    switch (sc.agg) {
                        case AggFunc::COUNT: std::cout << st.count; break;
                        case AggFunc::SUM:   std::cout << st.sum; break;
                        case AggFunc::AVG:   std::cout << (st.count ? st.sum / st.count : 0); break;
                        case AggFunc::MIN:   std::cout << (st.has_min ? st.min_val : 0); break;
                        case AggFunc::MAX:   std::cout << (st.has_max ? st.max_val : 0); break;
                        default: break;
                    }
                } else if (sc.col < schema_.size()) {
                    // show group key column
                    int gk_idx = -1;
                    for (size_t gi = 0; gi < group_cols.size(); gi++) {
                        if (group_cols[gi] == sc.col) { gk_idx = (int)gi; break; }
                    }
                    const auto& vref = (gk_idx >= 0 && (size_t)gk_idx < gk.size()) ? gk[gk_idx] : rows_[0].columns[sc.col];
                    Value v = apply_scalar(sc.func, vref);
                    if (is_null(v)) std::cout << "NULL";
                    else std::visit([](auto&& x) {
                        using T = std::decay_t<decltype(x)>;
                        if constexpr (!std::is_same_v<T, std::monostate>) std::cout << x;
                    }, v);
                }
            }
            std::cout << "\n";
            printed++;
        }
        std::cout << "  (" << printed << " row(s))\n";
        return;
    }

    // --- Non-aggregate path: DISTINCT, ORDER BY, output ---

    // DISTINCT: deduplicate by selected columns
    if (distinct) {
        std::vector<size_t> deduped;
        auto row_key = [&](size_t ri) -> std::vector<Value> {
            std::vector<Value> k;
            for (auto& sc : sel_cols) k.push_back(ri < rows_.size() && sc.col < rows_[ri].columns.size() ? rows_[ri].columns[sc.col] : Value{});
            return k;
        };
        std::set<std::vector<Value>> seen;
        for (auto ri : filtered_idx) {
            auto k = row_key(ri);
            if (seen.insert(k).second) deduped.push_back(ri);
        }
        filtered_idx.swap(deduped);
    }

    // ORDER BY: sort indices
    if (!order_by.empty()) {
        std::sort(filtered_idx.begin(), filtered_idx.end(), [&](size_t a, size_t b) {
            for (auto& ob : order_by) {
                const auto& va = ob.col < rows_[a].columns.size() ? rows_[a].columns[ob.col] : Value{};
                const auto& vb = ob.col < rows_[b].columns.size() ? rows_[b].columns[ob.col] : Value{};
                int c = 0;
                // Compare as variant
                std::visit([&](auto&& x) {
                    using T = std::decay_t<decltype(x)>;
                    std::visit([&](auto&& y) {
                        using U = std::decay_t<decltype(y)>;
                        if constexpr (std::is_same_v<T, std::monostate> && std::is_same_v<U, std::monostate>) c = 0;
                        else if constexpr (std::is_same_v<T, std::monostate>) c = -1;
                        else if constexpr (std::is_same_v<U, std::monostate>) c = 1;
                        else if constexpr (std::is_same_v<T, std::string> && std::is_same_v<U, std::string>) { if (x < y) c = -1; else if (x > y) c = 1; }
                        else if constexpr (std::is_same_v<T, int32_t> && std::is_same_v<U, int32_t>) { if (x < y) c = -1; else if (x > y) c = 1; }
                        else if constexpr (std::is_same_v<T, int64_t> && std::is_same_v<U, int64_t>) { if (x < y) c = -1; else if (x > y) c = 1; }
                        else if constexpr (std::is_same_v<T, double> && std::is_same_v<U, double>) { if (x < y) c = -1; else if (x > y) c = 1; }
                        else if constexpr (std::is_same_v<T, float> && std::is_same_v<U, float>) { if (x < y) c = -1; else if (x > y) c = 1; }
                        else {
                            // Cross-type: promote to double
                            auto to_d = [](auto z) -> double {
                                if constexpr (std::is_same_v<decltype(z), std::monostate>) return 0;
                                else if constexpr (std::is_same_v<decltype(z), std::string>) return 0;
                                else return (double)z;
                            };
                            double dx = to_d(x), dy = to_d(y);
                            if (dx < dy) c = -1; else if (dx > dy) c = 1;
                        }
                    }, vb);
                }, va);
                if (c != 0) return ob.asc ? c < 0 : c > 0;
            }
            return false;
        });
    }

    // Print header
    for (size_t i = 0; i < sel_cols.size(); i++) {
        if (i > 0) std::cout << " | ";
        auto& sc = sel_cols[i];
        if (!sc.alias.empty()) std::cout << sc.alias;
        else if (sc.func != ScalarFunc::NONE) {
            const char* fn = "FN";
            switch (sc.func) {
                case ScalarFunc::UPPER: fn = "UPPER"; break;
                case ScalarFunc::LOWER: fn = "LOWER"; break;
                case ScalarFunc::LENGTH: fn = "LENGTH"; break;
                default: break;
            }
            std::cout << fn << "(" << schema_[sc.col].name << ")";
        } else std::cout << schema_[sc.col].name;
    }
    std::cout << "\n";
    for (size_t i = 0; i < sel_cols.size(); i++) {
        if (i > 0) std::cout << "-+-";
        else {
            auto& sc = sel_cols[i];
            std::string h = sc.alias.empty() ? schema_[sc.col].name : sc.alias;
            for (size_t j = 0; j < h.size(); j++) std::cout << "-";
        }
    }
    if (sel_cols.size() > 0) std::cout << "\n";

    size_t count = 0, skipped = 0;
    for (auto ri : filtered_idx) {
        auto& row = rows_[ri];
        if (skipped < offset) { skipped++; continue; }
        if (limit > 0 && count >= limit) break;

        for (size_t i = 0; i < sel_cols.size(); i++) {
            if (i > 0) std::cout << " | ";
            auto& sc = sel_cols[i];
            Value val = apply_scalar(sc.func, row.columns[sc.col]);
            if (is_null(val)) std::cout << "NULL";
            else {
                std::visit([&](auto&& v) {
                    using T = std::decay_t<decltype(v)>;
                    if constexpr (std::is_same_v<T, std::monostate>) std::cout << "NULL";
                    else if constexpr (std::is_same_v<T, std::string>) std::cout << v;
                    else std::cout << v;
                }, val);
            }
        }
        std::cout << "\n";
        count++;
    }
    std::cout << "  (" << count << " row(s))\n";
}

void Database::exec_update(const std::vector<std::string>& tok, size_t& pos) {
    if (schema_.empty()) { std::cout << "  no table\n"; return; }
    // UPDATE name SET col=val, ... WHERE cond
    pos++; // table name
    if (pos >= tok.size() || to_upper(tok[pos]) != "SET") { std::cout << "  expected SET\n"; return; }
    pos++;

    std::vector<std::pair<std::string, Value>> assigns;
    while (pos < tok.size() && to_upper(tok[pos]) != "WHERE") {
        std::string col = tok[pos++];
        if (pos >= tok.size() || tok[pos] != "=") { std::cout << "  expected =\n"; return; }
        pos++;
        if (pos >= tok.size()) { std::cout << "  expected value\n"; return; }
        Value v = parse_value(tok[pos++]);
        assigns.push_back({col, v});
        if (pos < tok.size() && tok[pos] == ",") pos++;
    }

    WhereClause wc;
    if (pos < tok.size() && to_upper(tok[pos]) == "WHERE") { pos++; wc = parse_where(tok, pos); }

    size_t updated = 0;
    for (size_t i = 0; i < rows_.size(); i++) {
        if (!eval_where(rows_[i], wc)) continue;

        Row new_row = rows_[i];
        for (auto& [cn, v] : assigns) {
            size_t ci = resolve_col(cn);
            if (ci < new_row.columns.size()) new_row.columns[ci] = ci < schema_.size() ? coerce_value(v, schema_[ci].type) : v;
        }

        auto bytes = row_to_bytes(new_row);
        uint64_t idx = (uint64_t)i;
        std::vector<uint8_t> payload(sizeof(idx) + bytes.size());
        memcpy(payload.data(), &idx, sizeof(idx));
        memcpy(payload.data() + sizeof(idx), bytes.data(), bytes.size());

        if (wal_append((uint8_t)WalOp::UPDATE, payload.data(), (uint32_t)payload.size())) {
            rows_[i] = new_row;
            dirty_ = true;
            updated++;
        }
    }
    std::cout << "  " << updated << " row(s) updated\n";
}

void Database::exec_delete(const std::vector<std::string>& tok, size_t& pos) {
    if (schema_.empty()) { std::cout << "  no table\n"; return; }
    // DELETE FROM name WHERE cond
    if (pos >= tok.size() || to_upper(tok[pos]) != "FROM") { std::cout << "  expected FROM\n"; return; }
    pos++;
    pos++; // table name

    WhereClause wc;
    if (pos < tok.size() && to_upper(tok[pos]) == "WHERE") { pos++; wc = parse_where(tok, pos); }
    else {
        // DELETE all
        if (rows_.empty()) { std::cout << "  0 row(s) deleted\n"; return; }
        uint64_t n = rows_.size();
        auto bytes = row_to_bytes(rows_.back());
        std::vector<uint8_t> payload(sizeof(n));
        memcpy(payload.data(), &n, sizeof(n));
        // Just log one DELETE for the last row as a marker; erase all
        if (wal_append((uint8_t)WalOp::DELETE, payload.data(), (uint32_t)payload.size())) {
            std::cout << "  " << rows_.size() << " row(s) deleted\n";
            rows_.clear();
            dirty_ = true;
        }
        return;
    }

    // Delete matching rows in reverse order to keep indices valid
    size_t deleted = 0;
    for (size_t i = rows_.size(); i > 0; i--) {
        size_t idx = i - 1;
        if (!eval_where(rows_[idx], wc)) continue;

        uint64_t di = (uint64_t)idx;
        if (wal_append((uint8_t)WalOp::DELETE, (const uint8_t*)&di, 8)) {
            rows_.erase(rows_.begin() + idx);
            dirty_ = true;
            deleted++;
        }
    }
    std::cout << "  " << deleted << " row(s) deleted\n";
}

void Database::exec_alter(const std::vector<std::string>& tok, size_t& pos) {
    if (schema_.empty()) { std::cout << "  no table\n"; return; }
    if (pos >= tok.size() || to_upper(tok[pos]) != "TABLE") { std::cout << "  syntax: ALTER TABLE name ADD|DROP COLUMN ...\n"; return; }
    pos++;
    pos++; // table name
    if (pos >= tok.size()) { std::cout << "  expected ADD or DROP\n"; return; }
    std::string action = to_upper(tok[pos++]);
    if (pos >= tok.size() || to_upper(tok[pos]) != "COLUMN") { std::cout << "  expected COLUMN\n"; return; }
    pos++;
    if (action == "ADD") {
        if (pos >= tok.size()) { std::cout << "  syntax: ALTER TABLE name ADD COLUMN name type\n"; return; }
        std::string col = tok[pos++];
        if (pos >= tok.size()) { std::cout << "  expected type\n"; return; }
        std::string type_str = to_upper(tok[pos++]);
        ColumnType ct;
        if (type_str == "I32" || type_str == "INT" || type_str == "INTEGER") ct = ColumnType::I32;
        else if (type_str == "I64" || type_str == "BIGINT") ct = ColumnType::I64;
        else if (type_str == "F32" || type_str == "FLOAT") ct = ColumnType::F32;
        else if (type_str == "F64" || type_str == "DOUBLE") ct = ColumnType::F64;
        else if (type_str == "STRING" || type_str == "TEXT" || type_str == "VARCHAR") ct = ColumnType::STRING;
        else { std::cout << "  unknown type: " << type_str << "\n"; return; }
        bool nullable = true;
        Encoding enc = Encoding::PLAIN;
        while (pos < tok.size() && to_upper(tok[pos]) != "ADD" && to_upper(tok[pos]) != "DROP") {
            std::string kw = to_upper(tok[pos]);
            if (kw == "NOT" && pos + 1 < tok.size() && to_upper(tok[pos+1]) == "NULL") { nullable = false; pos += 2; }
            else if (kw == "NULL" || kw == "NULLABLE") { nullable = true; pos++; }
            else if (kw == "DICT") { enc = Encoding::DICT; pos++; }
            else if (kw == "BIT_PACKED") { enc = Encoding::BIT_PACKED; pos++; }
            else if (kw == "PLAIN") { enc = Encoding::PLAIN; pos++; }
            else break;
        }
        ColMeta cm;
        cm.name = col; cm.type = ct; cm.nullable = nullable; cm.encoding = enc;
        schema_.push_back(cm);
        for (auto& row : rows_) row.columns.push_back(std::monostate{});
        dirty_ = true;
        std::cout << "  column '" << col << "' added\n";
    } else if (action == "DROP") {
        if (pos >= tok.size()) { std::cout << "  syntax: ALTER TABLE name DROP COLUMN name\n"; return; }
        std::string col = tok[pos++];
        size_t ci = resolve_col(col);
        if (ci == (size_t)-1) { std::cout << "  unknown column: " << col << "\n"; return; }
        schema_.erase(schema_.begin() + ci);
        for (auto& row : rows_) {
            if (ci < row.columns.size()) row.columns.erase(row.columns.begin() + ci);
        }
        dirty_ = true;
        std::cout << "  column '" << col << "' dropped\n";
    } else {
        std::cout << "  expected ADD or DROP, got " << action << "\n";
    }
}

void Database::exec_drop(const std::vector<std::string>& tok, size_t& pos) {
    if (schema_.empty()) { std::cout << "  no table\n"; return; }
    if (pos >= tok.size() || to_upper(tok[pos]) != "TABLE") { std::cout << "  syntax: DROP TABLE name\n"; return; }
    pos++;
    pos++; // table name

    // Log a special checkpoint to invalidate all prior WAL entries
    uint64_t kill_seq = wal_seq_ + 1;
    wal_append((uint8_t)WalOp::CHECKPOINT, (const uint8_t*)&kill_seq, 8);

    rows_.clear();
    schema_.clear();
    dirty_ = false;
    std::cout << "  table dropped\n";
}

} // namespace zepto

// ---- REPL ----

#ifdef ZEPTO_REPL
int main(int argc, char** argv) {
    zepto::Database db;
    std::string dir;

    std::cout << "zepto REPL v1.0 (WAL+CoW)\n";
    if (argc > 1) {
        dir = argv[1];
    } else {
        std::cout << "database directory: ";
        std::getline(std::cin, dir);
        if (dir.empty()) dir = "zepto_db";
    }

    if (!db.open(dir)) {
        std::cerr << "error: cannot open database '" << dir << "'\n";
        return 1;
    }

    std::cout << "database '" << dir << "' ready. type .help for commands.\n";
    std::string line;
    while (true) {
        std::cout << "zepto> ";
        if (!std::getline(std::cin, line)) break;
        if (line.empty()) continue;

        std::string uline = line;
        for (auto& c : uline) c = (char)std::toupper((unsigned char)c);
        std::string trimmed;
        for (auto c : uline) { if (!std::isspace((unsigned char)c)) { trimmed += c; break; } }
        size_t start = uline.find_first_not_of(" \t");
        if (start != std::string::npos) {
            std::string first = uline.substr(start);
            size_t end = first.find_first_of(" \t");
            if (end != std::string::npos) first = first.substr(0, end);
            if (first == ".EXIT" || first == ".QUIT") break;
        }

        db.exec(line);
    }

    db.close();
    std::cout << "bye.\n";
    return 0;
}
#endif
