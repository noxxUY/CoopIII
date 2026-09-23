#include "ui/theme.h"

#include <windows.h>

#include <cstddef>

namespace ui {

const Theme &Theme::Dark() {
	static const Theme t = [] {
		Theme d{};
		d.bgPage         = Rgb(0x0B0D10);
		d.bgTitlebar     = Rgb(0x0D1014);
		d.bgWindow       = Rgb(0x111418);
		d.bgPanel        = Rgb(0x171B20);
		d.bgCard         = Rgb(0x15191E);
		d.bgConsole      = Rgb(0x0E1115);
		d.bgSubtle       = Rgb(0x1B2027);
		d.mapBlock2      = Rgb(0x20262E);
		d.mapLot         = Rgb(0x252B33);
		d.bgBadge        = Rgb(0x1F252C);
		d.bgDisabled     = Rgb(0x1C2127);
		d.bgSelected     = Rgb(0x2A313A);
		d.textPrimary    = Rgb(0xEEF0F2);
		d.textButton     = Rgb(0xD5DAE0);
		d.textSecondary  = Rgb(0xAAB3BE);
		d.textTertiary   = Rgb(0x8A94A1);
		d.textMuted      = Rgb(0x7E8895);
		d.actionBg       = Rgb(0xF4F4EF);
		d.actionFg       = Rgb(0x0E1013);
		d.border         = Rgb(0x242A32);
		d.borderControl  = Rgb(0x2F3640);
		d.borderTitlebar = Rgb(0x1F242B);
		d.borderStrong   = Rgb(0x3A424D);
		d.borderDisabled = Rgb(0x2A3038);
		d.mapRoute       = Rgb(0x5B6573);
		d.statusOk       = Rgb(0x52C98A);
		d.statusWarn     = Rgb(0xE3AD4B);
		d.statusFail     = Rgb(0xFF7E74);
		d.brandRed       = Rgb(0xD20000);
		d.okPillBg       = Rgb(0x182623);
		d.okPillBorder   = Rgb(0x214135);
		d.scrim          = Rgb(0x07090B, 0.74f);
		d.isLight        = false;
		return d;
	}();
	return t;
}

const Theme &Theme::Light() {
	static const Theme t = [] {
		Theme d{};
		d.bgPage         = Rgb(0xE7EAEE);
		d.bgTitlebar     = Rgb(0xE9ECEF);
		d.bgWindow       = Rgb(0xF4F5F7);
		d.bgPanel        = Rgb(0xFAFAFB);
		d.bgCard         = Rgb(0xFFFFFF);
		d.bgConsole      = Rgb(0xEFF1F4);
		d.bgSubtle       = Rgb(0xE3E6EA);
		d.mapBlock2      = Rgb(0xDADEE3);
		d.mapLot         = Rgb(0xD3D8DE);
		d.bgBadge        = Rgb(0xECEEF1);
		d.bgDisabled     = Rgb(0xECEEF1);
		d.bgSelected     = Rgb(0xFFFFFF);
		d.textPrimary    = Rgb(0x15191E);
		d.textButton     = Rgb(0x2A3038);
		d.textSecondary  = Rgb(0x4A5360);
		d.textTertiary   = Rgb(0x5B6572);
		d.textMuted      = Rgb(0x636D7A);
		d.actionBg       = Rgb(0x14171B);
		d.actionFg       = Rgb(0xFFFFFF);
		d.border         = Rgb(0xE1E4E8);
		d.borderControl  = Rgb(0xCDD2D8);
		d.borderTitlebar = Rgb(0xDADEE3);
		d.borderStrong   = Rgb(0xB7BEC7);
		d.borderDisabled = Rgb(0xD3D8DE);
		d.mapRoute       = Rgb(0x9AA3AE);
		d.statusOk       = Rgb(0x166B40);
		d.statusWarn     = Rgb(0x8E5F0A);
		d.statusFail     = Rgb(0xC0352B);
		d.brandRed       = Rgb(0xD20000);
		d.okPillBg       = Rgb(0xE5F3EB);
		d.okPillBorder   = Rgb(0xB9DEC9);
		d.scrim          = Rgb(0x181C21, 0.38f);
		d.isLight        = true;
		return d;
	}();
	return t;
}

bool WindowsPrefersLight() {
	DWORD value = 0;
	DWORD size  = sizeof(value);
	// Anything unreadable means dark: this design's home ground, and a wrong
	// guess towards dark looks deliberate where the other way looks broken.
	const LSTATUS status = RegGetValueW(
	    HKEY_CURRENT_USER,
	    L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
	    L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size);
	return status == ERROR_SUCCESS && value != 0;
}

Theme Theme::Blend(const Theme &a, const Theme &b, float t) {
	if (t <= 0.0f)
		return a;
	if (t >= 1.0f)
		return b;

	// Every colour token is an ImU32 and they are all in front of isLight, so
	// the run of them can be walked rather than named one by one - a token
	// added to the struct is then blended without anyone remembering to come
	// back here.
	Theme out = t < 0.5f ? a : b;
	static_assert(offsetof(Theme, isLight) % sizeof(ImU32) == 0, "tokens must pack");
	constexpr size_t count = offsetof(Theme, isLight) / sizeof(ImU32);

	const auto *from = reinterpret_cast<const ImU32 *>(&a);
	const auto *to   = reinterpret_cast<const ImU32 *>(&b);
	auto       *into = reinterpret_cast<ImU32 *>(&out);
	for (size_t i = 0; i < count; ++i)
		into[i] = Mix(from[i], to[i], t);
	return out;
}

bool SystemWantsAnimation() {
	BOOL wants = TRUE;
	if (!SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &wants, 0))
		return true;
	return wants != FALSE;
}

ImU32 WithAlpha(ImU32 colour, float alpha) {
	const ImU32 a = static_cast<ImU32>(
	    ((colour >> IM_COL32_A_SHIFT) & 0xFF) * (alpha < 0.0f ? 0.0f : alpha) + 0.5f);
	return (colour & ~static_cast<ImU32>(0xFFu << IM_COL32_A_SHIFT)) |
	       ((a > 255 ? 255 : a) << IM_COL32_A_SHIFT);
}

ImU32 Mix(ImU32 a, ImU32 b, float t) {
	if (t <= 0.0f)
		return a;
	if (t >= 1.0f)
		return b;
	auto chan = [t](ImU32 x, ImU32 y, int shift) -> ImU32 {
		const float xv = static_cast<float>((x >> shift) & 0xFF);
		const float yv = static_cast<float>((y >> shift) & 0xFF);
		return static_cast<ImU32>(xv + (yv - xv) * t + 0.5f) << shift;
	};
	return chan(a, b, IM_COL32_R_SHIFT) | chan(a, b, IM_COL32_G_SHIFT) |
	       chan(a, b, IM_COL32_B_SHIFT) | chan(a, b, IM_COL32_A_SHIFT);
}

} // namespace ui
