# ---------------------------------------------------------------------------
# EconSpace — root build file.
# CMake reads it and generates the commands for the compiler.
# ---------------------------------------------------------------------------

# Minimum CMake version we require.
cmake_minimum_required(VERSION 3.16)

# Project name and language. After this line CMake knows we write in C++.
project(EconSpace LANGUAGES CXX)

# Use the C++17 standard and require exactly it (no silent fallback).
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Generate build/compile_commands.json — a project "map" for the editor
# (VS Code uses it to resolve #include and hints correctly; clang-tidy needs it too).
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# --- ccache: compilation cache (if installed) — noticeably faster rebuilds ------
find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
    set(CMAKE_CXX_COMPILER_LAUNCHER ${CCACHE_PROGRAM})
    message(STATUS "ccache found — compilation cache enabled")
endif()

# --- Pulling in raylib ------------------------------------------------------
# FetchContent — a CMake built-in mechanism: it downloads the raylib sources
# from GitHub and builds them as part of our project.
include(FetchContent)

# Don't build raylib's own examples — we only need the library.
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    raylib
    GIT_REPOSITORY https://github.com/raysan5/raylib.git
    GIT_TAG        5.5            # pin the version to keep the build stable
    GIT_SHALLOW    TRUE           # fetch only the slice of history we need — faster
)
FetchContent_MakeAvailable(raylib)

# --- Pulling in nlohmann/json ----------------------------------------------
# Header-only library for reading/writing JSON. We grab a ready-made release archive.
FetchContent_Declare(
    json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
)
FetchContent_MakeAvailable(json)

# Third-party library headers — marked SYSTEM: their warnings (-Wall/-Wextra) don't
# clutter our output, and we only see warnings from OUR own code.
get_target_property(_raylib_inc raylib INTERFACE_INCLUDE_DIRECTORIES)
if(_raylib_inc)
    set_target_properties(raylib PROPERTIES
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_raylib_inc}")
endif()

# --- doctest: header-only framework for unit tests (the `tests` target, below) --
# Its CMake declares an old policy minimum — newer CMake fails without this.
# The define only affects this sub-build (raylib/json are already configured above).
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
FetchContent_Declare(
    doctest
    URL https://github.com/doctest/doctest/archive/refs/tags/v2.4.11.tar.gz
)
FetchContent_MakeAvailable(doctest)

# --- Optional clang-tidy at build time (off by default — don't slow builds down) -
# Enable with: cmake -S . -B build -G "MinGW Makefiles" -DENABLE_CLANG_TIDY=ON
# Install in MSYS2: pacman -S mingw-w64-x86_64-clang-tools-extra
option(ENABLE_CLANG_TIDY "Run clang-tidy when building our targets" OFF)
if(ENABLE_CLANG_TIDY)
    find_program(CLANG_TIDY_PROGRAM clang-tidy)
    if(CLANG_TIDY_PROGRAM)
        set(CMAKE_CXX_CLANG_TIDY ${CLANG_TIDY_PROGRAM})
        message(STATUS "clang-tidy enabled for the build")
    else()
        message(WARNING "ENABLE_CLANG_TIDY=ON, but clang-tidy was not found in PATH")
    endif()
endif()
# Third-party targets (raylib/json/doctest) are declared ABOVE — tidy won't touch them.

# ---------------------------------------------------------------------------
# Layout: engine (shared core, static lib) ← game and editor (executables).
#   src/engine — world, entities, rendering, UI primitives (reused by both)
#   src/game   — game logic (Game, ships, player, economy, missions)
#   src/editor — world editor
# ---------------------------------------------------------------------------

# --- Engine (shared static library) ----------------------------------------
add_library(engine STATIC
    src/engine/core/WorldLoader.cpp
    src/engine/core/Faction.cpp
    src/engine/entities/Entity.cpp
    src/engine/entities/Star.cpp
    src/engine/entities/Planet.cpp
    src/engine/entities/Station.cpp
    src/engine/entities/AsteroidField.cpp
    src/engine/entities/Nebula.cpp
    src/engine/entities/Derelict.cpp
    src/engine/entities/JumpGate.cpp
    src/engine/economy/Resource.cpp
    src/engine/render/Textures.cpp
    src/engine/ui/UiTheme.cpp
    src/engine/ui/Button.cpp
    src/engine/ui/Window.cpp
    src/engine/ui/ContextMenu.cpp
)
# PUBLIC: the src/engine root is visible to everyone who links engine. That's why
# includes like "entities/Entity.h" from game and editor resolve automatically.
target_include_directories(engine PUBLIC ${CMAKE_SOURCE_DIR}/src/engine)
target_link_libraries(engine PUBLIC raylib nlohmann_json::nlohmann_json)
# Compiler warnings for OUR code (engine + everything that links it: game/
# editor/server/tests). Third-party raylib/json build as their own targets, untouched.
target_compile_options(engine PUBLIC -Wall -Wextra)

# -Werror is opt-in and CI turns it on. Local builds keep warnings visible but
# non-fatal, so a work-in-progress edit is not blocked by an unused parameter, while
# nothing warning-dirty can reach main. It rides on `engine` so it reaches everything
# that links it and nothing that doesn't — raylib and json are untouched.
option(WARNINGS_AS_ERRORS "Treat compiler warnings in our code as errors" OFF)
if(WARNINGS_AS_ERRORS)
    target_compile_options(engine PUBLIC -Werror)
endif()

# --- Shared wire layer -----------------------------------------------------
# Protocol + transport, compiled ONCE and linked by everything that speaks the wire:
# the client, the server and the tests. These two files used to appear in three
# copy-pasted source lists, and econagent (#42) would have made it four.
add_library(netproto STATIC
    src/game/sim/Protocol.cpp
    src/game/sim/Events.cpp
    src/game/net/Tcp.cpp
)
# PUBLIC: consumers include "sim/Protocol.h" and "net/Tcp.h" relative to src/game.
target_include_directories(netproto PUBLIC ${CMAKE_SOURCE_DIR}/src/game)
# engine, because Protocol.h uses core/Faction.h and raylib's Vector2/Color.
target_link_libraries(netproto PUBLIC engine)
# winsock, guarded here so the guard lives in one place when other platforms arrive (#12).
if(WIN32)
    target_link_libraries(netproto PUBLIC ws2_32)
endif()

# --- Game ------------------------------------------------------------------
# The client does NOT link Simulation: the world lives on econserver. It shares only
# the wire protocol and PlayerStep, which is the movement step it must run identically
# to the server in order to predict its own ship.
add_executable(econspace
    src/game/main.cpp
    src/game/core/Game.cpp
    src/game/sim/PlayerStep.cpp
    src/game/entities/Ship.cpp       # the predicted player ship + NPC proxies
    src/game/entities/ShipType.cpp   # hangar catalog
    src/game/entities/NpcShip.cpp
    src/game/player/Player.cpp       # account mirror from the snapshot
    src/game/player/Skills.cpp
    src/game/missions/MissionSystem.cpp  # mission mirror from the snapshot
)
target_link_libraries(econspace PRIVATE engine netproto)  # netproto brings src/game + winsock
set_target_properties(econspace PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin/game)
# Dev convenience: the game reads the world from the SOURCE data/ (like the editor),
# so edits made in the editor show up immediately without a rebuild. For a "release"
# build the define can be dropped — then it reads the copy next to the exe.
target_compile_definitions(econspace PRIVATE GAME_DATA_DIR="${CMAKE_SOURCE_DIR}/data/")

# --- Headless galaxy server (track M, M3) ----------------------------------
# The same Simulation as in the game, but WITHOUT rendering/window/input: an
# authoritative server running real agents in every system. No client (Game) over it yet.
add_executable(econserver
    src/game/sim/main_headless.cpp
    src/game/sim/Simulation.cpp
    src/game/sim/PlayerStep.cpp
    src/game/sim/Orders.cpp
    src/game/economy/Market.cpp
    src/game/entities/NpcShip.cpp
    src/game/entities/Ship.cpp       # Simulation owns the player's ship (M4d-2b)
    src/game/entities/ShipType.cpp   # ShipStats catalog (for CreatePlayer)
    src/game/player/Player.cpp       # player account in Simulation (M4f)
    src/game/player/Skills.cpp       # account skills (M4f)
    src/game/missions/MissionSystem.cpp  # player missions in Simulation (M4f-2)
)
target_link_libraries(econserver PRIVATE engine netproto)  # netproto brings src/game + winsock
set_target_properties(econserver PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin/server)
target_compile_definitions(econserver PRIVATE SIM_DATA_DIR="${CMAKE_SOURCE_DIR}/data/")

# --- World editor ----------------------------------------------------------
add_executable(worldeditor
    src/editor/main.cpp
    src/editor/Editor.cpp
)
target_include_directories(worldeditor PRIVATE ${CMAKE_SOURCE_DIR}/src/editor)
target_link_libraries(worldeditor PRIVATE engine)
set_target_properties(worldeditor PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin/editor)
# The editor edits the repository's SOURCE data (not the copy next to the exe), so
# we bake the absolute path to data/ in at configure time.
target_compile_definitions(worldeditor PRIVATE EDITOR_DATA_DIR="${CMAKE_SOURCE_DIR}/data/")

# --- Data -------------------------------------------------------------------
# Copy data/ next to EACH executable (they now live in different folders).
# The target is in ALL — it runs on every build (including data/-only edits).
add_custom_target(copy_data ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_SOURCE_DIR}/data $<TARGET_FILE_DIR:econspace>/data
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_SOURCE_DIR}/data $<TARGET_FILE_DIR:worldeditor>/data
    COMMENT "Copying data/ next to game and editor executables")
add_dependencies(copy_data econspace worldeditor)

# --- Tests (doctest + ctest) -----------------------------------------------
# Regression unit tests for the wire protocol and TCP transport. Run with:
#   cmake --build build --target tests   (build)
#   ctest --test-dir build --output-on-failure   (run)
enable_testing()
add_executable(tests
    tests/test_main.cpp
    tests/protocol_tests.cpp
    tests/net_tests.cpp
    tests/observation_tests.cpp
    src/game/sim/Observation.cpp   # shared with econagent (#42); tests are its only build today
)
target_link_libraries(tests PRIVATE engine netproto doctest::doctest)
set_target_properties(tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin/tests)
add_test(NAME unit COMMAND tests)

# --- cppcheck: static analysis as a separate target (if installed) ----------
# Install in MSYS2: pacman -S mingw-w64-x86_64-cppcheck
# Run with: cmake --build build --target cppcheck
find_program(CPPCHECK_PROGRAM cppcheck)
if(CPPCHECK_PROGRAM)
    add_custom_target(cppcheck
        COMMAND ${CPPCHECK_PROGRAM} --enable=warning,performance,portability
                --std=c++17 --quiet --inline-suppr --error-exitcode=0
                -I ${CMAKE_SOURCE_DIR}/src/engine
                -I ${CMAKE_SOURCE_DIR}/src/game
                ${CMAKE_SOURCE_DIR}/src
        COMMENT "cppcheck: static analysis of src/")
endif()
