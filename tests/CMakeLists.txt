# SPDX-License-Identifier: AGPL-3.0-or-later
# mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.

include(Catch)

add_executable(mwstime_tests
    unit/SmokeTests.cpp
    unit/test_autocorr.cpp
    unit/test_buffer.cpp
    unit/test_butterworth.cpp
    unit/test_modelspec.cpp
    unit/test_movinglpf.cpp
    unit/test_quantizer.cpp
    unit/test_wavio.cpp
    unit/test_resampler.cpp
)

target_link_libraries(mwstime_tests
    PRIVATE
        mwstime_core
        Catch2::Catch2WithMain
)

catch_discover_tests(mwstime_tests)

# License-header convention check (docs/CONTRIBUTING.md). Every committed C++
# source under libs/ plugin/ tools/ tests/ must carry the SPDX header; every
# later task inherits this gate automatically.
add_test(
    NAME license_headers
    COMMAND "${CMAKE_SOURCE_DIR}/tools/check_license_headers.sh" "${CMAKE_SOURCE_DIR}"
)
