# as_admin_time — Plugify port of the Pisex admin_system module "[AS] Admin Time" (records how
# long each admin is online in the shared admin database, table `as_admin_time`).
#
#   plugify-as_admin_time  the plugin shared library (entry "bin/as_admin_time")
#
# Pure s2sdk + plgdb: no pisexcompat and no s2guard (docs/PLUGIN_STANDARD.md §7). The guards of
# handoff/weaponskins/PITFALLS.md live in src/plugin.cpp itself — the B1/B2 map gate, the C2 client
# checks, the C11 command rules and the C12 event hook — and admin_time.ini is read by the
# self-contained KV1 reader in src/config.hpp. Like every admin_system module the .so compiles the
# binding globals the plugify module fills at load time:
#   * s2sdk (__s2sdk_*),
#   * the generated admin_system consumer bindings (__admin_system_*) plus the hand-written
#     consumer wrapper include/admin_api.hpp.
# The module's database work runs on a private plgdb::Database, opened with the settings the admin
# core exports (libs/plgdb, static).

set(AS_ADMIN_TIME_VERSION "1.1.0")

# The generated admin_system binding globals (__admin_system_*) have to be compiled into every
# consumer plugin; the admin_system plugin directory already builds them as an object library (it
# is added before plugins/as_admin_time), this is the fallback for a build of this directory alone.
if(NOT TARGET admin_system_bindings_obj)
    add_library(admin_system_bindings_obj OBJECT
            "${CMAKE_SOURCE_DIR}/plugins/admin_system/include/external/plugify/include/admin_system/admin_system.cpp")
    target_include_directories(admin_system_bindings_obj SYSTEM PUBLIC
            "${CMAKE_SOURCE_DIR}/plugins/admin_system/include/external/plugify/include"
            "${CMAKE_SOURCE_DIR}/sdk/include")
    target_compile_definitions(admin_system_bindings_obj PUBLIC _GLIBCXX_USE_CXX11_ABI=1)
endif()

add_library(plugify-as_admin_time SHARED
        src/plugin.cpp
        $<TARGET_OBJECTS:s2sdk_bindings>
        $<TARGET_OBJECTS:admin_system_bindings_obj>
)
target_include_directories(plugify-as_admin_time PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
# SYSTEM: warnings from the generated plg/s2sdk/admin_system headers, admin_api.hpp and plgdb must
# not fail -Werror on our sources.
target_include_directories(plugify-as_admin_time SYSTEM PRIVATE
        "${CMAKE_SOURCE_DIR}/plugins/admin_system/include"
        "${CMAKE_SOURCE_DIR}/plugins/admin_system/include/external/plugify/include"
        "${CMAKE_SOURCE_DIR}/sdk/include"
        "${CMAKE_SOURCE_DIR}/libs/plgdb/include"
)
target_link_libraries(plugify-as_admin_time PRIVATE
        plgdb
        -static-libstdc++ -static-libgcc
)
target_compile_options(plugify-as_admin_time PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(plugify-as_admin_time PRIVATE
        _GLIBCXX_USE_CXX11_ABI=1
        PLUGIFY_MODULE="as_admin_time"
        PLUGIFY_IS_DEBUG=0
        PLUGIFY_IS_RELEASE=1
)
set_target_properties(plugify-as_admin_time PROPERTIES OUTPUT_NAME as_admin_time)

if(UNIX AND NOT APPLE)
    target_link_options(plugify-as_admin_time PRIVATE
            "-Wl,--version-script,${CMAKE_CURRENT_SOURCE_DIR}/sym/version_script.lds")
endif()

configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/as_admin_time.pplugin.in"
        "${CMAKE_CURRENT_BINARY_DIR}/as_admin_time.pplugin"
)

# --- tests --------------------------------------------------------------------
# as_admin_time_tests: the plugin TU is compiled into the test (its handlers live in an anonymous
# namespace) and every __s2sdk_* / __admin_system_* binding slot is a recording fake. The database
# half runs against a real throwaway SQLite file through plgdb, so the statements, the schema and
# the crash-recovery reset are exercised as they run on the server. Nothing here links pisexcompat.
#
# The top-level CMakeLists declares the PLUGIFY_PORT_TESTS option *after* the plugin directories,
# so on a first configure it is not defined here yet: default it to the same ON. An explicit
# -DPLUGIFY_PORT_TESTS=OFF (the exported standalone build) is already in the cache and still wins.
if(NOT DEFINED PLUGIFY_PORT_TESTS)
    set(PLUGIFY_PORT_TESTS ON)
endif()
if(PLUGIFY_PORT_TESTS)
    add_executable(as_admin_time_tests
            tests/as_admin_time_tests.cpp
            $<TARGET_OBJECTS:s2sdk_bindings>
            $<TARGET_OBJECTS:admin_system_bindings_obj>
    )
    target_include_directories(as_admin_time_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_include_directories(as_admin_time_tests SYSTEM PRIVATE
            "${CMAKE_SOURCE_DIR}/plugins/admin_system/include"
            "${CMAKE_SOURCE_DIR}/plugins/admin_system/include/external/plugify/include"
            "${CMAKE_SOURCE_DIR}/sdk/include"
            "${CMAKE_SOURCE_DIR}/libs/plgdb/include"
    )
    target_link_libraries(as_admin_time_tests PRIVATE plgdb -static-libstdc++ -static-libgcc)
    target_compile_options(as_admin_time_tests PRIVATE -Wall -Wextra -Werror)
    target_compile_definitions(as_admin_time_tests PRIVATE
            _GLIBCXX_USE_CXX11_ABI=1
            PLUGIFY_MODULE="as_admin_time"
            PLUGIFY_IS_DEBUG=0
            PLUGIFY_IS_RELEASE=1
            # Lets the tests parse the configs the module ships and the production ones, so a
            # broken file fails CI.
            AS_ADMIN_TIME_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data"
    )
    set_target_properties(as_admin_time_tests PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
endif()
