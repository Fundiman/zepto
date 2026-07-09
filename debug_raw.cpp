#include <cstdio>
#include <cstdint>
#include <iostream>
#include <fstream>
#include <vector>
int main() {
    std::ifstream f("debug.zepto", std::ios::binary);
    if (!f) { std::cerr << "no file\n"; return 1; }
    f.seekg(0, std::ios::end);
    auto sz = f.tellg();
    f.seekg(0);
    std::vector<uint8_t> buf((size_t)sz);
    f.read((char*)buf.data(), sz);

    // Dump header
    auto r4 = [&](size_t p) -> uint32_t { uint32_t v; memcpy(&v, buf.data()+p, 4); return v; };
    auto r2 = [&](size_t p) -> uint16_t { uint16_t v; memcpy(&v, buf.data()+p, 2); return v; };
    auto r8 = [&](size_t p) -> uint64_t { uint64_t v; memcpy(&v, buf.data()+p, 8); return v; };

    std::cout << "file_size=" << sz << "\n";
    std::cout << "magic=" << r4(0) << " (expect " << 0x5450455A << ")\n";
    std::cout << "version=" << r2(4) << "\n";
    std::cout << "num_cols_hdr=" << r2(6) << "\n";
    std::cout << "total_rows=" << r8(8) << "\n";
    std::cout << "num_chunks=" << r4(16) << "\n";
    std::cout << "meta_size=" << r4(20) << "\n";

    size_t pos = 24 + r4(20); // after header + metadata
    std::cout << "first_chunk_at=" << pos << "\n";

    // Dump chunk header
    if (pos + 8 > (size_t)sz) { std::cerr << "no chunk\n"; return 1; }
    std::cout << "chunk_magic=" << r4(pos) << "\n";
    std::cout << "chunk_orig_size=" << r4(pos+4) << "\n";

    uint32_t orig_size = r4(pos+4);
    size_t nb = (orig_size + 223 - 1) / 223;
    size_t interleaved_len = nb * 255;
    std::cout << "nb=" << nb << " interleaved_len=" << interleaved_len << "\n";
    std::cout << "expected_end=" << (pos + 8 + interleaved_len) << " file_size=" << sz << "\n";

    // Print first 32 bytes of interleaved blob
    std::cout << "interleaved[0..31]: ";
    for (int i = 0; i < 32 && (size_t)(pos+8+i) < buf.size(); i++)
        printf("%02x ", buf[pos+8+i]);
    std::cout << "\n";
    return 0;
}
