file(GLOB_RECURSE PRIVATE_SOURCES CONFIGURE_DEPENDS "*.cpp")

add_library(WorldClient STATIC
    ${PRIVATE_SOURCES}
)

target_include_directories(WorldClient
    PUBLIC
        ${CMAKE_SOURCE_DIR}
)

# ICC Navigator uses its bundled legacy Detour tile format.
target_include_directories(WorldClient PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/IccNavigator/vendor/Detour/Include")
