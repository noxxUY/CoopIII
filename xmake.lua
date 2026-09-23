set_project("CoopIII")
set_version("0.0.1")

-- GTA III retail is a 32-bit executable; the client DLL must match its
-- bitness to be loadable in-process. Server/launcher don't strictly need to
-- be x86, but keeping everything on one arch keeps the build simple for now.
set_arch("x86")
set_languages("cxx17")

add_rules("mode.debug", "mode.release")


-- Static CRT everywhere. The installer has to be one self-contained exe, and
-- everything else benefits for the same reason: a player who has never
-- installed a Visual C++ redistributable still gets a launcher that starts,
-- and the .asi stops caring which CRT the other plugins in gta3.exe brought
-- with them. Packages have to match, or the link fails.
set_runtimes(is_mode("debug") and "MTd" or "MT")

add_requires("enet")
add_requires("minhook")

-- The GUIs. Dear ImGui with its Win32 and Direct3D 11 backends, and stb_image
-- for the one PNG the apps carry (the logo).
add_requires("imgui", {configs = {win32 = true, dx11 = true}})
add_requires("stb")

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
    -- user32 for the one key GTA III has no binding for: the passenger seat
    -- (client/src/game/seat.cpp). Nothing else here talks to Windows.
    add_syslinks("user32")

-- Standalone authoritative server process.
-- The session, the relay and the settings file. No interface attached, so the
-- window and the console are two front ends over one server.
target("server-core")
    set_kind("static")
    add_files("server/core/*.cpp")
    add_includedirs("server/core", {public = true})
    add_deps("sdk")

-- Finding the game, the pre-launch checks, CoopIII.ini and starting gta3.exe
-- with the co-op marker (docs/roadmap.md §5.6). No interface attached, so the
-- console launcher, the window and the installer all get the same answers.
target("launcher-core")
    set_kind("static")
    add_files("launcher/core/*.cpp")
    add_includedirs("launcher/include", {public = true})
    add_syslinks("advapi32", {public = true})

-- ---------------------------------------------------------------------------
-- The graphical front ends. design/DESIGN.md is the spec; design/screens/ has
-- the exact values.
-- ---------------------------------------------------------------------------

-- Theme, fonts, icons, the city map background and the components. Shared by
-- the launcher, the server and the installer so the three cannot drift apart.
target("ui")
    set_kind("static")
    add_rules("utils.bin2c", {extensions = {".ttf", ".png"}})
    add_files("ui/src/*.cpp")
    add_files("ui/assets/fonts/*.ttf")
    add_files("ui/assets/logo-mark.png")
    add_includedirs("ui/include", {public = true})
    add_packages("imgui", "stb", {public = true})
    add_syslinks("d3d11", "dxgi", "dwmapi", "ole32", "shell32", "user32", "gdi32", "advapi32",
                 {public = true})

-- CoopIII-Server.ini: its defaults are docs/roadmap.md 5, and a default that
-- drifts is a settled decision being reopened by accident.
target("configtest")
    set_kind("binary")
    set_default(false)
    add_files("tools/configtest/*.cpp")
    add_deps("server-core")

-- Name and port validation, the CoopIII.ini rewrite that has to leave a
-- hand-edited file alone, MD5, and the pre-launch checks.
target("launchertest")
    set_kind("binary")
    set_default(false)
    add_files("tools/launchertest/*.cpp")
    add_deps("launcher-core")

-- The colour tokens' contrast, the city map's loop and the icon paths, none of
-- which need a window or a GPU.
target("uitest")
    set_kind("binary")
    set_default(false)
    add_files("tools/uitest/*.cpp")
    add_deps("ui")

-- Every component in both themes, with a switch. Not shipped; it exists so a
-- widget can be looked at on its own rather than only inside a screen.
target("ui-gallery")
    set_kind("binary")
    set_default(false)
    add_files("ui/gallery/*.cpp")
    add_deps("ui")
    add_ldflags("/subsystem:windows", "/entry:mainCRTStartup", {force = true})

-- The Setup's working parts: what it can install, the SHA-256 that proves a
-- download is what its manifest said, and the patch that puts a Steam copy of
-- gta3.exe back to v1.0.
target("installer-core")
    set_kind("static")
    add_rules("utils.bin2c", {extensions = {".json"}})
    add_files("installer/core/*.cpp")
    add_files("installer/assets/components.json")
    add_includedirs("installer/include", {public = true})
    add_deps("launcher-core")
    add_syslinks("winhttp", "ole32", "shell32", "uuid", {public = true})

-- design/screens/Installer*.dc.html. One self-contained exe, which is why
-- the whole project is on the static CRT.
-- What the Setup carries: CoopIII.asi, the launcher and CoopIII.ini, written
-- into a header the Setup compiles in. A target of its own rather than a step
-- inside the Setup's, so the ordering is the build graph's job: this one runs
-- after the two artifacts exist, and the Setup runs after this one.
target("installer-payload")
    set_kind("phony")
    -- inherit = false: the Setup carries these two, it does not link them.
    add_deps("client", "launcher", {inherit = false})
    -- A fence: everything this depends on finishes before anything that
    -- depends on it starts, which is what makes the header below exist by
    -- the time the Setup compiles.
    set_policy("build.fence", true)

    after_build(function (target)
        import("core.project.project")

        local payload = {
            {name = "CoopIII.asi", path = project.target("client"):targetfile()},
            {name = "coopiii-launcher.exe", path = project.target("launcher"):targetfile()},
            {name = "CoopIII.ini", path = path.join(os.projectdir(), "CoopIII.ini")},
        }

        local out = path.join(os.projectdir(), "build", ".gens", "installer", "payload.inc")
        os.mkdir(path.directory(out))

        local newline = string.char(10)
        local lines = {
            "// Generated by xmake from the artifacts the Setup carries. Do not edit.",
            "// This is what makes a release one exe rather than a folder.",
            "",
        }
        for i, file in ipairs(payload) do
            local bytes = io.readfile(file.path, {encoding = "binary"})
            if not bytes then
                raise("the Setup needs " .. file.path .. " but it is not there")
            end
            local parts = {}
            for j = 1, #bytes do
                parts[#parts + 1] = string.format("0x%02X,", bytes:byte(j))
                if j % 20 == 0 then
                    parts[#parts + 1] = newline
                end
            end
            lines[#lines + 1] = string.format("static const unsigned char kPayload%d[] = {%s};",
                                              i, table.concat(parts))
        end

        lines[#lines + 1] = "static const PayloadFile kPayload[] = {"
        for i, file in ipairs(payload) do
            lines[#lines + 1] = string.format("    {%q, kPayload%d, sizeof(kPayload%d)},",
                                              file.name, i, i)
        end
        lines[#lines + 1] = "};"

        io.writefile(out, table.concat(lines, newline))
    end)

target("installer")
    set_kind("binary")
    set_basename("CoopIII-Setup")
    add_files("installer/gui/*.cpp")
    add_deps("ui", "installer-core", "installer-payload")
    add_ldflags("/subsystem:windows", "/entry:mainCRTStartup", {force = true})
    add_includedirs("$(builddir)/.gens/installer")

-- Turns two gta3.exe files into the patch the Setup ships. Run by whoever
-- owns both copies; the repo carries neither.
target("mkpatch")
    set_kind("binary")
    set_default(false)
    add_files("tools/mkpatch/*.cpp")
    add_deps("installer-core")

-- The patch round trip, SHA-256 against the published vectors, and the
-- manifest reader.
target("installertest")
    set_kind("binary")
    set_default(false)
    add_files("tools/installertest/*.cpp")
    add_deps("installer-core")

-- One server.exe with two front ends: the window of
-- design/screens/Server.dc.html, and the console behind --nogui.
--
-- Linked as a Windows application rather than a console one, because the
-- window is the common case and a console subsystem flashes a black rectangle
-- every time somebody double-clicks it. --nogui goes and finds a console
-- instead - see ui/console.h.
target("server")
    set_kind("binary")
    add_files("server/*.cpp", "server/cli/*.cpp", "server/gui/*.cpp")
    add_includedirs("server")
    add_deps("ui", "server-core")
    add_syslinks("iphlpapi", "ws2_32")
    add_ldflags("/subsystem:windows", "/entry:mainCRTStartup", {force = true})

-- One coopiii-launcher.exe with two front ends, the same way as the server:
-- the window of design/screens/Main.dc.html and LauncherBlocked.dc.html, and
-- the console behind --nogui. It starts gta3.exe with the co-op marker in its
-- environment (docs/roadmap.md §5.6).
target("launcher")
    set_kind("binary")
    set_basename("coopiii-launcher")
    add_files("launcher/*.cpp", "launcher/cli/*.cpp", "launcher/gui/*.cpp")
    add_includedirs("launcher")
    add_deps("ui", "launcher-core")
    add_ldflags("/subsystem:windows", "/entry:mainCRTStartup", {force = true})

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
              "client/src/interp.cpp", "client/src/log.cpp",
              "client/src/game/sessionclock.cpp",
              "client/src/helisync.cpp", "client/src/moneysync.cpp")
    add_includedirs("client/src")
    add_deps("sdk")

-- Breakable street objects (docs/objects.md). The identity matcher runs over
-- a pool this test builds by hand out of the same offsets and the same 0x19C
-- stride the engine uses, so the lookup is the real one walking real bytes
-- rather than a mock of itself.
target("objecttest")
    set_kind("binary")
    set_default(false)
    add_files("tools/objecttest/*.cpp",
              "client/src/game/object.cpp",
              "client/src/hook/hook.cpp", "client/src/hook/pattern.cpp",
              "client/src/log.cpp")
    add_includedirs("client/src")
    add_deps("sdk")
    add_packages("minhook")

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
    add_files("tools/sessiontest/*.cpp")
    add_deps("sdk", "server-core")

-- Resolves rel32 call targets and checks whether they land on a function
-- start. Exists because hand arithmetic got one wrong and crashed the game.
target("calltarget")
    set_kind("binary")
    set_default(false)
    add_files("tools/calltarget/*.cpp")
