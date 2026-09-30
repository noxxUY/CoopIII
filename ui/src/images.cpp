// The one image the apps carry: the CoopIII logo mark.
//
// Embedded rather than loaded from disk, for the same reason the fonts are -
// an app that cannot find its own logo looks broken, and this one is drawn on
// every screen.
#include "ui/widgets.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include <stb_image.h>

#include <d3d11.h>

namespace ui {
namespace {

const unsigned char kLogoPng[] = {
#include "logo-mark.png.h"
};

ID3D11ShaderResourceView *g_logo = nullptr;
float                     g_logoAspect = 387.0f / 259.0f;

} // namespace

void LoadImages(void *d3dDevice) {
	if (g_logo || !d3dDevice)
		return;
	auto *device = static_cast<ID3D11Device *>(d3dDevice);

	int            w = 0, h = 0, channels = 0;
	unsigned char *pixels = stbi_load_from_memory(kLogoPng, static_cast<int>(sizeof(kLogoPng)),
	                                              &w, &h, &channels, 4);
	if (!pixels)
		return;
	g_logoAspect = static_cast<float>(w) / static_cast<float>(h);

	D3D11_TEXTURE2D_DESC desc = {};
	desc.Width            = static_cast<UINT>(w);
	desc.Height           = static_cast<UINT>(h);
	desc.MipLevels        = 1;
	desc.ArraySize        = 1;
	desc.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
	desc.SampleDesc.Count = 1;
	desc.Usage            = D3D11_USAGE_DEFAULT;
	desc.BindFlags        = D3D11_BIND_SHADER_RESOURCE;

	D3D11_SUBRESOURCE_DATA data = {};
	data.pSysMem                = pixels;
	data.SysMemPitch            = static_cast<UINT>(w * 4);

	ID3D11Texture2D *texture = nullptr;
	if (SUCCEEDED(device->CreateTexture2D(&desc, &data, &texture)) && texture) {
		D3D11_SHADER_RESOURCE_VIEW_DESC srv = {};
		srv.Format                    = desc.Format;
		srv.ViewDimension             = D3D11_SRV_DIMENSION_TEXTURE2D;
		srv.Texture2D.MipLevels       = 1;
		device->CreateShaderResourceView(texture, &srv, &g_logo);
		texture->Release();
	}
	stbi_image_free(pixels);
}

float Logo(ImVec2 pos, float height) {
	const float width = height * g_logoAspect;
	if (g_logo)
		ImGui::GetWindowDrawList()->AddImage(
		    reinterpret_cast<ImTextureID>(g_logo), pos,
		    ImVec2(pos.x + width, pos.y + height));
	return width;
}

float LogoWidth(ImVec2 pos, float width) {
	const float height = width / g_logoAspect;
	if (g_logo)
		ImGui::GetWindowDrawList()->AddImage(
		    reinterpret_cast<ImTextureID>(g_logo), pos,
		    ImVec2(pos.x + width, pos.y + height));
	return height;
}

} // namespace ui
