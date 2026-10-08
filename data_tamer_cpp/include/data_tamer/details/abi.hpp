#pragma once

/// Hidden visibility for the inline `stats()` functions: each binary must use the copy
/// compiled against its own header, because another loaded library's copy may be built
/// for an older `Stats` that has since grown. See CLAUDE.md, "Invariants".
#if defined(__GNUC__) || defined(__clang__)
#define DATA_TAMER_INLINE_LOCAL __attribute__((visibility("hidden")))
#else
#define DATA_TAMER_INLINE_LOCAL
#endif
