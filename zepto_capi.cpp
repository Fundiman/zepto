#include "zepto_capi.h"
#include "zepto.h"
#include <cstring>
#include <cstdlib>
#include <vector>

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
            if (!w.append_row(row.columns)) return 0;
        }
        w.close();
        return 1;
    } catch (...) { return 0; }
}

// ---- Reader ----

int zepto_read(const char* path, zepto_read_result* out) {
    std::memset(out, 0, sizeof(*out));
    try {
        zepto::Reader r(path);
        if (!r.open()) return 0;

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
        return 1;
    } catch (...) { return 0; }
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

void* zepto_db_open(const char* dir) {
    auto* db = new zepto::Database();
    if (!db->open(dir)) { delete db; return nullptr; }
    return db;
}

void zepto_db_close(void* db) {
    if (!db) return;
    auto* d = (zepto::Database*)db;
    d->close();
    delete d;
}

int zepto_db_exec(void* db, const char* sql) {
    if (!db || !sql) return 0;
    return ((zepto::Database*)db)->exec(sql) ? 1 : 0;
}

int zepto_db_query(void* db, const char* sql, zepto_read_result* out) {
    std::memset(out, 0, sizeof(*out));
    if (!db || !sql) return 0;
    // We use exec to run the SQL which prints to stdout.
    // For query results, we hack: capture schema from last exec SELECT output.
    // Instead, just exec (the SELECT output goes to cout in the REPL path).
    // For programmatic query results, the user should use the Reader directly.
    // This is a simplified version.
    ((zepto::Database*)db)->exec(sql);
    return 0; // query results via stdout in this simplified version
}

int zepto_db_snapshot(void* db, const char* name) {
    if (!db || !name) return 0;
    return ((zepto::Database*)db)->create_snapshot(name) ? 1 : 0;
}

int zepto_db_restore(void* db, const char* name) {
    if (!db || !name) return 0;
    return ((zepto::Database*)db)->restore_snapshot(name) ? 1 : 0;
}

char** zepto_db_list_snapshots(void* db, int* out_count) {
    if (!db || !out_count) return nullptr;
    auto names = ((zepto::Database*)db)->list_snapshots();
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
