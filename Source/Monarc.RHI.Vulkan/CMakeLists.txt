# Monarc.RHI is declared a dependency now, per the Phase A3 plan's graph, but nothing in
# this module uses it yet: VulkanBackend.h names only Status, and the stub in
# Private/VulkanBackend.cpp calls nothing. It becomes load-bearing in Task 2, when IBackend,
# AdapterInfo and the RHI-to-Vulkan enum translation exist for this module to implement --
# and PUBLIC is correct then, because the public header will name RHI types. It is declared
# early rather than added later so that the graph, and module-graph.json with it, matches the
# plan from the first commit. Note that gate 3 polices this module because it says TIER 2,
# not because of this edge: the gate keys off tier and never reads dependencies.
monarc_module(
    NAME        Monarc.RHI.Vulkan
    KIND        Runtime
    TIER        2
    PUBLIC_DEPS Monarc.Core Monarc.RHI)

monarc_test_module(Monarc.RHI.Vulkan)
