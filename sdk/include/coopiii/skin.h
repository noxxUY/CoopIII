// A player's custom skin on the wire, the parts the client and the server
// both run: what a valid skin is, its hash, the pieces it travels in, putting
// them back together, how fast they go, and turning pixels into a payload and
// back. protocol.h, C_PlayerSkin, has the layout; docs/protocol.md 1.72 the
// design.
//
// Nothing in here trusts a piece. A receiver checks every field before a byte
// of it is kept, and a skin is only used once all of it is in and its hash
// matches.
#pragma once

#include "protocol.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

namespace coopiii {

constexpr uint32_t SKIN_PALETTE_BYTES = 256 * 3;
constexpr uint32_t SKIN_MAX_BYTES =
    uint32_t(SKIN_SIDE_MAX) * SKIN_SIDE_MAX * 3;   // the biggest RGB payload

// How fast pieces go to one machine, and how much can go at once after a
// pause. A 256x256 skin is 192 KiB in RGB and 66 KiB with a palette, so a
// joiner has everybody's within a few seconds, and the pieces never crowd
// the events that share their channel for long.
constexpr uint32_t SKIN_BYTES_PER_S = 128 * 1024;
constexpr uint32_t SKIN_BURST_BYTES = 16 * SKIN_CHUNK_BYTES;

inline bool SkinSideOk(uint32_t side) {
	return side >= SKIN_SIDE_MIN && side <= SKIN_SIDE_MAX && (side & (side - 1)) == 0;
}

// The payload a format and size take, or 0 for one that is not a skin.
// SKIN_FORMAT_DEFAULT takes nothing, and is not "not a skin": SkinInfoOk is
// the question to ask.
inline uint32_t SkinPayloadBytes(uint8_t format, uint32_t width, uint32_t height) {
	if (format == SKIN_FORMAT_DEFAULT || !SkinSideOk(width) || !SkinSideOk(height))
		return 0;
	if (format == SKIN_FORMAT_RGB)
		return width * height * 3;
	if (format == SKIN_FORMAT_PALETTE)
		return SKIN_PALETTE_BYTES + width * height;
	return 0;
}

// FNV-1a, 32 bits. Not a defence against anybody; it catches a payload that
// was put together wrong.
inline uint32_t SkinHash(const uint8_t *bytes, size_t len) {
	uint32_t h = 2166136261u;
	for (size_t i = 0; i < len; ++i) {
		h ^= bytes[i];
		h *= 16777619u;
	}
	return h;
}

// Printable ASCII only, terminated, zero after the terminator. The name is
// only ever written to a log, but a log is still somewhere a stranger's bytes
// should not go raw. Whatever is not printable becomes '_'.
inline void CleanSkinName(char (&name)[SKIN_NAME_LEN]) {
	size_t n = 0;
	while (n < SKIN_NAME_LEN - 1 && name[n] != '\0')
		++n;
	for (size_t i = 0; i < n; ++i)
		if (name[i] < 0x20 || name[i] > 0x7E)
			name[i] = '_';
	for (size_t i = n; i < SKIN_NAME_LEN; ++i)
		name[i] = '\0';
}

// Everything about a skin a piece claims, apart from its payload. The name
// is cleaned in place rather than checked: it says nothing that matters.
inline bool SkinInfoOk(SkinInfo &info) {
	CleanSkinName(info.name);
	std::memset(info.pad, 0, sizeof info.pad);
	if (info.serial == 0)
		return false;
	if (info.format == SKIN_FORMAT_DEFAULT)
		return info.width == 0 && info.height == 0 && info.bytes == 0 &&
		       info.hash == SkinHash(nullptr, 0);
	const uint32_t want = SkinPayloadBytes(info.format, info.width, info.height);
	return want != 0 && info.bytes == want;
}

// The length the piece at `offset` must have: whole pieces, then what is
// left. A skin with no payload is one empty piece.
inline uint32_t SkinPieceLength(uint32_t bytes, uint32_t offset) {
	const uint32_t left = bytes - offset;
	return left < SKIN_CHUNK_BYTES ? left : uint32_t(SKIN_CHUNK_BYTES);
}

// One whole skin: what a sender cuts up and a receiver ends up with.
struct Skin {
	SkinInfo             info{};
	std::vector<uint8_t> payload;
};

using SkinRef = std::shared_ptr<const Skin>;

// The game's default skin, under `serial`.
inline Skin DefaultSkin(uint32_t serial, const char *name) {
	Skin s;
	s.info.serial = serial;
	s.info.format = SKIN_FORMAT_DEFAULT;
	s.info.hash   = SkinHash(nullptr, 0);
	if (name)
		std::strncpy(s.info.name, name, SKIN_NAME_LEN - 1);
	CleanSkinName(s.info.name);
	return s;
}

// The piece of `skin` at `offset`, and the offset after it. False once the
// skin has been sent whole. A skin with no payload sends one empty piece.
inline bool CutSkinPiece(const Skin &skin, uint32_t &offset, SkinChunk &out) {
	const uint32_t bytes = skin.info.bytes;
	if (offset > bytes || (offset == bytes && (bytes != 0 || offset != 0)))
		return false;
	std::memset(&out, 0, sizeof out);
	out.info   = skin.info;
	out.offset = offset;
	out.length = static_cast<uint16_t>(SkinPieceLength(bytes, offset));
	if (out.length != 0)
		std::memcpy(out.data, skin.payload.data() + offset, out.length);
	offset += out.length;
	if (bytes == 0)
		offset = 1;   // the one empty piece has gone
	return true;
}

// Whether a cut has nothing left to send.
inline bool SkinPiecesDone(const Skin &skin, uint32_t offset) {
	return skin.info.bytes == 0 ? offset != 0 : offset >= skin.info.bytes;
}

// The pieces of one sender's skin, put back together. Anything out of place
// - a piece that does not follow the last, a length that is not the one it
// must be, a size that is not a skin - throws away what was held, and the
// next skin has to start again from its first piece.
class SkinAssembly {
public:
	enum class Step : uint8_t { Partial, Complete, Rejected };

	Step Take(const SkinChunk &piece) {
		SkinInfo info = piece.info;
		if (!SkinInfoOk(info))
			return Reject();
		const bool same = m_open && std::memcmp(&info, &m_info, sizeof info) == 0;
		if (!same) {
			// A new skin, which can only start at the beginning.
			if (piece.offset != 0)
				return Reject();
			m_info = info;
			m_have = 0;
			m_open = true;
			m_bytes.assign(info.bytes, 0);
		}
		if (piece.offset != m_have || piece.length != SkinPieceLength(info.bytes, m_have) ||
		    piece.length > SKIN_CHUNK_BYTES)
			return Reject();
		if (piece.length != 0)
			std::memcpy(m_bytes.data() + m_have, piece.data, piece.length);
		m_have += piece.length;
		if (m_have < info.bytes)
			return Step::Partial;
		if (SkinHash(m_bytes.data(), m_bytes.size()) != info.hash)
			return Reject();
		m_open = false;
		return Step::Complete;
	}

	// The skin Take just completed, moved out.
	Skin Finished() {
		Skin s;
		s.info    = m_info;
		s.payload = std::move(m_bytes);
		m_bytes.clear();
		return s;
	}

	void Clear() {
		m_open = false;
		m_have = 0;
		m_bytes.clear();
		m_bytes.shrink_to_fit();
	}

	bool     Open() const { return m_open; }
	uint32_t Have() const { return m_have; }

private:
	Step Reject() {
		Clear();
		return Step::Rejected;
	}

	SkinInfo             m_info{};
	bool                 m_open = false;
	uint32_t             m_have = 0;
	std::vector<uint8_t> m_bytes;
};

// How many bytes of pieces may go to one machine now: SKIN_BYTES_PER_S,
// with up to SKIN_BURST_BYTES saved up while nothing went.
class SkinBudget {
public:
	bool Spend(uint32_t nowMs, uint32_t bytes) {
		if (!m_started) {
			m_started = true;
			m_credit  = SKIN_BURST_BYTES;
			m_lastMs  = nowMs;
		}
		const uint32_t gap = nowMs - m_lastMs;
		m_lastMs           = nowMs;
		const uint64_t add = uint64_t(gap) * SKIN_BYTES_PER_S / 1000u;
		m_credit = static_cast<uint32_t>(
		    m_credit + add > SKIN_BURST_BYTES ? SKIN_BURST_BYTES : m_credit + add);
		if (m_credit < bytes)
			return false;
		m_credit -= bytes;
		return true;
	}

	void Reset() { m_started = false; }

private:
	bool     m_started = false;
	uint32_t m_credit  = 0;
	uint32_t m_lastMs  = 0;
};

// What one piece costs against a budget: its bytes, and never nothing, so a
// run of empty pieces is paced too.
inline uint32_t SkinPieceCost(const SkinChunk &piece) {
	return piece.length < 64 ? 64u : piece.length;
}

// Pixels to a payload: RGBA, top row first, width * height * 4 bytes, the
// alpha ignored (the engine draws the skin opaque). A palette when the
// picture has 256 colours or fewer, which is what the game's own skins are,
// and RGB otherwise. False for a size that is not a skin.
inline bool EncodeSkin(uint32_t width, uint32_t height, const uint8_t *rgba, uint32_t serial,
                       const char *name, Skin &out) {
	if (!rgba || !SkinSideOk(width) || !SkinSideOk(height) || serial == 0)
		return false;
	const size_t pixels = size_t(width) * height;

	// Up to 256 distinct colours, found by a small open-addressed table.
	std::vector<uint32_t> keys(1024, 0xFFFFFFFFu);
	std::vector<uint8_t>  slot(1024, 0);
	std::vector<uint8_t>  palette(SKIN_PALETTE_BYTES, 0);
	std::vector<uint8_t>  indices(pixels);
	uint32_t              colours = 0;
	bool                  fits    = true;
	for (size_t i = 0; i < pixels && fits; ++i) {
		const uint8_t *p   = rgba + i * 4;
		const uint32_t key = uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16;
		uint32_t       at  = (key * 2654435761u) >> 22;
		while (keys[at] != 0xFFFFFFFFu && keys[at] != key)
			at = (at + 1) & 1023;
		if (keys[at] != key) {
			if (colours == 256) {
				fits = false;
				break;
			}
			keys[at] = key;
			slot[at] = static_cast<uint8_t>(colours);
			palette[colours * 3 + 0] = p[0];
			palette[colours * 3 + 1] = p[1];
			palette[colours * 3 + 2] = p[2];
			++colours;
		}
		indices[i] = slot[at];
	}

	out = Skin{};
	out.info.serial = serial;
	out.info.width  = static_cast<uint16_t>(width);
	out.info.height = static_cast<uint16_t>(height);
	if (name)
		std::strncpy(out.info.name, name, SKIN_NAME_LEN - 1);
	CleanSkinName(out.info.name);
	if (fits) {
		out.info.format = SKIN_FORMAT_PALETTE;
		out.payload     = std::move(palette);
		out.payload.insert(out.payload.end(), indices.begin(), indices.end());
	} else {
		out.info.format = SKIN_FORMAT_RGB;
		out.payload.resize(pixels * 3);
		for (size_t i = 0; i < pixels; ++i)
			std::memcpy(&out.payload[i * 3], rgba + i * 4, 3);
	}
	out.info.bytes = static_cast<uint32_t>(out.payload.size());
	out.info.hash  = SkinHash(out.payload.data(), out.payload.size());
	return true;
}

// And back, to RGBA with an opaque alpha. False for anything that is not a
// whole skin of its own size; a default skin has no pixels and is false too.
inline bool DecodeSkin(const Skin &skin, std::vector<uint8_t> &rgba) {
	SkinInfo info = skin.info;
	if (!SkinInfoOk(info) || info.format == SKIN_FORMAT_DEFAULT ||
	    skin.payload.size() != info.bytes)
		return false;
	const size_t pixels = size_t(info.width) * info.height;
	rgba.resize(pixels * 4);
	const uint8_t *src = skin.payload.data();
	for (size_t i = 0; i < pixels; ++i) {
		const uint8_t *c = info.format == SKIN_FORMAT_RGB
		                       ? src + i * 3
		                       : src + size_t(src[SKIN_PALETTE_BYTES + i]) * 3;
		rgba[i * 4 + 0] = c[0];
		rgba[i * 4 + 1] = c[1];
		rgba[i * 4 + 2] = c[2];
		rgba[i * 4 + 3] = 0xFF;
	}
	return true;
}

} // namespace coopiii
