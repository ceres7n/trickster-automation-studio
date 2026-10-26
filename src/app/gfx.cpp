#include "app/gfx.h"
#include "app/text_util.h"
#include <d3d11.h>
#include <wincodec.h>
#include <windows.h>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <vector>
#pragma comment(lib, "windowscodecs.lib")

namespace nj::gfx {
namespace {

constexpr int kCell = 32, kPerRow = 64;

struct Sheet {
    ID3D11ShaderResourceView* srv = nullptr;
    int w = 0, h = 0;
};
struct Ref {
    uint16_t sheet = 0, cell = 0;
    uint8_t  w = 0, h = 0;
};

std::vector<Sheet> g_sheets;
std::unordered_map<uint32_t, Ref> g_items, g_skills, g_buffs;

// PNG -> RGBA through WIC (no image library needed).
bool load_png(ID3D11Device* dev, const std::string& path, Sheet& out, std::string& err)
{
    IWICImagingFactory* wic = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) {
        err = "WIC is not available";
        return false;
    }
    const std::wstring wpath(path.begin(), path.end());
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* conv = nullptr;
    bool ok = false;
    UINT w = 0, h = 0;
    std::vector<uint8_t> px;
    if (SUCCEEDED(wic->CreateDecoderFromFilename(wpath.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) &&
        SUCCEEDED(dec->GetFrame(0, &frame)) && SUCCEEDED(wic->CreateFormatConverter(&conv)) &&
        SUCCEEDED(conv->Initialize(frame, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                   WICBitmapPaletteTypeCustom)) &&
        SUCCEEDED(conv->GetSize(&w, &h)) && w && h) {
        px.resize(size_t(w) * h * 4);
        ok = SUCCEEDED(conv->CopyPixels(nullptr, w * 4, UINT(px.size()), px.data()));
    }
    if (conv) conv->Release();
    if (frame) frame->Release();
    if (dec) dec->Release();
    wic->Release();
    if (!ok) { err = "cannot read " + path; return false; }

    D3D11_TEXTURE2D_DESC td{};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd{ px.data(), w * 4, 0 };
    ID3D11Texture2D* tex = nullptr;
    if (FAILED(dev->CreateTexture2D(&td, &sd, &tex))) { err = "CreateTexture2D failed"; return false; }
    D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
    vd.Format = td.Format;
    vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    vd.Texture2D.MipLevels = 1;
    const HRESULT hr = dev->CreateShaderResourceView(tex, &vd, &out.srv);
    tex->Release();
    if (FAILED(hr)) { err = "CreateShaderResourceView failed"; return false; }
    out.w = int(w);
    out.h = int(h);
    return true;
}

Icon make(const std::unordered_map<uint32_t, Ref>& m, uint32_t id)
{
    auto it = m.find(id);
    if (it == m.end() || it->second.sheet >= g_sheets.size()) return Icon{};
    const Ref& r = it->second;
    const Sheet& s = g_sheets[r.sheet];
    if (!s.srv) return Icon{};
    const int cx = (r.cell % kPerRow) * kCell + (kCell - r.w) / 2;
    const int cy = (r.cell / kPerRow) * kCell + (kCell - r.h) / 2;
    Icon ic;
    ic.tex = (ImTextureID)(intptr_t)s.srv;
    ic.uv0 = ImVec2(float(cx) / s.w, float(cy) / s.h);
    ic.uv1 = ImVec2(float(cx + r.w) / s.w, float(cy + r.h) / s.h);
    ic.w = r.w;
    ic.h = r.h;
    return ic;
}

} // namespace

bool init(ID3D11Device* dev, const std::string& dir, std::string& err)
{
    shutdown();
    std::string base = dir;
    if (!base.empty() && base.back() != '\\' && base.back() != '/') base += '\\';
    std::ifstream in(base + "icons.tsv");
    if (!in) { err = "no icons (" + base + "icons.tsv: made from the client's art, not included)"; return false; }
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    std::string line;
    int sheets = 0;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ls(line);
        std::string kind;
        unsigned long id = 0, sheet = 0, cell = 0, w = 0, h = 0;
        if (!(ls >> kind >> id >> sheet >> cell >> w >> h) || w > kCell || h > kCell) continue;
        const Ref r{ uint16_t(sheet), uint16_t(cell), uint8_t(w), uint8_t(h) };
        (kind == "i" ? g_items : kind == "s" ? g_skills : g_buffs)[uint32_t(id)] = r;
        if (int(sheet) + 1 > sheets) sheets = int(sheet) + 1;
    }
    g_sheets.assign(size_t(sheets), Sheet{});
    for (int i = 0; i < sheets; ++i)
        if (!load_png(dev, base + fmt("icons_%d.png", i), g_sheets[size_t(i)], err)) return false;
    return true;
}

void shutdown()
{
    for (Sheet& s : g_sheets)
        if (s.srv) s.srv->Release();
    g_sheets.clear();
    g_items.clear();
    g_skills.clear();
    g_buffs.clear();
}

size_t count() { return g_items.size() + g_skills.size() + g_buffs.size(); }

Icon item(uint32_t id) { return make(g_items, id); }
Icon skill(uint32_t id) { return make(g_skills, id); }
Icon buff(uint32_t id)
{
    Icon i = make(g_buffs, id);
    return i.ok() ? i : make(g_skills, id);
}

} // namespace nj::gfx
