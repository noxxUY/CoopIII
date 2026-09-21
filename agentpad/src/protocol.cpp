#include "protocol.h"

#include <cstring>

namespace agentpad {

namespace {

struct KeyRow {
	Key         bit;
	const char *name;
	size_t      offset;   // CKeyboardState member offset, or NO_OFFSET
};

// 0 is a real offset (F[0]), so the sentinel has to be something else.
constexpr size_t NO_OFFSET = static_cast<size_t>(-1);

// Offsets mirror coopiii::game::pad::KEY_* in client/src/game/addresses.h.
// Duplicated rather than included because this file has to build without the
// game headers - tools/padtest links it standalone. Tied to the game's copy
// via a static_assert in inject.cpp, which sees both.
constexpr KeyRow KEY_TABLE[] = {
    {KEY_UP,     "Up",        0x226},
    {KEY_DOWN,   "Down",      0x228},
    {KEY_LEFT,   "Left",      0x22A},
    {KEY_RIGHT,  "Right",     0x22C},
    {KEY_ENTER,  "Enter",     NO_OFFSET},   // sets ENTER (0x23C) and EXTENTER (0x25A)
    {KEY_ESC,    "Escape",    0x218},
    {KEY_TAB,    "Tab",       0x256},
    {KEY_BACKSP, "Backspace", 0x254},
    {KEY_SPACE,  "Space",     NO_OFFSET},   // lives inside VK_KEYS[' ']
    {KEY_PGUP,   "PageUp",    0x222},
    {KEY_PGDN,   "PageDown",  0x224},
    {KEY_HOME,   "Home",      0x21E},
    {KEY_END,    "End",       0x220},
    {KEY_DEL,    "Delete",    0x21C},
    {KEY_INS,    "Insert",    0x21A},
    {KEY_SHIFT,  "Shift",     NO_OFFSET},   // sets LSHIFT (0x25C) and SHIFT (0x260)
};

// Index order is re3's CControllerState declaration order, which is also the
// order CPad::Update's unrolled OldState copy walks. Don't reorder this.
constexpr const char *PAD_NAMES[21] = {
    "LeftStickX",     "LeftStickY",
    "RightStickX",    "RightStickY",
    "LeftShoulder1",  "LeftShoulder2",
    "RightShoulder1", "RightShoulder2",
    "DPadUp",         "DPadDown",     "DPadLeft", "DPadRight",
    "Start",          "Select",
    "Square",         "Triangle",     "Cross",    "Circle",
    "LeftShock",      "RightShock",
    "NetworkTalk",
};

bool EqualsIgnoreCase(const char *a, const char *b) {
	for (; *a && *b; ++a, ++b) {
		char ca = *a, cb = *b;
		if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca - 'A' + 'a');
		if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb - 'A' + 'a');
		if (ca != cb)
			return false;
	}
	return *a == '\0' && *b == '\0';
}

} // namespace

bool FormatSectionName(uint32_t pid, char *out, size_t cap) {
	if (!out || cap == 0)
		return false;

	// Decimal, smallest digit first, then reversed. Hand-rolled instead of
	// snprintf so this stays a pure function of its arguments - no locale, no
	// CRT state, nothing that could behave differently inside the game
	// process than it does inside padtest.
	char digits[16];
	size_t nd = 0;
	if (pid == 0) {
		digits[nd++] = '0';
	} else {
		for (uint32_t v = pid; v != 0; v /= 10)
			digits[nd++] = static_cast<char>('0' + (v % 10));
	}

	const size_t prefixLen = sizeof(SHM_PREFIX) - 1;
	const size_t needed    = prefixLen + 1 /* '.' */ + nd + 1 /* NUL */;
	if (needed > cap)
		return false;   // never truncate - that's a different valid name and
		                // two instances could end up sharing it

	std::memcpy(out, SHM_PREFIX, prefixLen);
	out[prefixLen] = '.';
	for (size_t i = 0; i < nd; ++i)
		out[prefixLen + 1 + i] = digits[nd - 1 - i];
	out[prefixLen + 1 + nd] = '\0';
	return true;
}

bool KeyFieldOffset(Key key, size_t *offsetOut) {
	for (const KeyRow &row : KEY_TABLE) {
		if (row.bit != key)
			continue;
		if (row.offset == NO_OFFSET)
			return false;
		if (offsetOut)
			*offsetOut = row.offset;
		return true;
	}
	return false;
}

uint32_t KeyFromName(const char *name) {
	if (!name)
		return 0;
	for (const KeyRow &row : KEY_TABLE)
		if (EqualsIgnoreCase(name, row.name))
			return row.bit;

	// A few spellings people reach for out of habit. Kept as aliases here
	// rather than in the table so KeyName() stays single-valued.
	if (EqualsIgnoreCase(name, "Esc"))    return KEY_ESC;
	if (EqualsIgnoreCase(name, "Return")) return KEY_ENTER;
	if (EqualsIgnoreCase(name, "Back"))   return KEY_ESC;
	return 0;
}

const char *KeyName(Key key) {
	for (const KeyRow &row : KEY_TABLE)
		if (row.bit == key)
			return row.name;
	return "";
}

int PadFieldFromName(const char *name) {
	if (!name)
		return -1;
	for (int i = 0; i < 21; ++i)
		if (EqualsIgnoreCase(name, PAD_NAMES[i]))
			return i;

	// Everyone calls the shoulder buttons L1/L2/R1/R2 - nobody's typing
	// "RightShoulder2" in a script.
	if (EqualsIgnoreCase(name, "L1")) return 4;
	if (EqualsIgnoreCase(name, "L2")) return 5;
	if (EqualsIgnoreCase(name, "R1")) return 6;
	if (EqualsIgnoreCase(name, "R2")) return 7;
	return -1;
}

const char *PadFieldName(int index) {
	if (index < 0 || index >= 21)
		return "";
	return PAD_NAMES[index];
}

int32_t DrainStep(int32_t budget, uint32_t stepMax) {
	if (budget == 0)
		return 0;
	if (stepMax == 0)
		return budget;

	// Cap as a magnitude so a negative budget steps negatively too. stepMax
	// gets clamped to INT32_MAX first - a driver writing 0xFFFFFFFF should get
	// "the whole thing", not a sign flip.
	const int32_t cap = stepMax > 0x7FFFFFFFu ? 0x7FFFFFFF : static_cast<int32_t>(stepMax);
	if (budget > 0)
		return budget < cap ? budget : cap;
	// -budget overflows for INT32_MIN, so compare against -cap instead.
	return budget > -cap ? budget : -cap;
}

Decision Decide(const Shared &in, uint32_t nowMs, const Decision &lastGood) {
	Decision out;

	// Read the sequence first, copy, read it again. Driver writes seq last, so
	// an unchanged seq across the copy means we got one generation, not a
	// blend of two.
	const uint32_t seqBefore = in.seq;

	const uint32_t magic    = in.magic;
	const uint32_t version  = in.version;
	const uint32_t enabled  = in.enabled;
	const uint32_t channels = in.channels;
	const uint32_t keys     = in.keys;
	const uint32_t tick     = in.writerTickMs;
	const uint32_t timeout  = in.timeoutMs;
	const uint32_t mouseSeq = in.mouseSeq;
	const int32_t  mouseX   = in.mouseBudgetX;
	const int32_t  mouseY   = in.mouseBudgetY;
	const uint32_t mouseCap = in.mouseStepMax;
	int16_t        pad[21];
	std::memcpy(pad, in.pad, sizeof(pad));

	const uint32_t seqAfter = in.seq;

	if (magic != MAGIC || version != VERSION) {
		out.status = STATUS_BAD_HEADER;
		return out;   // channels stays 0, touch nothing, human keeps control
	}

	if (seqBefore != seqAfter) {
		// Torn read. Reuse whatever we last accepted - a repeated frame of
		// input is invisible, a half-written controller state is a stuck
		// button.
		return lastGood;
	}

	out.seq = seqAfter;

	if (!enabled) {
		out.status = STATUS_PASSTHROUGH;
		return out;
	}

	// Watchdog. If the driver asked to be supervised and then stopped
	// refreshing its heartbeat, stop injecting. This is the difference between
	// "the script crashed mid-hold" and the player walking into the sea
	// forever with no way to stop it.
	//
	// Unsigned subtraction wraps correctly across the 49.7-day GetTickCount
	// rollover, which is why neither side compares raw ticks directly.
	if (timeout != 0 && (nowMs - tick) > timeout) {
		out.status = STATUS_STALE;
		return out;
	}

	out.status       = STATUS_INJECTING;
	out.channels     = channels & CHANNEL_ALL;
	out.keys         = keys;
	out.mouseSeq     = mouseSeq;
	out.mouseBudgetX = mouseX;
	out.mouseBudgetY = mouseY;
	out.mouseStepMax = mouseCap;
	std::memcpy(out.pad, pad, sizeof(pad));
	return out;
}

Engine::Frame Engine::Step(Shared &shm, uint32_t nowMs) {
	++m_frames;

	const Decision d = Decide(shm, nowMs, m_lastGood);
	m_lastGood       = d;

	Frame f;
	f.status = d.status;

	if (!d.Injecting()) {
		// Cancel any pan still in flight. Otherwise turning injection off
		// mid-turn leaves the camera spinning - the game only recomputes
		// NewMouseControllerState while the window has focus, so a stale
		// non-zero delta just sits there getting re-read every frame.
		m_mouseLeftX = 0;
		m_mouseLeftY = 0;

		shm.modStatus     = d.status;
		shm.modSeqSeen    = d.seq;
		shm.modFrames     = m_frames;
		shm.modMouseLeftX = 0;
		shm.modMouseLeftY = 0;
		return f;
	}

	f.channels = d.channels;
	f.keys     = d.keys;
	std::memcpy(f.pad, d.pad, sizeof(f.pad));

	if (d.channels & CHANNEL_MOUSE) {
		// A new budget replaces what's left rather than adding to it, so a
		// driver cancels a pan by publishing zero with a fresh seq.
		if (d.mouseSeq != m_mouseSeqSeen) {
			m_mouseSeqSeen = d.mouseSeq;
			m_mouseLeftX   = d.mouseBudgetX;
			m_mouseLeftY   = d.mouseBudgetY;
		}

		f.mouseDx = DrainStep(m_mouseLeftX, d.mouseStepMax);
		f.mouseDy = DrainStep(m_mouseLeftY, d.mouseStepMax);
		m_mouseLeftX -= f.mouseDx;
		m_mouseLeftY -= f.mouseDy;
	}

	++m_applied;

	shm.modStatus     = d.status;
	shm.modSeqSeen    = d.seq;
	shm.modFrames     = m_frames;
	shm.modApplied    = m_applied;
	shm.modMouseLeftX = m_mouseLeftX;
	shm.modMouseLeftY = m_mouseLeftY;
	return f;
}

} // namespace agentpad
