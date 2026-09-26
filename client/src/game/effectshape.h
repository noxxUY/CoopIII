// How much of what the owner's mission does goes out, and when (docs/protocol.md
// 1.29). Pure, for tools/clienttest; game/mission.cpp feeds it.
//
// A mission is a script, and a script says the same thing over and over: a
// loop that runs every frame repeats every instruction in it every frame.
// S.A.M. is the case that showed it (73_asusb3.sc, LOOP_AS3_2): every frame it
// takes its plane's radar blip off, puts a new one where the plane is now, and
// makes that one bigger. Each of the three went to every participant as a
// reliable packet of its own, sixty times a second, for a blip that moved a
// few metres.
//
// Two things stand between the mission and the wire:
//
//   - **BlipAliases.** A blip taken off and put back by the same kind of
//     instruction within a frame is the same blip, moved. Everybody keeps
//     knowing it by the owner's first handle for it, whatever handle the
//     owner's engine hands out now; it is put back on their radar at most
//     every BLIP_MOVE_MS, where it is by then, with whatever was done to it
//     done again; and when it goes for good it goes. A blip put back where it
//     already was sends nothing at all.
//   - **EffectShaper.** An instruction that sets something, where saying it
//     twice changes nothing, is dropped when it says what was said last, but
//     for once every STATE_REFRESH_MS: a participant's own engine can undo a
//     setting on its own (a cutscene's end takes the widescreen bars off, a
//     respawn gives the controls back), and a mission that keeps saying it
//     means it. One that says something new goes at most every STATE_GAP_MS
//     for the same thing, the newest kept until the gap is up. The words on
//     the screen are said again only when they would otherwise run out.
//     Nothing that makes, takes away, pays or happens once is ever held back.
//
// Neither loses the last word: whatever is held goes when its time comes, and
// all of it when the mission ends (Flush).
#pragma once

#include "replay.h"

#include <coopiii/protocol.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace coopiii::game::shape {

// A blip moved goes to everybody at most this often.
constexpr uint32_t BLIP_MOVE_MS = 500;
// The same thing set again, to something new, at most this often; set again
// the same, at most this often.
constexpr uint32_t STATE_GAP_MS     = 100;
constexpr uint32_t STATE_REFRESH_MS = 1000;
// Words said again the same are sent again only this often at most, and no
// sooner than half the time they stay up for; never more rarely than
// PRINT_REFRESH_MAX_MS.
constexpr uint32_t PRINT_REFRESH_MIN_MS = 100;
constexpr uint32_t PRINT_REFRESH_MAX_MS = 2000;

// The help box, which starts over whenever it is said, so is never said again
// the same.
constexpr uint16_t PRINT_HELP = 0x03E5;

constexpr size_t MAX_BLIP_ALIASES = 32;
constexpr size_t MAX_SHAPE_SLOTS  = 64;
constexpr size_t MAX_BLIP_MODS    = 4;

inline uint16_t OpcodeOf(const MissionEffectBody &b) {
	return b.length >= 2 ? static_cast<uint16_t>(b.code[0] | (b.code[1] << 8)) : 0;
}

inline bool SameCode(const MissionEffectBody &a, const MissionEffectBody &b) {
	return a.length == b.length && a.kind == b.kind && std::memcmp(a.code, b.code, a.length) == 0;
}

// ---- what an instruction is, said again ---------------------------------------------

enum class Repeat : uint8_t {
	Pass,         // goes as it is, every time
	State,        // sets something: a repeat of the last is dropped, changes are paced
	Print,        // words on the screen: a repeat goes when they would run out
	Remove,       // takes a thing away: whatever is held about it goes with it
	ClearPrints,  // takes the words away: every print is said afresh after it
};

struct RepeatRule {
	Repeat   rule     = Repeat::Pass;
	uint8_t  identity = 0;        // how many leading operands say what it is about
	uint16_t family   = 0;        // instructions that set the same thing share one
	int8_t   duration = -1;       // a print's operand that says how long it stays
	uint16_t forgets  = 0;        // a family whatever this is makes the engine forget
};

inline RepeatRule RuleOf(uint16_t opcode) {
	auto s = [&](uint8_t identity, uint16_t family = 0) {
		RepeatRule r;
		r.rule     = Repeat::State;
		r.identity = identity;
		r.family   = family ? family : opcode;
		return r;
	};
	// A print replaces what its own kind of print shows, whatever label that
	// was, so each kind is one thing on the screen.
	auto p = [&](int8_t duration) {
		RepeatRule r;
		r.rule     = Repeat::Print;
		r.identity = 0;
		r.family   = opcode;
		r.duration = duration;
		return r;
	};
	auto rm = [&]() {
		RepeatRule r;
		r.rule     = Repeat::Remove;
		r.identity = 1;
		r.family   = opcode;
		return r;
	};
	switch (opcode) {
	// A blip's colour, brightness, size and what it shows on.
	case 0x0165: case 0x0166: case 0x0168: case 0x018B: return s(1);
	// The HUD's widgets, and the timer held still.
	case 0x014E: case 0x0150: case 0x03C4: return s(1);
	case 0x0396: return s(0);
	// The player's controls, the screen's bars, a HUD item flashing, being
	// seen or left alone, never tired, the brakes, the bomb shop, where the
	// next restart is, the player's heading, health, ammo and weapon.
	case 0x01B4: case 0x0336: case 0x03BF: case 0x01F7: case 0x0330: case 0x0221: return s(1);
	case 0x02A3: case 0x03E7: case 0x021D: return s(0);
	// And what a mission sets for a while (replay.h): the free resprays, the
	// police's eye for crime, the restart levels, the world held for a
	// cutscene, every car unhurt, traffic round the camera, the near clip and
	// the music through a fade.
	case 0x0335: case 0x03C7: case 0x041F: case 0x0420: case 0x03B7: case 0x03F4: case 0x03EA:
	case 0x041D: case 0x043C:
		return s(0);
	case 0x016E: case 0x01F6: return s(0, 0x016E);
	case 0x0171: case 0x0222: case 0x01B8: return s(1);
	case 0x017A: return s(2);
	// An object put somewhere, turned, made solid or loose, flashing, pushed,
	// or placed on a car. SLIDE_OBJECT and ROTATE_OBJECT step it each time and
	// are not repeats of anything.
	case 0x01BC: case 0x0177: case 0x0382: case 0x0392: case 0x0381: case 0x0240: case 0x035C:
		return s(1);
	// The streets, the zones, the gangs, and where the mission's line is said,
	// which a line loaded or cleared forgets (cAudioManager::PreloadMissionAudio
	// puts it back in the ear).
	case 0x03DE: case 0x01EB: case 0x03D7: return s(0);
	case 0x03CF: case 0x040D: {
		RepeatRule r;
		r.forgets = 0x03D7;
		return r;
	}
	case 0x0152: case 0x015C: return s(2);
	case 0x0237: return s(1);
	// A gang's threat on or off: the two are one setting, the type and the
	// threat.
	case 0x03F1: case 0x03F2: return s(2, 0x03F1);
	// A garage open or shut.
	case 0x0360: case 0x0361: return s(1, 0x0360);
	// The words: how long each stays is the operand after the label and its
	// numbers. The help box starts again when it is said again, so it is
	// never said again the same.
	case 0x00BA: case 0x00BB: case 0x00BC: case 0x00BD: return p(1);
	case 0x01E3: case 0x01E4: case 0x01E5: return p(2);
	case 0x02FC: case 0x02FD: case 0x036D: case 0x02FE: return p(3);
	case 0x0384: return p(2);
	case PRINT_HELP: return s(1);
	// What takes a thing away.
	case 0x0164: case 0x0108: case 0x01C4: case 0x014F: case 0x0151: case 0x03BD: case 0x0215:
	case 0x02D1:
		return rm();
	// And what takes words away: whatever is said next is said afresh.
	case 0x00BE: case 0x03EB: case 0x03E6: case 0x03D5: case 0x03D6: {
		RepeatRule r;
		r.rule   = Repeat::ClearPrints;
		r.family = opcode;
		return r;
	}
	default: return RepeatRule{};
	}
}

// FNV-1a over the operands an instruction is about, or 0 for none. The same
// thing named by any instruction hashes the same: a blip's `01` and four
// bytes, a global's `02 lo hi`, a label's eight.
inline uint32_t SubjectOf(const MissionEffectBody &b, uint8_t identity) {
	const replay::Entry *e = replay::Find(OpcodeOf(b));
	if (!e || identity == 0 || identity > e->count)
		return 0;
	size_t end = 2;
	for (uint8_t i = 0; i < identity; ++i)
		end += replay::EncodedSize(e->args[i]);
	if (end > b.length)
		return 0;
	uint32_t h = 2166136261u;
	for (size_t i = 2; i < end; ++i) {
		h ^= b.code[i];
		h *= 16777619u;
	}
	return h == 0 ? 1 : h;
}

// The thing an instruction that is not a repeat of anything is about, for
// whatever is held about it to go first: its first operand when that names
// something.
inline uint32_t PassSubjectOf(const MissionEffectBody &b) {
	const replay::Entry *e = replay::Find(OpcodeOf(b));
	if (!e || e->count == 0)
		return 0;
	switch (e->args[0]) {
	case replay::Arg::Blip:
	case replay::Arg::Object:
	case replay::Arg::Char:
	case replay::Arg::Car:
	case replay::Arg::Pickup:
	case replay::Arg::Fire:
	case replay::Arg::Sphere:
	case replay::Arg::ObjGlobal:
	case replay::Arg::Global:
		return SubjectOf(b, 1);
	default:
		return 0;
	}
}

// How long a print held back may wait before it is said again the same.
inline uint32_t PrintRefreshMs(const MissionEffectBody &b, const RepeatRule &r) {
	int32_t ms = 0;
	if (r.duration < 0 || !replay::LiteralAt(b.code, b.length, static_cast<uint8_t>(r.duration), &ms) ||
	    ms <= 0)
		return PRINT_REFRESH_MAX_MS;
	const uint32_t half = static_cast<uint32_t>(ms) / 2;
	return half < PRINT_REFRESH_MIN_MS ? PRINT_REFRESH_MIN_MS
	       : half > PRINT_REFRESH_MAX_MS ? PRINT_REFRESH_MAX_MS
	                                     : half;
}

// ---- the shaper ---------------------------------------------------------------------

class EffectShaper {
public:
	// One effect of the mission's, in the order it ran. `send(body)` for what
	// goes now.
	template <class Send>
	void Offer(const MissionEffectBody &b, uint32_t nowMs, Send send) {
		const RepeatRule r = b.onlyTo == 0 && (b.kind == MISSION_EFFECT_RUN || b.kind == MISSION_EFFECT_BLIP_USE)
		                         ? RuleOf(OpcodeOf(b))
		                         : RepeatRule{};
		switch (r.rule) {
		case Repeat::Pass: {
			if (r.forgets != 0)
				ForgetFamily(r.forgets);
			const uint32_t subject = PassSubjectOf(b);
			if (subject != 0)
				FlushSubject(subject, nowMs, send);
			send(b);
			return;
		}
		case Repeat::Remove:
			Forget(SubjectOf(b, r.identity));
			send(b);
			return;
		case Repeat::ClearPrints:
			ForgetPrints();
			send(b);
			return;
		case Repeat::State:
		case Repeat::Print: break;
		}
		const uint32_t subject = SubjectOf(b, r.identity);
		Slot          *slot    = Find(r.family, subject);
		if (!slot) {
			slot = Take();
			if (!slot) {
				send(b);   // no room to remember it: it goes, as it always did
				return;
			}
			*slot         = Slot{};
			slot->used    = true;
			slot->family  = r.family;
			slot->subject = subject;
			slot->print   = r.rule == Repeat::Print;
			slot->sent    = b;
			slot->sentMs  = nowMs;
			slot->refresh = r.rule == Repeat::Print ? PrintRefreshMs(b, r)
			                : r.family == PRINT_HELP  ? UINT32_MAX
			                                          : STATE_REFRESH_MS;
			send(b);
			return;
		}
		if (SameCode(b, slot->sent) && nowMs - slot->sentMs < slot->refresh) {
			slot->pending = false;   // the newest is what everybody has
			++m_dropped;
			return;
		}
		if (nowMs - slot->sentMs >= STATE_GAP_MS) {
			slot->sent    = b;
			slot->sentMs  = nowMs;
			slot->pending = false;
			if (slot->print)
				slot->refresh = PrintRefreshMs(b, r);
			send(b);
			return;
		}
		if (!slot->pending)
			++m_held;
		slot->pending   = true;
		slot->held      = b;
		slot->heldOrder = ++m_order;
	}

	// Once a frame: whatever has waited out its gap goes, oldest first.
	template <class Send>
	void Tick(uint32_t nowMs, Send send) {
		for (;;) {
			Slot *next = nullptr;
			for (Slot &s : m_slots)
				if (s.used && s.pending && nowMs - s.sentMs >= STATE_GAP_MS &&
				    (!next || s.heldOrder < next->heldOrder))
					next = &s;
			if (!next)
				return;
			Release(*next, nowMs, send);
		}
	}

	// The mission is over: everything held goes, in the order it came.
	template <class Send>
	void Flush(uint32_t nowMs, Send send) {
		for (;;) {
			Slot *next = nullptr;
			for (Slot &s : m_slots)
				if (s.used && s.pending && (!next || s.heldOrder < next->heldOrder))
					next = &s;
			if (!next)
				return;
			Release(*next, nowMs, send);
		}
	}

	void Clear() {
		for (Slot &s : m_slots)
			s = Slot{};
		m_dropped = 0;
		m_held    = 0;
	}

	uint32_t Dropped() const { return m_dropped; }
	uint32_t Held() const { return m_held; }
	size_t   Pending() const {
		size_t n = 0;
		for (const Slot &s : m_slots)
			n += s.used && s.pending ? 1 : 0;
		return n;
	}

private:
	struct Slot {
		bool              used    = false;
		bool              print   = false;
		bool              pending = false;
		uint16_t          family  = 0;
		uint32_t          subject = 0;
		uint32_t          sentMs  = 0;
		uint32_t          refresh = 0;
		uint32_t          heldOrder = 0;
		MissionEffectBody sent{};
		MissionEffectBody held{};
	};

	template <class Send>
	void Release(Slot &s, uint32_t nowMs, Send send) {
		s.pending = false;
		s.sent    = s.held;
		s.sentMs  = nowMs;
		send(s.held);
	}

	template <class Send>
	void FlushSubject(uint32_t subject, uint32_t nowMs, Send send) {
		for (;;) {
			Slot *next = nullptr;
			for (Slot &s : m_slots)
				if (s.used && s.pending && s.subject == subject && (!next || s.heldOrder < next->heldOrder))
					next = &s;
			if (!next)
				return;
			Release(*next, nowMs, send);
		}
	}

	// Whatever is held or remembered about `subject`: taken away, so what is
	// said about it next goes.
	void Forget(uint32_t subject) {
		if (subject == 0)
			return;
		for (Slot &s : m_slots)
			if (s.used && s.subject == subject)
				s = Slot{};
	}

	void ForgetFamily(uint16_t family) {
		for (Slot &s : m_slots)
			if (s.used && s.family == family)
				s = Slot{};
	}

	void ForgetPrints() {
		for (Slot &s : m_slots)
			if (s.used && (s.print || s.family == PRINT_HELP))
				s = Slot{};
	}

	Slot *Find(uint16_t family, uint32_t subject) {
		for (Slot &s : m_slots)
			if (s.used && s.family == family && s.subject == subject)
				return &s;
		return nullptr;
	}

	// A free slot, or the one said longest ago with nothing held.
	Slot *Take() {
		Slot *oldest = nullptr;
		for (Slot &s : m_slots) {
			if (!s.used)
				return &s;
			if (!s.pending && (!oldest || s.sentMs < oldest->sentMs))
				oldest = &s;
		}
		return oldest;
	}

	Slot     m_slots[MAX_SHAPE_SLOTS];
	uint32_t m_order   = 0;
	uint32_t m_dropped = 0;
	uint32_t m_held    = 0;
};

// ---- a blip taken off and put back --------------------------------------------------

// The blips that are made by an instruction naming a car, a char or an object:
// put back for the same one, or it is another blip.
inline bool AddNamesEntity(uint16_t opcode) {
	return opcode == 0x0186 || opcode == 0x0187 || opcode == 0x0188 || opcode == 0x0162;
}

class BlipAliases {
public:
	// One of the mission's blip instructions, in the frame `frame`. What goes
	// on goes to `send`, which is where the shaper takes over.
	template <class Send>
	void Offer(const MissionEffectBody &b, uint32_t frame, uint32_t nowMs, Send send) {
		if (b.onlyTo != 0) {
			send(b);
			return;
		}
		if (b.kind == MISSION_EFFECT_BLIP_NEW) {
			Alias *a = PutBack(b, frame);
			if (!a) {
				Alias *fresh = Take();
				if (fresh) {
					*fresh         = Alias{};
					fresh->used    = true;
					fresh->wire    = b.ownerBlip;
					fresh->current = b.ownerBlip;
					fresh->added   = b;
					fresh->addedMs = nowMs;
				}
				send(b);
				return;
			}
			MissionEffectBody add = b;
			add.ownerBlip         = a->wire;
			a->current            = b.ownerBlip;
			a->removing           = false;
			if (SameCode(add, a->added)) {
				a->pending = false;   // back where everybody has it
				++m_kept;
				return;
			}
			if (nowMs - a->addedMs >= BLIP_MOVE_MS) {
				Readd(*a, add, nowMs, send);
				return;
			}
			a->pending    = true;
			a->pendingAdd = add;
			++m_kept;
			return;
		}
		if (b.kind != MISSION_EFFECT_BLIP_USE) {
			send(b);
			return;
		}
		Alias *a = Current(b.ownerBlip);
		if (!a) {
			send(b);
			return;
		}
		MissionEffectBody wire = b;
		wire.ownerBlip         = a->wire;
		if (wire.handleAt != 0xFF && wire.handleAt + 4u <= wire.length)
			std::memcpy(wire.code + wire.handleAt, &a->wire, 4);
		if (OpcodeOf(b) == 0x0164) {
			// Taken off: gone for good unless it is back by the next frame.
			a->removing  = true;
			a->removedAt = frame;
			a->remove    = wire;
			return;
		}
		KeepMod(*a, wire);
		send(wire);
	}

	// Once a frame, before the scripts run: a blip taken off a frame ago and
	// not put back is gone; one moved goes where it is now once its time is up.
	template <class Send>
	void Tick(uint32_t frame, uint32_t nowMs, Send send) {
		for (Alias &a : m_aliases) {
			if (!a.used)
				continue;
			if (a.removing && frame - a.removedAt >= 2) {
				send(a.remove);
				a = Alias{};
				continue;
			}
			if (!a.removing && a.pending && nowMs - a.addedMs >= BLIP_MOVE_MS)
				Readd(a, a.pendingAdd, nowMs, send);
		}
	}

	// The mission is over: what was taken off goes, and what moved is put
	// where it ended up.
	template <class Send>
	void Flush(uint32_t nowMs, Send send) {
		for (Alias &a : m_aliases) {
			if (!a.used)
				continue;
			if (a.removing)
				send(a.remove);
			else if (a.pending)
				Readd(a, a.pendingAdd, nowMs, send);
			a = Alias{};
		}
	}

	void Clear() {
		for (Alias &a : m_aliases)
			a = Alias{};
		m_kept = 0;
	}

	// The handle everybody knows the owner's blip `current` by.
	int32_t WireOf(int32_t current) const {
		for (const Alias &a : m_aliases)
			if (a.used && a.current == current)
				return a.wire;
		return current;
	}

	uint32_t Kept() const { return m_kept; }

private:
	struct Alias {
		bool              used      = false;
		bool              removing  = false;
		bool              pending   = false;
		int32_t           wire      = -1;
		int32_t           current   = -1;
		uint32_t          removedAt = 0;
		uint32_t          addedMs   = 0;
		MissionEffectBody added{};
		MissionEffectBody pendingAdd{};
		MissionEffectBody remove{};
		MissionEffectBody mods[MAX_BLIP_MODS]{};
		uint8_t           modCount  = 0;
	};

	// The blip taken off in this frame or the last that `add` puts back: made
	// by the same instruction, and for the same car, char or object.
	Alias *PutBack(const MissionEffectBody &add, uint32_t frame) {
		const uint16_t opcode = OpcodeOf(add);
		for (Alias &a : m_aliases) {
			if (!a.used || !a.removing || frame - a.removedAt > 1 || OpcodeOf(a.added) != opcode)
				continue;
			if (AddNamesEntity(opcode) &&
			    (add.length < 7 || a.added.length < 7 || std::memcmp(add.code + 2, a.added.code + 2, 5) != 0))
				continue;
			return &a;
		}
		return nullptr;
	}

	Alias *Current(int32_t owner) {
		for (Alias &a : m_aliases)
			if (a.used && a.current == owner)
				return &a;
		return nullptr;
	}

	Alias *Take() {
		for (Alias &a : m_aliases)
			if (!a.used)
				return &a;
		return nullptr;
	}

	void KeepMod(Alias &a, const MissionEffectBody &mod) {
		for (uint8_t i = 0; i < a.modCount; ++i)
			if (OpcodeOf(a.mods[i]) == OpcodeOf(mod)) {
				a.mods[i] = mod;
				return;
			}
		if (a.modCount < MAX_BLIP_MODS)
			a.mods[a.modCount++] = mod;
	}

	// Off everybody's radar and back on where it is now, with what was done to
	// it done again.
	template <class Send>
	void Readd(Alias &a, const MissionEffectBody &add, uint32_t nowMs, Send send) {
		MissionEffectBody off = a.remove;
		if (OpcodeOf(off) != 0x0164) {
			// Taken off and put back in the same breath as it was made: the
			// removal to send is built from the blip's own handle.
			off                = MissionEffectBody{};
			off.missionNumber  = add.missionNumber;
			off.kind           = MISSION_EFFECT_BLIP_USE;
			off.handleAt       = 3;
			off.ownerBlip      = a.wire;
			off.length         = 7;
			off.code[0]        = 0x64;
			off.code[1]        = 0x01;
			off.code[2]        = scripts::PARAM_INT32;
			std::memcpy(off.code + 3, &a.wire, 4);
		}
		send(off);
		send(add);
		for (uint8_t i = 0; i < a.modCount; ++i)
			send(a.mods[i]);
		a.added   = add;
		a.addedMs = nowMs;
		a.pending = false;
	}

	Alias    m_aliases[MAX_BLIP_ALIASES];
	uint32_t m_kept = 0;
};

// ---- a corona the mission draws every frame ----------------------------------------
//
// DRAW_CORONA (024F) is one frame of a corona: the checkpoints of the 4x4 runs
// and the Mayhem are drawn by the script asking again every frame, as a blue
// marker is (mission.h, "the blue markers"), and it goes the same way. The
// owner's machine says when one goes up, again every CORONA_RESEND_MS while it
// stays, when it moves or changes, and when it has not been drawn for
// CORONA_GONE_MS. It goes as DRAW_CORONA with its nine operands as literals and
// a tenth, 1 for up and 0 for down; the owner's id for it (where in the script
// it is drawn) in ownerBlip. A participant never runs it: DrawMissionMarkers
// registers it with the engine every frame until it comes down.
//
// DRAW_LIGHT (0250) and DRAW_SHADOW (016F) are one frame each the same way,
// and go the same way: Chaperone's club, lit red with a shadow on its floor
// for as long as the scene runs. Each goes as its own instruction, its
// operands in their own order and the up-or-down flag after them, and a
// participant runs it through its own interpreter every frame it is up,
// since a light or a shadow is added fresh each frame too.
struct CoronaDraw {
	uint16_t opcode = 0x024F;   // DRAW_CORONA, DRAW_LIGHT or DRAW_SHADOW
	float    x = 0.0f, y = 0.0f, z = 0.0f, size = 0.0f;   // a shadow's size is its length
	float    angle = 0.0f;                                // a shadow's
	int32_t  type = 0, flare = 0, r = 0, g = 0, b = 0;    // a shadow's flare is its intensity
};

constexpr uint16_t DRAW_LIGHT  = 0x0250;
constexpr uint16_t DRAW_SHADOW = 0x016F;

constexpr uint16_t DRAW_CORONA          = 0x024F;
constexpr uint32_t CORONA_GONE_MS       = 400;
constexpr uint32_t CORONA_RESEND_MS     = 2000;
constexpr uint32_t CORONA_FORGET_MS     = 3 * CORONA_RESEND_MS;
constexpr uint32_t CORONA_MOVE_MS       = 250;
constexpr float    CORONA_MOVE_M        = 0.5f;
constexpr size_t   MAX_CORONAS          = 16;
constexpr size_t   CORONA_OPERANDS      = 10;   // DRAW_CORONA's nine, and up
constexpr size_t   CORONA_EFFECT_LENGTH = 2 + CORONA_OPERANDS * 5;
constexpr size_t   FRAME_DRAW_MAX       = 10;   // DRAW_SHADOW's, the most of the three

// How many operands the instruction has: 9, 6, 10, or 0 for one that is not
// drawn a frame at a time.
inline size_t FrameDrawOperands(uint16_t opcode) {
	switch (opcode) {
	case DRAW_CORONA: return 9;
	case DRAW_LIGHT:  return 6;
	case DRAW_SHADOW: return 10;
	default:          return 0;
	}
}

inline int32_t FloatBits(float f) {
	int32_t v = 0;
	std::memcpy(&v, &f, 4);
	return v;
}

inline float BitsFloat(int32_t v) {
	float f = 0.0f;
	std::memcpy(&f, &v, 4);
	return f;
}

// The operands as the instruction takes them, in its own order. How many.
inline size_t FrameDrawValues(const CoronaDraw &c, int32_t (&v)[FRAME_DRAW_MAX]) {
	switch (c.opcode) {
	case DRAW_CORONA:   // x y z size type flare r g b
		v[0] = FloatBits(c.x), v[1] = FloatBits(c.y), v[2] = FloatBits(c.z), v[3] = FloatBits(c.size);
		v[4] = c.type, v[5] = c.flare, v[6] = c.r, v[7] = c.g, v[8] = c.b;
		return 9;
	case DRAW_LIGHT:    // x y z r g b
		v[0] = FloatBits(c.x), v[1] = FloatBits(c.y), v[2] = FloatBits(c.z);
		v[3] = c.r, v[4] = c.g, v[5] = c.b;
		return 6;
	case DRAW_SHADOW:   // type x y z angle length intensity r g b
		v[0] = c.type, v[1] = FloatBits(c.x), v[2] = FloatBits(c.y), v[3] = FloatBits(c.z);
		v[4] = FloatBits(c.angle), v[5] = FloatBits(c.size), v[6] = c.flare;
		v[7] = c.r, v[8] = c.g, v[9] = c.b;
		return 10;
	default:
		return 0;
	}
}

// And back. False for an opcode that is none of the three, too few values, or
// a position or size that is no number.
inline bool FrameDrawFrom(uint16_t opcode, const int32_t *v, size_t count, CoronaDraw *out) {
	if (FrameDrawOperands(opcode) == 0 || count < FrameDrawOperands(opcode))
		return false;
	CoronaDraw c;
	c.opcode = opcode;
	switch (opcode) {
	case DRAW_CORONA:
		c.x = BitsFloat(v[0]), c.y = BitsFloat(v[1]), c.z = BitsFloat(v[2]), c.size = BitsFloat(v[3]);
		c.type = v[4], c.flare = v[5], c.r = v[6], c.g = v[7], c.b = v[8];
		break;
	case DRAW_LIGHT:
		c.x = BitsFloat(v[0]), c.y = BitsFloat(v[1]), c.z = BitsFloat(v[2]);
		c.r = v[3], c.g = v[4], c.b = v[5];
		break;
	default:
		c.type = v[0], c.x = BitsFloat(v[1]), c.y = BitsFloat(v[2]), c.z = BitsFloat(v[3]);
		c.angle = BitsFloat(v[4]), c.size = BitsFloat(v[5]), c.flare = v[6];
		c.r = v[7], c.g = v[8], c.b = v[9];
		break;
	}
	if (!std::isfinite(c.x) || !std::isfinite(c.y) || !std::isfinite(c.z) || !std::isfinite(c.size) ||
	    !std::isfinite(c.angle))
		return false;
	*out = c;
	return true;
}

inline bool CoronaChanged(const CoronaDraw &a, const CoronaDraw &b) {
	return a.opcode != b.opcode || std::fabs(a.x - b.x) > CORONA_MOVE_M ||
	       std::fabs(a.y - b.y) > CORONA_MOVE_M || std::fabs(a.z - b.z) > CORONA_MOVE_M ||
	       std::fabs(a.size - b.size) > 0.05f || std::fabs(a.angle - b.angle) > 0.05f ||
	       a.type != b.type || a.flare != b.flare || a.r != b.r || a.g != b.g || a.b != b.b;
}

// The instruction itself, as a participant runs it: its operands as
// literals. How long, 0 for none of the three.
inline size_t FrameDrawCode(const CoronaDraw &c, uint8_t *code, size_t room) {
	int32_t      v[FRAME_DRAW_MAX];
	const size_t n = FrameDrawValues(c, v);
	if (n == 0 || room < 2 + n * 5)
		return 0;
	code[0]     = static_cast<uint8_t>(c.opcode & 0xFF);
	code[1]     = static_cast<uint8_t>(c.opcode >> 8);
	size_t at   = 2;
	for (size_t i = 0; i < n; ++i) {
		code[at++] = scripts::PARAM_INT32;
		std::memcpy(code + at, &v[i], 4);
		at += 4;
	}
	return at;
}

inline MissionEffectBody CoronaEffect(uint16_t missionNumber, uint32_t id, const CoronaDraw &c, bool up) {
	MissionEffectBody e{};
	e.missionNumber = missionNumber;
	e.kind          = MISSION_EFFECT_RUN;
	e.handleAt      = 0xFF;
	e.ownerBlip     = static_cast<int32_t>(id);
	size_t n        = FrameDrawCode(c, e.code, sizeof e.code - 5);
	if (n == 0)
		return MissionEffectBody{};
	const int32_t flag = up ? 1 : 0;
	e.code[n++]        = scripts::PARAM_INT32;
	std::memcpy(e.code + n, &flag, 4);
	n += 4;
	e.length = static_cast<uint8_t>(n);
	return e;
}

inline bool ReadCoronaEffect(const MissionEffectBody &e, uint32_t *id, CoronaDraw *c, bool *up) {
	const uint16_t opcode = OpcodeOf(e);
	const size_t   count  = FrameDrawOperands(opcode);
	if (e.kind != MISSION_EFFECT_RUN || count == 0 || e.length != 2 + (count + 1) * 5)
		return false;
	int32_t values[FRAME_DRAW_MAX + 1];
	for (size_t i = 0; i <= count; ++i) {
		if (e.code[2 + i * 5] != scripts::PARAM_INT32)
			return false;
		std::memcpy(&values[i], e.code + 3 + i * 5, 4);
	}
	CoronaDraw d;
	if (!FrameDrawFrom(opcode, values, count, &d))
		return false;
	*id = static_cast<uint32_t>(e.ownerBlip);
	*c  = d;
	*up = values[count] != 0;
	return true;
}

// The owner's side: what its mission drew, and what everybody has been told.
class OwnCoronas {
public:
	bool Drawn(uint32_t id, const CoronaDraw &c, uint32_t nowMs) {
		for (size_t i = 0; i < m_count; ++i)
			if (m_marks[i].id == id) {
				m_marks[i].now     = c;
				m_marks[i].drawnMs = nowMs;
				return true;
			}
		if (m_count == MAX_CORONAS)
			return false;
		m_marks[m_count++] = Mark{id, c, c, nowMs, 0, false};
		return true;
	}

	// `tell(id, corona, up, first)` for what everybody has to hear now.
	template <class Tell>
	void Tick(uint32_t nowMs, Tell tell) {
		size_t kept = 0;
		for (size_t i = 0; i < m_count; ++i) {
			Mark &m = m_marks[i];
			if (nowMs - m.drawnMs >= CORONA_GONE_MS) {
				if (m.told)
					tell(m.id, m.sent, false, false);
				continue;
			}
			const bool first = !m.told;
			if (first || nowMs - m.sentMs >= CORONA_RESEND_MS ||
			    (CoronaChanged(m.now, m.sent) && nowMs - m.sentMs >= CORONA_MOVE_MS)) {
				m.told   = true;
				m.sent   = m.now;
				m.sentMs = nowMs;
				tell(m.id, m.sent, true, first);
			}
			m_marks[kept++] = m;
		}
		m_count = kept;
	}

	size_t Count() const { return m_count; }
	void   Clear() { m_count = 0; }

private:
	struct Mark {
		uint32_t   id = 0;
		CoronaDraw now, sent;
		uint32_t   drawnMs = 0, sentMs = 0;
		bool       told    = false;
	};
	Mark   m_marks[MAX_CORONAS];
	size_t m_count = 0;
};

// A participant's side: the owner's coronas this machine draws.
class ShownCoronas {
public:
	bool Heard(uint32_t id, const CoronaDraw &c, bool up, uint32_t nowMs) {
		for (size_t i = 0; i < m_count; ++i) {
			if (m_marks[i].id != id)
				continue;
			if (up)
				m_marks[i] = Mark{id, c, nowMs};
			else
				m_marks[i] = m_marks[--m_count];
			return true;
		}
		if (!up)
			return true;
		if (m_count == MAX_CORONAS)
			return false;
		m_marks[m_count++] = Mark{id, c, nowMs};
		return true;
	}

	bool Has(uint32_t id) const {
		for (size_t i = 0; i < m_count; ++i)
			if (m_marks[i].id == id)
				return true;
		return false;
	}

	template <class Draw>
	void Each(uint32_t nowMs, Draw draw) {
		size_t kept = 0;
		for (size_t i = 0; i < m_count; ++i) {
			if (nowMs - m_marks[i].heardMs >= CORONA_FORGET_MS)
				continue;
			m_marks[kept++] = m_marks[i];
			draw(m_marks[i].id, m_marks[i].corona);
		}
		m_count = kept;
	}

	size_t Count() const { return m_count; }
	void   Clear() { m_count = 0; }

private:
	struct Mark {
		uint32_t   id = 0;
		CoronaDraw corona;
		uint32_t   heardMs = 0;
	};
	Mark   m_marks[MAX_CORONAS];
	size_t m_count = 0;
};

} // namespace coopiii::game::shape
