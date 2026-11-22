set(runtime_link "${EXECUTABLE_DIR}/libSDL3.dylib")
if(IS_SYMLINK "${runtime_link}" AND EXISTS "${runtime_link}")
    file(READ_SYMLINK "${runtime_link}" link_target)
    # Preserve the relative symlink sealed by codesign during packaging.
    if(link_target STREQUAL SDL3_RUNTIME OR link_target STREQUAL "../Frameworks/libSDL3.dylib")
        return()
    endif()
endif()

if(NOT EXISTS "${SDL3_RUNTIME}")
    message(FATAL_ERROR "SDL3 runtime is missing: ${SDL3_RUNTIME}")
endif()

file(MAKE_DIRECTORY "${EXECUTABLE_DIR}")
file(CREATE_LINK "${SDL3_RUNTIME}" "${runtime_link}" SYMBOLIC)
