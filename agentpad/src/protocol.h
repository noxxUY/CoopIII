// AgentPad - the shared-memory contract between the driver and the .asi.
//
// Kept free of <windows.h> and of every game address on purpose. It's the one
// piece both sides of the wire agree on, and the piece tools/padtest can
// exercise standalone, without GTA III. Anything that needs the process
// (mapping the section, hooking, writing CPad) lives elsewhere; anything
// decidable from bytes alone lives here.
//
// The offsets below are a contract, not an implementation detail. The
// PowerShell driver in tools/GtaInput hardcodes them, so they're asserted at
// compile time - don't reorder them. New fields go in the reserved tail.
#pragma once

#include <cstddef>
#include <cstdint>

namespace agentpad {

// Section is named per game process: SHM_PREFIX, a dot, the game's pid in
// decimal - "CoopIII.AgentPad.v1.4312".
//
// Used to be the fixed name "CoopIII.AgentPad.v1", which was wrong in two
// ways at once. Two copies of the game would collide - the second one's
// CreateFileMapping attached to the first one's section instead of making
// its own, so both mods ended up serving the same driver and neither could
// be addressed individually. And tools/padtest, which creates that same
// fixed name to test the protocol, was quietly attaching to a live game's
// section and writing a pressed button into it. Explains why its last two
// checks failed whenever GTA III happened to be running.
//
// No "Global\" prefix, on purpose. That namespace needs
// SeCreateGlobalPrivilege to create, which would mean elevating the game just
// to let the mod publish a section. Session-local is the right scope anyway
// since the driver and the game run as the same interactive user.
inline constexpr char SHM_PREFIX[] = "CoopIII.AgentPad.v1";

// "CoopIII.AgentPad.v1." is 20 characters, a 32-bit pid is at most 10 digits,
// so 31 plus the terminator. Rounded up.
inline constexpr size_t SHM_NAME_MAX = 40;

// The one name that stays fixed: a directory of pids currently publishing a
// section, so a driver can enumerate instances instead of guessing the
// game's executable name or walking every pid on the box. Kept small and
// append-only, see struct Index below.
inline constexpr char SHM_INDEX_NAME[] = "CoopIII.AgentPad.v1.index";

inline constexpr uint32_t MAGIC   = 0x44415041u;   // 'APAD' little-endian
inline constexpr uint32_t VERSION = 1u;

// Writes "CoopIII.AgentPad.v1.<pid>" into `out`. Returns false and writes
// nothing if `cap` can't hold the whole name - a truncated section name
// would just be a different, still-valid name, which is the worst possible
// failure mode here.
bool FormatSectionName(uint32_t pid, char *out, size_t cap);

// Which of the three input surfaces the driver wants overwritten. Kept
// separate because they fail differently - pad state is what gameplay reads,
// keyboard state is what the frontend menu reads, mouse state is the only
// thing the PC camera reads. A driver that just wants menu navigation
// shouldn't silently pin the analog sticks to zero.
enum ChannelBit : uint32_t {
	CHANNEL_PAD   = 1u << 0,   // CPad::Pads[0].NewState
	CHANNEL_KEYS  = 1u << 1,   // CPad::NewKeyState
	CHANNEL_MOUSE = 1u << 2,   // CPad::NewMouseControllerState
	CHANNEL_ALL   = CHANNEL_PAD | CHANNEL_KEYS | CHANNEL_MOUSE,
};

// Named keys as a bitmask, rather than mirroring the whole 0x270-byte
// CKeyboardState. The frontend only looks at a handful of members anyway, and
// a bitmask is something a PowerShell script can write in one 32-bit poke.
enum Key : uint32_t {
	KEY_UP     = 1u << 0,
	KEY_DOWN   = 1u << 1,
	KEY_LEFT   = 1u << 2,
	KEY_RIGHT  = 1u << 3,
	KEY_ENTER  = 1u << 4,   // sets both ENTER and EXTENTER, see below
	KEY_ESC    = 1u << 5,
	KEY_TAB    = 1u << 6,
	KEY_BACKSP = 1u << 7,
	KEY_SPACE  = 1u << 8,   // VK_KEYS[' ']
	KEY_PGUP   = 1u << 9,
	KEY_PGDN   = 1u << 10,
	KEY_HOME   = 1u << 11,
	KEY_END    = 1u << 12,
	KEY_DEL    = 1u << 13,
	KEY_INS    = 1u << 14,
	KEY_SHIFT  = 1u << 15,  // sets LSHIFT and SHIFT
	KEY_ALL    = 0xFFFFu,
};

// What the mod is doing, published back to the driver so a script can tell
// "mod isn't loaded" apart from "mod's loaded and ignoring me".
enum Status : uint32_t {
	STATUS_UNKNOWN     = 0,
	STATUS_PASSTHROUGH = 1,   // injection off; the human has the keyboard
	STATUS_INJECTING   = 2,
	STATUS_STALE       = 3,   // watchdog fired: the driver stopped refreshing
	STATUS_BAD_HEADER  = 4,   // magic/version mismatch; treated as pass-through
};

#pragma pack(push, 1)

// 128 bytes. Fields above `modMagic` belong to the driver, fields from
// `modMagic` through `pad` belong to the mod, and `pad` goes back to the
// driver. Nothing here is written by both sides - that's what makes it safe
// without a lock.
struct Shared {
	// ---- written by the driver -------------------------------------------
	uint32_t magic;          // 0x00  MAGIC
	uint32_t version;        // 0x04  VERSION
	uint32_t seq;            // 0x08  bumped LAST, after everything else
	uint32_t enabled;        // 0x0C  0 = pass-through
	uint32_t channels;       // 0x10  Channel bitmask
	uint32_t writerTickMs;   // 0x14  driver's tick count at the last write
	uint32_t timeoutMs;      // 0x18  0 = no watchdog
	uint32_t keys;           // 0x1C  Key bitmask
	int32_t  mouseBudgetX;   // 0x20  total mouse units still owed, signed
	int32_t  mouseBudgetY;   // 0x24
	uint32_t mouseSeq;       // 0x28  bump to latch a NEW budget
	uint32_t mouseStepMax;   // 0x2C  cap per frame; 0 = deliver in one frame

	// ---- written by the mod ----------------------------------------------
	uint32_t modMagic;       // 0x30  MAGIC once the hook is live
	uint32_t modPid;         // 0x34  the game's process id
	uint32_t modFrames;      // 0x38  UpdatePads calls seen since install
	uint32_t modApplied;     // 0x3C  frames on which something was injected
	uint32_t modSeqSeen;     // 0x40  last driver seq observed
	uint32_t modStatus;      // 0x44  Status
	int32_t  modMouseLeftX;  // 0x48  budget the mod has still to deliver
	int32_t  modMouseLeftY;  // 0x4C

	// ---- written by the driver -------------------------------------------
	int16_t  pad[21];        // 0x50  CControllerState, re3 field order
	int16_t  reserved0;      // 0x7A

	// ---- written by the mod ----------------------------------------------
	// gGameState. 7 = GS_FRONTEND (menu), 9 = GS_PLAYING_GAME. Driver needs
	// this to pick menu keys vs gameplay input. Without it a script has to
	// guess from timing, and that's how unattended automation ends up
	// pressing Enter straight into a cutscene.
	uint32_t modGameState;   // 0x7C
};

#pragma pack(pop)

static_assert(sizeof(Shared) == 0x80, "the driver hardcodes a 128-byte section");
static_assert(offsetof(Shared, seq) == 0x08, "");
static_assert(offsetof(Shared, enabled) == 0x0C, "");
static_assert(offsetof(Shared, channels) == 0x10, "");
static_assert(offsetof(Shared, writerTickMs) == 0x14, "");
static_assert(offsetof(Shared, timeoutMs) == 0x18, "");
static_assert(offsetof(Shared, keys) == 0x1C, "");
static_assert(offsetof(Shared, mouseBudgetX) == 0x20, "");
static_assert(offsetof(Shared, mouseSeq) == 0x28, "");
static_assert(offsetof(Shared, mouseStepMax) == 0x2C, "");
static_assert(offsetof(Shared, modMagic) == 0x30, "");
static_assert(offsetof(Shared, modStatus) == 0x44, "");
static_assert(offsetof(Shared, pad) == 0x50, "");
static_assert(offsetof(Shared, modGameState) == 0x7C, "");
static_assert(sizeof(Shared::pad) == 0x2A, "pad[] mirrors CControllerState exactly");

// How many game instances the index can hold. Eight, because that's already
// more copies of GTA III than this machine will ever run. The limit is the
// table, not the protocol - the tail is reserved so it can grow without
// moving a field the driver pokes by offset.
inline constexpr uint32_t INDEX_SLOTS = 8;

#pragma pack(push, 1)

// Instance directory, published under SHM_INDEX_NAME.
//
// Every mod that publishes a section also claims a slot here, clearing it on
// unload. A driver reads the table, then opens "SHM_PREFIX.<pid>" for each
// non-zero entry. An entry whose section won't open is stale (game crashed,
// or the pid got recycled) and gets ignored, then reclaimed by the next mod
// that needs a slot.
//
// No lock, no header handshake beyond the magic - slots are claimed with a
// single InterlockedCompareExchange, which is the entire concurrency story
// here. A fresh section comes zero-filled from the kernel, so "nobody's
// registered" and "the section was just created" are the same state, nothing
// needs initializing before it's safe to read.
struct Index {
	uint32_t magic;               // 0x00  MAGIC once anyone has registered
	uint32_t version;             // 0x04  VERSION
	uint32_t capacity;            // 0x08  == INDEX_SLOTS, so a driver can
	                              //       read the table without assuming it
	uint32_t reserved0;           // 0x0C
	uint32_t pid[INDEX_SLOTS];    // 0x10  0 = free
	uint32_t reserved1[4];        // 0x30
};

#pragma pack(pop)

static_assert(sizeof(Index) == 0x40, "the driver hardcodes a 64-byte index");
static_assert(offsetof(Index, capacity) == 0x08, "");
static_assert(offsetof(Index, pid) == 0x10, "");

// A snapshot the mod has decided to act on. Kept separate from Shared so the
// hook never reads the mapped section twice, never acts on a half-written one.
struct Decision {
	Status   status   = STATUS_UNKNOWN;
	uint32_t channels = 0;          // surfaces to overwrite; 0 = touch nothing
	uint32_t keys     = 0;
	int16_t  pad[21]  = {};
	uint32_t seq      = 0;

	// Mouse arrives as a budget rather than a per-frame speed, so a command
	// means the same rotation whether the game runs at 30fps or 60. The hook
	// latches a new budget when mouseSeq changes and drains it over however
	// many frames mouseStepMax needs.
	uint32_t mouseSeq     = 0;
	int32_t  mouseBudgetX = 0;
	int32_t  mouseBudgetY = 0;
	uint32_t mouseStepMax = 0;

	bool Injecting() const { return status == STATUS_INJECTING && channels != 0; }
};

// Reads `in` into `out` and says whether it should be acted on.
//
// `nowMs` is the caller's tick count. `lastGood` is the last Decision this
// caller accepted, used when a read tears. Pure function - no clock, no I/O,
// no game.
//
// Torn reads get handled without a lock because the driver bumps `seq` only
// after writing everything else. So: read seq, copy, read seq again. If it
// moved, the copy might be a mix of two generations, so keep last frame's
// instead. A stale frame at 60Hz is 16ms and invisible; a mixed controller
// state is a stuck button.
Decision Decide(const Shared &in, uint32_t nowMs, const Decision &lastGood);

// Splits a signed budget into "how much to deliver this frame" and "what's
// left", honoring a per-frame cap. This is where framerate independence
// actually lives - the driver asks for a total rotation, not a per-frame
// speed, so the same command turns the camera the same amount at 30fps or 60.
//
// stepMax of 0 means deliver the whole budget in one go. Negative budget
// steps negative. Never overshoots, never flips sign.
int32_t DrainStep(int32_t budget, uint32_t stepMax);

// Maps a Key bit to the byte offset of its CKeyboardState member, so the
// mapping can be tested without a game running. Returns false for bits that
// aren't a single named key - KEY_ENTER and KEY_SHIFT each set two members,
// KEY_SPACE lands inside VK_KEYS - those get their own helpers.
bool KeyFieldOffset(Key key, size_t *offsetOut);

// The whole per-frame state machine, no game and no Windows in it.
//
// Not inlined into the hook on purpose. The hook's job shrinks down to "call
// Step, then write the three results into CPad" - so the logic that decides
// what to press (torn-read rule, watchdog, latching and draining a mouse
// budget, cancelling a pan when injection stops) runs outside the game
// entirely. tools/padtest exercises this class directly and also runs it
// against a live PowerShell driver, so the tests cover the actual code the
// mod runs instead of some parallel reimplementation of it.
class Engine {
public:
	// What the caller should write into the game this frame. `channels` says
	// which of the three are meaningful; the others must be left alone.
	struct Frame {
		Status   status   = STATUS_UNKNOWN;
		uint32_t channels = 0;
		uint32_t keys     = 0;
		int16_t  pad[21]  = {};
		int32_t  mouseDx  = 0;   // this frame's slice of the budget
		int32_t  mouseDy  = 0;
	};

	// Runs one frame against the section and publishes the mod's half of it
	// (frame counts, status, remaining budget) so a driver can see liveness.
	// `nowMs` is a tick count - the class never reads a clock itself.
	Frame Step(Shared &shm, uint32_t nowMs);

	uint32_t Frames() const { return m_frames; }
	uint32_t Applied() const { return m_applied; }
	int32_t  MouseRemainingX() const { return m_mouseLeftX; }
	int32_t  MouseRemainingY() const { return m_mouseLeftY; }

private:
	Decision m_lastGood;
	uint32_t m_frames       = 0;
	uint32_t m_applied      = 0;
	uint32_t m_mouseSeqSeen = 0;
	int32_t  m_mouseLeftX   = 0;
	int32_t  m_mouseLeftY   = 0;
};

// Every key bit, in ascending order, for iteration and for tests.
inline constexpr Key ALL_KEYS[] = {
    KEY_UP,   KEY_DOWN,  KEY_LEFT, KEY_RIGHT, KEY_ENTER, KEY_ESC,
    KEY_TAB,  KEY_BACKSP, KEY_SPACE, KEY_PGUP, KEY_PGDN, KEY_HOME,
    KEY_END,  KEY_DEL,   KEY_INS,  KEY_SHIFT,
};

// Name <-> bit, so the driver script and the mod agree on spelling. Returns
// zero when the name's unknown - never a guess.
uint32_t KeyFromName(const char *name);
const char *KeyName(Key key);

// Name -> index into Shared::pad. Returns -1 when unknown.
int PadFieldFromName(const char *name);
const char *PadFieldName(int index);

// Indices into Shared::pad. Same order as re3's CControllerState declaration,
// which is also the order CPad::Update's unrolled OldState copy walks in the
// retail binary. Duplicated from coopiii::game::pad::Field on purpose - this
// header has to build without the game headers so it can be unit-tested -
// and pinned to it via static_asserts in agentpad/src/inject.cpp.
enum PadIndex : int {
	IDX_LEFT_STICK_X = 0, IDX_LEFT_STICK_Y,
	IDX_RIGHT_STICK_X,    IDX_RIGHT_STICK_Y,
	IDX_LEFT_SHOULDER1,   IDX_LEFT_SHOULDER2,
	IDX_RIGHT_SHOULDER1,  IDX_RIGHT_SHOULDER2,
	IDX_DPAD_UP,          IDX_DPAD_DOWN,   IDX_DPAD_LEFT, IDX_DPAD_RIGHT,
	IDX_START,            IDX_SELECT,
	IDX_SQUARE,           IDX_TRIANGLE,    IDX_CROSS,     IDX_CIRCLE,
	IDX_LEFT_SHOCK,       IDX_RIGHT_SHOCK,
	IDX_NETWORK_TALK,
	IDX_COUNT,
};

static_assert(IDX_COUNT == 21, "CControllerState has exactly 21 fields");

// Value a digital pad button takes when held. Game compares against 0 for
// buttons and uses magnitude for analog, so 255 is what the PC input layer
// itself writes for a pressed button (re3 ControllerConfig.cpp).
inline constexpr int16_t BUTTON_DOWN = 255;

// Full analog deflection. CControllerState::GetLeftStickX divides by 32767.
inline constexpr int16_t AXIS_MAX = 32767;

} // namespace agentpad
