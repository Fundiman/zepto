#include "zepto_capi.h"
#include "zepto.h"
#include <cstring>
#include <cstdlib>
#include <vector>
#include <unordered_map>
#include <algorithm>

// ---- Opaque database handle ----

struct zepto_db {
    zepto::Database db;
};

// ---- Version ----

int zepto_version(void) {
    return ZEPTO_VERSION_MAJOR * 10000 + ZEPTO_VERSION_MINOR * 100 + ZEPTO_VERSION_PATCH;
}

const char* zepto_version_string(void) {
    return "1.0.0";
}

// ---- Error messages ----

const char* zepto_error_message(int error_code) {
    switch (error_code) {
        case ZEPTO_OK:          return "success";
        case ZEPTO_ERROR:       return "general error";
        case ZEPTO_NOT_FOUND:   return "file not found";
        case ZEPTO_CORRUPT:     return "data corruption detected";
        case ZEPTO_IO_ERROR:    return "I/O error";
        case ZEPTO_BAD_SCHEMA:  return "invalid schema";
        case ZEPTO_BAD_ARGS:    return "invalid arguments";
        default:                return "unknown error";
    }
}

// ---- row serialization (same format as WAL) ----

static std::vector<uint8_t> row_to_buf(const zepto::Row& r) {
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
            if constexpr (std::is_same_v<T, std::string>) {
                uint32_t len = (uint32_t)val.size();
                le(len);
                buf.insert(buf.end(), val.begin(), val.end());
            } else { le(val); }
        }, v);
    }
    return buf;
}

static size_t parse_rows(const uint8_t* data, size_t size, std::vector<zepto::Row>& out) {
    size_t off = 0;
    while (off < size) {
        if (off + 4 > size) break;
        uint32_t n;
        memcpy(&n, data + off, 4); off += 4;
        zepto::Row r;
        r.columns.resize(n);
        for (uint32_t i = 0; i < n; i++) {
            if (off >= size) break;
            uint8_t tag = data[off++];
            switch (tag) {
            case 0: break;
            case 1: { int32_t v; if (off + 4 <= size) { memcpy(&v, data + off, 4); off += 4; r.columns[i] = v; } break; }
            case 2: { int64_t v; if (off + 8 <= size) { memcpy(&v, data + off, 8); off += 8; r.columns[i] = v; } break; }
            case 3: { float v; if (off + 4 <= size) { memcpy(&v, data + off, 4); off += 4; r.columns[i] = v; } break; }
            case 4: { double v; if (off + 8 <= size) { memcpy(&v, data + off, 8); off += 8; r.columns[i] = v; } break; }
            case 5: {
                uint32_t slen;
                if (off + 4 <= size) { memcpy(&slen, data + off, 4); off += 4; }
                if (off + slen <= size) {
                    r.columns[i] = std::string((const char*)(data + off), slen);
                    off += slen;
                }
                break;
            }
            }
        }
        out.push_back(std::move(r));
    }
    return out.size();
}

// ---- Writer ----

int zepto_write(const char* path, const char** col_names, const int* col_types,
                const int* col_nullable, const int* col_encodings,
                int num_cols, const uint8_t* row_data, size_t row_data_size, int num_rows) {
    try {
        zepto::Writer w(path);
        for (int i = 0; i < num_cols; i++) {
            w.add_column(col_names[i],
                         (zepto::ColumnType)col_types[i],
                         col_nullable[i] != 0,
                         (zepto::Encoding)col_encodings[i]);
        }
        std::vector<zepto::Row> rows;
        parse_rows(row_data, row_data_size, rows);
        for (auto& row : rows) {
            if (!w.append_row(row.columns)) return ZEPTO_ERROR;
        }
        w.close();
        return ZEPTO_OK;
    } catch (...) { return ZEPTO_ERROR; }
}

// ---- Reader ----

int zepto_read(const char* path, zepto_read_result* out) {
    std::memset(out, 0, sizeof(*out));
    try {
        zepto::Reader r(path);
        if (!r.open()) return ZEPTO_NOT_FOUND;

        out->num_cols = (int)r.column_types().size();
        out->num_rows = (int)r.num_rows();

        out->col_names = (char**)std::calloc(out->num_cols, sizeof(char*));
        out->col_types = (int*)std::calloc(out->num_cols, sizeof(int));
        for (int i = 0; i < out->num_cols; i++) {
            auto& n = r.column_names()[i];
            out->col_names[i] = (char*)std::malloc(n.size() + 1);
            std::memcpy(out->col_names[i], n.c_str(), n.size() + 1);
            out->col_types[i] = (int)r.column_types()[i];
        }

        std::vector<uint8_t> all;
        for (size_t ci = 0; ci < r.num_chunks(); ci++) {
            auto chunk = r.read_chunk(ci);
            for (auto& row : chunk) {
                auto buf = row_to_buf(row);
                all.insert(all.end(), buf.begin(), buf.end());
            }
        }

        out->row_data_size = all.size();
        if (!all.empty()) {
            out->row_data = (uint8_t*)std::malloc(all.size());
            std::memcpy(out->row_data, all.data(), all.size());
        }
        return ZEPTO_OK;
    } catch (...) { zepto_read_free(out); return ZEPTO_ERROR; }
}

void zepto_read_free(zepto_read_result* res) {
    if (!res) return;
    std::free(res->row_data);
    if (res->col_names) {
        for (int i = 0; i < res->num_cols; i++) std::free(res->col_names[i]);
        std::free(res->col_names);
    }
    std::free(res->col_types);
    std::memset(res, 0, sizeof(*res));
}

// ---- Database ----

zepto_db* zepto_db_open(const char* dir) {
    if (!dir) return nullptr;
    auto* handle = new zepto_db();
    if (!handle->db.open(dir)) { delete handle; return nullptr; }
    return handle;
}

void zepto_db_close(zepto_db* db) {
    if (!db) return;
    db->db.close();
    delete db;
}

int zepto_db_exec(zepto_db* db, const char* sql) {
    if (!db || !sql) return ZEPTO_BAD_ARGS;
    return db->db.exec(sql) ? ZEPTO_OK : ZEPTO_ERROR;
}

int zepto_db_snapshot(zepto_db* db, const char* name) {
    if (!db || !name) return ZEPTO_BAD_ARGS;
    return db->db.create_snapshot(name) ? ZEPTO_OK : ZEPTO_ERROR;
}

int zepto_db_restore(zepto_db* db, const char* name) {
    if (!db || !name) return ZEPTO_BAD_ARGS;
    return db->db.restore_snapshot(name) ? ZEPTO_OK : ZEPTO_ERROR;
}

char** zepto_db_list_snapshots(zepto_db* db, int* out_count) {
    if (!db || !out_count) { if (out_count) *out_count = 0; return nullptr; }
    auto names = db->db.list_snapshots();
    *out_count = (int)names.size();
    if (names.empty()) return nullptr;
    char** result = (char**)std::calloc(names.size(), sizeof(char*));
    for (size_t i = 0; i < names.size(); i++) {
        result[i] = (char*)std::malloc(names[i].size() + 1);
        std::memcpy(result[i], names[i].c_str(), names[i].size() + 1);
    }
    return result;
}

void zepto_free_strings(char** strs, int count) {
    if (!strs) return;
    for (int i = 0; i < count; i++) std::free(strs[i]);
    std::free(strs);
}

// ---- Query results (from last SELECT exec) ----

int zepto_db_query_col_count(zepto_db* db) {
    if (!db) return 0;
    return db->db.query_col_count();
}

int zepto_db_query_row_count(zepto_db* db) {
    if (!db) return 0;
    return db->db.query_row_count();
}

const char* zepto_db_query_col_name(zepto_db* db, int col_index) {
    if (!db || col_index < 0 || col_index >= db->db.query_col_count()) return "";
    return db->db.query_col_names()[col_index].c_str();
}

int zepto_db_query_col_type(zepto_db* db, int col_index) {
    if (!db || col_index < 0 || col_index >= db->db.query_col_count()) return -1;
    return (int)db->db.query_col_types()[col_index];
}

const char* zepto_db_query_value(zepto_db* db, int row_index, int col_index) {
    if (!db) return "";
    auto& rows = db->db.query_rows();
    if (row_index < 0 || row_index >= (int)rows.size()) return "";
    auto& row = rows[row_index];
    if (col_index < 0 || col_index >= (int)row.size()) return "";
    return row[col_index].c_str();
}

// ---- Columnar API ----

int zepto_write_cols(
    const char* path,
    const char** col_names, const int* col_types,
    const int* col_nullable, const int* col_encodings,
    int num_cols,
    const uint8_t** col_data, const size_t* col_data_sizes,
    const uint8_t** null_bitmaps,
    int num_rows,
    int use_rs,
    int codec)
{
    try {
        zepto::Writer w(path, zepto::DEFAULT_CHUNK_SIZE, use_rs != 0, (zepto::Codec)codec);
        for (int i = 0; i < num_cols; i++) {
            w.add_column(col_names[i],
                         (zepto::ColumnType)col_types[i],
                         col_nullable[i] != 0,
                         (zepto::Encoding)col_encodings[i]);
        }

        std::vector<zepto::Writer::WriteColArray> wc(num_cols);
        for (int ci = 0; ci < num_cols; ci++) {
            wc[ci].type = (zepto::ColumnType)col_types[ci];
            wc[ci].data = col_data[ci];
            wc[ci].validity = null_bitmaps[ci];
            wc[ci].num_values = num_rows;
        }

        w.write_columns(wc, num_rows);
        w.close();
        return ZEPTO_OK;
    } catch (...) { return ZEPTO_ERROR; }
}

int zepto_read_cols(const char* path, zepto_read_cols_result* out) {
    std::memset(out, 0, sizeof(*out));
    try {
        zepto::Reader r(path);
        if (!r.open()) return ZEPTO_NOT_FOUND;

        int nc = (int)r.column_types().size();
        int total_rows = (int)r.num_rows();
        out->num_cols = nc;
        out->num_rows = total_rows;

        out->col_names = (char**)std::calloc(nc, sizeof(char*));
        out->col_types = (int*)std::calloc(nc, sizeof(int));
        for (int i = 0; i < nc; i++) {
            auto& n = r.column_names()[i];
            out->col_names[i] = (char*)std::malloc(n.size() + 1);
            std::memcpy(out->col_names[i], n.c_str(), n.size() + 1);
            out->col_types[i] = (int)r.column_types()[i];
        }

        std::vector<std::vector<zepto::Reader::ColumnArray>> per_col(nc);
        for (size_t ci = 0; ci < r.num_chunks(); ci++) {
            auto chunk = r.read_chunk_cols(ci);
            for (int col = 0; col < nc; col++) {
                if (col < (int)chunk.size() && chunk[col].num_values > 0)
                    per_col[col].push_back(std::move(chunk[col]));
            }
        }

        out->col_data = (uint8_t**)std::calloc(nc, sizeof(uint8_t*));
        out->col_data_sizes = (size_t*)std::calloc(nc, sizeof(size_t));
        out->null_bitmaps = (uint8_t**)std::calloc(nc, sizeof(uint8_t*));

        for (int ci = 0; ci < nc; ci++) {
            std::vector<uint8_t> all_bmp;
            std::vector<uint8_t> all_data;

            for (auto& ca : per_col[ci]) {
                if (!ca.validity.empty())
                    all_bmp.insert(all_bmp.end(), ca.validity.begin(), ca.validity.end());
                if (!ca.data.empty())
                    all_data.insert(all_data.end(), ca.data.begin(), ca.data.end());
            }

            bool has_null = false;
            for (auto& ca : per_col[ci]) {
                for (size_t i = 0; i < ca.num_values; i++) {
                    bool valid = ca.validity.empty() || ((ca.validity[i / 8] >> (i % 8)) & 1);
                    if (!valid) { has_null = true; break; }
                }
                if (has_null) break;
            }

            if (has_null) {
                size_t bmp_size = ((size_t)total_rows + 7) / 8;
                uint8_t* bmp = (uint8_t*)std::calloc(bmp_size, 1);
                for (size_t i = 0; i < bmp_size; i++) bmp[i] = 0xFF;
                if (total_rows % 8) {
                    uint8_t mask = (uint8_t)((1 << (total_rows % 8)) - 1);
                    bmp[bmp_size - 1] = mask;
                }
                size_t row_offset = 0;
                for (auto& ca : per_col[ci]) {
                    for (size_t i = 0; i < ca.num_values; i++) {
                        bool valid = ca.validity.empty() || ((ca.validity[i / 8] >> (i % 8)) & 1);
                        if (!valid) bmp[row_offset / 8] &= ~(1 << (row_offset % 8));
                        row_offset++;
                    }
                }
                out->null_bitmaps[ci] = bmp;
            }

            out->col_data[ci] = (uint8_t*)std::malloc(all_data.size());
            std::memcpy(out->col_data[ci], all_data.data(), all_data.size());
            out->col_data_sizes[ci] = all_data.size();
        }
        return ZEPTO_OK;
    } catch (...) { zepto_read_cols_free(out); return ZEPTO_ERROR; }
}

void zepto_read_cols_free(zepto_read_cols_result* res) {
    if (!res) return;
    for (int i = 0; i < res->num_cols; i++) {
        std::free(res->col_data[i]);
        std::free(res->null_bitmaps[i]);
        std::free(res->col_names[i]);
    }
    std::free(res->col_data);
    std::free(res->col_data_sizes);
    std::free(res->null_bitmaps);
    std::free(res->col_names);
    std::free(res->col_types);
    std::memset(res, 0, sizeof(*res));
}

// ---- Filtered Query API ----

static bool zepto_query_cmp(int64_t a, int64_t b, int op) {
    switch (op) { case 0: return a == b; case 1: return a != b; case 2: return a > b; case 3: return a >= b; case 4: return a < b; case 5: return a <= b; } return true;
}
static bool zepto_query_cmp(double a, double b, int op) {
    switch (op) { case 0: return a == b; case 1: return a != b; case 2: return a > b; case 3: return a >= b; case 4: return a < b; case 5: return a <= b; } return true;
}
static bool zepto_query_cmp(const std::string& a, const std::string& b, int op) {
    switch (op) { case 0: return a == b; case 1: return a != b; case 2: return a > b; case 3: return a >= b; case 4: return a < b; case 5: return a <= b; } return true;
}

int zepto_query_cols(
    const char* path,
    int num_predicates,
    const int* pred_col,
    const int* pred_op,
    const int* pred_val_type,
    const int64_t* pred_val_i64,
    const double* pred_val_f64,
    const char** pred_val_str,
    zepto_read_cols_result* out)
{
    std::memset(out, 0, sizeof(*out));
    try {
        zepto::Reader r(path);
        if (!r.open()) return ZEPTO_NOT_FOUND;

        zepto::Query q;
        for (int i = 0; i < num_predicates; i++) {
            zepto::Query::Predicate p;
            p.col_index = pred_col[i];
            p.op = (zepto::Query::Op)pred_op[i];
            switch (pred_val_type[i]) {
            case 0: p.value = (int32_t)pred_val_i64[i]; break;
            case 1: p.value = pred_val_i64[i]; break;
            case 2: p.value = (float)pred_val_f64[i]; break;
            case 3: p.value = pred_val_f64[i]; break;
            case 4: p.value = std::string(pred_val_str[i] ? pred_val_str[i] : ""); break;
            }
            q.predicates.push_back(std::move(p));
        }

        int nc = (int)r.column_types().size();

        struct MatchedRow { size_t chunk; size_t row; };
        std::vector<MatchedRow> matches;
        std::unordered_map<size_t, std::vector<zepto::Reader::ColumnArray>> chunk_data_cache;

        for (size_t ci = 0; ci < r.num_chunks(); ci++) {
            auto meta = r.chunk_meta(ci);

            bool skip = false;
            for (auto& pred : q.predicates) {
                if (pred.col_index >= meta.zone_maps.size()) continue;
                auto& zm = meta.zone_maps[pred.col_index];
                if (!zm.has_min && !zm.has_max) continue;

                auto check = [&](auto val, auto min, auto max) -> bool {
                    switch (pred.op) {
                    case zepto::Query::EQ: return val >= min && val <= max;
                    case zepto::Query::NE: return true;
                    case zepto::Query::GT: return max > val;
                    case zepto::Query::GE: return max >= val;
                    case zepto::Query::LT: return min < val;
                    case zepto::Query::LE: return min <= val;
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
                            case zepto::Query::EQ: return val >= mn && val <= mx;
                            case zepto::Query::NE: return true;
                            case zepto::Query::GT: return mx > val;
                            case zepto::Query::GE: return mx >= val;
                            case zepto::Query::LT: return mn < val;
                            case zepto::Query::LE: return mn <= val;
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

            auto chunk_cols = r.read_chunk_cols(ci);
            if (chunk_cols.empty()) continue;
            size_t nrows = chunk_cols[0].num_values;

            std::vector<std::vector<std::string>> decoded_strings(chunk_cols.size());
            for (auto& pred : q.predicates) {
                if (pred.col_index >= chunk_cols.size()) continue;
                auto& ca = chunk_cols[pred.col_index];
                if (ca.type != zepto::ColumnType::STRING) continue;
                auto& ds = decoded_strings[pred.col_index];
                ds.resize(nrows);
                const uint8_t* sptr = ca.data.data();
                for (size_t si = 0; si < nrows; si++) {
                    uint32_t slen;
                    memcpy(&slen, sptr, 4); sptr += 4;
                    if (slen > 0) {
                        ds[si].assign((const char*)sptr, slen);
                        sptr += slen;
                    } else {
                        sptr += slen;
                    }
                }
            }

            for (size_t ri = 0; ri < nrows; ri++) {
                bool match = true;
                for (auto& pred : q.predicates) {
                    if (pred.col_index >= chunk_cols.size()) continue;
                    auto& ca = chunk_cols[pred.col_index];

                    bool valid = ca.validity.empty() || ((ca.validity[ri / 8] >> (ri % 8)) & 1);
                    if (!valid) { match = false; break; }

                    bool ok = false;
                    switch (ca.type) {
                    case zepto::ColumnType::I32: {
                        int32_t val = reinterpret_cast<const int32_t*>(ca.data.data())[ri];
                        if (auto* p = std::get_if<int64_t>(&pred.value)) ok = zepto_query_cmp(val, *p, pred.op);
                        else if (auto* p = std::get_if<double>(&pred.value)) ok = zepto_query_cmp((double)val, *p, pred.op);
                        break;
                    }
                    case zepto::ColumnType::I64: {
                        int64_t val = reinterpret_cast<const int64_t*>(ca.data.data())[ri];
                        if (auto* p = std::get_if<int64_t>(&pred.value)) ok = zepto_query_cmp(val, *p, pred.op);
                        else if (auto* p = std::get_if<double>(&pred.value)) ok = zepto_query_cmp((double)val, *p, pred.op);
                        break;
                    }
                    case zepto::ColumnType::F32: {
                        float val = reinterpret_cast<const float*>(ca.data.data())[ri];
                        if (auto* p = std::get_if<double>(&pred.value)) ok = zepto_query_cmp((double)val, *p, pred.op);
                        break;
                    }
                    case zepto::ColumnType::F64: {
                        double val = reinterpret_cast<const double*>(ca.data.data())[ri];
                        if (auto* p = std::get_if<double>(&pred.value)) ok = zepto_query_cmp(val, *p, pred.op);
                        break;
                    }
                    case zepto::ColumnType::STRING: {
                        auto& ds = decoded_strings[pred.col_index];
                        if (ri < ds.size()) {
                            if (auto* p = std::get_if<std::string>(&pred.value)) ok = zepto_query_cmp(ds[ri], *p, pred.op);
                        }
                        break;
                    }
                    }
                    if (!ok) { match = false; break; }
                }
                if (match) {
                    matches.push_back({ci, ri});
                }
            }

            chunk_data_cache[ci] = std::move(chunk_cols);
        }

        int total_rows = (int)matches.size();
        out->num_cols = nc;
        out->num_rows = total_rows;

        out->col_names = (char**)std::calloc(nc, sizeof(char*));
        out->col_types = (int*)std::calloc(nc, sizeof(int));
        for (int i = 0; i < nc; i++) {
            auto& n = r.column_names()[i];
            out->col_names[i] = (char*)std::malloc(n.size() + 1);
            std::memcpy(out->col_names[i], n.c_str(), n.size() + 1);
            out->col_types[i] = (int)r.column_types()[i];
        }

        out->col_data = (uint8_t**)std::calloc(nc, sizeof(uint8_t*));
        out->col_data_sizes = (size_t*)std::calloc(nc, sizeof(size_t));
        out->null_bitmaps = (uint8_t**)std::calloc(nc, sizeof(uint8_t*));

        for (int col = 0; col < nc; col++) {
            auto ct = (zepto::ColumnType)out->col_types[col];

            switch (ct) {
            case zepto::ColumnType::I32: {
                size_t sz = (size_t)total_rows * sizeof(int32_t);
                int32_t* buf = (int32_t*)std::malloc(sz);
                for (int ri = 0; ri < total_rows; ri++) {
                    auto& ca = chunk_data_cache[matches[ri].chunk][col];
                    size_t r = matches[ri].row;
                    bool valid = ca.validity.empty() || ((ca.validity[r / 8] >> (r % 8)) & 1);
                    buf[ri] = valid ? reinterpret_cast<const int32_t*>(ca.data.data())[r] : 0;
                }
                out->col_data[col] = (uint8_t*)buf;
                out->col_data_sizes[col] = sz;
                break;
            }
            case zepto::ColumnType::I64: {
                size_t sz = (size_t)total_rows * sizeof(int64_t);
                int64_t* buf = (int64_t*)std::malloc(sz);
                for (int ri = 0; ri < total_rows; ri++) {
                    auto& ca = chunk_data_cache[matches[ri].chunk][col];
                    size_t r = matches[ri].row;
                    bool valid = ca.validity.empty() || ((ca.validity[r / 8] >> (r % 8)) & 1);
                    buf[ri] = valid ? reinterpret_cast<const int64_t*>(ca.data.data())[r] : 0;
                }
                out->col_data[col] = (uint8_t*)buf;
                out->col_data_sizes[col] = sz;
                break;
            }
            case zepto::ColumnType::F32: {
                size_t sz = (size_t)total_rows * sizeof(float);
                float* buf = (float*)std::malloc(sz);
                for (int ri = 0; ri < total_rows; ri++) {
                    auto& ca = chunk_data_cache[matches[ri].chunk][col];
                    size_t r = matches[ri].row;
                    bool valid = ca.validity.empty() || ((ca.validity[r / 8] >> (r % 8)) & 1);
                    buf[ri] = valid ? reinterpret_cast<const float*>(ca.data.data())[r] : 0;
                }
                out->col_data[col] = (uint8_t*)buf;
                out->col_data_sizes[col] = sz;
                break;
            }
            case zepto::ColumnType::F64: {
                size_t sz = (size_t)total_rows * sizeof(double);
                double* buf = (double*)std::malloc(sz);
                for (int ri = 0; ri < total_rows; ri++) {
                    auto& ca = chunk_data_cache[matches[ri].chunk][col];
                    size_t r = matches[ri].row;
                    bool valid = ca.validity.empty() || ((ca.validity[r / 8] >> (r % 8)) & 1);
                    buf[ri] = valid ? reinterpret_cast<const double*>(ca.data.data())[r] : 0;
                }
                out->col_data[col] = (uint8_t*)buf;
                out->col_data_sizes[col] = sz;
                break;
            }
            case zepto::ColumnType::STRING: {
                std::unordered_map<size_t, std::vector<std::string>> chunk_strs;
                for (auto& m : matches) {
                    if (chunk_strs.count(m.chunk)) continue;
                    auto& ca = chunk_data_cache[m.chunk][col];
                    size_t nrows = ca.num_values;
                    auto& strs = chunk_strs[m.chunk];
                    strs.resize(nrows);
                    const uint8_t* sptr = ca.data.data();
                    for (size_t si = 0; si < nrows; si++) {
                        uint32_t slen;
                        memcpy(&slen, sptr, 4); sptr += 4;
                        if (slen > 0) {
                            strs[si].assign((const char*)sptr, slen);
                            sptr += slen;
                        } else {
                            sptr += slen;
                        }
                    }
                }
                std::vector<uint8_t> packed;
                packed.reserve((size_t)total_rows * 8);
                for (int ri = 0; ri < total_rows; ri++) {
                    auto& s = chunk_strs[matches[ri].chunk][matches[ri].row];
                    uint32_t slen = (uint32_t)s.size();
                    packed.insert(packed.end(), reinterpret_cast<uint8_t*>(&slen), reinterpret_cast<uint8_t*>(&slen) + 4);
                    packed.insert(packed.end(), s.begin(), s.end());
                }
                uint8_t* buf = (uint8_t*)std::malloc(packed.size());
                std::memcpy(buf, packed.data(), packed.size());
                out->col_data[col] = buf;
                out->col_data_sizes[col] = packed.size();
                break;
            }
            }

            // Build the null bitmap for this column from matched-row validity.
            bool has_null = false;
            for (int ri = 0; ri < total_rows; ri++) {
                auto& ca = chunk_data_cache[matches[ri].chunk][col];
                size_t r = matches[ri].row;
                bool valid = ca.validity.empty() || ((ca.validity[r / 8] >> (r % 8)) & 1);
                if (!valid) { has_null = true; break; }
            }
            if (has_null) {
                size_t bmp_size = ((size_t)total_rows + 7) / 8;
                uint8_t* bmp = (uint8_t*)std::calloc(bmp_size, 1);
                for (size_t i = 0; i < bmp_size; i++) bmp[i] = 0xFF;
                if (total_rows % 8) {
                    uint8_t mask = (uint8_t)((1 << (total_rows % 8)) - 1);
                    bmp[bmp_size - 1] = mask;
                }
                for (int ri = 0; ri < total_rows; ri++) {
                    auto& ca = chunk_data_cache[matches[ri].chunk][col];
                    size_t r = matches[ri].row;
                    bool valid = ca.validity.empty() || ((ca.validity[r / 8] >> (r % 8)) & 1);
                    if (!valid) bmp[ri / 8] &= ~(1 << (ri % 8));
                }
                out->null_bitmaps[col] = bmp;
            }
        }

        return ZEPTO_OK;
    } catch (...) { zepto_read_cols_free(out); return ZEPTO_ERROR; }
}

// ---- Schema introspection ----

int zepto_file_col_count(const char* path) {
    try {
        zepto::Reader r(path);
        if (!r.open()) return -1;
        return (int)r.column_types().size();
    } catch (...) { return -1; }
}

int zepto_file_row_count(const char* path) {
    try {
        zepto::Reader r(path);
        if (!r.open()) return -1;
        return (int)r.num_rows();
    } catch (...) { return -1; }
}

int zepto_file_col_type(const char* path, int col_index) {
    try {
        zepto::Reader r(path);
        if (!r.open()) return -1;
        if (col_index < 0 || col_index >= (int)r.column_types().size()) return -1;
        return (int)r.column_types()[col_index];
    } catch (...) { return -1; }
}

const char* zepto_file_col_name(const char* path, int col_index) {
    try {
        static thread_local std::string cached_name;
        zepto::Reader r(path);
        if (!r.open()) return "";
        if (col_index < 0 || col_index >= (int)r.column_names().size()) return "";
        cached_name = r.column_names()[col_index];
        return cached_name.c_str();
    } catch (...) { return ""; }
}
