# Tier 3, depending on Core and RHI. Docs/Architecture/Module-Graph.md shows this module's
# eventual dependencies as Engine and Render; neither exists yet, and the entry that adds
# them is Phase D's. The tier does not change either way -- see the open questions in
# Docs/Plans/2026-09-06-phase-a3-rhi-and-first-light.md.
monarc_module(
    NAME        Monarc.Host.Windowed
    KIND        Runtime
    TIER        3
    PUBLIC_DEPS Monarc.Core Monarc.RHI)

monarc_test_module(Monarc.Host.Windowed)
