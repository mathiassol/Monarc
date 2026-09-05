#pragma once

#include <Monarc/Core/Memory/Allocator.h>

#include <atomic>

namespace Monarc {

/// General-purpose allocator backed by the platform's aligned allocation.
///
/// This is the fallback, not the default. Systems with a clear lifetime should use an
/// arena or pool instead. It sits behind IAllocator so replacing it later with a custom
/// general allocator is an internal change.
class SystemAllocator final : public IAllocator {
public:
    SystemAllocator() = default;

    [[nodiscard]] void* Allocate(usize size, usize alignment) override;
    void                Deallocate(void* pointer, usize size, usize alignment) override;
    [[nodiscard]] usize BytesAllocated() const override;
    [[nodiscard]] const char* Name() const override { return "System"; }

    /// Process-wide instance, for the small number of places that genuinely have no
    /// allocator to hand — chiefly start-up.
    static SystemAllocator& Get();

private:
    std::atomic<usize> m_bytesAllocated{0};
};

}  // namespace Monarc
