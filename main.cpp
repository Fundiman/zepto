#include "zepto.h"
#include <iostream>
#include <cassert>
#include <fstream>

static size_t tests_passed = 0;
static size_t tests_failed = 0;

#define TEST(name) do { std::cout << "  " << name << "... "; } while(0)
#define PASS do { std::cout << "PASS\n"; tests_passed++; } while(0)
#define FAIL(msg) do { std::cout << "FAIL: " << msg << "\n"; tests_failed++; } while(0)

void test_basic_write_read() {
    std::cout << "=== test_basic_write_read ===\n";
    {
        zepto::Writer w("test_basic.zepto");
        w.add_column("id", zepto::ColumnType::I32);
        w.add_column("name", zepto::ColumnType::STRING);
        w.add_column("score", zepto::ColumnType::F64);

        for (int i = 0; i < 1000; i++) {
            w.append_row({i, "user_" + std::to_string(i), (double)i * 1.5});
        }
        TEST("wrote rows");
        if (w.rows_written() == 1000) PASS; else FAIL("rows_written mismatch");
    }

    {
        zepto::Reader r("test_basic.zepto");
        TEST("open");
        if (!r.open()) { FAIL("open failed"); return; } else PASS;

        TEST("verify integrity");
        if (!r.verify_integrity()) { FAIL("integrity check failed"); return; } else PASS;

        TEST("metadata");
        if (r.num_chunks() != 1 || r.num_rows() != 1000 || r.column_names().size() != 3)
            { FAIL("metadata mismatch"); return; } else PASS;

        TEST("column names");
        auto& names = r.column_names();
        if (names[0] == "id" && names[1] == "name" && names[2] == "score")
            PASS; else FAIL("names mismatch");

        auto rows = r.read_chunk(0);
        TEST("read data");
        if (rows.size() != 1000) { FAIL("row count"); return; } else PASS;

        TEST("first row");
        if (std::get<int32_t>(rows[0].columns[0]) == 0 &&
            std::get<std::string>(rows[0].columns[1]) == "user_0" &&
            std::get<double>(rows[0].columns[2]) == 0.0) PASS;
        else FAIL("first row mismatch");

        TEST("last row");
        auto& last = rows.back().columns;
        if (std::get<int32_t>(last[0]) == 999 &&
            std::get<std::string>(last[1]) == "user_999") PASS;
        else FAIL("last row mismatch");
    }
}

void test_query() {
    std::cout << "=== test_query ===\n";
    {
        zepto::Writer w("test_query.zepto");
        w.add_column("age", zepto::ColumnType::I32);
        w.add_column("name", zepto::ColumnType::STRING);

        for (int i = 0; i < 500; i++) {
            w.append_row({i % 100, "person_" + std::to_string(i)});
        }
    }

    {
        zepto::Reader r("test_query.zepto");
        assert(r.open());
        assert(r.verify_integrity());

        zepto::Query q;
        q.select_columns = {0, 1};
        q.predicates.push_back({0, zepto::Query::GT, (int64_t)90});
        q.limit = 5;

        auto res = r.query(q);
        TEST("query age > 90");
        if (res.total_rows == 5) PASS; else FAIL("expected 5 got " + std::to_string(res.total_rows));

        TEST("first result");
        if (std::get<int32_t>(res.rows[0].columns[0]) == 91) PASS;
        else FAIL("first result mismatch");
    }
}

void test_zone_map_skip() {
    std::cout << "=== test_zone_map_skip ===\n";
    {
        zepto::Writer w("test_zoneskip.zepto", 1024);
        w.add_column("x", zepto::ColumnType::I32);

        for (int i = 0; i < 100; i++) w.append_row({i});
        w.flush_chunk();
        for (int i = 200; i < 300; i++) w.append_row({i});
        w.flush_chunk();
        for (int i = 500; i < 600; i++) w.append_row({i});
    }

    {
        zepto::Reader r("test_zoneskip.zepto");
        assert(r.open());
        TEST("3 chunks");
        if (r.num_chunks() == 3) PASS; else FAIL("got " + std::to_string(r.num_chunks()));

        for (size_t i = 0; i < r.num_chunks(); i++) {
            auto m = r.chunk_meta(i);
            std::cout << "    chunk " << i << ": rows=" << m.num_rows
                      << " zone=[" << m.zone_maps[0].min_i64 << ","
                      << m.zone_maps[0].max_i64 << "]\n";
        }

        zepto::Query q;
        q.predicates.push_back({0, zepto::Query::GT, (int64_t)400});
        auto res = r.query(q);
        TEST("x > 400");
        if (res.total_rows == 100) PASS; else FAIL("expected 100 got " + std::to_string(res.total_rows));
        TEST("first value");
        if (std::get<int32_t>(res.rows[0].columns[0]) == 500) PASS;
        else FAIL("mismatch");
    }
}

void test_integrity() {
    std::cout << "=== test_integrity ===\n";
    {
        zepto::Writer w("test_integrity.zepto");
        w.add_column("val", zepto::ColumnType::I64);
        for (int64_t i = 0; i < 100; i++) w.append_row({i});
    }
    {
        zepto::Reader r("test_integrity.zepto");
        assert(r.open());
        TEST("clean");
        if (r.verify_integrity()) PASS; else FAIL("should be clean");

        std::fstream f("test_integrity.zepto", std::ios::binary | std::ios::in | std::ios::out);
        f.seekp(100);
        char bad = 0xFF;
        f.write(&bad, 1);
        f.close();
    }
    {
        zepto::Reader r("test_integrity.zepto");
        assert(r.open());
        bool ok = r.verify_integrity();
        TEST("corrupted");
        if (!ok) PASS; else FAIL("should detect corruption");
    }
}

void test_column_names_persisted() {
    std::cout << "=== test_column_names_persisted ===\n";
    {
        zepto::Writer w("test_names.zepto");
        w.add_column("hello", zepto::ColumnType::I32);
        w.add_column("world!!", zepto::ColumnType::STRING);
        w.add_column("a", zepto::ColumnType::F64);
        w.append_row({42, "test", 3.14});
        w.append_row({99, "foo", 2.71});
    }
    {
        zepto::Reader r("test_names.zepto");
        assert(r.open());
        TEST("names");
        auto& names = r.column_names();
        if (names.size() == 3 && names[0] == "hello" && names[1] == "world!!" && names[2] == "a") PASS;
        else FAIL("got " + (names.empty() ? "empty" : names[0] + "," + names[1] + "," + names[2]));

        TEST("types");
        if (r.column_types()[0] == zepto::ColumnType::I32 &&
            r.column_types()[1] == zepto::ColumnType::STRING &&
            r.column_types()[2] == zepto::ColumnType::F64) PASS;
        else FAIL("types mismatch");

        TEST("nullable default");
        if (!r.column_nullable()[0] && !r.column_nullable()[1] && !r.column_nullable()[2]) PASS;
        else FAIL("nullable should be false");
    }
}

void test_null_values() {
    std::cout << "=== test_null_values ===\n";
    {
        zepto::Writer w("test_nulls.zepto");
        w.add_column("val", zepto::ColumnType::I32, true);
        w.add_column("label", zepto::ColumnType::STRING, true);

        w.append_row({42, "hello"});
        w.append_row({std::monostate{}, "null_val"});
        w.append_row({99, std::monostate{}});
        w.append_row({std::monostate{}, std::monostate{}});
    }
    {
        zepto::Reader r("test_nulls.zepto");
        assert(r.open());

        TEST("nullable flags");
        if (r.column_nullable()[0] && r.column_nullable()[1]) PASS;
        else FAIL("not set");

        auto rows = r.read_chunk(0);
        TEST("row count");
        if (rows.size() == 4) PASS; else FAIL("expected 4 got " + std::to_string(rows.size()));

        TEST("row 0");
        if (!zepto::is_null(rows[0].columns[0]) && !zepto::is_null(rows[0].columns[1]) &&
            std::get<int32_t>(rows[0].columns[0]) == 42 &&
            std::get<std::string>(rows[0].columns[1]) == "hello") PASS;
        else FAIL("mismatch");

        TEST("row 1 val null");
        if (zepto::is_null(rows[1].columns[0]) &&
            !zepto::is_null(rows[1].columns[1]) &&
            std::get<std::string>(rows[1].columns[1]) == "null_val") PASS;
        else FAIL("mismatch");

        TEST("row 2 label null");
        if (!zepto::is_null(rows[2].columns[0]) &&
            zepto::is_null(rows[2].columns[1]) &&
            std::get<int32_t>(rows[2].columns[0]) == 99) PASS;
        else FAIL("mismatch");

        TEST("row 3 both null");
        if (zepto::is_null(rows[3].columns[0]) && zepto::is_null(rows[3].columns[1])) PASS;
        else FAIL("mismatch");
    }

    {
        zepto::Reader r("test_nulls.zepto");
        assert(r.open());
        zepto::Query q;
        q.predicates.push_back({0, zepto::Query::GT, (int64_t)10});
        auto res = r.query(q);
        TEST("nulls excluded from query");
        if (res.total_rows == 2) PASS;
        else FAIL("expected 2 got " + std::to_string(res.total_rows));
    }
}

void test_dict_encoding() {
    std::cout << "=== test_dict_encoding ===\n";
    std::vector<std::string> statuses = {"active", "inactive", "pending", "archived"};
    {
        zepto::Writer w("test_dict.zepto");
        w.add_column("id", zepto::ColumnType::I32);
        w.add_column("status", zepto::ColumnType::STRING, false, zepto::Encoding::DICT);

        for (int i = 0; i < 1000; i++) {
            w.append_row({i, statuses[i % 4]});
        }
    }
    {
        zepto::Reader r("test_dict.zepto");
        assert(r.open());

        TEST("encoding stored");
        if (r.column_encoding()[1] == zepto::Encoding::DICT) PASS;
        else FAIL("not DICT");

        auto rows = r.read_chunk(0);
        TEST("row count");
        if (rows.size() == 1000) PASS; else FAIL("expected 1000 got " + std::to_string(rows.size()));

        TEST("values correct");
        bool ok = true;
        for (int i = 0; i < 1000; i++) {
            if (std::get<std::string>(rows[i].columns[1]) != statuses[i % 4]) {
                ok = false; break;
            }
        }
        if (ok) PASS; else FAIL("value mismatch");

        std::ifstream f1("test_dict.zepto", std::ios::binary | std::ios::ate);
        auto s1 = f1.tellg();
        TEST("dict file has data");
        if (s1 > 200) PASS; else FAIL("file too small: " + std::to_string(s1));
    }
}

void test_bit_packed() {
    std::cout << "=== test_bit_packed ===\n";
    {
        zepto::Writer w("test_bitpack.zepto");
        w.add_column("small", zepto::ColumnType::I32, false, zepto::Encoding::BIT_PACKED);
        w.add_column("big", zepto::ColumnType::I64, false, zepto::Encoding::BIT_PACKED);

        for (int i = 0; i < 500; i++) {
            w.append_row({i % 16, (int64_t)i * 1000});
        }
    }
    {
        zepto::Writer wp("test_bitpack_plain.zepto");
        wp.add_column("small", zepto::ColumnType::I32);
        wp.add_column("big", zepto::ColumnType::I64);
        for (int i = 0; i < 500; i++) {
            wp.append_row({i % 16, (int64_t)i * 1000});
        }
    }

    {
        zepto::Reader r("test_bitpack.zepto");
        assert(r.open());

        TEST("encoding stored");
        if (r.column_encoding()[0] == zepto::Encoding::BIT_PACKED &&
            r.column_encoding()[1] == zepto::Encoding::BIT_PACKED) PASS;
        else FAIL("not BIT_PACKED");

        auto rows = r.read_chunk(0);
        TEST("row count");
        if (rows.size() == 500) PASS; else FAIL("expected 500 got " + std::to_string(rows.size()));

        TEST("values correct");
        bool ok = true;
        for (int i = 0; i < 500; i++) {
            if (std::get<int32_t>(rows[i].columns[0]) != i % 16 ||
                std::get<int64_t>(rows[i].columns[1]) != (int64_t)i * 1000) {
                ok = false; break;
            }
        }
        if (ok) PASS; else FAIL("value mismatch");
    }

    {
        std::ifstream f1("test_bitpack.zepto", std::ios::binary | std::ios::ate);
        std::ifstream f2("test_bitpack_plain.zepto", std::ios::binary | std::ios::ate);
        auto s1 = f1.tellg();
        auto s2 = f2.tellg();
        TEST("bitpack smaller than plain");
        if (s1 < s2) PASS;
        else FAIL("bitpack " + std::to_string(s1) + " >= plain " + std::to_string(s2));
    }
}

int main() {
    test_basic_write_read();
    test_query();
    test_zone_map_skip();
    test_integrity();
    test_column_names_persisted();
    test_null_values();
    test_dict_encoding();
    test_bit_packed();

    std::cout << "\nResults: " << (tests_passed + tests_failed)
              << " tests, " << tests_passed << " passed, "
              << tests_failed << " failed.\n";
    return tests_failed > 0 ? 1 : 0;
}
