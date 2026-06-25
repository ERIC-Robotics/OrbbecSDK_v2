include(FetchContent)

# ── MCAP ──────────────────────────────────────────────────────────────────
FetchContent_Declare(
    mcap_builder
    GIT_REPOSITORY https://github.com/marc-medley/mcap_builder.git
    GIT_TAG        origin/platform_checks
)
FetchContent_MakeAvailable(mcap_builder)
