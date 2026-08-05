#include "zepto.h"
#include <cstdio>
#include <cstdlib>
#include <vector>

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("  FAIL: %s\n", msg); failures++; } \
    else { printf("  PASS: %s\n", msg); } \
} while (0)

using namespace zepto;

static bool rows_match(const std::vector<Row>& rows, size_t col,
                       const std::vector<int64_t>& expected, bool nullable,
                       const std::vector<bool>& nulls) {
    if (rows.size() != expected.size()) return false;
    for (size_t i = 0; i < rows.size(); i++) {
        if (nullable && nulls[i]) {
            if (!is_null(rows[i].columns[col])) return false;
        } else {
            if (is_null(rows[i].columns[col])) return false;
            const auto& v = rows[i].columns[col];
            if (std::holds_alternative<int64_t>(v)) {
                if (std::get<int64_t>(v) != expected[i]) return false;
            } else if (std::holds_alternative<int32_t>(v)) {
                if ((int64_t)std::get<int32_t>(v) != expected[i]) return false;
            } else {
                return false;
            }
        }
    }
    return true;
}

int main() {
    // ---- 1. Row-based: monotonic I64 -> DELTA, roundtrip ----
    {
        const size_t N = 10000;
        Writer w("dt1.zdb", 1 << 20);
        w.add_column("seq", ColumnType::I64, false, Encoding::BIT_PACKED);
        w.add_column("id", ColumnType::I32, false, Encoding::BIT_PACKED);
        for (size_t i = 0; i < N; i++) {
            w.append_row({(int64_t)i * 3, (int32_t)(i / 4)});
        }
        w.flush_chunk();
        w.close();
        Reader r("dt1.zdb");
        CHECK(r.open(), "open");
        auto meta = r.chunk_meta(0);
        CHECK(meta.pages.size() == 2, "two pages");
        CHECK(meta.pages[0].encoding == Encoding::DELTA, "I64 monotonic -> DELTA");
        CHECK(meta.pages[1].encoding == Encoding::DELTA, "I32 monotonic -> DELTA");
        auto rows = r.read_chunk(0);
        std::vector<int64_t> seq(N), id(N);
        for (size_t i = 0; i < N; i++) { seq[i] = (int64_t)i * 3; id[i] = (int64_t)(i / 4); }
        CHECK(rows_match(rows, 0, seq, false, {}), "I64 values roundtrip");
        CHECK(rows_match(rows, 1, id, false, {}), "I32 values roundtrip");
    }

    // ---- 2. Descending (non-increasing) I64 -> DELTA ----
    {
        const size_t N = 5000;
        Writer w("dt2.zdb", 1 << 20);
        w.add_column("cnt", ColumnType::I64);
        for (size_t i = 0; i < N; i++) w.append_row({(int64_t)(N - i)});
        w.flush_chunk();
        w.close();
        Reader r("dt2.zdb");
        CHECK(r.open(), "open");
        auto meta = r.chunk_meta(0);
        CHECK(meta.pages[0].encoding == Encoding::DELTA, "I64 descending -> DELTA");
        auto rows = r.read_chunk(0);
        std::vector<int64_t> exp(N);
        for (size_t i = 0; i < N; i++) exp[i] = (int64_t)(N - i);
        CHECK(rows_match(rows, 0, exp, false, {}), "descending values roundtrip");
    }

    // ---- 3. Non-monotonic BIT_PACKED stays BIT_PACKED ----
    {
        const size_t N = 1000;
        Writer w("dt3.zdb", 1 << 20);
        w.add_column("v", ColumnType::I32, false, Encoding::BIT_PACKED);
        for (size_t i = 0; i < N; i++) w.append_row({(int32_t)((i * 37) % 251)});
        w.flush_chunk();
        w.close();
        Reader r("dt3.zdb");
        CHECK(r.open(), "open");
        auto meta = r.chunk_meta(0);
        CHECK(meta.pages[0].encoding == Encoding::BIT_PACKED, "non-monotonic -> BIT_PACKED");
        auto rows = r.read_chunk(0);
        std::vector<int64_t> exp(N);
        for (size_t i = 0; i < N; i++) exp[i] = (int64_t)((i * 37) % 251);
        CHECK(rows_match(rows, 0, exp, false, {}), "values roundtrip");
    }

    // ---- 4. Monotonic with nulls (nullable) -> DELTA, roundtrip ----
    {
        const size_t N = 800;
        Writer w("dt4.zdb", 1 << 20);
        w.add_column("ts", ColumnType::I64, true);
        for (size_t i = 0; i < N; i++) {
            if (i % 4 == 0) w.append_row({Value{}});
            else w.append_row({(int64_t)i * 10});
        }
        w.flush_chunk();
        w.close();
        Reader r("dt4.zdb");
        CHECK(r.open(), "open");
        auto meta = r.chunk_meta(0);
        CHECK(meta.pages[0].encoding == Encoding::DELTA, "monotonic + nulls -> DELTA");
        auto rows = r.read_chunk(0);
        std::vector<int64_t> exp(N);
        std::vector<bool> nulls(N, false);
        for (size_t i = 0; i < N; i++) {
            if (i % 4 == 0) nulls[i] = true;
            else exp[i] = (int64_t)i * 10;
        }
        CHECK(rows_match(rows, 0, exp, true, nulls), "nullable DELTA roundtrip");
    }

    // ---- 5. Values near int64 sign boundary ----
    {
        const size_t N = 1000;
        Writer w("dt5.zdb", 1 << 20);
        w.add_column("big", ColumnType::I64);
        for (size_t i = 0; i < N; i++) w.append_row({(int64_t)(INT64_MAX - (N - i))});
        w.flush_chunk();
        w.close();
        Reader r("dt5.zdb");
        CHECK(r.open(), "open");
        auto meta = r.chunk_meta(0);
        CHECK(meta.pages[0].encoding == Encoding::DELTA, "near-INT64_MAX -> DELTA");
        auto rows = r.read_chunk(0);
        std::vector<int64_t> exp(N);
        for (size_t i = 0; i < N; i++) exp[i] = (int64_t)(INT64_MAX - (N - i));
        CHECK(rows_match(rows, 0, exp, false, {}), "sign-boundary roundtrip");
    }

    // ---- 6. Columnar path (write_columns) -> DELTA, read_chunk_cols roundtrip ----
    {
        const size_t N = 5000;
        Writer w("dt6.zdb", 1 << 20);
        w.add_column("id", ColumnType::I64);
        w.add_column("name", ColumnType::STRING, false, Encoding::DICT);
        std::vector<int64_t> ids(N);
        for (size_t i = 0; i < N; i++) ids[i] = (int64_t)i * 7;
        const char* names[] = {"a", "b", "c"};
        std::vector<std::string> vals;
        for (size_t i = 0; i < N; i++) vals.push_back(names[i % 3]);
        std::vector<uint8_t> sbuf;
        for (size_t i = 0; i < N; i++) {
            uint32_t l = (uint32_t)vals[i].size();
            sbuf.push_back((uint8_t)l); sbuf.push_back((uint8_t)(l >> 8));
            sbuf.push_back((uint8_t)(l >> 16)); sbuf.push_back((uint8_t)(l >> 24));
            sbuf.insert(sbuf.end(), (const uint8_t*)vals[i].data(),
                        (const uint8_t*)vals[i].data() + l);
        }
        Writer::WriteColArray c0, c1;
        c0.type = ColumnType::I64; c0.data = (const uint8_t*)ids.data(); c0.num_values = N;
        c1.type = ColumnType::STRING; c1.data = sbuf.data(); c1.num_values = N;
        w.write_columns({c0, c1}, (int)N);
        w.close();

        Reader r("dt6.zdb");
        CHECK(r.open(), "open");
        auto meta = r.chunk_meta(0);
        CHECK(meta.pages[0].encoding == Encoding::DELTA, "columnar I64 monotonic -> DELTA");
        auto cols = r.read_chunk_cols(0);
        CHECK(cols.size() == 2, "two columns read");
        if (cols.size() == 2) {
            bool ok = cols[0].num_values == N;
            const int64_t* got = reinterpret_cast<const int64_t*>(cols[0].data.data());
            for (size_t i = 0; i < N && ok; i++)
                if (got[i] != ids[i]) ok = false;
            CHECK(ok, "columnar I64 values roundtrip");
        }
    }

    // ---- 7. Row-based: duplicate-heavy I64 -> RLE, roundtrip ----
    {
        const size_t N = 10000;
        Writer w("dt7.zdb", 1 << 20);
        w.add_column("cat", ColumnType::I64);
        w.add_column("id", ColumnType::I32, false, Encoding::BIT_PACKED);
        for (size_t i = 0; i < N; i++) {
            w.append_row({(int64_t)((i / 500) % 8), (int32_t)((i / 1000) % 5)});
        }
        w.flush_chunk();
        w.close();
        Reader r("dt7.zdb");
        CHECK(r.open(), "open");
        auto meta = r.chunk_meta(0);
        CHECK(meta.pages.size() == 2, "two pages");
        CHECK(meta.pages[0].encoding == Encoding::RLE, "I64 runs -> RLE");
        CHECK(meta.pages[1].encoding == Encoding::RLE, "I32 runs -> RLE");
        auto rows = r.read_chunk(0);
        std::vector<int64_t> cat(N), id(N);
        for (size_t i = 0; i < N; i++) { cat[i] = (int64_t)((i / 500) % 8); id[i] = (int64_t)((i / 1000) % 5); }
        CHECK(rows_match(rows, 0, cat, false, {}), "I64 RLE values roundtrip");
        CHECK(rows_match(rows, 1, id, false, {}), "I32 RLE values roundtrip");
    }

    // ---- 8. Row-based: all-identical I64 -> RLE, roundtrip ----
    {
        const size_t N = 4000;
        Writer w("dt8.zdb", 1 << 20);
        w.add_column("const", ColumnType::I64);
        for (size_t i = 0; i < N; i++) w.append_row({(int64_t)42});
        w.flush_chunk();
        w.close();
        Reader r("dt8.zdb");
        CHECK(r.open(), "open");
        auto meta = r.chunk_meta(0);
        CHECK(meta.pages[0].encoding == Encoding::RLE, "all-identical -> RLE");
        auto rows = r.read_chunk(0);
        std::vector<int64_t> exp(N, 42);
        CHECK(rows_match(rows, 0, exp, false, {}), "constant values roundtrip");
    }

    // ---- 9. Columnar path: duplicate-heavy I64/I32 -> RLE, read_chunk_cols roundtrip ----
    {
        const size_t N = 6000;
        Writer w("dt9.zdb", 1 << 20);
        w.add_column("cat", ColumnType::I64);
        w.add_column("flag", ColumnType::I32, false, Encoding::BIT_PACKED);
        std::vector<int64_t> cat(N);
        std::vector<int32_t> flag(N);
        for (size_t i = 0; i < N; i++) { cat[i] = (int64_t)((i / 300) % 12); flag[i] = (int32_t)((i / 800) % 3); }
        Writer::WriteColArray c0, c1;
        c0.type = ColumnType::I64; c0.data = (const uint8_t*)cat.data(); c0.num_values = N;
        c1.type = ColumnType::I32; c1.data = (const uint8_t*)flag.data(); c1.num_values = N;
        w.write_columns({c0, c1}, (int)N);
        w.close();

        Reader r("dt9.zdb");
        CHECK(r.open(), "open");
        auto meta = r.chunk_meta(0);
        CHECK(meta.pages[0].encoding == Encoding::RLE, "columnar I64 runs -> RLE");
        CHECK(meta.pages[1].encoding == Encoding::RLE, "columnar I32 runs -> RLE");
        auto cols = r.read_chunk_cols(0);
        CHECK(cols.size() == 2, "two columns read");
        if (cols.size() == 2) {
            bool ok = cols[0].num_values == N && cols[1].num_values == N;
            const int64_t* c0g = reinterpret_cast<const int64_t*>(cols[0].data.data());
            const int32_t* c1g = reinterpret_cast<const int32_t*>(cols[1].data.data());
            for (size_t i = 0; i < N && ok; i++)
                if (c0g[i] != cat[i] || (int64_t)c1g[i] != (int64_t)flag[i]) ok = false;
            CHECK(ok, "columnar RLE values roundtrip");
        }
    }

    // ---- 10. Nullable RLE roundtrip ----
    {
        const size_t N = 900;
        Writer w("dt10.zdb", 1 << 20);
        w.add_column("c", ColumnType::I32, true);
        for (size_t i = 0; i < N; i++) {
            if (i % 3 == 0) w.append_row({Value{}});
            else w.append_row({(int32_t)((i / 100) % 6)});
        }
        w.flush_chunk();
        w.close();
        Reader r("dt10.zdb");
        CHECK(r.open(), "open");
        auto meta = r.chunk_meta(0);
        CHECK(meta.pages[0].encoding == Encoding::RLE, "nullable runs -> RLE");
        auto rows = r.read_chunk(0);
        std::vector<int64_t> exp(N);
        std::vector<bool> nulls(N, false);
        for (size_t i = 0; i < N; i++) {
            if (i % 3 == 0) nulls[i] = true;
            else exp[i] = (int64_t)((i / 100) % 6);
        }
        CHECK(rows_match(rows, 0, exp, true, nulls), "nullable RLE roundtrip");
    }

    // ---- 11. High-cardinality random stays BIT_PACKED (no spurious RLE) ----
    {
        const size_t N = 1000;
        Writer w("dt11.zdb", 1 << 20);
        w.add_column("v", ColumnType::I32, false, Encoding::BIT_PACKED);
        for (size_t i = 0; i < N; i++) w.append_row({(int32_t)((i * 37) % 251)});
        w.flush_chunk();
        w.close();
        Reader r("dt11.zdb");
        CHECK(r.open(), "open");
        auto meta = r.chunk_meta(0);
        CHECK(meta.pages[0].encoding == Encoding::BIT_PACKED, "random -> stays BIT_PACKED");
    }

    printf(failures == 0 ? "delta_test: all passed\n" : "delta_test: %d FAILURES\n", failures);
    return failures == 0 ? 0 : 1;
}
