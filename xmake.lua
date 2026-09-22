set_project("CoopIII")
set_version("0.0.1")

-- GTA III retail is a 32-bit executable; the client DLL must match its
-- bitness to be loadable in-process. Server/launcher don't strictly need to
-- be x86, but keeping everything on one arch keeps the build simple for now.
set_arch("x86")
set_languages("cxx17")

add_rules("mode.debug", "mode.release")

add_requires("enet")
add_requires("minhook")

-- Wire protocol + ENet transport shared by client and server.
-- Layout contract lives in sdk/include/coopiii/protocol.h.
target("sdk")
    set_kind("static")
    add_files("sdk/src/*.cpp")
    add_includedirs("sdk/include", {public = true})
    add_packages("enet", {public = true})

-- Loaded into gta3.exe by the ASI loader the player already has (see
-- docs/compat.md §2.1). Hooks the game loop, applies/sends net state.
-- `.asi` is just a DLL with a different extension.
target("client")
    set_kind("shared")
    set_basename("CoopIII")
    set_extension(".asi")
    add_files("client/src/*.cpp", "client/src/hook/*.cpp", "client/src/game/*.cpp")
    add_includedirs("client/src")
    add_deps("sdk")
    add_packages("minhook")

-- Standalone authoritative server process.
target("server")
    set_kind("binary")
    add_files("server/src/*.cpp")
    add_deps("sdk")

-- Starts gta3.exe with whatever args/env the mod needs.
target("launcher")
    set_kind("binary")
    add_files("launcher/src/*.cpp")
    add_deps("sdk")

-- Headless protocol test. Drives real clients against a running server.exe,
-- so the wire format can be tested without GTA III. See tools/nettest.
target("nettest")
    set_kind("binary")
    set_default(false)
    add_files("tools/nettest/*.cpp")
    add_deps("sdk")

-- Unit tests for client-side logic that has no engine dependency.
target("interptest")
    set_kind("binary")
    set_default(false)
    add_files("tools/interptest/*.cpp", "client/src/interp.cpp")
    add_includedirs("client/src")
    add_deps("sdk")

-- Config parsing, network clock, send-rate limiter, thread hand-off queue,
-- and the log's per-process file naming.
target("basetest")
    set_kind("binary")
    set_default(false)
    add_files("tools/basetest/*.cpp", "client/src/config.cpp", "client/src/log.cpp")
    add_includedirs("client/src")
    add_deps("sdk")

-- Roster, two-phase spawn and the engine seam, with no socket and no game.
target("clienttest")
    set_kind("binary")
    set_default(false)
    add_files("tools/clienttest/*.cpp",
              "client/src/client.cpp", "client/src/netthread.cpp",
              "client/src/interp.cpp", "client/src/log.cpp")
    add_includedirs("client/src")
    add_deps("sdk")

target("patterntest")
    set_kind("binary")
    set_default(false)
    add_files("tools/patterntest/*.cpp", "client/src/hook/pattern.cpp")
    add_includedirs("client/src")
    add_deps("sdk")

-- Generates runtime-scannable patterns from addresses found during analysis,
-- and proves each one is unique inside .text.
target("sigmaker")
    set_kind("binary")
    set_default(false)
    add_files("tools/sigmaker/*.cpp", "client/src/hook/pattern.cpp")
    add_includedirs("client/src")
    add_deps("sdk")

-- Hooks real functions inside the test process, so MinHook actually rewrites
-- instructions here rather than being mocked.
target("hooktest")
    set_kind("binary")
    set_default(false)
    add_files("tools/hooktest/*.cpp", "client/src/hook/*.cpp")
    add_includedirs("client/src")
    add_deps("sdk")
    add_packages("minhook")

-- A synthetic second player, so the ped spawn path can be exercised against a
-- live game without a second machine. See tools/ghost.
target("ghost")
    set_kind("binary")
    set_default(false)
    add_files("tools/ghost/*.cpp")
    add_includedirs("client/src")
    add_deps("sdk")

-- The server's roster, its host election and its clock, with no socket.
-- Session used to be a list and a counter, which nettest covered well
-- enough by driving two real clients through it. It now decides whose game
-- the whole session takes its time of day from, and that is worth being
-- able to test without waiting for two clients to connect.
target("sessiontest")
    set_kind("binary")
    set_default(false)
    add_files("tools/sessiontest/*.cpp", "server/src/session.cpp")
    add_includedirs("server/src")
    add_deps("sdk")

-- Resolves rel32 call targets and checks whether they land on a function
-- start. Exists because hand arithmetic got one wrong and crashed the game.
target("calltarget")
    set_kind("binary")
    set_default(false)
    add_files("tools/calltarget/*.cpp")
