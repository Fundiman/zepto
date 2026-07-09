#include "zepto.h"
#include <iostream>
int main() {
    { zepto::Writer w("corrupt.rw.zepto", 4096);
      w.add_column("id", zepto::ColumnType::I32, false, zepto::Encoding::BIT_PACKED);
      w.add_column("val", zepto::ColumnType::I64);
      w.add_column("tag", zepto::ColumnType::STRING, false, zepto::Encoding::DICT);
      w.add_column("data", zepto::ColumnType::F64, true);
      for (int c = 0; c < 3; c++) {
        for (int i = 0; i < 100; i++)
          w.append_row({i%16, (int64_t)(c*100+i)*10, "g"+std::to_string((c*100+i)%4),
                        (i%5==0)?zepto::Value{}:zepto::Value((double)i*0.5)});
        w.flush_chunk();
    }}
    zepto::Reader r("corrupt.rw.zepto");
    std::cout << "open=" << r.open() << " integ=" << r.verify_integrity()
              << " chunks=" << r.num_chunks() << " rows=" << r.num_rows() << "\n";
    auto m0 = r.chunk_meta(0);
    std::cout << "chunk0: num_rows=" << m0.num_rows << "\n";
    auto rows = r.read_chunk(0);
    std::cout << "read_chunk(0).size()=" << rows.size() << "\n";
    return 0;
}
