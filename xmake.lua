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

-- AgentPad.asi, a SEPARATE standalone plugin. Lets an external process drive
-- the game's input by writing CPad's composed state from inside the process,
-- which is the only route that works: synthetic input (keybd_event,
-- PostMessage of WM_KEYDOWN, SendInput with scancodes, synthetic mouse) never
-- reaches GTA III, because it reads DirectInput device state directly.
--
-- Shares nothing with the client at runtime. It reuses client/src/game/verify
-- (same image guard), client/src/hook (same detour helper) and client/src/log
-- because those solve the same problems, not because the mods are coupled.
target("agentpad")
    set_kind("shared")
    set_basename("AgentPad")
    set_extension(".asi")
    add_files("agentpad/src/*.cpp",
              "client/src/hook/*.cpp",
              "client/src/game/verify.cpp",
              "client/src/log.cpp")
    add_includedirs("agentpad/src", "client/src")
    add_packages("minhook")

-- Shared-memory protocol, state encoding and the mouse budget, with no game
-- and no hook. Creates and opens a real named section in-process, and since
-- the section name now carries the pid, running this while GTA III is up no
-- longer attaches to the live game's section (which it used to, and then wrote
-- a pressed button into it).
--
-- intro.cpp and cdstream.cpp are here because the decisions in them (which
-- gGameState transitions are safe to force, and what name this process's
-- streaming semaphore gets) are arithmetic, and arithmetic should not need
-- the game to be tested. Only the parts that touch the process live in
-- functions this cannot reach.
target("padtest")
    set_kind("binary")
    set_default(false)
    add_files("tools/padtest/*.cpp",
              "agentpad/src/protocol.cpp", "agentpad/src/channel.cpp",
              "agentpad/src/settings.cpp", "agentpad/src/intro.cpp",
              "agentpad/src/cdstream.cpp")
    add_includedirs("agentpad/src")

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

-- Resolves rel32 call targets and checks whether they land on a function
-- start. Exists because hand arithmetic got one wrong and crashed the game.
target("calltarget")
    set_kind("binary")
    set_default(false)
    add_files("tools/calltarget/*.cpp")
