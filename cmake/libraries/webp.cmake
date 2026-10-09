if(NOT BUILD_RENDERER_GL1 AND NOT BUILD_RENDERER_GL2)
    return()
endif()

include(utils/disable_warnings)

# Decoder-only subset of libwebp, used for .webp textures
set(INTERNAL_WEBP_DIR ${SOURCE_DIR}/thirdparty/libwebp-1.6.0)

file(GLOB WEBP_SOURCES
    ${INTERNAL_WEBP_DIR}/src/dec/*.c
    ${INTERNAL_WEBP_DIR}/src/dsp/*.c
    ${INTERNAL_WEBP_DIR}/src/utils/*.c)
disable_warnings(${WEBP_SOURCES})

if(EMSCRIPTEN)
    # libwebp's SSE2 paths map onto wasm SIMD
    set_property(SOURCE ${WEBP_SOURCES} APPEND PROPERTY COMPILE_OPTIONS -msse2)
endif()

list(APPEND RENDERER_LIBRARY_SOURCES ${WEBP_SOURCES})
list(APPEND RENDERER_INCLUDE_DIRS ${INTERNAL_WEBP_DIR})
list(APPEND RENDERER_DEFINITIONS USE_WEBP)
