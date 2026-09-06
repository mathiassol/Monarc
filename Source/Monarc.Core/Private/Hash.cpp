#include <Monarc/Core/Hash.h>

namespace Monarc {

u64 HashBytes(const void* data, usize size) {
    // 64-bit FNV-1a: an offset basis, then XOR-then-multiply per byte. Simple and fast;
    // nothing here claims cryptographic strength.
    constexpr u64 kOffsetBasis = 0xcbf29ce484222325ULL;
    constexpr u64 kPrime       = 0x100000001b3ULL;

    const auto* bytes = static_cast<const unsigned char*>(data);
    u64         hash  = kOffsetBasis;
    for (usize i = 0; i < size; ++i) {
        hash ^= static_cast<u64>(bytes[i]);
        hash *= kPrime;
    }
    return hash;
}

}  // namespace Monarc
