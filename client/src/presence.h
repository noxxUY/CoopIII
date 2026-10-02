// Discord Rich Presence: "Playing CoopIII" on the player's profile instead of
// the "Grand Theft Auto III" Discord finds by itself.
//
// Discord's desktop app listens on a named pipe, \\.\pipe\discord-ipc-0 to -9.
// Every message either way is a frame: a little-endian u32 opcode, a
// little-endian u32 length and that many bytes of JSON. The client opens with
// a handshake frame (op 0) naming the application, Discord answers with a
// READY dispatch (op 1), and from then on a SET_ACTIVITY command (op 1) sets
// what the profile says. The command carries the pid of the game, so Discord
// puts our activity in place of the one it detected for gta3.exe; this runs
// inside gta3.exe, so that is our own pid. Closing the pipe clears it.
//
// Everything that decides what is sent is in this header, so clienttest
// checks it without a pipe. presence.cpp is the thread that sends it: it
// never touches the game, and the game thread hands it an Activity through
// Update(), once a second at most.
//
// What is shown is deliberately little: co-op or not, the session mission's
// title or free roam with how many are in the session out of how many fit
// ("Free roam (2/8)"), and since when. No server address and no names.
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

namespace coopiii::presence {

// ---- the frames -------------------------------------------------------------------

constexpr uint32_t OP_HANDSHAKE = 0;
constexpr uint32_t OP_FRAME     = 1;
constexpr uint32_t OP_CLOSE     = 2;
constexpr uint32_t OP_PING      = 3;
constexpr uint32_t OP_PONG      = 4;

constexpr uint32_t FRAME_HEADER = 8;
// Nothing Discord sends us is anywhere near this; a length past it means the
// stream is not what we think it is.
constexpr uint32_t FRAME_MAX = 64 * 1024;

// How long to wait before looking for Discord again when it is not running or
// the pipe broke, and the shortest gap between two activities sent. Discord
// takes five updates in twenty seconds; nothing here changes that often.
constexpr uint32_t RETRY_MS   = 15000;
constexpr uint32_t MIN_GAP_MS = 15000;

// Discord refuses a details or state line over 128 characters.
constexpr size_t TEXT_MAX = 128;

inline void PutU32(std::string &out, uint32_t v) {
	for (int i = 0; i < 4; ++i)
		out.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}

inline uint32_t GetU32(const uint8_t *p) {
	return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
	       (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

inline std::string Frame(uint32_t op, const std::string &json) {
	std::string out;
	out.reserve(FRAME_HEADER + json.size());
	PutU32(out, op);
	PutU32(out, static_cast<uint32_t>(json.size()));
	out += json;
	return out;
}

// The opcode and length out of a frame's first eight bytes. False when the
// length is one no real frame has.
inline bool ParseHeader(const uint8_t *header, uint32_t *op, uint32_t *length) {
	*op     = GetU32(header);
	*length = GetU32(header + 4);
	return *length <= FRAME_MAX;
}

// ---- the JSON ---------------------------------------------------------------------

// How many bytes the UTF-8 sequence at `p` takes, or 0 when it is not one.
inline size_t Utf8Length(const unsigned char *p, size_t left) {
	const unsigned char c = p[0];
	size_t n = c >= 0xF0 && c <= 0xF4 ? 4 : c >= 0xE0 ? 3 : c >= 0xC2 && c <= 0xDF ? 2 : 0;
	if (c >= 0xF5 || n == 0 || n > left)
		return 0;
	for (size_t i = 1; i < n; ++i)
		if ((p[i] & 0xC0) != 0x80)
			return 0;
	return n;
}

// A JSON string's contents. Text that is not UTF-8 becomes '?', since Discord
// refuses the whole command over one bad byte.
inline std::string JsonEscape(const std::string &in) {
	std::string out;
	out.reserve(in.size());
	const unsigned char *p = reinterpret_cast<const unsigned char *>(in.data());
	for (size_t i = 0; i < in.size();) {
		const unsigned char c = p[i];
		if (c == '"' || c == '\\') {
			out.push_back('\\');
			out.push_back(static_cast<char>(c));
		} else if (c < 0x20 || c == 0x7F) {
			char buf[8];
			std::snprintf(buf, sizeof buf, "\\u%04x", c);
			out += buf;
		} else if (c >= 0x80) {
			const size_t n = Utf8Length(p + i, in.size() - i);
			if (n == 0) {
				out.push_back('?');
				++i;
			} else {
				out.append(in, i, n);
				i += n;
			}
			continue;
		} else {
			out.push_back(static_cast<char>(c));
		}
		++i;
	}
	return out;
}

inline std::string HandshakeJson(uint64_t appId) {
	return "{\"v\":1,\"client_id\":\"" + std::to_string(appId) + "\"}";
}

// What this game is doing, as the profile shows it.
struct Activity {
	bool        coop      = false;   // connected to a session
	bool        onMission = false;   // the session's mission is running
	std::string mission;             // its title, empty when we have none
	uint8_t     players   = 0;       // in the session, us included
	uint8_t     slots     = 0;       // the server's maximum
	int64_t     startUnix = 0;       // seconds; 0 leaves the timer off

	bool operator==(const Activity &o) const {
		return coop == o.coop && onMission == o.onMission && mission == o.mission &&
		       players == o.players && slots == o.slots && startUnix == o.startUnix;
	}
	bool operator!=(const Activity &o) const { return !(*this == o); }
};

inline std::string Details(const Activity &a) {
	return a.coop ? "In a co-op session" : "Single player";
}

// " (2/8)": who is in the session out of how many fit, in the text. Discord's
// own party size would read "(2 of 8)".
inline std::string Count(const Activity &a) {
	if (a.slots == 0)
		return {};
	const unsigned max = a.slots;
	unsigned       cur = a.players < 1 ? 1u : a.players;
	if (cur > max)
		cur = max;
	return " (" + std::to_string(cur) + "/" + std::to_string(max) + ")";
}

// Empty in single player: the missions there are the game's own, and "free
// roam" would be wrong half the time.
inline std::string State(const Activity &a) {
	if (!a.coop)
		return {};
	const std::string count = Count(a);
	std::string       s     = !a.onMission       ? "Free roam"
	                          : a.mission.empty() ? "On a mission"
	                                              : "Mission: " + a.mission;
	// The title gives way, never the count.
	if (s.size() + count.size() > TEXT_MAX)
		s.resize(TEXT_MAX - count.size());
	return s + count;
}

inline std::string ActivityJson(uint32_t pid, const Activity &a, uint32_t nonce) {
	std::string j = "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":" + std::to_string(pid) +
	                ",\"activity\":{\"details\":\"" + JsonEscape(Details(a)) + "\"";
	const std::string state = State(a);
	if (!state.empty())
		j += ",\"state\":\"" + JsonEscape(state) + "\"";
	if (a.startUnix > 0)
		j += ",\"timestamps\":{\"start\":" + std::to_string(a.startUnix) + "}";
	// No party: its size is drawn "(2 of 8)", and the count is in the state
	// line already.
	j += ",\"assets\":{\"large_image\":\"logo\",\"large_text\":\"CoopIII\"}";
	j += "}},\"nonce\":\"" + std::to_string(nonce) + "\"}";
	return j;
}

// SET_ACTIVITY with no activity takes ours off the profile.
inline std::string ClearJson(uint32_t pid, uint32_t nonce) {
	return "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":" + std::to_string(pid) +
	       "},\"nonce\":\"" + std::to_string(nonce) + "\"}";
}

// Whether a dispatch is Discord's READY, the answer to a handshake it took.
inline bool IsReady(const std::string &json) {
	return json.find("\"READY\"") != std::string::npos;
}

// Whether a dispatch is Discord saying a command failed.
inline bool IsError(const std::string &json) {
	return json.find("\"evt\":\"ERROR\"") != std::string::npos;
}

// ---- when ----------------------------------------------------------------------------

// Send when what we would say changed, straight away the first time on a pipe
// and otherwise no sooner than MIN_GAP_MS after the last one. A change held
// back is still a change on the next pass, so it goes out once the gap is up.
inline bool ShouldSend(bool changed, bool sentOnThisPipe, uint32_t nowMs, uint32_t lastSentMs) {
	if (!sentOnThisPipe)
		return true;
	if (!changed)
		return false;
	return nowMs - lastSentMs >= MIN_GAP_MS;
}

// Turns what the game thread can see into an Activity, keeping the times: the
// timer counts from the game's start in single player and from the join in a
// session, and starts again on every join.
struct Tracker {
	int64_t bootUnix  = 0;
	bool    wasCoop   = false;
	int64_t joinUnix  = 0;

	Activity Observe(bool connected, bool missionRunning, const char *missionTitle,
	                 uint8_t players, uint8_t slots, int64_t nowUnix) {
		if (bootUnix == 0)
			bootUnix = nowUnix;
		if (connected && !wasCoop)
			joinUnix = nowUnix;
		wasCoop = connected;

		Activity a;
		a.coop      = connected;
		a.startUnix = connected ? joinUnix : bootUnix;
		if (connected) {
			a.onMission = missionRunning;
			if (missionRunning && missionTitle)
				a.mission = missionTitle;
			a.players = players;
			a.slots   = slots;
		}
		return a;
	}
};

// ---- the thread (presence.cpp) --------------------------------------------------

// Starts the thread with `appId`, the application from discordapp.h or
// CoopIII.ini. Zero starts nothing.
void Start(uint64_t appId);

// Game thread: what to show now. Cheap; the thread picks it up on its own.
void Update(const Activity &activity);

// Takes the activity off the profile, closes the pipe and ends the thread.
// Only for an unload: at process exit the thread is already gone, and the
// pipe closing with the process clears the profile by itself.
void Stop();

} // namespace coopiii::presence
