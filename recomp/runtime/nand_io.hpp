#pragma once

#include <cstdint>
#include <istream>

namespace qilin::rt {

// EOF is a successful short read; a host I/O failure is NAND_RESULT_UNKNOWN.
// Clear a successful EOF so a later NANDClose does not mistake it for an
// I/O failure. Keep hard errors available to the caller.
inline int32_t nand_read(std::istream& stream, char* bytes, uint32_t length) {
    stream.clear();
    stream.read(bytes, length);
    if (stream.bad() || (stream.fail() && !stream.eof())) return -64;
    const auto count = static_cast<int32_t>(stream.gcount());
    stream.clear();
    return count;
}

}  // namespace qilin::rt
