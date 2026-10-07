#pragma once

/// Marks an inline function whose body is built in the caller from exported
/// getters (the `stats()` functions). Each binary then keeps its own copy,
/// compiled against its own header: without it, the weak default-visibility
/// copy that the first loaded shared object defines would serve every other
/// binary in the process, and a `Stats` struct that grew in a later 2.x header
/// would be built by the old code into the new caller's buffer.
#if defined(__GNUC__) || defined(__clang__)
#define DATA_TAMER_INLINE_LOCAL __attribute__((visibility("hidden")))
#else
#define DATA_TAMER_INLINE_LOCAL
#endif
