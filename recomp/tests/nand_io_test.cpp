#include "nand_io.hpp"

#include <cstdio>
#include <sstream>
#include <stdexcept>
#include <streambuf>

namespace {
int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++failures; }
}
struct ReadFailure : std::streambuf {
    std::streamsize xsgetn(char*, std::streamsize) override { throw std::runtime_error("synthetic disk read failure"); }
};
}  // namespace

int main() {
    char bytes[8]{};
    std::istringstream file("save");
    check(qilin::rt::nand_read(file, bytes, 2) == 2 && std::string(bytes, 2) == "sa", "full read");
    check(qilin::rt::nand_read(file, bytes, 8) == 2 && std::string(bytes, 2) == "ve", "EOF is a successful short read");
    check(file.good(), "successful short read leaves the stream ready to close");
    check(qilin::rt::nand_read(file, bytes, 8) == 0, "EOF returns zero");
    check(file.good(), "zero-byte EOF leaves the stream ready to close");
    file.clear(); file.seekg(0);
    check(qilin::rt::nand_read(file, bytes, 0) == 0, "zero length read");
    ReadFailure buffer;
    std::istream broken(&buffer);
    check(qilin::rt::nand_read(broken, bytes, 8) == -64, "host error returns NAND_RESULT_UNKNOWN");
    check(broken.bad(), "hard error is available to the caller");
    return failures ? 1 : 0;
}
