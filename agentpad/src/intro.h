// Skips GTA III's two startup movies without touching the movie files.
//
// Costs about 70 seconds on the target machine, and it's paid on every
// automated run. AgentPad couldn't help with this before since it arms at
// GS_FRONTEND, and by then the movies have already finished.
//
// ---------------------------------------------------------------------------
// The state machine, read out of the retail binary
// ---------------------------------------------------------------------------
//
// WinMain (0x00582710) switches on gGameState (0x008F5838). Every write to it
// in the image, found by scanning .text for `C7 05 38 58 8F 00` (mov dword
// [gGameState], imm32) and `FF 05 38 58 8F 00` (inc dword [gGameState]):
//
//     0x005811F8  mov 0      WinMain init            gGameState = GS_START_UP
//     0x00582A75  mov 1      case GS_START_UP
//     0x00582A9A  mov 2      case GS_INIT_LOGO_MPEG  after PlayMovieInWindow
//     0x00582BEE  inc        case GS_LOGO_MPEG       <- skip-on-keypress
//     0x00582C2D  mov 4      case GS_INIT_INTRO_MPEG after PlayMovieInWindow
//     0x00582D7F  inc        case GS_INTRO_MPEG      <- skip-on-keypress
//     0x00582DB9  mov 6      case GS_INIT_ONCE
//     0x00582E0E  mov 7      case GS_INIT_FRONTEND
//     0x00582E55  mov 8  }   case GS_FRONTEND
//     0x00582E78  mov 9  }
//     0x0058173F  mov 3  }   WndProc, WM_GRAPHNOTIFY: the movie ran to its end
//     0x0058174B  mov 5  }
//     0x00582FEC  mov 6  }   the restart / rsACTIVATE path
//     0x00582FF8  mov 8  }
//
// Two things fall out of that table. States reached with an `inc` instead of
// a `mov` are 3 and 5, reached from 2 and 4 - that's re3's `++gGameState` in
// the two movie cases, i.e. the game's own "player pressed a key, skip this
// movie" branch. And the four bodies that matter disassemble statement for
// statement as re3's win.cpp:
//
//   case 1  GS_INIT_LOGO_MPEG:   PlayMovieInWindow("movies\Logo.mpg")
//   case 2  GS_LOGO_MPEG:        CPad::UpdatePads(); test six skip keys; break
//   case 3  GS_INIT_INTRO_MPEG:  CloseClip(); CoUninitialize();
//                                PlayMovieInWindow("movies\GTAtitles.mpg")
//   case 4  GS_INTRO_MPEG:       CPad::UpdatePads(); test six skip keys; break
//   case 5  GS_INIT_ONCE:        CloseClip(); CoUninitialize();
//                                LoadingScreen(nil, nil, "loadsc0");
//                                CGame::InitialiseOnceAfterRW()   <-- !!
//   case 6  GS_INIT_FRONTEND:    LoadingScreen(...);
//                                m_bGameNotLoaded = true;
//                                m_bStartUpFrontEndRequested = true
//
// (0x00582D8A onwards is case 5 verbatim: call 0x582680 = CloseClip, call
// [0x0061D684] = ole32!CoUninitialize, push "loadsc0"/0/0 + call 0x0048D770 =
// LoadingScreen, call 0x0048BD50 = CGame::InitialiseOnceAfterRW, and
// RsGlobal.quit on failure.)
//
// ---------------------------------------------------------------------------
// Which transitions are safe, and why
// ---------------------------------------------------------------------------
//
// SAFE:   2 -> 5   and   4 -> 5.
// UNSAFE: anything past 5, anything starting before 2.
//
// 4 -> 5 isn't really a jump - it's exactly what the game's own keypress
// branch does (`++gGameState` from 4). Nothing to argue about there.
//
// 2 -> 5 skips cases 3 and 4. Case 4 only waits, so that's free. Case 3 does
// three things, and skipping it is safe because case 5 redoes the first two:
//
//   * CloseClip() - case 5 calls this too, null-safe either way (re3
//     CdStream-era win.cpp:1976: `if (pMC) pMC->Stop();` then SAFE_RELEASE on
//     each interface). Skip case 3 and case 5 just ends up closing the logo
//     clip instead of the titles clip. Still one clip open, one CloseClip.
//
//   * CoUninitialize() - this is the one that actually matters.
//     PlayMovieInWindow calls CoInitialize, and cases 3 and 5 each call
//     CoUninitialize, so an unmodified run is two inits, two uninits. Skip
//     case 3 and you drop one CoInitialize (the titles movie's) and one
//     CoUninitialize together, so it still balances to zero: init in case 1,
//     uninit in case 5. That's why 2 -> 5 works and 1 -> 5 doesn't - from
//     case 1 the logo movie hasn't started yet, so its CoInitialize never
//     ran, and case 5's CoUninitialize would tear down an apartment this
//     thread doesn't own. Hence this only ever fires from 2 or 4, never
//     earlier: seeing gGameState == 2 is itself proof case 1's body already
//     ran, since `mov 2` is the instruction right after PlayMovieInWindow
//     returns.
//
//   * PlayMovieInWindow("movies\GTAtitles.mpg") - the movie itself. Not
//     starting it is the whole point. File is untouched, not even read.
//
// Nothing that initializes anything gets skipped. Cases 5 and 6 hold the
// one-time init - CGame::InitialiseOnceAfterRW, the loading screen,
// m_bGameNotLoaded, m_bStartUpFrontEndRequested - and both run in full.
// Jumping to 6 or past would skip InitialiseOnceAfterRW and the game would
// come up broken in a way that has nothing to do with movies. That's the
// failure this whole comment is here to prevent.
//
// movies/ is never renamed, moved, deleted, or read. Just a runtime state
// write, nothing more.
//
// ---------------------------------------------------------------------------
// Racing WinMain
// ---------------------------------------------------------------------------
//
// gGameState is a plain aligned dword, written here from AgentPad's boot
// thread, so the write itself is atomic. The only other writer while we're in
// 2 or 4 is WndProc's WM_GRAPHNOTIFY handler (0x0058173F / 0x0058174B), which
// sets 3 or 5 when a movie finishes on its own. Either order is fine - if it
// wins, we land in 3 then 4 and just force again; if we win, it writes either
// 5 (what we wanted anyway) or 3, and 3 leads back to 4. The loop below only
// ever acts on exactly 2 or 4, so it can't push a game that's already moved
// past that, and it stops for good once the state passes 4.
#pragma once

#include <cstdint>

namespace agentpad {

// One poll of the intro skipper. `state` is the gGameState just read. Returns
// true when the caller should write GS_INIT_ONCE (5) over it.
//
// Kept pure so padtest can walk the whole state machine without a game - the
// safety argument above only means anything if this provably acts on 2 and 4
// and nothing else.
bool ShouldSkipIntroFrom(uint32_t state);

} // namespace agentpad
