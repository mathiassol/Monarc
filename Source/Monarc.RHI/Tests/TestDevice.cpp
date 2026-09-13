#include <doctest/doctest.h>

#include <Monarc/RHI/Device.h>

#include <iterator>
#include <string_view>

// The two attachment-op spellings Device.h declares.
//
// **Device-free, and there is nothing here that could need a device.** `LoadOp` and `StoreOp`
// are plain enums and `ToString` is a lookup over them; the interfaces this header's name comes
// from -- `ICommandList`, `IQueue`, `IDevice` -- are pure virtual and have no behaviour of their
// own to test without an implementation. That is why this file appeared only when the header
// grew its first free function, rather than sitting empty beside the others since A3.

using Monarc::RHI::LoadOp;
using Monarc::RHI::StoreOp;
using Monarc::RHI::ToString;

namespace {

/// Every enumerator of each enum, once. The switches in Private/Device.cpp carry no `default`,
/// so the compiler already refuses a new enumerator that nobody gave a case to. These lists
/// cover the half the compiler cannot: that the case it was given returns a real name rather
/// than one belonging to a neighbouring row.
/// @{
constexpr LoadOp kAllLoadOps[] = {
    LoadOp::Load,
    LoadOp::Clear,
    LoadOp::DontCare,
};

constexpr StoreOp kAllStoreOps[] = {
    StoreOp::Store,
    StoreOp::DontCare,
};
/// @}

/// A value each enum can hold but that no enumerator names. Well-defined: both have a fixed
/// underlying type, so every `u32` is a valid value of them.
/// @{
constexpr LoadOp  kNotALoadOp  = static_cast<LoadOp>(4242);
constexpr StoreOp kNotAStoreOp = static_cast<StoreOp>(4242);
/// @}

std::string_view Name(LoadOp op) { return std::string_view(ToString(op)); }
std::string_view Name(StoreOp op) { return std::string_view(ToString(op)); }

}  // namespace

TEST_CASE("an attachment op's name is the enumerator's own spelling") {
    CHECK(Name(LoadOp::Load) == "Load");
    CHECK(Name(LoadOp::Clear) == "Clear");
    CHECK(Name(LoadOp::DontCare) == "DontCare");

    CHECK(Name(StoreOp::Store) == "Store");
    CHECK(Name(StoreOp::DontCare) == "DontCare");
}

TEST_CASE("the attachment-op lists name every enumerator") {
    // TestTypes.cpp's mechanism, and its argument applies unchanged: C++ cannot ask an enum how
    // many enumerators it has, so the check asks whether the index one past the end of each list
    // names one. `ToString` answers with its own not-an-enumerator marker for any value no
    // enumerator names, and comparing against `ToString` of a value that is definitely not one
    // is how that marker is identified without this file hardcoding its spelling.
    //
    // What it rests on: each enum's enumerators are one contiguous run from zero, which Device.h
    // keeps by giving no explicit value after the first. An enumerator added with a value
    // outside that run would still slip past; the guard covers the append, which is the only way
    // either list has ever grown.
    CHECK(Name(static_cast<LoadOp>(std::size(kAllLoadOps))) == Name(kNotALoadOp));
    CHECK(Name(static_cast<StoreOp>(std::size(kAllStoreOps))) == Name(kNotAStoreOp));
}

TEST_CASE("every attachment op is named exactly once within its own enum") {
    // **Within its own enum, because `LoadOp::DontCare` and `StoreOp::DontCare` deliberately
    // share a spelling.** They are the same word about two different moments, and a report
    // naming the field -- `load=DontCare store=DontCare` -- is what keeps them apart. What would
    // be a real defect is two enumerators of one enum sharing a name, which is what a copy-paste
    // in the switch produces and what these loops catch.
    for (const LoadOp first : kAllLoadOps) {
        for (const LoadOp second : kAllLoadOps) {
            CHECK((Name(first) == Name(second)) == (first == second));
        }
    }
    for (const StoreOp first : kAllStoreOps) {
        for (const StoreOp second : kAllStoreOps) {
            CHECK((Name(first) == Name(second)) == (first == second));
        }
    }
}
