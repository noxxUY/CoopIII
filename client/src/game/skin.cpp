#include "skin.h"

#include "addresses.h"
#include "log.h"
#include "teardown.h"

#include <cstdio>
#include <cstring>
#include <vector>

namespace coopiii::game {

namespace {

using AtomicFn          = void *(__cdecl *)(void *atomic);
using AtomicVisitFn     = void *(__cdecl *)(void *atomic, void *data);
using ForAllAtomicsFn   = void *(__cdecl *)(void *clump, AtomicVisitFn visit, void *data);
using GetSkinTextureFn  = void *(__cdecl *)(const char *name);
using ImageCreateFn     = void *(__cdecl *)(int32_t width, int32_t height, int32_t depth);
using ImageAllocateFn   = void *(__cdecl *)(void *image);
using ImageDestroyFn    = int32_t(__cdecl *)(void *image);
using FindRasterFormatFn = void *(__cdecl *)(void *image, int32_t type, int32_t *width,
                                             int32_t *height, int32_t *depth, int32_t *format);
using RasterCreateFn    = void *(__cdecl *)(int32_t width, int32_t height, int32_t depth,
                                            int32_t format);
using RasterFromImageFn = void *(__cdecl *)(void *raster, void *image);
using TextureCreateFn   = void *(__cdecl *)(void *raster);
using TextureDestroyFn  = int32_t(__cdecl *)(void *texture);

// The biggest file a skin can come in: 512x512 at 32 bits and its headers.
constexpr size_t SKIN_FILE_MAX = size_t(SKIN_SIDE_MAX) * SKIN_SIDE_MAX * 4 + 4096;

struct Slot {
	void     *texture = nullptr;   // one reference of ours; null is the default skin
	void     *clump   = nullptr;   // this frame's ped's clump, for SkinRenderCB
	bool      pending = false;     // `image` is to become the texture
	SkinImage image;
};

Slot  g_slots[MAX_PLAYERS];
void *g_default         = nullptr;   // the default skin, one reference of ours
bool  g_saidNoDefault   = false;
bool  g_saidNoTexture   = false;
char  g_saidUnreadable[SKIN_NAME_LEN] = {};

// The engine has made its own skin texture, so the device is up and the skin
// dictionary exists. Nothing is made before that.
bool EngineReady() { return Global<void *>(PLAYER_SKIN_TEXTURE) != nullptr; }

void DropTexture(void *&texture) {
	if (texture)
		Func<TextureDestroyFn>(RwTextureDestroy)(texture);
	texture = nullptr;
}

// A texture of our own out of RGBA, the way GetSkinTexture makes one out of a
// BMP (addresses.h), only from memory.
void *MakeTexture(const SkinImage &img) {
	const int32_t w = img.width, h = img.height;
	if (img.rgba.size() != size_t(w) * h * 4)
		return nullptr;
	void *image = Func<ImageCreateFn>(RwImageCreate)(w, h, 32);
	if (!image)
		return nullptr;
	if (!Func<ImageAllocateFn>(RwImageAllocatePixels)(image)) {
		Func<ImageDestroyFn>(RwImageDestroy)(image);
		return nullptr;
	}
	uint8_t      *pixels = Field<uint8_t *>(image, offs::RWIMAGE_PIXELS);
	const int32_t stride = Field<int32_t>(image, offs::RWIMAGE_STRIDE);
	if (!pixels || stride < w * 4) {
		Func<ImageDestroyFn>(RwImageDestroy)(image);
		return nullptr;
	}
	for (int32_t y = 0; y < h; ++y)
		std::memcpy(pixels + size_t(y) * stride, img.rgba.data() + size_t(y) * w * 4,
		            size_t(w) * 4);

	int32_t rw = 0, rh = 0, depth = 0, format = 0;
	void   *raster = nullptr;
	if (Func<FindRasterFormatFn>(RwImageFindRasterFormat)(image, rwRASTERTYPETEXTURE, &rw, &rh,
	                                                      &depth, &format) &&
	    rw > 0 && rh > 0)
		raster = Func<RasterCreateFn>(RwRasterCreate)(rw, rh, depth, format);
	void *texture = nullptr;
	if (raster) {
		Func<RasterFromImageFn>(RwRasterSetFromImage)(raster, image);
		texture = Func<TextureCreateFn>(RwTextureCreate)(raster);
	}
	Func<ImageDestroyFn>(RwImageDestroy)(image);
	return texture;
}

// The callback a remote player's atomics are drawn with: RenderPlayerCB, with
// that player's skin where it reads ours, for the one call.
void *__cdecl SkinRenderCB(void *atomic) {
	void *clump   = Field<void *>(atomic, offs::RPATOMIC_CLUMP);
	void *texture = g_default;
	for (const Slot &s : g_slots)
		if (s.clump && s.clump == clump) {
			if (s.texture)
				texture = s.texture;
			break;
		}
	void *&own  = Global<void *>(PLAYER_SKIN_TEXTURE);
	void *kept  = own;
	if (texture)
		own = texture;
	Func<AtomicFn>(CVisibilityPlugins__RenderPlayerCB)(atomic);
	own = kept;
	return atomic;
}

void *__cdecl TakeAtomic(void *atomic, void *) {
	void *&cb = Field<void *>(atomic, offs::RPATOMIC_RENDER_CB);
	if (cb == reinterpret_cast<void *>(CVisibilityPlugins__RenderPlayerCB))
		cb = reinterpret_cast<void *>(&SkinRenderCB);
	return atomic;
}

// ---- SkinBridge -------------------------------------------------------------

bool ReadLocalSkinName(char (&name)[SKIN_NAME_LEN]) {
	std::memset(name, 0, sizeof name);
	std::memcpy(name, Ptr<const char>(PLAYER_SKIN_NAME), SKIN_NAME_LEN - 1);
	return name[0] != '\0';
}

bool LoadLocalSkin(const char *name, SkinImage &out) {
	if (!name || name[0] == '\0' ||
	    std::strncmp(name, Ptr<const char>(PLAYER_SKIN_DEFAULT_NAME), SKIN_NAME_LEN) == 0)
		return false;
	// A file name and nothing else, the way the menu lists them.
	for (const char *c = name; *c; ++c)
		if (*c == '\\' || *c == '/' || *c == ':' || (c[0] == '.' && c[1] == '.'))
			return false;

	char path[SKIN_NAME_LEN + 16];
	std::snprintf(path, sizeof path, "skins\\%s.bmp", name);
	std::vector<uint8_t> file;
	if (FILE *fh = std::fopen(path, "rb")) {
		file.resize(SKIN_FILE_MAX + 1);
		file.resize(std::fread(file.data(), 1, file.size(), fh));
		std::fclose(fh);
	}
	const bool ok = !file.empty() && file.size() <= SKIN_FILE_MAX &&
	                ReadSkinBmp(file.data(), file.size(), out);
	if (!ok && std::strncmp(g_saidUnreadable, name, SKIN_NAME_LEN) != 0) {
		std::strncpy(g_saidUnreadable, name, SKIN_NAME_LEN - 1);
		Log("skin: %s is %s, so the others see us in the default skin", path,
		    file.empty() ? "not there" : "not a skin the session can send (an uncompressed "
		                                 "BMP, each side a power of two from 16 to 512)");
	}
	return ok;
}

void SetRemoteSkin(uint8_t playerId, const SkinImage *image) {
	if (playerId >= MAX_PLAYERS)
		return;
	Slot &s   = g_slots[playerId];
	s.pending = true;
	s.image   = image ? *image : SkinImage{};
}

void PlaceRemoteSkins(const int32_t (&peds)[MAX_PLAYERS]) {
	if (!EngineReady()) {
		for (Slot &s : g_slots)
			s.clump = nullptr;
		return;
	}
	if (!g_default && !g_saidNoDefault) {
		g_default = Func<GetSkinTextureFn>(CPlayerSkin__GetSkinTexture)(
		    Ptr<const char>(PLAYER_SKIN_DEFAULT_NAME));
		if (!g_default) {
			g_saidNoDefault = true;
			Log("skin: the game's default skin could not be loaded; remote players are "
			    "drawn in ours until theirs comes in");
		}
	}
	for (uint8_t id = 0; id < MAX_PLAYERS; ++id) {
		Slot &s = g_slots[id];
		if (s.pending) {
			s.pending = false;
			DropTexture(s.texture);
			if (!s.image.rgba.empty()) {
				s.texture = MakeTexture(s.image);
				if (!s.texture && !g_saidNoTexture) {
					g_saidNoTexture = true;
					Log("skin: player %u's skin could not be made into a texture; they wear the "
					    "default skin (said once)", unsigned(id));
				}
			}
			s.image = SkinImage{};
		}
		void *ped   = peds[id] >= 0 ? PedAt(peds[id]) : nullptr;
		void *clump = ped ? Field<void *>(ped, offs::RW_OBJECT) : nullptr;
		if (clump && Field<uint8_t>(clump, RWOBJECT_TYPE) != RWTYPE_CLUMP)
			clump = nullptr;
		s.clump = clump;
		if (clump)
			Func<ForAllAtomicsFn>(RpClumpForAllAtomics)(clump, &TakeAtomic, nullptr);
	}
}

} // namespace

void AddSkinsToBridge(WorldBridge &bridge) {
	SkinBridge &b       = bridge.skins;
	b.ReadLocalSkinName = &ReadLocalSkinName;
	b.LoadLocalSkin     = &LoadLocalSkin;
	b.SetRemoteSkin     = &SetRemoteSkin;
	b.PlaceRemoteSkins  = &PlaceRemoteSkins;
}

} // namespace coopiii::game
