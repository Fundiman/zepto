#pragma once
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <variant>
#include <functional>
#include <span>
#include <memory>
#include <fstream>

namespace zepto {

constexpr uint32_t MAGIC = 0x5450455A; // "ZEPT" little-endian
constexpr uint16_t VERSION = 2;
constexpr size_t DEFAULT_CHUNK_SIZE = 64 * 1024 * 1024; // 64MB

constexpr uint8_t COL_NULLABLE = 0x01;

enum class ColumnType : uint8_t {
    I32 = 0,
    I64 = 1,
    F32 = 2,
    F64 = 3,
    STRING = 4,
};

enum class Encoding : uint8_t {
    PLAIN = 0,
    DICT = 1,
    BIT_PACKED = 2,
    DELTA = 3,
};

struct PageHeader {
    uint32_t offset;
    uint32_t compressed_size;
    uint32_t uncompressed_size;
    uint32_t crc32c;
    ColumnType type;
    Encoding encoding;
    uint32_t num_values;
    uint32_t validity_bitmap_size;
};

struct ZoneMap {
    bool has_min = false;
    bool has_max = false;
    int64_t min_i64 = 0;
    int64_t max_i64 = 0;
    double min_f64 = 0;
    double max_f64 = 0;
    std::string min_str;
    std::string max_str;
};

struct ChunkMeta {
    uint32_t chunk_index;
    uint64_t chunk_offset;
    uint64_t chunk_size;
    uint32_t num_pages;
    uint32_t num_rows;
    std::vector<PageHeader> pages;
    std::vector<ZoneMap> zone_maps;
};

using Value = std::variant<std::monostate, int32_t, int64_t, float, double, std::string>;

struct Row {
    std::vector<Value> columns;
};

struct ColMeta {
    ColumnType type;
    bool nullable;
    std::string name;
    Encoding encoding;
};

struct ChunkBuilder {
    uint32_t chunk_index;
    size_t target_size = DEFAULT_CHUNK_SIZE;
    std::vector<std::vector<Value>> columns;
    std::vector<ColumnType> types;
    std::vector<std::string> col_names;
    std::vector<ZoneMap> zone_maps;
};

struct Query {
    enum Op { EQ, NE, GT, GE, LT, LE, AND, OR };
    struct Predicate {
        size_t col_index;
        Op op;
        Value value;
    };
    std::vector<Predicate> predicates;
    std::vector<size_t> select_columns;
    size_t limit = 0;
    size_t offset = 0;
};

struct QueryResult {
    uint64_t total_rows;
    std::vector<Row> rows;
    std::vector<std::string> col_names;
    std::vector<ColumnType> col_types;
};

class Writer {
public:
    Writer(std::string path, size_t chunk_size = DEFAULT_CHUNK_SIZE);
    ~Writer();
    Writer(const Writer&) = delete;
    Writer& operator=(const Writer&) = delete;
    Writer(Writer&&) noexcept;
    Writer& operator=(Writer&&) noexcept;

    bool add_column(const std::string& name, ColumnType type, bool nullable = false, Encoding encoding = Encoding::PLAIN);
    bool append_row(const std::vector<Value>& row);
    uint64_t flush_chunk();
    uint64_t rows_written() const { return total_rows_; }
    void close();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::string path_;
    size_t chunk_size_;
    std::vector<ColMeta> col_meta_;
    std::vector<std::vector<Value>> pending_;
    size_t pending_bytes_ = 0;
    size_t total_rows_ = 0;
    uint32_t chunk_index_ = 0;
    bool closed_ = false;
    bool metadata_written_ = false;

    void write_metadata();
    void write_chunk(const std::vector<std::vector<Value>>& cols,
                     const std::vector<ColMeta>& meta,
                     uint32_t idx);
};

class Reader {
public:
    Reader(std::string path);
    ~Reader();
    Reader(const Reader&) = delete;
    Reader& operator=(const Reader&) = delete;
    Reader(Reader&&) noexcept;
    Reader& operator=(Reader&&) noexcept;

    bool open();
    void close();

    const std::vector<std::string>& column_names() const { return col_names_; }
    const std::vector<ColumnType>& column_types() const { return col_types_; }
    const std::vector<bool>& column_nullable() const { return col_nullable_; }
    const std::vector<Encoding>& column_encoding() const { return col_encoding_; }
    size_t num_chunks() const { return chunks_.size(); }
    size_t num_rows() const { return total_rows_; }

    QueryResult query(const Query& q);
    std::vector<Row> read_chunk(size_t chunk_idx);
    ChunkMeta chunk_meta(size_t chunk_idx) const;

    bool verify_integrity();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::string path_;
    std::vector<std::string> col_names_;
    std::vector<ColumnType> col_types_;
    std::vector<bool> col_nullable_;
    std::vector<Encoding> col_encoding_;
    std::vector<ChunkMeta> chunks_;
    size_t total_rows_ = 0;
    bool opened_ = false;

    bool read_header();
    bool read_chunk_meta(ChunkMeta& meta);
};

inline bool is_null(const Value& v) {
    return std::holds_alternative<std::monostate>(v);
}

// --- WAL operation types ---

enum class WalOp : uint8_t {
    INSERT = 1,
    DELETE = 2,
    UPDATE = 3,
    CHECKPOINT = 4,
};

// --- CoW Database with WAL ---

class Database {
public:
    Database() = default;
    ~Database();

    bool open(const std::string& dir);
    void close();
    bool exec(const std::string& sql);

    bool create_snapshot(const std::string& name);
    bool restore_snapshot(const std::string& name);
    std::vector<std::string> list_snapshots();

private:
    struct Condition {
        size_t col = 0;
        std::string op;
        Value val;
    };

    // WAL
    bool wal_append(uint8_t op, const uint8_t* payload, uint32_t plen);
    bool wal_append_checkpoint();
    bool recover();
    bool checkpoint();

    // row helpers
    std::vector<uint8_t> serialize_row(const Row& r);
    Row deserialize_row(const uint8_t* data, size_t len);

    // query helpers
    size_t resolve_col(const std::string& name) const;
    bool eval(const Row& row, const Condition& c) const;

    // SQL parsing
    std::vector<std::string> tokenize(const std::string& sql);
    std::string to_upper(const std::string& s);
    void exec_insert(const std::vector<std::string>& tok, size_t& pos);
    void exec_select(const std::vector<std::string>& tok, size_t& pos);
    void exec_update(const std::vector<std::string>& tok, size_t& pos);
    void exec_delete(const std::vector<std::string>& tok, size_t& pos);
    void exec_create(const std::vector<std::string>& tok, size_t& pos);

    Value parse_value(const std::string& s);
    std::vector<Condition> parse_where(const std::vector<std::string>& tok, size_t& pos);

    // state
    std::string dir_;
    std::string wal_path_;
    std::string current_path_;
    std::string snap_dir_;
    std::vector<Row> rows_;
    std::vector<ColMeta> schema_;
    uint64_t wal_seq_ = 0;
    uint64_t checkpoint_seq_ = 0;
    bool dirty_ = false;
    bool opened_ = false;
    std::ofstream wal_stream_;
};

} // namespace zepto
