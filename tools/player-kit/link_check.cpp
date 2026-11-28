// Stands in for the code `qilin lift --runtime` makes from main.dol (empty
// tables: no functions, no replaced functions, no names), so the player kit
// can be linked without it: CI builds this to show that libbeatthebeat_kit.a,
// libbeatthebeat_libs.a and their system libraries are complete. Started
// with no folder, the program stops at once with its usage (exit 2).
#include "native.hpp"

namespace qilin::rt {
extern const FnEntry g_functions[] = {{0, nullptr}};
extern const size_t g_function_count = 0;
extern const NativeEntry g_natives[] = {{0, nullptr, nullptr}};
extern const size_t g_native_count = 0;
extern const Symbol g_symbols[] = {{0, nullptr}};
extern const size_t g_symbol_count = 0;
}  // namespace qilin::rt
