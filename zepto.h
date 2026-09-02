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
#include <mutex>
#include <condition_variable>
#include <thread>
#include <deque>
#include <atomic>

namespace zepto {

constexpr uint32_t MAGIC = 0x5450455A; // "ZEPT" little-endian
constexpr uint16_t VERSION = 4;
constexpr size_t DEFAULT_CHUNK_SIZE = 64 * 1024 * 1024; // 64MB

// File-level flags (stored in header flags field, 4 bytes at offset 24)
constexpr uint32_t FLAG_RS_ECC   = 0x01;  // bit 0: chunks use RS error correction
constexpr uint32_t FLAG_LZ4      = 0x02;  // bit 1: pages use LZ4 block compression
constexpr uint32_t FLAG_ZSTD     = 0x04;  // bit 2: pages use ZSTD block compression

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
    RLE = 4,
};

// Compression codec applied to each chunk (stored in file header flags)
enum class Codec : uint8_t {
    NONE = 0,
    LZ4 = 1,
    ZSTD = 2,
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
    Writer(std::string path, size_t chunk_size = DEFAULT_CHUNK_SIZE,
           bool use_rs = false, Codec codec = Codec::NONE);
    ~Writer();
    Writer(const Writer&) = delete;
    Writer& operator=(const Writer&) = delete;
    Writer(Writer&&) noexcept;
    Writer& operator=(Writer&&) noexcept;

    bool add_column(const std::string& name, ColumnType type, bool nullable = false, Encoding encoding = Encoding::PLAIN);
    bool append_row(const std::vector<Value>& row);
    bool begin_batch();
    bool push_value(size_t col_idx, Value val);
    bool push_col_i32(size_t col_idx, const int32_t* vals, size_t count);
    bool push_col_i64(size_t col_idx, const int64_t* vals, size_t count);
    bool push_col_f32(size_t col_idx, const float* vals, size_t count);
    bool push_col_f64(size_t col_idx, const double* vals, size_t count);
    bool push_col_str(size_t col_idx, const char* const* strs, const size_t* lens, size_t count);
    bool end_batch();
    bool finish_batch(size_t num_rows);
    uint64_t flush_chunk();
    uint64_t rows_written() const { return total_rows_; }
    void close();

    // Typed columnar write: encode directly from raw arrays (no variant overhead)
    struct WriteColArray {
        ColumnType type;
        const uint8_t* data = nullptr;       // typed values
        const uint8_t* validity = nullptr;   // null bitmap (1=valid, 0=null)
        size_t num_values = 0;
    };
    void write_chunk_cols(const std::vector<WriteColArray>& cols,
                          const std::vector<ColMeta>& meta,
                          uint32_t idx);

    // High-level: write a full chunk from typed arrays (handles metadata + state)
    bool write_columns(const std::vector<WriteColArray>& cols, int num_rows);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::string path_;
    size_t chunk_size_;
    bool use_rs_;
    Codec codec_;
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

    // Typed columnar read — returns raw arrays per column, no variant overhead
    struct ColumnArray {
        ColumnType type;
        std::vector<uint8_t> data;       // typed values (int32_t[], double[], etc.)
        std::vector<uint8_t> validity;   // null bitmap (1=valid, 0=null)
        std::vector<size_t> offsets;     // flat string layout: offsets[i] = start of string i in data
        size_t num_values = 0;
    };
    std::vector<ColumnArray> read_chunk_cols(size_t chunk_idx,
                                             const std::vector<size_t>& col_indices = {});

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
    bool use_rs_ = false;
    Codec codec_ = Codec::NONE;
    uint16_t file_version_ = 0;

    bool read_header();
    bool read_chunk_meta(ChunkMeta& meta);
    std::vector<uint8_t> read_and_decode_chunk(size_t chunk_idx);
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

    // dir may be a database directory (data.zdb + wal + snapshots/) or a
    // .zdb checkpoint file. A .zdb path names the checkpoint file directly:
    // WAL at <name>/wal/wal, checkpoint at <name>/data/<name>.zdb.
    bool open(const std::string& dir);
    void close();
    bool exec(const std::string& sql);

    // Runs a SELECT, printing its output to std::cout line by line as the
    // scan finds matches. Used internally by StreamedQuery (which redirects
    // std::cout to a sink while running this on a worker thread).
    void stream_select(const std::string& sql);

    bool create_snapshot(const std::string& name);
    bool restore_snapshot(const std::string& name);
    std::vector<std::string> list_snapshots();

    // Query results from last SELECT exec()
    const std::vector<std::string>& query_col_names() const { return query_col_names_; }
    const std::vector<ColumnType>& query_col_types() const { return query_col_types_; }
    const std::vector<std::vector<std::string>>& query_rows() const { return query_rows_; }
    int query_row_count() const { return (int)query_rows_.size(); }
    int query_col_count() const { return (int)query_col_names_.size(); }

private:
    struct Condition {
        size_t col = 0;
        std::string op;        // "=" "!=" "<>" "<" ">" "<=" ">=" "LIKE" "IS NULL" "IS NOT NULL" "IN" "NOT IN"
        Value val;
        std::vector<Value> val_list; // for IN/NOT IN
        bool not_like = false; // NOT LIKE
    };

    // An OR-group: the row matches if ANY ConditionGroup is fully satisfied
    using ConditionGroup = std::vector<Condition>;
    using WhereClause = std::vector<ConditionGroup>;

    // SELECT helpers
    enum class AggFunc : uint8_t { NONE, COUNT, SUM, AVG, MIN, MAX };
    struct SelectCol {
        size_t col = 0;
        bool is_star = false;
        AggFunc agg = AggFunc::NONE;
        bool distinct = false;
    };
    struct OrderByItem {
        size_t col = 0;
        bool asc = true;
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
    bool eval_where(const Row& row, const WhereClause& wc) const;
    bool like_match(const std::string& s, const std::string& pat) const;

    // SQL parsing
    std::vector<std::string> tokenize(const std::string& sql);
    std::string to_upper(const std::string& s);
    void exec_insert(const std::vector<std::string>& tok, size_t& pos);
    void exec_select(const std::vector<std::string>& tok, size_t& pos);
    void exec_update(const std::vector<std::string>& tok, size_t& pos);
    void exec_delete(const std::vector<std::string>& tok, size_t& pos);
    void exec_create(const std::vector<std::string>& tok, size_t& pos);
    void exec_drop(const std::vector<std::string>& tok, size_t& pos);
    void exec_alter(const std::vector<std::string>& tok, size_t& pos);

    Value parse_value(const std::string& s);
    WhereClause parse_where(const std::vector<std::string>& tok, size_t& pos);

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

    // query result storage (populated by exec_select)
    std::vector<std::string> query_col_names_;
    std::vector<ColumnType> query_col_types_;
    std::vector<std::vector<std::string>> query_rows_;
    std::ofstream wal_stream_;
};

// --- Streaming SELECT ---
// Runs a SELECT on a worker thread and delivers each matched row as the scan
// finds it, instead of waiting for the whole result set. Row values are
// strings, matching the buffered exec() results. Aggregate / GROUP BY /
// DISTINCT / ORDER BY queries necessarily buffer until the scan completes
// before producing any row, but a plain SELECT streams rows immediately.
class StreamedQuery {
public:
    StreamedQuery() = default;
    ~StreamedQuery();
    StreamedQuery(const StreamedQuery&) = delete;
    StreamedQuery& operator=(const StreamedQuery&) = delete;

    // Spawn the query worker and block until the result header is available
    // (or the query fails before scanning). Returns false and sets error()
    // on query-level failure (e.g. "no table", syntax error).
    bool start(Database& db, const std::string& sql);

    // Block until the next row is produced. Returns true when row() holds a
    // new row; false when the stream is exhausted.
    bool next();

    // True once the stream is exhausted.
    bool done() const { return impl_ && impl_->exhausted; }

    // Non-empty on query-level failure.
    const std::string& error() const;

    // Result schema. All columns are reported as STRING (same as exec()).
    int col_count() const;
    const std::string& col_name(int index) const;
    ColumnType col_type(int index) const;

    // Most recently produced row; valid after next() returns true.
    const std::vector<std::string>& row() const;

    // Number of rows yielded so far.
    int row_count() const;

private:
    struct Impl {
        std::mutex mtx;
        std::condition_variable cv;
        std::deque<std::string> lines;
        bool finished = false;
        bool header_ok = false;
        bool exhausted = false;
        std::string error;
        std::vector<std::string> col_names;
        std::vector<std::string> current_row;
        int rows_yielded = 0;
        std::atomic<bool> cancel{false};
        std::thread worker;

        void push_line(const std::string& line) {
            if (cancel.load(std::memory_order_relaxed)) return;
            std::lock_guard<std::mutex> lock(mtx);
            lines.push_back(line);
            cv.notify_one();
        }
        void finish() {
            std::lock_guard<std::mutex> lock(mtx);
            finished = true;
            cv.notify_all();
        }
    };

    class SinkBuf; // std::streambuf adapter, defined in zepto.cpp

    std::shared_ptr<Impl> impl_;
    static void worker_run(std::shared_ptr<Impl> impl, Database* db, std::string sql);
    bool consume_header();
    static std::vector<std::string> split_cell_line(const std::string& line);
};

} // namespace zepto
