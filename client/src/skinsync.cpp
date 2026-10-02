#include "skinsync.h"

#include "log.h"

#include <coopiii/net.h>

#include <cstring>

namespace coopiii {

namespace {

uint32_t Le32(const uint8_t *p) {
	return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
uint16_t Le16(const uint8_t *p) { return static_cast<uint16_t>(p[0] | p[1] << 8); }

} // namespace

// ---- the file ---------------------------------------------------------------

bool ReadSkinBmp(const uint8_t *file, size_t len, SkinImage &out) {
	out = SkinImage{};
	// BITMAPFILEHEADER, then a BITMAPINFOHEADER or one of its longer kin.
	if (!file || len < 14 + 40 || file[0] != 'B' || file[1] != 'M')
		return false;
	const uint32_t pixelsAt = Le32(file + 10);
	const uint32_t infoSize = Le32(file + 14);
	if (infoSize < 40 || infoSize > len - 14)
		return false;
	const int32_t  width    = static_cast<int32_t>(Le32(file + 18));
	const int32_t  rawH     = static_cast<int32_t>(Le32(file + 22));
	const uint16_t bits     = Le16(file + 28);
	const uint32_t compress = Le32(file + 30);
	uint32_t       colours  = Le32(file + 46);
	if (compress != 0 || width <= 0 || rawH == 0 || rawH == INT32_MIN)
		return false;
	const bool     topDown = rawH < 0;
	const uint32_t w       = static_cast<uint32_t>(width);
	const uint32_t h       = static_cast<uint32_t>(topDown ? -rawH : rawH);
	if (!SkinSideOk(w) || !SkinSideOk(h))
		return false;
	if (bits != 4 && bits != 8 && bits != 24 && bits != 32)
		return false;

	// The palette, BGRx, straight after the info header.
	uint8_t palette[256][3] = {};
	if (bits <= 8) {
		const uint32_t most = 1u << bits;
		if (colours == 0 || colours > most)
			colours = most;
		const size_t at = 14 + size_t(infoSize);
		if (at + size_t(colours) * 4 > len)
			return false;
		for (uint32_t i = 0; i < colours; ++i) {
			palette[i][0] = file[at + i * 4 + 2];
			palette[i][1] = file[at + i * 4 + 1];
			palette[i][2] = file[at + i * 4 + 0];
		}
	}

	const size_t stride = ((size_t(w) * bits + 31) / 32) * 4;
	if (pixelsAt > len || stride * h > len - pixelsAt)
		return false;

	out.width  = static_cast<uint16_t>(w);
	out.height = static_cast<uint16_t>(h);
	out.rgba.resize(size_t(w) * h * 4);
	for (uint32_t y = 0; y < h; ++y) {
		const uint8_t *row = file + pixelsAt + stride * (topDown ? y : h - 1 - y);
		uint8_t       *dst = out.rgba.data() + size_t(y) * w * 4;
		for (uint32_t x = 0; x < w; ++x, dst += 4) {
			if (bits == 24 || bits == 32) {
				const uint8_t *px = row + size_t(x) * (bits / 8);
				dst[0]            = px[2];
				dst[1]            = px[1];
				dst[2]            = px[0];
			} else {
				uint32_t index = bits == 8 ? row[x] : (row[x / 2] >> ((x & 1) ? 0 : 4)) & 0x0F;
				if (index >= colours)
					index = 0;
				dst[0] = palette[index][0];
				dst[1] = palette[index][1];
				dst[2] = palette[index][2];
			}
			dst[3] = 0xFF;
		}
	}
	return true;
}

// ---- the rule ---------------------------------------------------------------

void SkinSync::SetRule(uint8_t rule) {
	rule = SaneSkinRule(rule);
	if (rule == m_rule)
		return;
	m_rule = rule;
	// Either way what anybody was wearing is over: off, everybody is in the
	// default skin; on again, the server sends everybody's afresh.
	for (uint8_t id = 0; id < MAX_PLAYERS; ++id)
		Forget(id);
	m_sending = false;
	m_sent    = false;
	std::memset(m_sentName, 0, sizeof m_sentName);
	m_polled = false;
	Log("client: custom skins %s", rule == SKIN_RULE_SYNC
	                                   ? "travel again; ours goes out to the session"
	                                   : "are off in this session; everybody wears the default skin");
}

// ---- theirs -----------------------------------------------------------------

void SkinSync::OnSkin(const S_PlayerSkin &pkt, uint8_t localPlayerId, bool known) {
	if (m_rule != SKIN_RULE_SYNC)
		return;
	const uint8_t id = pkt.playerId;
	if (id >= MAX_PLAYERS || id == localPlayerId || !known)
		return;
	switch (m_in[id].Take(pkt.chunk)) {
	case SkinAssembly::Step::Partial:
		return;
	case SkinAssembly::Step::Rejected:
		Log("client: a piece of player %u's skin does not fit; it is thrown away and they "
		    "wear the default skin until a whole one comes",
		    unsigned(id));
		return;
	case SkinAssembly::Step::Complete: {
		const Skin skin = m_in[id].Finished();
		Wear(id, &skin);
		return;
	}
	}
}

void SkinSync::Wear(uint8_t playerId, const Skin *skin) {
	SkinImage image;
	const bool custom = skin && skin->info.format != SKIN_FORMAT_DEFAULT &&
	                    DecodeSkin(*skin, image.rgba);
	m_worn[playerId] = custom;
	if (custom) {
		m_wornInfo[playerId] = skin->info;
		image.width          = skin->info.width;
		image.height         = skin->info.height;
		Log("client: player %u wears the skin '%s' (%ux%u)", unsigned(playerId),
		    skin->info.name, unsigned(image.width), unsigned(image.height));
	} else {
		m_wornInfo[playerId] = SkinInfo{};
		if (skin)
			Log("client: player %u wears the default skin", unsigned(playerId));
	}
	if (m_bridge && m_bridge->SetRemoteSkin)
		m_bridge->SetRemoteSkin(playerId, custom ? &image : nullptr);
}

void SkinSync::Forget(uint8_t playerId) {
	if (playerId >= MAX_PLAYERS)
		return;
	m_in[playerId].Clear();
	Wear(playerId, nullptr);
}

void SkinSync::Clear() {
	for (uint8_t id = 0; id < MAX_PLAYERS; ++id)
		Forget(id);
	m_rule    = SKIN_RULE_SYNC;
	m_sending = false;
	m_sent    = false;
	std::memset(m_sentName, 0, sizeof m_sentName);
	m_polled = false;
	m_budget.Reset();
	m_mine = Skin{};
}

// ---- ours -------------------------------------------------------------------

void SkinSync::StartSending(const char *name) {
	if (++m_serial == 0)
		m_serial = 1;
	SkinImage image;
	const bool custom = m_bridge->LoadLocalSkin && m_bridge->LoadLocalSkin(name, image) &&
	                    image.rgba.size() == size_t(image.width) * image.height * 4 &&
	                    EncodeSkin(image.width, image.height, image.rgba.data(), m_serial, name,
	                               m_mine);
	if (!custom)
		m_mine = DefaultSkin(m_serial, name);
	m_offset  = 0;
	m_sending = true;
	m_sent    = true;
	std::memset(m_sentName, 0, sizeof m_sentName);
	std::strncpy(m_sentName, name, SKIN_NAME_LEN - 1);
	if (custom)
		Log("client: telling the session we wear the skin '%s' (%ux%u, %u bytes %s)",
		    m_mine.info.name, unsigned(m_mine.info.width), unsigned(m_mine.info.height),
		    unsigned(m_mine.info.bytes),
		    m_mine.info.format == SKIN_FORMAT_PALETTE ? "with a palette" : "of RGB");
	else
		Log("client: telling the session we wear the default skin ('%s')", m_mine.info.name);
}

void SkinSync::Tick(uint8_t localPlayerId, uint32_t nowMs) {
	if (m_rule != SKIN_RULE_SYNC || !m_bridge || !m_send || localPlayerId >= MAX_PLAYERS)
		return;

	if (!m_polled || nowMs - m_polledMs >= SKIN_POLL_MS) {
		m_polled   = true;
		m_polledMs = nowMs;
		char name[SKIN_NAME_LEN] = {};
		if (m_bridge->ReadLocalSkinName && m_bridge->ReadLocalSkinName(name)) {
			name[SKIN_NAME_LEN - 1] = '\0';
			if (!m_sent || std::strncmp(name, m_sentName, SKIN_NAME_LEN) != 0)
				StartSending(name);
		}
	}

	while (m_sending) {
		C_PlayerSkin out;
		InitHeader(out, nowMs);
		uint32_t next = m_offset;
		if (!CutSkinPiece(m_mine, next, out.chunk)) {
			m_sending = false;
			break;
		}
		if (!m_budget.Spend(nowMs, SkinPieceCost(out.chunk)))
			break;
		m_send(m_sendCtx, &out, sizeof out, CH_EVENT);
		++m_piecesSent;
		m_offset = next;
		if (SkinPiecesDone(m_mine, m_offset))
			m_sending = false;
	}
}

} // namespace coopiii
