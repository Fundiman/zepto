#include "zepto.h"
#include <iostream>
#include <fstream>

int main() {
    // Create same file as corruption test
    {
        zepto::Writer w("debug_d.zepto", 4096);
        w.add_column("id", zepto::ColumnType::I32, false, zepto::Encoding::BIT_PACKED);
        w.add_column("val", zepto::ColumnType::I64);
        w.add_column("tag", zepto::ColumnType::STRING, false, zepto::Encoding::DICT);
        w.add_column("data", zepto::ColumnType::F64, true);
        for (int c = 0; c < 3; c++) {
            for (int i = 0; i < 100; i++)
                w.append_row({i%16, (int64_t)(c*100+i)*10,
                              "g" + std::to_string((c*100+i)%4),
                              (i%5==0)?zepto::Value{}:zepto::Value((double)i*0.5)});
            w.flush_chunk();
        }
    }

    zepto::Reader r("debug_d.zepto");
    std::cout << "open: " << r.open() << "\n";
    std::cout << "integrity: " << r.verify_integrity() << "\n";

    for (size_t ci = 0; ci < r.num_chunks(); ci++) {
        auto m = r.chunk_meta(ci);
        std::cout << "chunk " << ci << ": meta.num_rows=" << m.num_rows << "\n";
        auto rows = r.read_chunk(ci);
        std::cout << "  read_chunk returned " << rows.size() << "\n";
        if (rows.size() > 0)
            std::cout << "  [0][0]=" << std::get<int32_t>(rows[0].columns[0]) << "\n";
    }

    // Check raw bytes in chunk header for chunk 0
    std::ifstream f("debug_d.zepto", std::ios::binary);
    auto m = r.chunk_meta(0);
    f.seekg(m.chunk_offset);
    char buf[16];
    f.read(buf, 16);
    uint32_t magic; memcpy(&magic, buf, 4);
    uint16_t idx; memcpy(&idx, buf+4, 2);
    uint16_t nc; memcpy(&nc, buf+6, 2);
    uint32_t nr; memcpy(&nr, buf+8, 4);
    uint32_t droff; memcpy(&droff, buf+12, 4);
    std::cout << "raw chunk header: magic=" << std::hex << magic << " idx=" << idx
              << " nc=" << nc << " nr=" << std::dec << nr << " droff=" << droff << "\n";
    return 0;
}
