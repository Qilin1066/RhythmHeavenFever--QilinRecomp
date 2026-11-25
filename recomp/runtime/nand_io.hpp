#pragma once

#include <cstdint>
#include <istream>

namespace qilin::rt {

// EOF is a successful short read; a host I/O failure is NAND_RESULT_UNKNOWN.
// Leave this read's state intact so callers can account for bytes changed
// even when a partial read failed.
inline int32_t nand_read(std::istream& stream, char* bytes, uint32_t length) {
    stream.clear();
    stream.read(bytes, length);
    if (stream.bad() || (stream.fail() && !stream.eof())) return -64;
    return static_cast<int32_t>(stream.gcount());
}

}  // namespace qilin::rt
