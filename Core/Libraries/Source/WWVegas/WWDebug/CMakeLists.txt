set(WWDEBUG_SRC
    wwdebug.cpp
    wwdebug.h
    wwhack.h
    wwmemlog.cpp
    wwmemlog.h
    wwprofile.cpp
    wwprofile.h
)

add_library(core_wwdebug STATIC)
set_target_properties(core_wwdebug PROPERTIES OUTPUT_NAME wwdebug)

target_sources(core_wwdebug PRIVATE ${WWDEBUG_SRC})

target_link_libraries(core_wwdebug PRIVATE
    core_wwcommon
    corei_always
)

# GeneralsX @bugfix Android port 16/09/2026 wwmemlog.cpp/wwprofile.cpp need
# thread_compat.h's GetCurrentThreadIdAsInt() (see those files), and nothing
# else this Core-level target links already exposes CompatLib's include dir.
# thread_compat.h's _WIN32 branch is header-only now, so this is enough --
# no extra library to link. GeneralsMD's and Generals' copies are identical;
# either works for what this target needs.
if(WIN32)
    target_include_directories(core_wwdebug PRIVATE
        ${CMAKE_SOURCE_DIR}/GeneralsMD/Code/CompatLib/Include
    )
endif()
