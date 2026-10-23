# ---------------------------------------------------------------------------
# EconSpace — root build file.
# CMake reads it and generates the commands for the compiler.
# ---------------------------------------------------------------------------

# Minimum CMake version we require.
cmake_minimum_required(VERSION 3.16)

# The oldest macOS a build runs on (#478). Read only on Apple, and only before project().
# 11 is the first release on Apple silicon, so an arm64 slice cannot go lower anyway, and it
# keeps every Intel Mac still updated. A release is built universal on top of this:
# -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" (.github/workflows/release.yml).
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")

# Project name and language. After this line CMake knows we write in C++.
# C as well as C++: Clay, the layout engine inside windows, is C99 and is compiled as C (#297).
project(EconSpace VERSION 0.1.0 LANGUAGES C CXX)

# --- The version: one source of truth (#400) ---------------------------------------------
# The number above is the release's semver: tags are v<it>, and a client and a server refuse
# each other unless it matches. The name players see adds a codename, one per minor release
# from an alphabetical list of nautical words tied to the world (#186): 0.1 is Anchor, 0.2
# Beacon, and so on; a patch release keeps its minor's name. Everything else -- the binaries,
# the title bar, --version, the package names -- reads these, through core/Version.h.
set(ECON_CODENAMES Anchor Beacon Current Drift Ebb Fathom Gale Harbor)
list(LENGTH ECON_CODENAMES _econ_names)
if(NOT PROJECT_VERSION_MAJOR EQUAL 0 OR PROJECT_VERSION_MINOR LESS 1
   OR PROJECT_VERSION_MINOR GREATER _econ_names)
    message(FATAL_ERROR "No codename for ${PROJECT_VERSION}: extend ECON_CODENAMES")
endif()
math(EXPR _econ_name_index "${PROJECT_VERSION_MINOR} - 1")
list(GET ECON_CODENAMES ${_econ_name_index} ECON_CODENAME)
string(TOLOWER "${ECON_CODENAME}" ECON_CODENAME_LOWER)
# "0.1" for a minor release, "0.1.2" for a patch: the release is called EconSpace 0.1 Anchor.
if(PROJECT_VERSION_PATCH EQUAL 0)
    set(ECON_VERSION_SHORT "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}")
else()
    set(ECON_VERSION_SHORT "${PROJECT_VERSION}")
endif()
# Where a player is sent for the build a server runs.
set(ECON_RELEASES_URL "https://github.com/HEL3AN/econspace-mmo/releases" CACHE STRING
    "Where releases are downloaded, named in a version refusal")
configure_file(src/engine/core/Version.h.in ${CMAKE_BINARY_DIR}/generated/core/Version.h @ONLY)

# Use the C++17 standard and require exactly it (no silent fallback).
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# A Windows build runs from anywhere: MinGW's own runtime (libgcc, libstdc++, winpthread) is
# linked in rather than looked for next to the executable. Without this an .exe started from
# Explorer, from PowerShell or from any shell without MSYS2 on its PATH stops at a
# "libgcc_s_seh-1.dll was not found" dialog -- which a player double-clicking the game
# would meet first, and which blocks an unattended run until someone clicks OK.
if(MINGW)
    add_link_options(-static-libgcc -static-libstdc++ -static)
endif()

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

# --- PicoSHA2: header-only SHA-256 -------------------------------------------
# Needed so an account can prove it is itself (#106). Vendored through FetchContent like
# everything else and pinned to a tag: a hash function that changes under you would lock
# every player out at once. Header-only and MIT.
FetchContent_Declare(
    picosha2
    GIT_REPOSITORY https://github.com/okdshin/PicoSHA2.git
    GIT_TAG        v1.0.1
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(picosha2)

# --- doctest: header-only framework for unit tests (the `tests` target, below) --
# Its CMake declares an old policy minimum — newer CMake fails without this.
# The define only affects this sub-build (raylib/json are already configured above).
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
FetchContent_Declare(
    doctest
    URL https://github.com/doctest/doctest/archive/refs/tags/v2.4.11.tar.gz
)
FetchContent_MakeAvailable(doctest)

# --- Clay: flexbox-style layout for the inside of a window (#297) ---------------------
# One C99 header, zlib licence. Its header refuses C++ before C++20, so it is compiled in
# one C file (src/engine/ui/ClayBridge.c) and the client reaches it only through Ui::Layout.
# Pinned by tag and hash; SOURCE_SUBDIR names a directory that does not exist, so its own
# CMakeLists -- which builds the examples, and fetches a raylib of its own -- is never read.
FetchContent_Declare(
    clay
    URL      https://github.com/nicbarker/clay/archive/refs/tags/v0.14.tar.gz
    URL_HASH SHA256=ee8f6477020dd72afe8cf6f8d3ab6855980028289a2b677a1423d17aab983585
    SOURCE_SUBDIR header-only
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(clay)

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
    src/engine/core/Route.cpp        # the one route planner: map preview and route order (#432)
    src/engine/core/DataDir.cpp     # where data/ is, found at runtime (#399)
    src/engine/core/AdminChannel.cpp  # commands to a running server through its world folder (#400)
    src/engine/core/Faction.cpp
    src/engine/core/Archetype.cpp    # what a kind of object is, as data (#34)
    src/engine/core/ArchetypeEdit.cpp  # writing a tuned look back without reformatting (#118)
    src/engine/core/Orbits.cpp       # planets and their satellites, as a function of time (#210)
    src/engine/core/Blueprint.cpp    # how a player makes something: cost, time, placement (#39)
    src/engine/core/ShipDesign.cpp   # a ship as a design, its stats read off its parts (#279)
    src/engine/gen/Galaxy.cpp        # the skeleton of region slots, from a seed (#342)
    src/engine/core/Fitting.cpp       # what a refit may change, and what it costs (#279)
    src/engine/gen/Region.cpp        # the region beyond the wormhole, from a seed (#140)
    src/engine/gen/Names.cpp         # names in a people's own tongue, by culture (#431)
    src/engine/gen/Pins.cpp          # hand-written exceptions applied after generation (#147)
    src/engine/gen/Pack.cpp          # admin packs woven into a new world (#342 S7)
    src/engine/gen/PackExport.cpp    # a region written as a pack, or one begun from nothing (#342 S8)
    src/engine/gen/Survey.cpp        # many generated systems judged at once (#141)
    src/engine/entities/Entity.cpp
    src/engine/entities/Star.cpp
    src/engine/entities/Planet.cpp
    src/engine/entities/Station.cpp
    src/engine/entities/AsteroidField.cpp
    src/engine/entities/Nebula.cpp
    src/engine/entities/Derelict.cpp
    src/engine/entities/JumpGate.cpp
    src/engine/entities/Structure.cpp  # anything a player built (#39)
    src/engine/entities/Defence.cpp    # its owner's rules of engagement (#340)
    src/engine/economy/Resource.cpp
    src/engine/economy/Inventory.cpp   # goods by type, as any container holds them (#472)
    src/engine/render/Textures.cpp
    src/engine/render/Scene.cpp        # what an object looks like, apart from who draws it (#35)
    src/engine/render/Modules.cpp      # the module library: things placed by name, in variants (#240)
    src/engine/render/Kit.cpp          # sections, sockets, kits: where modules go (#240 phase 3)
    src/engine/render/Lighting.cpp     # where a system's light comes from, and how much (#119)
    src/engine/render/Silhouette.cpp       # what an object is made of, as data (#122)
    src/engine/render/CameraRig.cpp        # the player's camera, not the ship's (#158)
    src/engine/render/Material.cpp         # a shader plus what feeds it -- the data half (#121)
    src/engine/render/MaterialLibrary.cpp  # ...and the half that compiles and binds
    src/engine/render/TreatmentConfig.cpp  # the pass chain as data -- the testable half (#120)
    src/engine/render/Treatment.cpp        # ...and the half that talks to the GPU
    src/engine/render/Backdrop.cpp         # the world blurred once, for glass windows (#297)
    src/engine/render/TreatmentPanel.cpp   # its settings, drawn by the game and the editor
    src/engine/render/GlyphBackend.cpp
    src/engine/render/TextBackend.cpp  # no raylib drawing calls: agents and tests are headless
    src/engine/render/Perf.cpp         # what a frame costs, and where (#296)
    src/engine/ui/UiTheme.cpp
    src/engine/ui/TextCache.cpp        # measured text, kept until the fonts change (#363)
    src/engine/ui/Button.cpp
    src/engine/ui/Controls.cpp         # sliders and toggles, wanted by more than one screen
    src/engine/ui/Window.cpp
    src/engine/ui/Paint.cpp            # chamfers, glow, shadows, dashes: a theme's renderer tokens
    src/engine/ui/ContextMenu.cpp
    src/engine/ui/Input.cpp            # who the mouse belongs to while something draws (#297)
    src/engine/ui/Drag.cpp             # ...and what it carries between windows (#472)
    src/engine/ui/Focus.cpp            # ...and who the keyboard belongs to: one text field
    src/engine/ui/DeskLayout.cpp       # the window manager's rules, with no drawing -- tested
    src/engine/ui/Desk.cpp             # ...and the half that reads the mouse and draws
    src/engine/ui/Theme.cpp            # colours, spacing and sizes as data, and the UI scale (#297)
    src/engine/ui/Fonts.cpp            # the interface's typeface, crisp at every size (#297)
    src/engine/ui/Units.cpp            # numbers as a column reads them: 12.5k, 1.25M (#259)
    src/engine/ui/Layout.cpp           # rows, columns and text inside a window (#297)...
    src/engine/ui/ClayBridge.c         # ...solved by Clay, in C, behind a seam
)
# PUBLIC: the src/engine root is visible to everyone who links engine. That's why
# includes like "entities/Entity.h" from game and editor resolve automatically.
target_include_directories(engine PUBLIC ${CMAKE_SOURCE_DIR}/src/engine)
# core/Version.h, written by configure_file above from the project's version.
target_include_directories(engine PUBLIC ${CMAKE_BINARY_DIR}/generated)
target_link_libraries(engine PUBLIC raylib nlohmann_json::nlohmann_json)
# SYSTEM: Clay's warnings are not ours. PRIVATE: only ClayBridge.c includes it.
target_include_directories(engine SYSTEM PRIVATE ${clay_SOURCE_DIR})
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
    src/game/sim/Chat.cpp        # what a chat line may be and who hears it (#347)
    src/game/sim/Auth.cpp       # proving an account is itself (#106) -- both sides need it
    src/game/sim/Compat.cpp     # whether a client's build and data match the server's (#400)
    src/game/net/Tcp.cpp
    src/game/net/OsRandom.cpp    # nonces and salts from the system's generator (#351)
)
# PUBLIC: consumers include "sim/Protocol.h" and "net/Tcp.h" relative to src/game.
target_include_directories(netproto PUBLIC ${CMAKE_SOURCE_DIR}/src/game)
# PicoSHA2 ships a header and no target; SYSTEM so its warnings are not ours.
target_include_directories(netproto SYSTEM PRIVATE ${picosha2_SOURCE_DIR})
# The login stretch is a hundred thousand SHA-256 rounds, and it is meant to cost a client a
# fraction of a second. Unoptimized -- which is what a build with no CMAKE_BUILD_TYPE is,
# CI's included -- it took seconds, two clients logging in at once took longer still, and a
# server that drops a login after a limit then dropped one. Always optimized: nobody steps
# through a hash in a debugger.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set_source_files_properties(src/game/sim/Auth.cpp PROPERTIES COMPILE_OPTIONS -O2)
endif()
# engine, because Protocol.h uses core/Faction.h and raylib's Vector2/Color.
target_link_libraries(netproto PUBLIC engine)
# winsock, guarded here so the guard lives in one place when other platforms arrive (#12).
if(WIN32)
    target_link_libraries(netproto PUBLIC ws2_32 bcrypt)  # bcrypt: BCryptGenRandom (#351)
endif()

# --- Game ------------------------------------------------------------------
# The client does NOT link Simulation: the world lives on econserver. It shares only
# the wire protocol and PlayerStep, which is the movement step it must run identically
# to the server in order to predict its own ship.
add_executable(econspace
    src/game/main.cpp
    src/game/core/Game.cpp
    src/game/core/GameNet.cpp
    src/game/core/GameHud.cpp
    src/game/core/GameMap.cpp      # the galaxy map and its routes (#432)
    src/game/core/Navigation.cpp   # the map's itinerary, planned as the server plans it (#432)
    src/game/sim/SaveFile.cpp      # ui_navigation.json is renamed into place (#432)
    src/game/core/GameWindows.cpp  # missions, radar and settings, on Ui::Layout (#297)
    src/game/core/GameStation.cpp  # docked: the station, market and hangar windows (#297)
    src/game/core/GameChat.cpp     # the chat window: local, private, the operator (#347)
    src/game/core/GameScan.cpp     # the scanner window: a pass, and the signatures (#346)
    src/game/core/ErrorScreen.cpp  # why it could not get in, until the player closes it (#400)
    src/game/core/GameCorp.cpp     # the corporation window, tickers and corp mates (#347)
    src/game/core/Actions.cpp      # what can be done about a thing: one list for every window (#297)
    src/game/core/StationList.cpp  # what the market and hangar windows list (#297)
    src/game/core/Goal.cpp         # what to do next, in one line (#364)
    src/game/core/Prediction.cpp   # the own ship predicted and reconciled, without a window (#358)
    src/game/sim/Overview.cpp      # what the overview lists and in what order (#157)
    src/game/sim/MissionList.cpp   # what the missions window lists (#297)
    src/game/sim/InventoryList.cpp # what the inventory window lists (#472)
    src/game/core/GameInventory.cpp # the inventory window: containers, moves, drag and drop (#472)
    src/game/sim/Observation.cpp   # what a mission still needs, in the agent's words (#297)
    src/game/sim/Sensor.cpp        # the sensor screen's grid and allegiance colours (#123)
    src/game/sim/PlayerStep.cpp
    src/game/sim/WarpPath.cpp
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

# --- Headless galaxy server (track M, M3) ----------------------------------
# The same Simulation as in the game, but WITHOUT rendering/window/input: an
# authoritative server running real agents in every system. No client (Game) over it yet.
add_executable(econserver
    src/game/sim/main_headless.cpp
    src/game/sim/MacroBench.cpp           # econserver macrobench: the galaxy's cost by size (#295)
    src/game/sim/ProcessMemory.cpp        # resident memory, for the benchmark (#295)
    src/game/sim/Simulation.cpp
    src/game/sim/ClientSession.cpp        # one connected player, extracted from Simulation (#3)
    src/game/sim/DesignStore.cpp          # the designs players fly, filed by content (#279)
    src/game/sim/Corporation.cpp          # players' corporations: members, roles, invites (#347)
    src/game/sim/Simulation_World.cpp     # galaxy, hydration, routes, persistence (#17)
    src/game/sim/Simulation_Agents.cpp    # NPC AI, combat, spawn director, macro (#17)
    src/game/sim/Simulation_Factions.cpp  # factions reach where value outweighs risk (#231)
    src/game/sim/Simulation_Player.cpp    # the server-side player verbs and account (#17)
    src/game/sim/Simulation_Build.cpp     # construction: sites, their rules and their end (#39)
    src/game/sim/Simulation_Defence.cpp   # a structure's rules of engagement, and its owner (#340)
    src/game/sim/Simulation_Outposts.cpp  # where a faction's outpost stands, by purpose (#318)
    src/game/sim/Simulation_Sieges.cpp    # what keeping an outpost costs, and sieges (#295)
    src/game/sim/Simulation_Migration.cpp # ships a system has too many of leave (#295)
    src/game/sim/Simulation_Haulers.cpp   # NPC haulers carry goods between markets (#343)
    src/game/sim/Simulation_Orders.cpp    # the standing-order executor (#17)
    src/game/sim/Simulation_Snapshot.cpp  # state translated to wire messages (#17)
    src/game/sim/Simulation_Tide.cpp      # the opening log, sealing and the tide (#342 S3)
    src/game/sim/Simulation_Territory.cpp # influence, claims, a holder's tax and docking (#431)
    src/game/sim/Simulation_Owners.cpp    # whose a structure is, handing it over (#468)
    src/game/sim/Simulation_Patrols.cpp   # a corporation's own AI patrols (#431 slice 12)
    src/game/sim/Simulation_Fleets.cpp    # what each side flies, design by design (#464)
    src/game/sim/Simulation_Drift.cpp     # the archive of dead regions (#342 S4)
    src/game/sim/Simulation_Packs.cpp     # admin packs woven into a new world (#342 S7)
    src/game/sim/Simulation_Growth.cpp    # expeditions, growing stations, convoys (#342 S6)
    src/game/sim/Simulation_Jobs.cpp      # jobs posted by faction plans (#345)
    src/game/sim/Simulation_Contracts.cpp # contracts short markets post (#343, #407)
    src/game/sim/Simulation_Holes.cpp     # roaming holes, on a schedule of epochs (#342 S9)
    src/game/sim/Simulation_Hangars.cpp   # personal hangars at the stations, and their rent (#472)
    src/game/sim/Simulation_Knowledge.cpp # who knows which systems: public, own, corporation (#431)
    src/game/sim/Simulation_Scan.cpp      # signatures and the ship's scanner (#346)
    src/game/sim/WorldRules.cpp           # data/world_rules.json: how the world grows (#342 S3)
    src/game/sim/SaveFile.cpp             # a save is renamed into place, never half-written (#341)
    src/game/sim/HostRules.cpp            # who may log in and whether the world may start (#341)
    src/game/sim/AccessList.cpp           # the allowlist and the bans (#400)
    src/game/sim/ServerLog.cpp            # stamped log lines and the metrics line (#400)
    src/game/sim/ServerConfig.cpp         # server.json, init and the host's flags (#400)
    src/game/sim/ServerOps.cpp            # backups, the admin channel, a clean stop (#400)
    src/game/sim/PlayerStep.cpp
    src/game/sim/WarpPath.cpp
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

# --- Agent bridge (MCP) ----------------------------------------------------
# econagent wears two hats: an MCP server on stdio for a language model, and an ordinary
# TCP game client to econserver. It links netproto for exactly the reason the bridge is
# written in C++ at all -- so the wire format has one implementation and cannot drift.
add_executable(econagent
    src/agent/main.cpp
    src/agent/Jsonrpc.cpp
    src/agent/Session.cpp
    src/game/sim/Observation.cpp   # the world as text, which is what a model reads
    src/game/sim/InventoryList.cpp # the containers reachable here, as the window lists them (#472)
    src/game/sim/Orders.cpp        # order kinds, shared with the server
    src/game/entities/ShipType.cpp # the hangar catalog and what a station charges (#109)
)
target_include_directories(econagent PRIVATE ${CMAKE_SOURCE_DIR}/src)
# The selftest's watchdog is a thread: a self-test must not be able to hang.
find_package(Threads REQUIRED)
target_link_libraries(econagent PRIVATE engine netproto Threads::Threads)
set_target_properties(econagent PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin/agent)

# --- World editor ----------------------------------------------------------
add_executable(worldeditor
    src/editor/main.cpp
    src/editor/Editor.cpp
    src/editor/Editor_Panel.cpp     # the property editor for the selected object (#17)
    src/editor/Editor_Palette.cpp   # creating objects from the archetype registry (#17)
    src/editor/Editor_Galaxy.cpp    # the map view and its panel (#17)
    src/editor/Editor_Universe.cpp  # everything that changes universe.json (#17)
    src/editor/Editor_Gallery.cpp   # every archetype at once, for judging a look (#118)
    src/editor/Editor_Survey.cpp    # many generated systems at once, for judging rules (#141)
    src/editor/Editor_Pins.cpp      # a generated system edited, saved as a pin (#237)
    src/editor/Editor_Region.cpp    # the whole region a seed makes, as a map (#237)
    src/editor/Editor_World.cpp     # the world's galaxy of slots, a saved world, packs (#342 S8)
)
target_include_directories(worldeditor PRIVATE ${CMAKE_SOURCE_DIR}/src/editor)
target_link_libraries(worldeditor PRIVATE engine)
set_target_properties(worldeditor PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin/editor)

# --- Data -------------------------------------------------------------------
# Every binary finds data/ at runtime (src/engine/core/DataDir.h, #399): --data <dir>, else
# the source data/ while it runs from this build tree (ECON_DEV_DATA), else data/ beside the
# executable or one level up. So a dev build reads what the editor writes, with no rebuild,
# and a copied build reads what travels with it. Off by default in Release: a published
# binary should not carry this machine's source path.
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    set(ECON_DEV_DATA_DEFAULT OFF)
else()
    set(ECON_DEV_DATA_DEFAULT ON)
endif()
option(ECON_DEV_DATA "Read the source data/ when run from the build tree" ${ECON_DEV_DATA_DEFAULT})
if(ECON_DEV_DATA)
    set_source_files_properties(src/engine/core/DataDir.cpp PROPERTIES COMPILE_DEFINITIONS
        "ECON_DEV_DATA_DIR=\"${CMAKE_SOURCE_DIR}/data/\";ECON_BUILD_DIR=\"${CMAKE_BINARY_DIR}/\"")
endif()

# Copy data/ next to EACH executable (they live in different folders), so any bin/<tool>
# folder copied elsewhere runs as it is. In ALL: it runs on every build, data-only edits too.
add_custom_target(copy_data ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_SOURCE_DIR}/data $<TARGET_FILE_DIR:econspace>/data
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_SOURCE_DIR}/data $<TARGET_FILE_DIR:econserver>/data
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_SOURCE_DIR}/data $<TARGET_FILE_DIR:econagent>/data
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_SOURCE_DIR}/data $<TARGET_FILE_DIR:worldeditor>/data
    COMMENT "Copying data/ next to the executables")
add_dependencies(copy_data econspace econserver econagent worldeditor)

# --- Portable package --------------------------------------------------------
# One flat folder: the client, the server, the agent bridge and the editor side by side with
# data/ -- what a friend unpacks and runs (.zip on Windows, .tar.gz elsewhere; on Linux the
# server is the same folder on a headless machine). On macOS the binaries are plain
# executables, not an .app bundle, so they find data/ beside themselves exactly as on Linux. Only the `dist` component is installed, so the
# fetched libraries' own install rules stay out of it:
#   cmake --install build-release --component dist --prefix <dir>
#   cmake --build build-release --target dist   -> build-release/econspace-<version>-<codename>-<os>.zip|.tar.gz
install(TARGETS econspace econserver econagent worldeditor RUNTIME DESTINATION . COMPONENT dist)
install(DIRECTORY data/ DESTINATION data COMPONENT dist)
install(FILES tools/dist/README.txt DESTINATION . COMPONENT dist)
install(FILES docs/hosting.md DESTINATION . RENAME HOSTING.md COMPONENT dist)
if(NOT WIN32 AND NOT APPLE)
    install(FILES tools/dist/econserver.service DESTINATION . COMPONENT dist)  # docs/hosting.md
endif()
if(WIN32)
    install(FILES tools/dist/start-server.bat tools/dist/connect-example.bat
            DESTINATION . COMPONENT dist)
    set(ECON_DIST_ARCHIVE zip)
    set(ECON_DIST_TAR cf)
    set(ECON_DIST_FORMAT --format=zip)
else()
    set(ECON_DIST_ARCHIVE tar.gz)
    set(ECON_DIST_TAR czf)
    set(ECON_DIST_FORMAT)
endif()
# econspace-0.1.0-anchor-windows: the version and its codename in the name, so two releases
# downloaded into one folder are told apart (#400). Not a cache variable: a cached name would
# keep the version of the first configure after the project's had moved on.
# The platform as a player names it: CMake calls macOS "Darwin".
if(APPLE)
    set(ECON_DIST_OS macos)
else()
    set(ECON_DIST_OS ${CMAKE_SYSTEM_NAME})
endif()
string(TOLOWER "econspace-${PROJECT_VERSION}-${ECON_CODENAME}-${ECON_DIST_OS}" ECON_DIST_NAME)
set(ECON_DIST_FILE ${ECON_DIST_NAME}.${ECON_DIST_ARCHIVE})
add_custom_target(dist
    COMMAND ${CMAKE_COMMAND} -E rm -rf dist/${ECON_DIST_NAME} ${ECON_DIST_FILE}
    COMMAND ${CMAKE_COMMAND} --install . --component dist --prefix dist/${ECON_DIST_NAME}
    COMMAND ${CMAKE_COMMAND} -E chdir dist ${CMAKE_COMMAND} -E tar ${ECON_DIST_TAR}
            ../${ECON_DIST_FILE} ${ECON_DIST_FORMAT} ${ECON_DIST_NAME}
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Packing ${ECON_DIST_FILE}: client, server, agent, editor and data/")
add_dependencies(dist econspace econserver econagent worldeditor)

# --- Tests (doctest + ctest) -----------------------------------------------
# Regression unit tests for the wire protocol and TCP transport. Run with:
#   cmake --build build --target tests   (build)
#   ctest --test-dir build --output-on-failure   (run)
enable_testing()
add_executable(tests
    tests/test_main.cpp
    tests/protocol_tests.cpp
    tests/snapshot_delta_tests.cpp  # deltas against an acknowledged state, the area of interest (#349)
    tests/auth_tests.cpp
    tests/compat_tests.cpp         # the same build and the same data, or a Bye that says what differs (#400)
    tests/net_tests.cpp
    tests/raw_socket.cpp           # a peer that ignores the framing rules (#14)
    tests/observation_tests.cpp
    tests/economy_tests.cpp
    tests/goods_tests.cpp          # goods as data, stock and prices, buying (#343)
    tests/haulers_tests.cpp        # haulers, belts that run out, the fee to the owner (#343)
    tests/simulation_tests.cpp
    tests/archetype_tests.cpp
    tests/archetype_edit_tests.cpp
    tests/render_tests.cpp
    tests/lighting_tests.cpp
    tests/treatment_tests.cpp
    tests/material_tests.cpp
    tests/load_mistake_tests.cpp   # content mistakes that used to load as something else (#191)
    tests/silhouette_tests.cpp
    tests/camera_tests.cpp
    tests/overview_tests.cpp
    tests/route_tests.cpp
    tests/navigation_tests.cpp
    src/game/core/Navigation.cpp  # the map's itinerary, tested without a window (#432)
    tests/sensor_tests.cpp
    tests/gen_tests.cpp
    tests/centre_tests.cpp
    tests/survey_tests.cpp         # what the survey screen flags (#141)
    tests/warp_tests.cpp
    tests/warp_path_tests.cpp
    tests/clock_tests.cpp
    tests/build_tests.cpp          # construction: blueprints, sites, the clock (#39)
    tests/defence_tests.cpp        # rules of engagement for a structure (#340)
    tests/ship_design_tests.cpp    # ship designs and the stats derived from them (#279)
    tests/desk_tests.cpp           # the window manager: z-order, the mouse, Esc, layout (#297)
    tests/theme_tests.cpp          # the UI theme's strict loading, and the scale (#297)
    tests/layout_tests.cpp         # rows, columns, growing and wrapping, without a window (#297)
    tests/intel_tests.cpp          # what factions know, and surveying (#295)
    tests/widget_tests.cpp         # tables, tabs, fields, sliders and the keyboard focus (#297)
    tests/drag_tests.cpp           # drag and drop between windows (#472)
    tests/actions_tests.cpp        # one list of actions, each matched by an agent's tool (#297)
    tests/agent_args_tests.cpp     # a tool's arguments held to its schema (#425)
    src/agent/Jsonrpc.cpp          # ...which econagent's JSON-RPC layer checks
    tests/goal_tests.cpp           # what to do next, from the player's state (#364)
    tests/mission_list_tests.cpp   # what the missions window lists (#297)
    tests/inventory_tests.cpp      # what the inventory window lists, and what a move offers (#472)
    tests/hangar_tests.cpp         # personal hangars at the stations, and their rent (#472)
    tests/station_list_tests.cpp   # what the market and hangar windows list (#297)
    tests/settle_tests.cpp         # stock, outposts, and claims that need one (#295)
    tests/siege_tests.cpp          # upkeep, decay and sieges of outposts (#295)
    tests/law_tests.cpp            # the police answer a crime from real forces (#431)
    tests/pvp_tests.cpp            # PvP under the law: criminal flags, security status (#431)
    tests/security_tests.cpp       # security bought with police, and what it does (#431 slice 11)
    tests/patrol_tests.cpp         # a corporation's own AI patrols (#431 slice 12)
    tests/fleet_tests.cpp          # what each side in a system flies, design by design (#464)
    tests/style_tests.cpp          # what a region's NPCs fly, from its markets (#464)
    tests/outpost_tests.cpp        # outposts placed by purpose, and what each looks like (#318)
    tests/cold_tests.cpp           # ships only where a player is; the rest as numbers (#295)
    tests/hardening_tests.cpp      # exploits and lost saves closed before a public playtest (#341)
    tests/migration_tests.cpp      # surplus ships leave for a neighbour that lacks them (#295)
    tests/faction_tests.cpp        # factions as data: the registry, strict loading, saves by id (#355)
    tests/fitting_tests.cpp        # a ship flies a design of its own, filed by content (#279)
    tests/ship_loss_tests.cpp      # a lost ship is lost; insurance; waking at home (#344)
    tests/tide_tests.cpp           # the opening log, sealing, the tide and the cap (#342 S3)
    tests/mouth_tests.cpp          # unstable mouths, Anchors, drift and the archive (#342 S4)
    tests/holes_tests.cpp          # roaming holes and the scanner (#342 S9, #346)
    tests/knowledge_tests.cpp      # who knows which systems: public, own, corporation (#431)
    tests/contact_tests.cpp        # factions chart what they reach; first contact (#431)
    tests/prediction_tests.cpp     # the client's prediction against the server, same commands (#358)
    tests/natives_tests.cpp        # natives, their stations and frontier prices (#342 S5)
    tests/pack_tests.cpp           # admin packs: checked, woven, sealed (#342 S7)
    tests/growth_tests.cpp         # expeditions, stations grown by need, convoys (#342 S6)
    tests/jobs_tests.cpp           # jobs posted by faction plans (#345)
    tests/contracts_tests.cpp      # contracts short markets post (#343, #407)
    tests/access_tests.cpp         # the allowlist, bans and the server's log (#400)
    tests/server_ops_tests.cpp     # server.json, backups, the admin channel, log files (#400)
    tests/chat_tests.cpp           # local chat, private lines, limits and cleaning (#347)
    tests/corp_tests.cpp           # corporations: roles, the corp channel, trusted structures (#347)
    tests/territory_tests.cpp      # influence, claims, a holder's tax and docking (#431 slice 5)
    tests/licence_tests.cpp        # licences: leave and rent to build in a holder's space (#431 slice 6)
    tests/ownership_tests.cpp      # whose a structure is, and handing it over (#468)
    tests/chart_sales_tests.cpp    # charts sold to factions, priced by what they lack (#431 slice 16)
    src/game/sim/Observation.cpp   # shared with econagent (#42)
    src/game/sim/Overview.cpp      # the overview's rows, tested without a window (#157)
    src/game/sim/MissionList.cpp   # the missions window's rows, tested without one (#297)
    src/game/sim/InventoryList.cpp # the inventory window's containers and rows (#472)
    src/game/core/Actions.cpp      # the actions every window offers, tested without one (#297)
    src/game/core/StationList.cpp  # the market's and the hangar's rows, tested without them (#297)
    src/game/core/Goal.cpp         # the goal prompt, tested without a window (#364)
    src/game/core/Prediction.cpp   # the own ship's prediction, tested against the server (#358)
    src/game/sim/Sensor.cpp        # the sensor screen, tested without a window (#123)
    # The simulation itself is under test now, not only the wire (#10). That pulls in its
    # dependency tree -- ships, NPCs, the mission system -- which is the price of testing
    # the rules where they actually live rather than through a socket.
    src/game/sim/Simulation.cpp
    src/game/sim/ClientSession.cpp        # one connected player, extracted from Simulation (#3)
    src/game/sim/DesignStore.cpp          # the designs players fly, filed by content (#279)
    src/game/sim/Corporation.cpp          # players' corporations: members, roles, invites (#347)
    src/game/sim/Simulation_World.cpp     # galaxy, hydration, routes, persistence (#17)
    src/game/sim/Simulation_Agents.cpp    # NPC AI, combat, spawn director, macro (#17)
    src/game/sim/Simulation_Factions.cpp  # factions reach where value outweighs risk (#231)
    src/game/sim/Simulation_Player.cpp    # the server-side player verbs and account (#17)
    src/game/sim/Simulation_Build.cpp     # construction: sites, their rules and their end (#39)
    src/game/sim/Simulation_Defence.cpp   # a structure's rules of engagement, and its owner (#340)
    src/game/sim/Simulation_Outposts.cpp  # where a faction's outpost stands, by purpose (#318)
    src/game/sim/Simulation_Sieges.cpp    # what keeping an outpost costs, and sieges (#295)
    src/game/sim/Simulation_Migration.cpp # ships a system has too many of leave (#295)
    src/game/sim/Simulation_Haulers.cpp   # NPC haulers carry goods between markets (#343)
    src/game/sim/Simulation_Orders.cpp    # the standing-order executor (#17)
    src/game/sim/Simulation_Snapshot.cpp  # state translated to wire messages (#17)
    src/game/sim/Simulation_Tide.cpp      # the opening log, sealing and the tide (#342 S3)
    src/game/sim/Simulation_Territory.cpp # influence, claims, a holder's tax and docking (#431)
    src/game/sim/Simulation_Owners.cpp    # whose a structure is, handing it over (#468)
    src/game/sim/Simulation_Patrols.cpp   # a corporation's own AI patrols (#431 slice 12)
    src/game/sim/Simulation_Fleets.cpp    # what each side flies, design by design (#464)
    src/game/sim/Simulation_Drift.cpp     # the archive of dead regions (#342 S4)
    src/game/sim/Simulation_Packs.cpp     # admin packs woven into a new world (#342 S7)
    src/game/sim/Simulation_Growth.cpp    # expeditions, growing stations, convoys (#342 S6)
    src/game/sim/Simulation_Jobs.cpp      # jobs posted by faction plans (#345)
    src/game/sim/Simulation_Contracts.cpp # contracts short markets post (#343, #407)
    src/game/sim/Simulation_Holes.cpp     # roaming holes, on a schedule of epochs (#342 S9)
    src/game/sim/Simulation_Hangars.cpp   # personal hangars at the stations, and their rent (#472)
    src/game/sim/Simulation_Knowledge.cpp # who knows which systems: public, own, corporation (#431)
    src/game/sim/Simulation_Scan.cpp      # signatures and the ship's scanner (#346)
    src/game/sim/WorldRules.cpp           # data/world_rules.json: how the world grows (#342 S3)
    src/game/sim/SaveFile.cpp             # a save is renamed into place, never half-written (#341)
    src/game/sim/HostRules.cpp            # who may log in and whether the world may start (#341)
    src/game/sim/AccessList.cpp           # the allowlist and the bans (#400)
    src/game/sim/ServerLog.cpp            # stamped log lines and the metrics line (#400)
    src/game/sim/ServerConfig.cpp         # server.json, init and the host's flags (#400)
    src/game/sim/ServerOps.cpp            # backups, the admin channel, a clean stop (#400)
    src/game/sim/PlayerStep.cpp
    src/game/sim/WarpPath.cpp
    src/game/sim/Orders.cpp
    src/game/economy/Market.cpp
    src/game/entities/Ship.cpp
    src/game/entities/ShipType.cpp
    src/game/entities/NpcShip.cpp
    src/game/missions/MissionSystem.cpp
    src/game/player/Player.cpp
    src/game/player/Skills.cpp
)
target_compile_definitions(tests PRIVATE TEST_DATA_DIR="${CMAKE_SOURCE_DIR}/data/"
                                         # A small hand-written world in the format the loaders
                                         # read, for tests of everything but generation (#342)
                                         TEST_WORLD_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures/world/"
                                         # Admin packs, one of each kind (#342 S7)
                                         TEST_PACKS_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures/packs/")
target_link_libraries(tests PRIVATE engine netproto doctest::doctest)
target_include_directories(tests PRIVATE ${CMAKE_SOURCE_DIR}/src)  # agent/Jsonrpc.h (#425)
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
