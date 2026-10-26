#pragma once
// The game's own icons (items, skills, buff states), imported (not included: the client's art) into
// data/icons: icons_<n>.png sheets of 32x32 cells + icons.tsv (kind id sheet cell w h). The sheets
// become DX11 textures once, at start; a missing folder just leaves every icon empty.
#include "imgui.h"
#include <cstdint>
#include <string>

struct ID3D11Device;

namespace nj::gfx {

struct Icon {
    ImTextureID tex = 0;
    ImVec2      uv0, uv1;
    float       w = 0, h = 0;           // the image's own size in pixels (<= 32)
    bool ok() const { return tex != 0; }
};

// dir = the folder holding icons.tsv. false (with `err`) when nothing could be loaded.
bool init(ID3D11Device* dev, const std::string& dir, std::string& err);
void shutdown();
size_t count();

Icon item(uint32_t id);
Icon skill(uint32_t id);
Icon buff(uint32_t id);                 // the skill's state icon, else the skill icon

} // namespace nj::gfx
