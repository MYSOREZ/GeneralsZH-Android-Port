# Interface libraries to set common defines and includes, avoid duplication later
add_library(core_wwcommon INTERFACE)

# GeneralsX @bugfix Android port 16/09/2026 NOMINMAX was written here and then
# commented out. Without it, <windows.h>'s min/max macros collide with
# std::chrono's own min()/max() static members the moment any translation
# unit includes both -- <bits/chrono.h> stops parsing entirely ("macro 'max'
# requires 2 arguments, but only 1 given"). This is the standard, universal
# fix for mixing windows.h with the C++ standard library.
target_compile_definitions(core_wwcommon INTERFACE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
)

# GeneralsX @build BenderAI 13/02/2026 d3d8lib is INTERFACE (headers only) - safe for all
target_link_libraries(core_wwcommon INTERFACE
    d3d8lib     # INTERFACE target: provides DXVK/DirectX types (DWORD, BOOL, etc.)
    stlport
)

# GeneralsX @build BenderAI 13/02/2026 Platform-specific multimedia stubs
if(WIN32)
    target_link_libraries(core_wwcommon INTERFACE
        milesstub
    )
endif()

target_include_directories(core_wwcommon INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}
    WW3D2
    WWAudio
    WWDebug
    # WWDownload # is not added because headers are included as "WWDownload/file.h"
    WWLib
    WWMath
    WWSaveLoad
)

add_subdirectory(WW3D2)
add_subdirectory(WWAudio)
add_subdirectory(WWDebug)
add_subdirectory(WWDownload)
add_subdirectory(WWLib)
add_subdirectory(WWMath)
add_subdirectory(WWSaveLoad)
add_subdirectory(WWStub)

# Helpful interface to bundle the ww modules together.
add_library(core_wwvegas INTERFACE)

target_include_directories(core_wwvegas INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}
    WW3D2
    WWAudio
    WWDebug
    # WWDownload # is not added because headers are included as "WWDownload/file.h"
    WWLib
    WWMath
    WWSaveLoad
)

target_link_libraries(core_wwvegas INTERFACE
    # core_ww3d2
    # core_wwaudio
    core_wwdebug
    # core_wwdownload
    core_wwlib
    core_wwmath
    core_wwsaveload
)
