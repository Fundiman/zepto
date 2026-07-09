#include "zepto.h"
#include <iostream>
#include <fstream>
#include <vector>
#include <cstring>
#include <random>

static int total = 0, passed = 0;
#define CHECK(cond, msg) do { \
    total++; \
    if (!(cond)) { std::cout << "  FAIL: " << msg << "\n"; } \
    else { std::cout << "  PASS: " << msg << "\n"; passed++; } \
    std::cout.flush(); \
} while(0)

// Corrupt 17+ consecutive bytes to exceed RS(223,32) correction capability (t=16)
static void corrupt_multi(const std::string& path, size_t offset, int count = 20) {
    std::fstream f(path, std::ios::binary | std::ios::in | std::ios::out);
    for (int i = 0; i < count; i++) {
        f.seekp(offset + i);
        char c;
        f.read(&c, 1);
        f.seekp(offset + i);
        c ^= 0xFF;
        f.write(&c, 1);
    }
    f.close();
}

static void corrupt_file(const std::string& path, size_t offset) {
    std::fstream f(path, std::ios::binary | std::ios::in | std::ios::out);
    f.seekp(offset);
    char c;
    f.read(&c, 1);
    f.seekp(offset);
    c ^= 0xFF;
    f.write(&c, 1);
    f.close();
}

int main() {
    std::cout << "=== Creating test file ===\n";
    {
        zepto::Writer w("corrupt.rw.zepto", 4096);
        w.add_column("id", zepto::ColumnType::I32, false, zepto::Encoding::BIT_PACKED);
        w.add_column("val", zepto::ColumnType::I64);
        w.add_column("tag", zepto::ColumnType::STRING, false, zepto::Encoding::DICT);
        w.add_column("data", zepto::ColumnType::F64, true);
        for (int c = 0; c < 3; c++) {
            for (int i = 0; i < 100; i++)
                w.append_row({
                    i % 16,
                    (int64_t)(c * 100 + i) * 10,
                    "g" + std::to_string((c * 100 + i) % 4),
                    (i % 5 == 0) ? zepto::Value{} : zepto::Value((double)i * 0.5)
                });
            w.flush_chunk();
        }
    }
    std::cout << "  file created\n";

    // Test 1: Verify clean file works
    std::cout << "\n=== 1. Clean file ===\n";
    {
        zepto::Reader r("corrupt.rw.zepto");
        CHECK(r.open(), "open");
        CHECK(r.verify_integrity(), "integrity");
        CHECK(r.num_chunks() == 3, "3 chunks");
        CHECK(r.num_rows() == 300, "300 rows");
        CHECK(r.column_names()[0] == "id", "col0 name");
        CHECK(r.column_nullable()[3], "col3 nullable");
        auto rows = r.read_chunk(0);
        CHECK(rows.size() == 100, ("chunk0 has 100 rows (got " + std::to_string(rows.size()) + ")").c_str());
        CHECK(std::get<int32_t>(rows[0].columns[0]) == 0, "first id");
        CHECK(!zepto::is_null(rows[1].columns[3]), "row1 not null");
        CHECK(zepto::is_null(rows[0].columns[3]) || zepto::is_null(rows[5].columns[3]),
              "row0 or row5 null (i%5==0)");
        auto rows1 = r.read_chunk(2);
        CHECK(std::get<std::string>(rows1[0].columns[2]) == "g0", "chunk2 tag");
    }

    // Test 2: Corrupt file header magic
    std::cout << "\n=== 2. Header magic corruption ===\n";
    {
        corrupt_file("corrupt.rw.zepto", 0);
        zepto::Reader r("corrupt.rw.zepto");
        CHECK(!r.open(), "rejects bad magic");
    }

    // Re-create clean for remaining tests
    {
        zepto::Writer w("corrupt.rw.zepto", 4096);
        w.add_column("id", zepto::ColumnType::I32, false, zepto::Encoding::BIT_PACKED);
        w.add_column("val", zepto::ColumnType::I64);
        w.add_column("tag", zepto::ColumnType::STRING, false, zepto::Encoding::DICT);
        w.add_column("data", zepto::ColumnType::F64, true);
        for (int c = 0; c < 3; c++) {
            for (int i = 0; i < 100; i++)
                w.append_row({i%16, (int64_t)(c*100+i)*10, "g" + std::to_string((c*100+i)%4),
                              (i%5==0)?zepto::Value{}:zepto::Value((double)i*0.5)});
            w.flush_chunk();
        }
    }

    // Test 3: Corrupt version byte
    std::cout << "\n=== 3. Version corruption ===\n";
    {
        corrupt_file("corrupt.rw.zepto", 5);
        zepto::Reader r("corrupt.rw.zepto");
        CHECK(!r.open(), "rejects bad version");
    }

    // Re-create
    {
        zepto::Writer w("corrupt.rw.zepto", 4096);
        w.add_column("id", zepto::ColumnType::I32, false, zepto::Encoding::BIT_PACKED);
        w.add_column("val", zepto::ColumnType::I64);
        w.add_column("tag", zepto::ColumnType::STRING, false, zepto::Encoding::DICT);
        w.add_column("data", zepto::ColumnType::F64, true);
        for (int c = 0; c < 3; c++) {
            for (int i = 0; i < 100; i++)
                w.append_row({i%16, (int64_t)(c*100+i)*10, "g" + std::to_string((c*100+i)%4),
                              (i%5==0)?zepto::Value{}:zepto::Value((double)i*0.5)});
            w.flush_chunk();
        }
    }

    // Test 4: Corrupt metadata CRC
    std::cout << "\n=== 4. Metadata CRC ===\n";
    {
        std::fstream f("corrupt.rw.zepto", std::ios::binary | std::ios::in | std::ios::out);
        uint32_t meta_size;
        f.seekg(20);
        f.read(reinterpret_cast<char*>(&meta_size), 4);
        corrupt_file("corrupt.rw.zepto", 24 + meta_size - 4);
        zepto::Reader r("corrupt.rw.zepto");
        CHECK(!r.open(), "rejects bad metadata CRC");
    }

    // Test 5: Corrupt first chunk data page (beyond RS correction limit of 16 errors/block)
    std::cout << "\n=== 5. Chunk data corruption ===\n";
    {
        zepto::Writer w("corrupt_c.zepto", 4096);
        w.add_column("a", zepto::ColumnType::I32);
        w.add_column("b", zepto::ColumnType::I64);
        for (int i = 0; i < 50; i++) w.append_row({i, (int64_t)i*10});
        w.flush_chunk();
        for (int i = 100; i < 150; i++) w.append_row({i, (int64_t)i*10});
    }
    {
        // Find the first chunk's interleaved data offset
        std::fstream f("corrupt_c.zepto", std::ios::binary | std::ios::in);
        uint32_t meta_size;
        f.seekg(20);
        f.read(reinterpret_cast<char*>(&meta_size), 4);
        f.close();
        // First chunk header at 24+meta_size, interleaved data at 24+meta_size+8
        size_t data_start = 24 + meta_size + 8;
        // Corrupt 200 bytes (spans all RS blocks, many errors per block)
        corrupt_multi("corrupt_c.zepto", data_start, 200);
        zepto::Reader r("corrupt_c.zepto");
        bool opened = r.open();
        bool integ = r.verify_integrity();
        CHECK(!opened || !integ, "corruption detected by open() or verify_integrity()");
    }

    // Test 6: Corrupt chunk trailing CRC (>16 bytes to exceed RS correction)
    std::cout << "\n=== 6. Chunk CRC corruption ===\n";
    {
        zepto::Writer w("corrupt_crc2.zepto");
        w.add_column("a", zepto::ColumnType::I32);
        for (int i = 0; i < 20; i++) w.append_row({i});
        w.flush_chunk();
        for (int i = 50; i < 70; i++) w.append_row({i});
    }
    {
        std::fstream f("corrupt_crc2.zepto", std::ios::binary | std::ios::in);
        f.seekg(0, std::ios::end);
        auto sz = (uint64_t)f.tellg();
        f.close();
        // Corrupt 20 bytes at end of interleaved data → exceeds t=16 per block
        corrupt_multi("corrupt_crc2.zepto", (size_t)(sz - 20));
        zepto::Reader r("corrupt_crc2.zepto");
        // open() may fail (read_chunk_meta fails on corrupted chunk),
        // but at least verify_integrity will detect it
        bool opened = r.open();
        bool integ = r.verify_integrity();
        CHECK(!opened || !integ, "corruption detected by open() or verify_integrity()");
        if (opened) {
            auto r0 = r.read_chunk(0);
            CHECK(r0.size() == 20, "first chunk still readable");
            CHECK(std::get<int32_t>(r0[0].columns[0]) == 0, "chunk0 data intact");
        }
    }
    
    // Test 7: Truncated file
    std::cout << "\n=== 7. Truncation ===\n";
    {
        // Re-create clean file
        zepto::Writer w("corrupt_trunc2.zepto");
        w.add_column("a", zepto::ColumnType::I32);
        for (int i = 0; i < 20; i++) w.append_row({i});
        w.flush_chunk();
        for (int i = 50; i < 70; i++) w.append_row({i});
    }
    {
        std::fstream f("corrupt_trunc2.zepto", std::ios::binary | std::ios::in);
        f.seekg(0, std::ios::end);
        auto sz = (uint64_t)f.tellg();
        f.seekg(0);
        // Truncate enough to definitely cut into interleaved data
        auto trunc_sz = sz - 100;
        std::vector<char> buf((size_t)trunc_sz);
        f.read(buf.data(), buf.size());
        f.close();
        std::ofstream of("corrupt_trunc.zepto", std::ios::binary);
        of.write(buf.data(), buf.size());
        of.close();
        zepto::Reader r("corrupt_trunc.zepto");
        bool opened = r.open();
        bool integ = r.verify_integrity();
        CHECK(!opened || !integ, "truncation detected by open() or verify_integrity()");
    }

    // Test 8: Empty file
    std::cout << "\n=== 8. Empty file ===\n";
    {
        std::ofstream f("corrupt_empty.zepto", std::ios::binary);
        f.close();
        zepto::Reader r("corrupt_empty.zepto");
        CHECK(!r.open(), "rejects empty file");
    }

    // Test 9: Corrupt page directory offset (20 bytes to exceed RS correction)
    std::cout << "\n=== 9. Page directory corruption ===\n";
    {
        zepto::Writer w("corrupt_dir.zepto");
        w.add_column("x", zepto::ColumnType::I32);
        for (int i = 0; i < 10; i++) w.append_row({i});
    }
    {
        std::fstream f("corrupt_dir.zepto", std::ios::binary | std::ios::in);
        uint32_t meta_size;
        f.seekg(20);
        f.read(reinterpret_cast<char*>(&meta_size), 4);
        f.close();
        size_t dir_off_pos = 24 + meta_size + 12;
        corrupt_multi("corrupt_dir.zepto", dir_off_pos, 20);
    }
    {
        zepto::Reader r("corrupt_dir.zepto");
        bool opened = r.open();
        bool integ = r.verify_integrity();
        CHECK(!opened || !integ, "dir corruption detected by open() or verify_integrity()");
    }

    std::cout << "\n=== " << passed << "/" << total << " passed ===\n";
    return passed == total ? 0 : 1;
}
