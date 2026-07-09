#include "zepto.h"
#include <cstdio>
#include <iostream>
int main() {
    // Write some data
    { zepto::Writer w("debug.zepto", 4096);
      w.add_column("id", zepto::ColumnType::I32);
      w.add_column("name", zepto::ColumnType::STRING);
      w.append_row({42, "hello"});
      w.flush_chunk();
    }
    // Read it back
    zepto::Reader r("debug.zepto");
    std::cout << std::flush;
    bool ok = r.open();
    std::cout << "open=" << ok << std::endl;
    std::cout << "integ=" << r.verify_integrity() << std::endl;
    std::cout << "chunks=" << r.num_chunks() << std::endl;
    std::cout << "rows=" << r.num_rows() << std::endl;
    if (r.num_chunks() > 0) {
        auto rows = r.read_chunk(0);
        std::cout << "read_chunk(0).size()=" << rows.size() << std::endl;
        if (!rows.empty()) {
            std::cout << "col0 type idx=" << rows[0].columns[0].index() << std::endl;
            std::cout << "col1 type idx=" << rows[0].columns[1].index() << std::endl;
            std::cout << "  id=" << std::get<int32_t>(rows[0].columns[0])
                      << " name=" << std::get<std::string>(rows[0].columns[1]) << std::endl;
        }
    }
    return 0;
}
