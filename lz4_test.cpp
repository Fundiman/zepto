#include "zepto.h"
#include <cstdio>
int main() {
    // Test 1: row-based write with RS off, LZ4 off
    {
        zepto::Writer w("t1.zdb", zepto::DEFAULT_CHUNK_SIZE, false, zepto::Codec::NONE);
        w.add_column("id", zepto::ColumnType::I32);
        w.add_column("name", zepto::ColumnType::STRING);
        std::vector<zepto::Value> row1 = { (int32_t)1, std::string("alice") };
        std::vector<zepto::Value> row2 = { (int32_t)2, std::string("bob") };
        w.append_row(row1);
        w.append_row(row2);
        w.close();
        printf("T1 write done (no rs, no lz4)\n");
    }
    {
        zepto::Reader r("t1.zdb");
        bool ok = r.open();
        printf("T1 open=%d\n", ok);
        if (ok) {
            printf("T1 rows=%zu chunks=%zu\n", r.num_rows(), r.num_chunks());
            auto chunk = r.read_chunk(0);
            printf("T1 chunk rows=%zu\n", chunk.size());
            for (auto& row : chunk) {
                for (auto& v : row.columns) {
                    if (std::holds_alternative<std::monostate>(v)) printf("NULL ");
                    else if (std::holds_alternative<int32_t>(v)) printf("%d ", std::get<int32_t>(v));
                    else if (std::holds_alternative<int64_t>(v)) printf("%ld ", std::get<int64_t>(v));
                    else if (std::holds_alternative<double>(v)) printf("%.1f ", std::get<double>(v));
                    else if (std::holds_alternative<std::string>(v)) printf("%s ", std::get<std::string>(v).c_str());
                }
                printf("\n");
            }
        }
    }
    // Test 2: row-based write with LZ4 on
    {
        zepto::Writer w("t2.zdb", zepto::DEFAULT_CHUNK_SIZE, false, zepto::Codec::LZ4);
        w.add_column("id", zepto::ColumnType::I32);
        w.add_column("name", zepto::ColumnType::STRING);
        std::vector<zepto::Value> row1 = { (int32_t)1, std::string("alice") };
        std::vector<zepto::Value> row2 = { (int32_t)2, std::string("bob") };
        w.append_row(row1);
        w.append_row(row2);
        w.close();
        printf("T2 write done (no rs, lz4)\n");
    }
    {
        zepto::Reader r("t2.zdb");
        bool ok = r.open();
        printf("T2 open=%d\n", ok);
        if (ok) {
            printf("T2 rows=%zu chunks=%zu\n", r.num_rows(), r.num_chunks());
        }
    }
    // Test 3: row-based write with RS on
    {
        zepto::Writer w("t3.zdb", zepto::DEFAULT_CHUNK_SIZE, true, zepto::Codec::NONE);
        w.add_column("id", zepto::ColumnType::I32);
        w.add_column("name", zepto::ColumnType::STRING);
        std::vector<zepto::Value> row1 = { (int32_t)1, std::string("alice") };
        std::vector<zepto::Value> row2 = { (int32_t)2, std::string("bob") };
        w.append_row(row1);
        w.append_row(row2);
        w.close();
        printf("T3 write done (rs, no lz4)\n");
    }
    {
        zepto::Reader r("t3.zdb");
        bool ok = r.open();
        printf("T3 open=%d\n", ok);
        if (ok) {
            printf("T3 rows=%zu chunks=%zu\n", r.num_rows(), r.num_chunks());
        }
    }
    // Test 4: columnar write with LZ4 on (Python path)
    {
        zepto::Writer w("t4.zdb", zepto::DEFAULT_CHUNK_SIZE, false, zepto::Codec::LZ4);
        w.add_column("id", zepto::ColumnType::I32);
        w.add_column("name", zepto::ColumnType::STRING);
        int32_t ids[] = {1, 2};
        const char* names[] = {"alice", "bob"};
        uint32_t name_lens[] = {5, 3};
        uint32_t total_len = name_lens[0] + name_lens[1];
        std::vector<uint8_t> name_buf(total_len + 4 * 2 + 4);
        uint8_t* p = name_buf.data();
        for (int i = 0; i < 2; i++) {
            memcpy(p, &name_lens[i], 4); p += 4;
            memcpy(p, names[i], name_lens[i]); p += name_lens[i];
        }
        zepto::Writer::WriteColArray wc0, wc1;
        wc0.type = zepto::ColumnType::I32;
        wc0.data = (const uint8_t*)ids;
        wc0.num_values = 2;
        wc0.validity = nullptr;
        wc1.type = zepto::ColumnType::STRING;
        wc1.data = name_buf.data();
        wc1.num_values = 2;
        wc1.validity = nullptr;
        w.write_columns({wc0, wc1}, 2);
        w.close();
        printf("T4 write done (col, lz4)\n");
    }
    {
        zepto::Reader r("t4.zdb");
        bool ok = r.open();
        printf("T4 open=%d\n", ok);
        if (ok) {
            printf("T4 rows=%zu chunks=%zu\n", r.num_rows(), r.num_chunks());
            auto chunks = r.read_chunk_cols(0);
            printf("T4 col chunks=%zu\n", chunks.size());
            for (size_t i = 0; i < chunks.size(); i++) {
                printf("  col%zu: type=%d num_vals=%zu\n", i, (int)chunks[i].type, chunks[i].num_values);
            }
        }
    }
    // Test 5: row-based write with ZSTD on
    {
        zepto::Writer w("t5.zdb", zepto::DEFAULT_CHUNK_SIZE, false, zepto::Codec::ZSTD);
        w.add_column("id", zepto::ColumnType::I32);
        w.add_column("name", zepto::ColumnType::STRING);
        std::vector<zepto::Value> row1 = { (int32_t)1, std::string("alice") };
        std::vector<zepto::Value> row2 = { (int32_t)2, std::string("bob") };
        w.append_row(row1);
        w.append_row(row2);
        w.close();
        printf("T5 write done (no rs, zstd)\n");
    }
    {
        zepto::Reader r("t5.zdb");
        bool ok = r.open();
        printf("T5 open=%d\n", ok);
        if (ok) {
            printf("T5 rows=%zu chunks=%zu\n", r.num_rows(), r.num_chunks());
            auto chunk = r.read_chunk(0);
            printf("T5 chunk rows=%zu\n", chunk.size());
            for (auto& row : chunk) {
                for (auto& v : row.columns) {
                    if (std::holds_alternative<std::monostate>(v)) printf("NULL ");
                    else if (std::holds_alternative<int32_t>(v)) printf("%d ", std::get<int32_t>(v));
                    else if (std::holds_alternative<int64_t>(v)) printf("%ld ", std::get<int64_t>(v));
                    else if (std::holds_alternative<double>(v)) printf("%.1f ", std::get<double>(v));
                    else if (std::holds_alternative<std::string>(v)) printf("%s ", std::get<std::string>(v).c_str());
                }
                printf("\n");
            }
        }
    }
}
