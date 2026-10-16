#include "settings/native/native_page_skin_common.h"

#include "utils/common_utils.h"

#include "msimeui/DeviceResources.h"
#include "msimeui/Fonts.h"
#include "msimeui/Window.h"

#include <d2d1_1.h>
#include <wincodec.h>

#include <algorithm>
#include <cmath>
#include <filesystem>

using Microsoft::WRL::ComPtr;

namespace native_settings
{
namespace skinpreview
{
namespace
{
constexpr float kUnbounded = 100000.0f;

bool HitSelf(msimeui::Window *window, const msimeui::Visual &visual, const POINT &point)
{
    return window && visual.HitTest(window->ClientPixelsToDips(point));
}

D2D1_COLOR_F None()
{
    return D2D1::ColorF(0, 0.0f);
}

// A color from a skin manifest; alpha 0 when it is missing or not a plain notation.
D2D1_COLOR_F ManifestColor(const Json *object, const char *key)
{
    if (!object || !object->is_object())
        return None();
    const auto it = object->find(key);
    if (it == object->end() || !it->is_string())
        return None();
    std::string text = it->get<std::string>();
    D2D1_COLOR_F color{};
    if (ParseCssColor(text, color))
        return color;
    // rgb() / rgba()
    int r = 0, g = 0, b = 0;
    float a = 1.0f;
    if (sscanf_s(text.c_str(), "rgba(%d,%d,%d,%f)", &r, &g, &b, &a) == 4 ||
        sscanf_s(text.c_str(), "rgba(%d, %d, %d, %f)", &r, &g, &b, &a) == 4 ||
        sscanf_s(text.c_str(), "rgb(%d,%d,%d)", &r, &g, &b) == 3 ||
        sscanf_s(text.c_str(), "rgb(%d, %d, %d)", &r, &g, &b) == 3)
        return Rgba(r, g, b, a);
    return None();
}

bool Has(const D2D1_COLOR_F &color)
{
    return color.a > 0.0f;
}

float NumberOr(const Json *object, const char *key, float fallback, float max)
{
    if (!object || !object->is_object())
        return fallback;
    const auto it = object->find(key);
    if (it == object->end() || !it->is_number())
        return fallback;
    const float value = it->get<float>();
    return value >= 0.0f && value <= max ? value : fallback;
}

std::string StringOr(const Json *object, const char *key)
{
    if (!object || !object->is_object())
        return {};
    const auto it = object->find(key);
    return it != object->end() && it->is_string() ? it->get<std::string>() : std::string();
}

// ---------------------------------------------------------------------------------
// Candidate text: the candidate font with its supplementary families as fallback.
// ---------------------------------------------------------------------------------
struct FontKey
{
    std::vector<std::wstring> families;
    float size;
    bool operator<(const FontKey &o) const
    {
        return size != o.size ? size < o.size : families < o.families;
    }
};

IDWriteTextFormat *CandidateFormat(const std::vector<std::wstring> &families, float size)
{
    static std::map<FontKey, ComPtr<IDWriteTextFormat>> cache;
    const FontKey key{families, size};
    auto it = cache.find(key);
    if (it != cache.end())
        return it->second.Get();
    IDWriteFactory *factory = DWriteFactory();
    ComPtr<IDWriteTextFormat> format;
    std::wstring primary = L"Microsoft YaHei";
    for (const auto &family : families)
    {
        if (IsFontInstalled(family))
        {
            primary = family;
            break;
        }
    }
    if (!factory ||
        FAILED(factory->CreateTextFormat(primary.c_str(), nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                         DWRITE_FONT_STRETCH_NORMAL, size, L"zh-cn", format.GetAddressOf())))
        return nullptr;
    format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    msimeui::ApplyFontFallback(factory, format.Get(), families);
    cache[key] = format;
    return format.Get();
}

// Ascent / descent / line gap of the primary family at 1px, which is what the browser's
// line box uses for the strut.
struct FontMetrics
{
    float ascent = 1.06f;
    float descent = 0.26f;
    float gap = 0.0f;
};

FontMetrics PrimaryMetrics(const std::vector<std::wstring> &families)
{
    static std::map<std::vector<std::wstring>, FontMetrics> cache;
    auto it = cache.find(families);
    if (it != cache.end())
        return it->second;
    FontMetrics metrics;
    ComPtr<IDWriteFontCollection> collection;
    if (DWriteFactory() && SUCCEEDED(DWriteFactory()->GetSystemFontCollection(collection.GetAddressOf())))
    {
        std::vector<std::wstring> candidates = families;
        candidates.push_back(L"Microsoft YaHei");
        for (const auto &family : candidates)
        {
            UINT32 index = 0;
            BOOL exists = FALSE;
            if (FAILED(collection->FindFamilyName(family.c_str(), &index, &exists)) || !exists)
                continue;
            ComPtr<IDWriteFontFamily> fontFamily;
            ComPtr<IDWriteFont> font;
            if (FAILED(collection->GetFontFamily(index, fontFamily.GetAddressOf())) ||
                FAILED(fontFamily->GetFirstMatchingFont(DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                                        DWRITE_FONT_STYLE_NORMAL, font.GetAddressOf())))
                continue;
            DWRITE_FONT_METRICS m{};
            font->GetMetrics(&m);
            const float em = static_cast<float>(m.designUnitsPerEm);
            metrics = {m.ascent / em, m.descent / em, m.lineGap / em};
            break;
        }
    }
    cache[families] = metrics;
    return metrics;
}

struct Run
{
    ComPtr<IDWriteTextLayout> layout;
    float width = 0.0f;
    float baseline = 0.0f; // from the layout's top
};

Run MakeRun(const std::wstring &text, const std::vector<std::wstring> &families, float size)
{
    Run run;
    IDWriteTextFormat *format = CandidateFormat(families, size);
    if (!format || !DWriteFactory())
        return run;
    DWriteFactory()->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), format, kUnbounded, kUnbounded,
                                      run.layout.GetAddressOf());
    if (!run.layout)
        return run;
    DWRITE_TEXT_METRICS metrics{};
    run.layout->GetMetrics(&metrics);
    run.width = metrics.widthIncludingTrailingWhitespace;
    DWRITE_LINE_METRICS line{};
    UINT32 count = 0;
    run.layout->GetLineMetrics(&line, 1, &count);
    run.baseline = count ? line.baseline : size;
    return run;
}

void DrawRun(msimeui::DeviceResources &dr, const Run &run, float x, float baselineY, const D2D1_COLOR_F &color)
{
    ID2D1RenderTarget *target = dr.GetRenderTarget();
    ID2D1SolidColorBrush *brush = target ? dr.GetSolidColorBrush(color) : nullptr;
    if (!run.layout || !brush || color.a <= 0.0f)
        return;
    target->DrawTextLayout(D2D1::Point2F(x, baselineY - run.baseline), run.layout.Get(), brush,
                           D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
}

// Rounded rect with independent corner radii (tl, tr, br, bl).
void FillCorners(msimeui::DeviceResources &dr, const RectF &r, float tl, float tr, float br, float bl,
                 const D2D1_COLOR_F &color)
{
    ID2D1RenderTarget *target = dr.GetRenderTarget();
    ID2D1SolidColorBrush *brush = target ? dr.GetSolidColorBrush(color) : nullptr;
    if (!brush || color.a <= 0.0f || r.width <= 0.0f || r.height <= 0.0f)
        return;
    if (tl == tr && tr == br && br == bl)
    {
        FillRounded(dr, r, tl, color);
        return;
    }
    const float limit = std::min(r.width, r.height) * 0.5f;
    tl = std::min(tl, limit);
    tr = std::min(tr, limit);
    br = std::min(br, limit);
    bl = std::min(bl, limit);
    ComPtr<ID2D1Factory> factory;
    target->GetFactory(factory.GetAddressOf());
    ComPtr<ID2D1PathGeometry> path;
    ComPtr<ID2D1GeometrySink> sink;
    if (!factory || FAILED(factory->CreatePathGeometry(path.GetAddressOf())) || FAILED(path->Open(sink.GetAddressOf())))
        return;
    const float l = r.x, t = r.y, rt = r.x + r.width, b = r.y + r.height;
    sink->BeginFigure(D2D1::Point2F(l + tl, t), D2D1_FIGURE_BEGIN_FILLED);
    sink->AddLine(D2D1::Point2F(rt - tr, t));
    if (tr > 0)
        sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(rt, t + tr), D2D1::SizeF(tr, tr), 0, D2D1_SWEEP_DIRECTION_CLOCKWISE,
                                      D2D1_ARC_SIZE_SMALL));
    sink->AddLine(D2D1::Point2F(rt, b - br));
    if (br > 0)
        sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(rt - br, b), D2D1::SizeF(br, br), 0, D2D1_SWEEP_DIRECTION_CLOCKWISE,
                                      D2D1_ARC_SIZE_SMALL));
    sink->AddLine(D2D1::Point2F(l + bl, b));
    if (bl > 0)
        sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(l, b - bl), D2D1::SizeF(bl, bl), 0, D2D1_SWEEP_DIRECTION_CLOCKWISE,
                                      D2D1_ARC_SIZE_SMALL));
    sink->AddLine(D2D1::Point2F(l, t + tl));
    if (tl > 0)
        sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(l + tl, t), D2D1::SizeF(tl, tl), 0, D2D1_SWEEP_DIRECTION_CLOCKWISE,
                                      D2D1_ARC_SIZE_SMALL));
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    sink->Close();
    target->FillGeometry(path.Get(), brush);
}

// Clip layer to a rounded rect; returns false when it could not be pushed.
bool PushRoundedClip(msimeui::DeviceResources &dr, const RectF &rect, float radius)
{
    ID2D1RenderTarget *target = dr.GetRenderTarget();
    if (!target)
        return false;
    ComPtr<ID2D1Factory> factory;
    target->GetFactory(factory.GetAddressOf());
    ComPtr<ID2D1RoundedRectangleGeometry> geometry;
    ComPtr<ID2D1Layer> layer;
    if (!factory ||
        FAILED(factory->CreateRoundedRectangleGeometry(D2D1::RoundedRect(ToD2D(rect), radius, radius),
                                                       geometry.GetAddressOf())) ||
        FAILED(target->CreateLayer(nullptr, layer.GetAddressOf())))
        return false;
    // A clip-only layer initialized from what is below keeps ClearType text working in it.
    target->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), geometry.Get(), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                            D2D1::IdentityMatrix(), 1.0f, nullptr,
                                            D2D1_LAYER_OPTIONS_INITIALIZE_FOR_CLEARTYPE),
                      layer.Get());
    return true;
}

ComPtr<IWICImagingFactory> WicFactory()
{
    static ComPtr<IWICImagingFactory> factory = [] {
        ComPtr<IWICImagingFactory> created;
        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(created.GetAddressOf()));
        return created;
    }();
    return factory;
}

// Draws `bitmap` into `dest` with high-quality scaling.
void DrawImage(msimeui::DeviceResources &dr, ID2D1Bitmap *bitmap, const RectF &dest, float opacity = 1.0f)
{
    ID2D1RenderTarget *target = dr.GetRenderTarget();
    if (!target || !bitmap)
        return;
    ComPtr<ID2D1DeviceContext> context;
    if (SUCCEEDED(target->QueryInterface(IID_PPV_ARGS(context.GetAddressOf()))))
        context->DrawBitmap(bitmap, ToD2D(dest), opacity, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
    else
        target->DrawBitmap(bitmap, ToD2D(dest), opacity);
}

// `background: url() center / <fit> no-repeat` placement of an image in a box.
RectF FitImage(const SizeF &image, const RectF &box, const std::string &fit)
{
    if (image.width <= 0.0f || image.height <= 0.0f)
        return box;
    if (fit == "stretch")
        return box;
    const float sx = box.width / image.width;
    const float sy = box.height / image.height;
    const float scale = fit == "contain" ? std::min(sx, sy) : std::max(sx, sy);
    const float w = image.width * scale;
    const float h = image.height * scale;
    return {box.x + (box.width - w) * 0.5f, box.y + (box.height - h) * 0.5f, w, h};
}

const wchar_t *kSampleWords[] = {L"你们", L"你", L"尼", L"妮", L"泥", L"逆", L"拟", L"腻", L"拧"};
const wchar_t *kSampleCodes[] = {L"(rR)", L"(rX)", L"(uV)", L"(nV)", L"(dV)", L"(zQ)", L"(fR)", L"(oD)", L"(nG)"};
} // namespace

bool SystemPrefersLight()
{
    DWORD value = 0;
    DWORD size = sizeof(value);
    return RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                        L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS &&
           value != 0;
}

bool ResolveSurfaceLight(const SettingsModel &model, const char *surfacePath)
{
    const std::string surface = model.GetString(surfacePath, "follow");
    if (surface == "light")
        return true;
    if (surface == "dark")
        return false;
    std::string mode = model.GetString("appearance.theme_mode", "dark");
    if (mode == "auto")
        mode = "system";
    if (mode == "system")
        return SystemPrefersLight();
    return mode == "light";
}

const std::vector<std::string> &BuiltinSkins()
{
    static const std::vector<std::string> skins = {"fluent",       "wechat",           "graphite",
                                                   "willow_green", "autumn_osmanthus", "microsoft"};
    return skins;
}

bool IsBuiltinSkin(const std::string &id)
{
    const auto &skins = BuiltinSkins();
    return std::find(skins.begin(), skins.end(), id) != skins.end();
}

const Json *FindExternalSkin(const SettingsModel &model, const std::string &id)
{
    const Json *skins = model.Find("appearance.external_candidate_skins");
    if (!skins || !skins->is_array())
        return nullptr;
    for (const auto &skin : *skins)
    {
        if (skin.is_object() && skin.value("id", std::string()) == id)
            return &skin;
    }
    return nullptr;
}

std::string SkinBase(const SettingsModel &model, const std::string &id)
{
    if (IsBuiltinSkin(id))
        return id;
    if (const Json *external = FindExternalSkin(model, id))
    {
        const std::string base = external->value("base", std::string("fluent"));
        return IsBuiltinSkin(base) ? base : "fluent";
    }
    return "fluent";
}

std::wstring SkinResourcePath(const SettingsModel &model, const std::string &id, const std::string &relative)
{
    std::filesystem::path root = std::filesystem::u8path(model.GetString("appearance.candidate_skin_directory"));
    if (root.empty())
        root = std::filesystem::path(CommonUtils::get_ime_data_path_w()) / L"skins";
    return (root / std::filesystem::u8path(id) / std::filesystem::u8path(relative)).lexically_normal().wstring();
}

CandidateFont ReadCandidateFont(const SettingsModel &model)
{
    CandidateFont font;
    const std::string primary = model.GetString("appearance.english_font_css_family");
    if (!primary.empty())
        font.primary = Widen(primary);
    if (const Json *fallbacks = model.Find("appearance.fallback_font_css_families"); fallbacks && fallbacks->is_array())
    {
        for (const auto &item : *fallbacks)
        {
            if (item.is_string() && !item.get<std::string>().empty())
                font.fallbacks.push_back(Widen(item.get<std::string>()));
        }
    }
    return font;
}

CandidateLook ResolveCandidateLook(const SettingsModel &model, const std::string &skinId, bool light, bool horizontal,
                                   float f)
{
    const std::string base = SkinBase(model, skinId);
    const Json *external = IsBuiltinSkin(skinId) ? nullptr : FindExternalSkin(model, skinId);
    CandidateLook k;
    k.light = light;

    // style-v.css / style-h.css: the fluent preview.
    if (light)
    {
        k.bg = Hex(0xFFFFFF);
        k.border = Rgba(0, 0, 0, 0.12f);
        k.text = Hex(0x1A1A1A);
        k.num = Rgba(26, 26, 26, 0.55f);
        k.hover = Hex(0xECECEC);
        k.selected = Hex(0xE8E8E8);
        k.shadowAlpha1 = 0.18f;
        k.shadowAlpha2 = 0.10f;
    }
    else
    {
        k.bg = Hex(0x202020);
        k.border = Hex(0x9B9B9B, 0x2E / 255.0f);
        k.text = Hex(0xE9E8E8);
        k.num = Hex(0xE9E8E8, 0x9D / 255.0f);
        k.hover = Hex(0x414141);
        k.selected = Hex(0x3E3E3E, 0xB9 / 255.0f);
    }
    // --accent-strong comes from the settings page itself unless the skin sets it.
    k.accent = CurrentPalette().accentStrong;
    k.borderWidth = 1.5f;
    k.radius = 6.0f;
    k.itemRadius = 4.0f;
    k.firstRadius = 4.0f;
    k.numScale = 0.8f;
    k.barWidth = 0.2f * f;
    k.barHeight = 0.8f * f;
    if (horizontal)
    {
        k.containerPad = {3.0f, 1.0f, 3.0f, 2.0f};
        k.pinyinPad = {};
        k.pinyinMarginTop = 2.0f;
        k.pinyinMarginBottom = 0.0f;
        k.pinyinFontSize = 14.0f;
        k.pinyinTextPad = {0.2f * 14.0f + 3.0f, 0.0f, 5.0f, 0.0f};
        k.candPad = {0.0f, 2.0f, 4.0f, 2.0f};
        k.candMarginTop = 2.0f;
        k.candMarginBottom = 1.0f;
        k.candTextPad = {0.2f * f + 3.0f, 0.0f, 4.0f, 0.0f};
        k.candLineHeight = 1.25f;
        k.numMarginRight = 0.0f;
        k.barTop = 0.53f;
        k.edgeRadius = 6.0f;
    }
    else
    {
        k.containerPad = {2.0f, 2.0f, 2.0f, 2.0f};
        k.pinyinPad = {2.0f, 0.0f, 12.0f, 0.0f};
        k.pinyinMarginTop = 2.0f;
        k.pinyinFontSize = f;
        k.pinyinTextPad = {0.2f * f + 3.0f, 0.0f, 0.0f, 0.0f};
        k.candPad = {2.0f, 1.0f, 12.0f, 1.0f};
        k.candMarginTop = 2.0f;
        k.candTextPad = {0.2f * f + 3.0f, 0.0f, 0.0f, 0.0f};
        k.candLineHeight = 1.35f;
        k.numMarginRight = 1.5f;
        k.barTop = 0.5f;
    }

    // skin.css: the built-in skins.
    if (base == "wechat")
    {
        k.accent = Hex(0x07C160);
        k.bg = light ? Hex(0xF7F7F7) : Hex(0x151515);
        k.border = light ? Hex(0xDEDEDE) : Hex(0x292929);
        k.text = light ? Hex(0x333333) : Hex(0xB7B7B7);
        k.num = light ? Hex(0x757575) : Hex(0x858585);
        k.hover = light ? Rgba(7, 193, 96, 0.14f) : Rgba(7, 193, 96, 0.32f);
        k.selected = Hex(0x07C160);
        k.containerPad = horizontal ? Thickness{2.0f, 0.0f, 2.0f, 1.0f} : Thickness{2.0f, 2.0f, 2.0f, 2.0f};
        k.radius = 5.0f;
        k.firstRadius = 5.0f;
        k.showBar = false;
        k.selectedText = k.selectedNum = Hex(0xFFFFFF);
        k.edgeRadius = horizontal ? 5.0f : 0.0f;
    }
    else if (base == "graphite")
    {
        k.bg = light ? Hex(0xFBFBFC) : Hex(0x1C1F23);
        k.border = light ? Hex(0xE2E5E9) : Hex(0x30353B);
        k.text = light ? Hex(0x586476) : Hex(0xAEB6C2);
        k.num = light ? Hex(0x8993A1) : Hex(0x707987);
        k.hover = light ? Rgba(31, 41, 55, 0.055f) : Rgba(255, 255, 255, 0.055f);
        k.selected = None();
        k.accent = light ? Hex(0x5F6B7A) : Hex(0x8993A0);
        k.containerPad = {5.0f, 3.0f, 5.0f, 3.0f};
        k.radius = 3.0f;
        k.itemRadius = k.firstRadius = 2.0f;
        k.showBar = false;
        k.selectedText = k.selectedNum = light ? Hex(0x111827) : Hex(0xF1F3F5);
        k.edgeRadius = 0.0f;
    }
    else if (base == "willow_green")
    {
        k.accent = light ? Hex(0x58B980) : Hex(0x65C98D);
        k.bg = light ? Hex(0xF4F5F3) : Hex(0x2D2F2E);
        k.border = None();
        k.text = light ? Hex(0x343936) : Hex(0xD8DBD8);
        k.num = light ? Hex(0x686F6A) : Hex(0xA6ABA7);
        k.hover = light ? Rgba(88, 185, 128, 0.16f) : Rgba(101, 201, 141, 0.22f);
        k.selected = k.accent;
        k.containerPad = {};
        k.borderWidth = 0.0f;
        k.radius = 9.0f;
        k.clip = true;
        k.itemRadius = k.firstRadius = 0.0f;
        k.showBar = false;
        k.selectedText = k.selectedNum = Hex(0xFFFFFF);
        k.edgeRadius = 0.0f;
        if (horizontal)
        {
            k.letterSpacing = -0.31f * f;
            k.pinyinPad = {8.0f, 6.0f, 10.0f, 3.0f};
            k.pinyinMarginTop = k.pinyinMarginBottom = 0.0f;
            k.candPad = {8.0f, 6.0f, 8.0f, 6.0f};
            k.candMarginTop = k.candMarginBottom = 0.0f;
        }
        else
        {
            k.pinyinPad = {8.0f, 6.0f, 12.0f, 4.0f};
            k.pinyinMarginTop = 0.0f;
            k.candPad = {8.0f, 4.0f, 14.0f, 4.0f};
            k.candMarginTop = 0.0f;
        }
    }
    else if (base == "autumn_osmanthus")
    {
        k.accent = light ? Hex(0xE6A817) : Hex(0xF97D0A);
        k.bg = light ? Hex(0xD6ECF0) : Hex(0x7D929F);
        k.border = None();
        k.text = light ? Hex(0x1F3138) : Hex(0xF5F8FA);
        k.num = light ? Hex(0x5B727B) : Hex(0xE1E8EC);
        k.hover = light ? Rgba(255, 227, 153, 0.55f) : Rgba(249, 125, 10, 0.30f);
        k.selected = light ? Hex(0xFFE399) : Hex(0xF97D0A);
        k.selectedText = k.selectedNum = light ? Hex(0x1A1A1A) : Hex(0xFFFFFF);
        k.containerPad = {5.0f, 5.0f, 5.0f, 5.0f};
        k.borderWidth = 0.0f;
        k.radius = 10.0f;
        k.itemRadius = k.firstRadius = 6.0f;
        k.showBar = false;
        if (horizontal)
        {
            k.letterSpacing = -0.31f * f;
            k.pinyinPad = {6.0f, 1.0f, 10.0f, 1.0f};
            k.pinyinMarginTop = 0.0f;
            k.pinyinMarginBottom = 2.0f;
            k.candPad = {6.0f, 4.0f, 6.0f, 4.0f};
            k.candMarginTop = k.candMarginBottom = 0.0f;
            k.wrapperMarginRight = 2.0f;
            k.edgeRadius = 10.0f;
        }
        else
        {
            k.pinyinPad = {6.0f, 1.0f, 12.0f, 3.0f};
            k.pinyinMarginTop = 0.0f;
            k.candPad = {4.0f, 3.0f, 14.0f, 3.0f};
            k.candMarginTop = 2.0f;
            k.lastBottomRadius = true;
            k.edgeRadius = 10.0f;
        }
    }
    else if (base == "microsoft")
    {
        k.accent = Hex(0xE183D9);
        k.bg = light ? Hex(0xF9F9F9) : Hex(0x2C2C2C);
        k.border = light ? Rgba(0, 0, 0, 0.1f) : Hex(0x1C1C1C);
        k.text = light ? Hex(0x1A1A1A) : Hex(0xFFFFFF);
        k.num = light ? Hex(0x5F5F5F) : Hex(0xCFCFCF);
        k.hover = light ? Hex(0xF0F0F0) : Hex(0x353535);
        k.selected = light ? Hex(0xEAEAEA) : Hex(0x383838);
        k.borderWidth = 1.0f;
        k.radius = 8.0f;
        k.containerPad = {0.3f * f, 0.3f * f, 0.3f * f, horizontal ? 0.15f * f : 0.3f * f};
        k.candPad = {0.4f * f, 0.12f * f, 0.3f * f, 0.12f * f};
        k.candMarginTop = k.candMarginBottom = 0.0f;
        k.candTextPad = {};
        k.candLineHeight = 1.35f;
        k.pinyinMarginTop = 0.0f;
        k.pinyinMarginBottom = 2.0f;
        k.numScale = 1.0f;
        k.numMarginRight = 0.2f * f;
        k.barWidth = 3.0f;
        k.barHeight = 0.9f * f;
        k.barTop = 0.5f;
        k.edgeRadius = 0.0f;
        k.triangleArrows = true;
        k.arrowGap = 0.0f;
        if (horizontal)
        {
            k.letterSpacing = -0.31f * f;
            k.wrapperMarginRight = 0.15f * f;
            k.wrapperMarginBottom = 0.15f * f;
            k.candMinWidth = 3.5f * f;
        }
        else
        {
            k.wrapperGap = 2.0f;
        }
    }
    k.bar = k.accent;

    // The generated CSS of an external skin (skin.ts candidatePreviewCss).
    if (external)
    {
        const Json *candidate = external->contains("candidate") ? &(*external)["candidate"] : nullptr;
        const Json *colors = candidate && candidate->is_object() && candidate->contains(light ? "light" : "dark")
                                 ? &(*candidate)[light ? "light" : "dark"]
                                 : nullptr;
        const D2D1_COLOR_F accent = ManifestColor(colors, "accent");
        if (Has(accent))
            k.accent = k.bar = accent;
        if (const auto c = ManifestColor(colors, "selected"); Has(c))
            k.selected = c;
        if (const auto c = ManifestColor(colors, "hover"); Has(c))
            k.hover = c;
        if (const auto c = ManifestColor(colors, "surface"); Has(c))
            k.bg = c;
        if (const auto c = ManifestColor(colors, "border"); Has(c))
            k.border = c;
        if (const auto c = ManifestColor(colors, "text"); Has(c))
            k.text = c;
        if (const auto c = ManifestColor(colors, "number"); Has(c))
            k.num = c;
        if (colors && colors->is_object() && colors->contains("showSelectedBar") &&
            (*colors)["showSelectedBar"].is_boolean() && !(*colors)["showSelectedBar"].get<bool>())
            k.showBar = false;
        k.candText = ManifestColor(colors, "candidateText");
        k.preeditText = ManifestColor(colors, "preeditText");
        if (const auto c = ManifestColor(colors, "preeditCaret"); Has(c))
            k.accent = c;
        if (const auto c = ManifestColor(colors, "selectedBar"); Has(c))
            k.bar = c;
        if (const auto c = ManifestColor(colors, "selectedText"); Has(c))
            k.selectedText = c;
        if (const auto c = ManifestColor(colors, "selectedNumber"); Has(c))
            k.selectedNum = c;
        k.preeditBg = ManifestColor(colors, "preeditBackground");
        k.preeditDivider = ManifestColor(colors, "preeditDivider");

        const float radius = NumberOr(external, "cornerRadiusDip", -1.0f, 32.0f);
        if (radius >= 0.0f)
        {
            k.radius = radius;
            if (base == "fluent" || base == "wechat")
                k.edgeRadius = horizontal ? radius : 0.0f;
            if (base == "autumn_osmanthus")
                k.edgeRadius = radius;
        }
        const float borderWidth = NumberOr(external, "borderWidthDip", -1.0f, 4.0f);
        if (borderWidth >= 0.0f)
        {
            k.borderWidth = borderWidth;
            const bool noFrame = base == "willow_green" || base == "autumn_osmanthus";
            if (!Has(ManifestColor(colors, "border")) && noFrame)
                k.border = None();
        }
        const float itemRadius = NumberOr(external, "itemCornerRadiusDip", -1.0f, 16.0f);
        if (itemRadius >= 0.0f)
            k.itemRadius = k.firstRadius = itemRadius;
        const std::string shadow = StringOr(external, "shadow");
        if (shadow == "none")
            k.shadowAlpha1 = k.shadowAlpha2 = 0.0f;
        else if (shadow == "soft" || shadow == "strong")
        {
            const float scale = shadow == "soft" ? 0.5f : 1.6f;
            k.shadowAlpha1 = std::min((light ? 0.18f : 0.34f) * scale, 1.0f);
            k.shadowAlpha2 = std::min((light ? 0.10f : 0.22f) * scale, 1.0f);
        }
        const std::string font = StringOr(external, "fontFamily");
        if (!font.empty() && font.size() <= 64)
            k.fontOverride = Widen(font);
        k.minWidth = NumberOr(external, "minWidthDip", 0.0f, 4096.0f);
        const std::string background = StringOr(external, "backgroundImage");
        if (!background.empty())
        {
            k.backgroundImage = SkinResourcePath(model, skinId, background);
            k.backgroundFit = StringOr(external, "backgroundFit");
            k.backgroundOpacity = NumberOr(external, "backgroundOpacity", 1.0f, 1.0f);
        }
        const std::string decoration = StringOr(external, "decorationImage");
        const float decorationTop = NumberOr(external, "decorationTopDip", 0.0f, 4096.0f);
        if (!decoration.empty() && decorationTop > 0.0f)
        {
            k.decorationImage = SkinResourcePath(model, skinId, decoration);
            k.decorationTop = decorationTop;
            k.decorationWidth = NumberOr(external, "decorationWidthDip", 0.0f, 4096.0f);
            k.decorationAlign = StringOr(external, "decorationAlign");
        }
    }
    return k;
}

ToolbarLook ResolveToolbarLook(const SettingsModel &model, const std::string &skinId, bool light)
{
    const std::string base = SkinBase(model, skinId);
    ToolbarLook t;
    t.bg = light ? Hex(0xFFFFFF) : Hex(0x1A1A1A);
    t.border = light ? Rgba(0, 0, 0, 0.12f) : Rgba(255, 255, 255, 0.15f);
    t.handle = Hex(0x8E8CD8);
    t.divider = light ? Rgba(0, 0, 0, 0.12f) : Rgba(255, 255, 255, 0.15f);
    t.icon = light ? Hex(0x1A1A1A) : Hex(0xFFFFFF);
    t.shadows = {{8.0f, 10.0f, 24.0f, light ? 0.18f : 0.34f}, {2.0f, 3.0f, 8.0f, light ? 0.10f : 0.22f}};
    if (base == "wechat")
    {
        t.bg = light ? Hex(0xF7F7F7) : Hex(0x151515);
        t.border = light ? Hex(0xDEDEDE) : Hex(0x292929);
        t.shadows = {{0.0f, 5.0f, 14.0f, light ? 0.12f : 0.38f}};
        t.handle = Hex(0x07C160);
    }
    else if (base == "graphite")
    {
        t.bg = light ? Hex(0xFBFBFC) : Hex(0x1C1F23);
        t.border = light ? Hex(0xE2E5E9) : Hex(0x30353B);
        t.radius = 4.0f;
        t.shadows = {{0.0f, 2.0f, 8.0f, light ? 0.10f : 0.28f}};
        t.handle = light ? Hex(0x5F6B7A) : Hex(0x8993A0);
        t.iconRadius = 3.0f;
        t.icon = light ? Hex(0x374151) : Hex(0xD7DCE2);
    }
    else if (base == "willow_green")
    {
        t.bg = light ? Hex(0xF4F5F3) : Hex(0x2D2F2E);
        t.border = light ? Hex(0xDFE3DF) : Hex(0x3B3E3C);
        t.radius = 9.0f;
        t.shadows = {{0.0f, 7.0f, 18.0f, light ? 0.16f : 0.34f}};
        t.handle = light ? Hex(0x58B980) : Hex(0x65C98D);
    }
    else if (base == "autumn_osmanthus")
    {
        t.bg = light ? Hex(0xD6ECF0) : Hex(0x7D929F);
        t.border = light ? Hex(0xBCD8DE) : Hex(0x8FA3AF);
        t.radius = 10.0f;
        t.shadows = {{0.0f, 7.0f, 18.0f, light ? 0.16f : 0.34f}};
        t.handle = light ? Hex(0xE6A817) : Hex(0xF97D0A);
    }
    else if (base == "microsoft")
    {
        t.handle = Hex(0xE183D9);
    }
    if (const Json *external = IsBuiltinSkin(skinId) ? nullptr : FindExternalSkin(model, skinId))
    {
        const Json *toolbar = external->contains("toolbar") ? &(*external)["toolbar"] : nullptr;
        const Json *colors = toolbar && toolbar->is_object() && toolbar->contains(light ? "light" : "dark")
                                 ? &(*toolbar)[light ? "light" : "dark"]
                                 : nullptr;
        if (const auto c = ManifestColor(colors, "background"); Has(c))
            t.bg = c;
        if (const auto c = ManifestColor(colors, "border"); Has(c))
            t.border = c;
        if (const auto c = ManifestColor(colors, "handle"); Has(c))
            t.handle = c;
        if (const auto c = ManifestColor(colors, "divider"); Has(c))
            t.divider = c;
        if (const auto c = ManifestColor(colors, "icon"); Has(c))
            t.icon = c;
        const float radius = NumberOr(external, "toolbarCornerRadiusDip", -1.0f, 32.0f);
        if (radius >= 0.0f)
            t.radius = radius;
    }
    return t;
}

ID2D1Bitmap *CachedBitmap(msimeui::DeviceResources &dr, const std::wstring &path, D2D1_SIZE_F *size)
{
    struct Entry
    {
        ID2D1RenderTarget *target = nullptr;
        ComPtr<ID2D1Bitmap> bitmap;
        bool failed = false;
    };
    static std::map<std::wstring, Entry> cache;
    ID2D1RenderTarget *target = dr.GetRenderTarget();
    if (!target || path.empty())
        return nullptr;
    Entry &entry = cache[path];
    if (entry.target != target)
    {
        entry = Entry{};
        entry.target = target;
        ComPtr<IWICImagingFactory> wic = WicFactory();
        ComPtr<IWICBitmapDecoder> decoder;
        ComPtr<IWICBitmapFrameDecode> frame;
        ComPtr<IWICFormatConverter> converter;
        if (!wic ||
            FAILED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad,
                                                  decoder.GetAddressOf())) ||
            FAILED(decoder->GetFrame(0, frame.GetAddressOf())) ||
            FAILED(wic->CreateFormatConverter(converter.GetAddressOf())) ||
            FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr,
                                         0.0, WICBitmapPaletteTypeMedianCut)) ||
            FAILED(target->CreateBitmapFromWicBitmap(converter.Get(), nullptr, entry.bitmap.GetAddressOf())))
            entry.failed = true;
    }
    if (!entry.bitmap)
        return nullptr;
    if (size)
    {
        UINT w = 0, h = 0;
        D2D1_SIZE_U pixels = entry.bitmap->GetPixelSize();
        w = pixels.width;
        h = pixels.height;
        *size = D2D1::SizeF(static_cast<float>(w), static_cast<float>(h));
    }
    return entry.bitmap.Get();
}

SizeF ImageFileSize(const std::wstring &path)
{
    static std::map<std::wstring, SizeF> cache;
    auto it = cache.find(path);
    if (it != cache.end())
        return it->second;
    SizeF size{};
    ComPtr<IWICImagingFactory> wic = WicFactory();
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    UINT w = 0, h = 0;
    if (wic &&
        SUCCEEDED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand,
                                                 decoder.GetAddressOf())) &&
        SUCCEEDED(decoder->GetFrame(0, frame.GetAddressOf())) && SUCCEEDED(frame->GetSize(&w, &h)))
        size = {static_cast<float>(w), static_cast<float>(h)};
    cache[path] = size;
    return size;
}

// ---------------------------------------------------------------------------------
// CandidatePreview
// ---------------------------------------------------------------------------------
struct CandidatePreview::Impl
{
    CandidateLook look;
    CandidateFont font;
    Options options;
    bool built = false;
    std::vector<std::wstring> families;
    FontMetrics metrics;
    Run pinyin;
    Run hiddenPinyin;
    float cursorHeight = 0.0f;
    std::vector<Run> numbers;
    std::vector<Run> contents;
    float spaceWidth = 0.0f;

    struct Item
    {
        RectF box; // .row.cand, relative to the container's border box
        float textX = 0.0f;
        float baseline = 0.0f;
        bool first = false;
        bool last = false;
        bool lineStart = false;
    };

    struct Geometry
    {
        float width = 0.0f;  // container border box
        float height = 0.0f; // container border box
        RectF pinyinRow;
        float pinyinTextX = 0.0f;
        float pinyinBaseline = 0.0f;
        float hiddenX = 0.0f;
        std::vector<Item> items;
        bool arrows = false;
        PointF arrowOrigin;
        float arrowWidth = 0.0f, arrowHeight = 0.0f;
        float arrowDividerX = -1.0f, arrowDividerTop = 0.0f, arrowDividerHeight = 0.0f;
    };

    Geometry geometry;
    float laidOutFor = -1.0f;
    PointF origin; // container top-left in window space
    size_t hovered = static_cast<size_t>(-1);
    msimeui::ShadowCache shadows[2];

    void Build()
    {
        if (built)
            return;
        built = true;
        families.clear();
        if (!look.fontOverride.empty())
            families.push_back(look.fontOverride);
        families.push_back(font.primary);
        for (const auto &f : font.fallbacks)
            families.push_back(f);
        metrics = PrimaryMetrics(families);
        const float f = font.size;
        pinyin = MakeRun(L"ni'mf", families, look.pinyinFontSize);
        hiddenPinyin = MakeRun(L"n", families, look.pinyinFontSize);
        cursorHeight = 1.2f * look.pinyinFontSize;
        numbers.clear();
        contents.clear();
        const size_t count = std::min<size_t>(options.pageSize, std::size(kSampleWords));
        for (size_t i = 0; i < count; ++i)
        {
            numbers.push_back(MakeRun(std::to_wstring(i + 1), families, f * look.numScale));
            std::wstring content = kSampleWords[i];
            if (options.helpcode)
                content += kSampleCodes[i];
            contents.push_back(MakeRun(content, families, f));
        }
        Run space = MakeRun(L" ", families, f);
        spaceWidth = space.width;
        laidOutFor = -1.0f;
    }

    // Line box of a run of text in the strut of the primary font: returns height, sets
    // the baseline offset.
    float LineBox(float size, float lineHeight, float &baseline) const
    {
        const float ascent = metrics.ascent * size;
        const float descent = metrics.descent * size;
        if (lineHeight <= 0.0f)
        {
            const float half = metrics.gap * size * 0.5f;
            baseline = half + ascent;
            return ascent + descent + metrics.gap * size;
        }
        const float line = lineHeight * size;
        baseline = (line - (ascent + descent)) * 0.5f + ascent;
        return line;
    }

    float PinyinBlock(float &baseline) const
    {
        // .text with the 1.2em caret sitting on the baseline (inline-block, no content).
        const float size = look.pinyinFontSize;
        const float half = metrics.gap * size * 0.5f;
        const float above = std::max(metrics.ascent * size + half, cursorHeight);
        const float below = metrics.descent * size + half;
        baseline = above;
        return above + below;
    }

    float ItemTextWidth(size_t i) const
    {
        return numbers[i].width + look.numMarginRight + contents[i].width;
    }

    float BorderWidth() const
    {
        return look.borderWidth > 0.0f ? CssBorder(look.borderWidth) : 0.0f;
    }

    float ArrowWidth() const
    {
        const float f = font.size;
        if (look.triangleArrows)
            return 2.0f * (options.horizontal ? 0.96f : 0.81f) * f + look.arrowGap;
        return 2.0f * 0.72f * f + look.arrowGap;
    }

    float ArrowHeight() const
    {
        const float f = font.size;
        if (look.triangleArrows)
            return (options.horizontal ? 1.59f : 1.2f) * f;
        return 0.9f * f;
    }

    void Layout(float availableWidth)
    {
        if (laidOutFor == availableWidth)
            return;
        laidOutFor = availableWidth;
        Geometry g;
        const float f = font.size;
        const float b = BorderWidth();
        const Thickness pad = look.containerPad;
        const float minWidth = std::max({7.0f * f, look.minWidth, look.decorationWidth});
        float pinyinBaseline = 0.0f;
        const float pinyinTextHeight = PinyinBlock(pinyinBaseline);
        float hiddenBaseline = 0.0f;
        const float hiddenHeight = LineBox(look.pinyinFontSize, 0.0f, hiddenBaseline);
        const float pinyinRowHeight =
            std::max(pinyinTextHeight, hiddenHeight) + look.pinyinPad.top + look.pinyinPad.bottom;
        const float pinyinTextWidth = look.pinyinTextPad.left + pinyin.width + 1.0f + 1.5f + look.pinyinTextPad.right;
        const float pinyinRowWidth = look.pinyinPad.left + pinyinTextWidth + hiddenPinyin.width + look.pinyinPad.right;
        float candBaseline = 0.0f;
        const float candLine = LineBox(f, look.candLineHeight, candBaseline);
        const float candHeight = candLine + look.candPad.top + look.candPad.bottom;
        const size_t count = contents.size();

        float y = b + pad.top;
        if (options.preedit)
        {
            y += look.pinyinMarginTop;
            g.pinyinRow = {0.0f, y, 0.0f, pinyinRowHeight};
            const float textTop =
                y + look.pinyinPad.top +
                (pinyinRowHeight - look.pinyinPad.top - look.pinyinPad.bottom - pinyinTextHeight) * 0.5f;
            g.pinyinBaseline = textTop + pinyinBaseline;
            y += pinyinRowHeight + look.pinyinMarginBottom;
        }

        if (!options.horizontal)
        {
            float innerWidth = options.preedit ? pinyinRowWidth : 0.0f;
            for (size_t i = 0; i < count; ++i)
            {
                const float rowWidth = look.candPad.left + look.candTextPad.left + ItemTextWidth(i) +
                                       look.candTextPad.right + look.candPad.right;
                innerWidth = std::max(innerWidth, rowWidth);
            }
            const bool arrows = options.pageArrows;
            g.width = std::clamp(innerWidth + (pad.left + pad.right) + b * 2.0f, minWidth, 720.0f);
            const float contentX = b + pad.left;
            const float contentWidth = g.width - b * 2.0f - pad.left - pad.right;
            g.pinyinRow.x = contentX;
            g.pinyinRow.width = contentWidth;
            g.pinyinTextX = contentX + look.pinyinPad.left + look.pinyinTextPad.left;
            g.hiddenX = contentX + contentWidth - look.pinyinPad.right - hiddenPinyin.width;
            for (size_t i = 0; i < count; ++i)
            {
                if (i > 0)
                    y += look.wrapperGap;
                y += look.candMarginTop;
                Item item;
                item.box = {contentX, y, contentWidth, candHeight};
                item.textX = contentX + look.candPad.left + look.candTextPad.left;
                item.baseline = y + look.candPad.top + candBaseline;
                item.first = i == 0;
                item.last = i + 1 == count;
                g.items.push_back(item);
                y += candHeight + look.candMarginBottom;
            }
            if (arrows)
            {
                // .page-arrows-row: margin-top -1px, line height 0, only the arrows' height.
                y += -1.0f;
                const float left = look.triangleArrows
                                       ? contentX + look.candPad.left - 0.185f * f
                                       : contentX + look.candPad.left + look.candTextPad.left - 0.22f * f;
                g.arrows = true;
                g.arrowOrigin = {left, y};
                g.arrowWidth = ArrowWidth();
                g.arrowHeight = ArrowHeight();
                y += g.arrowHeight + (look.triangleArrows ? 0.0f : 1.0f);
            }
            g.height = y + pad.bottom + b;
        }
        else
        {
            // Inline-block items separated by the collapsed space of the markup.
            const float gap = spaceWidth + look.letterSpacing;
            std::vector<float> widths;
            for (size_t i = 0; i < count; ++i)
            {
                float w = look.candPad.left + look.candTextPad.left + ItemTextWidth(i) + look.candTextPad.right +
                          look.candPad.right;
                w = std::max(w, look.candMinWidth);
                widths.push_back(w);
            }
            const float wrapperHeight =
                look.candMarginTop + candHeight + look.candMarginBottom + look.wrapperMarginBottom;
            float strutBaseline = 0.0f;
            const float strut = LineBox(f, 0.0f, strutBaseline);
            const float lineHeight = std::max(wrapperHeight, strut);
            float maxContent = 0.0f;
            for (size_t i = 0; i < count; ++i)
                maxContent += widths[i] + (i + 1 < count ? look.wrapperMarginRight : 0.0f) + (i > 0 ? gap : 0.0f);
            float arrowsWidth = 0.0f;
            if (options.pageArrows)
            {
                arrowsWidth = gap + ArrowWidth() + (look.triangleArrows ? 0.3f * f + 0.12f * f + 1.0f : 2.0f + 1.0f);
                maxContent += arrowsWidth;
            }
            const float frame = pad.left + pad.right + b * 2.0f;
            const float natural = std::max(options.preedit ? pinyinRowWidth : 0.0f, maxContent);
            g.width = std::max(minWidth, std::min(natural + frame, availableWidth));
            const float contentX = b + pad.left;
            const float contentWidth = std::max(g.width - frame, 0.0f);
            g.pinyinRow.x = contentX;
            g.pinyinRow.width = contentWidth;
            g.pinyinTextX = contentX + look.pinyinPad.left + look.pinyinTextPad.left;
            g.hiddenX = contentX + contentWidth - look.pinyinPad.right - hiddenPinyin.width;

            float x = 0.0f;
            float lineTop = y;
            bool lineStart = true;
            for (size_t i = 0; i < count; ++i)
            {
                const float advance = (lineStart ? 0.0f : gap) + widths[i];
                if (!lineStart && x + advance > contentWidth + 0.5f)
                {
                    lineTop += lineHeight;
                    x = 0.0f;
                    lineStart = true;
                }
                if (!lineStart)
                    x += gap;
                Item item;
                item.box = {contentX + x, lineTop + look.candMarginTop, widths[i], candHeight};
                item.textX = item.box.x + look.candPad.left + look.candTextPad.left;
                item.baseline = item.box.y + look.candPad.top + candBaseline;
                item.first = i == 0;
                item.last = i + 1 == count;
                item.lineStart = lineStart;
                g.items.push_back(item);
                x += widths[i] + (i + 1 < count ? look.wrapperMarginRight : 0.0f);
                lineStart = false;
            }
            float bottom = lineTop + lineHeight;
            if (options.pageArrows)
            {
                const float width = ArrowWidth();
                const float height = ArrowHeight();
                float ax = x + gap;
                if (ax + arrowsWidth - gap > contentWidth + 0.5f)
                {
                    lineTop = bottom;
                    bottom += lineHeight;
                    ax = 0.0f;
                }
                g.arrows = true;
                g.arrowWidth = width;
                g.arrowHeight = height;
                if (look.triangleArrows)
                {
                    // margin 0 0 .15em .3em, padding-left .12em, border-left 1px, top aligned.
                    g.arrowDividerX = contentX + ax + 0.3f * f;
                    g.arrowDividerTop = lineTop;
                    g.arrowDividerHeight = height;
                    g.arrowOrigin = {g.arrowDividerX + 1.0f + 0.12f * f, lineTop};
                }
                else
                {
                    // vertical-align: bottom; margin 0 1px .25em 2px
                    g.arrowOrigin = {contentX + ax + 2.0f, bottom - 0.25f * f - height};
                }
            }
            g.height = bottom + pad.bottom + b;
        }
        geometry = g;
    }
};

CandidatePreview::CandidatePreview() : impl_(std::make_shared<Impl>())
{
}

void CandidatePreview::Set(const CandidateLook &look, const CandidateFont &font, const Options &options)
{
    impl_->look = look;
    impl_->font = font;
    impl_->options = options;
    impl_->built = false;
    InvalidateMeasure();
    InvalidateVisual();
}

SizeF CandidatePreview::Measure(const SizeF &availableSize)
{
    impl_->Build();
    impl_->Layout(availableSize.width);
    const float decoration = impl_->look.decorationTop;
    return {std::min(impl_->geometry.width, availableSize.width), impl_->geometry.height + decoration};
}

void CandidatePreview::Arrange(const RectF &finalRect)
{
    bounds_ = finalRect;
}

void CandidatePreview::Render(msimeui::DeviceResources &dr)
{
    Impl &m = *impl_;
    m.Build();
    m.Layout(m.laidOutFor >= 0.0f ? m.laidOutFor : bounds_.width);
    const auto &g = m.geometry;
    const CandidateLook &k = m.look;
    const float f = m.font.size;
    ID2D1RenderTarget *target = dr.GetRenderTarget();
    if (!target)
        return;

    const float x0 = bounds_.x;
    const float y0 = bounds_.y + k.decorationTop;
    m.origin = {x0, y0};
    const RectF box = {x0, y0, g.width, g.height};

    // Decoration above the frame, aligned to its left, center or right edge.
    if (!k.decorationImage.empty())
    {
        D2D1_SIZE_F size{};
        if (ID2D1Bitmap *bitmap = CachedBitmap(dr, k.decorationImage, &size))
        {
            const float w = k.decorationWidth;
            float dx = x0 + g.width - w;
            if (k.decorationAlign == "left")
                dx = x0;
            else if (k.decorationAlign == "center")
                dx = x0 + (g.width - w) * 0.5f;
            const RectF slot = {dx, bounds_.y, w, k.decorationTop};
            DrawImage(dr, bitmap, FitImage({size.width, size.height}, slot, "contain"));
        }
    }

    if (k.shadowAlpha1 > 0.0f)
    {
        const msimeui::ShadowPass first = {12.0f, k.shadowAlpha1, 8.0f, 10.0f};
        const msimeui::ShadowPass second = {4.0f, k.shadowAlpha2, 2.0f, 3.0f};
        m.shadows[0].Render(target, box, k.radius, &first, 1);
        m.shadows[1].Render(target, box, k.radius, &second, 1);
    }
    FillRounded(dr, box, k.radius, k.bg);
    if (!k.backgroundImage.empty())
    {
        D2D1_SIZE_F size{};
        if (ID2D1Bitmap *bitmap = CachedBitmap(dr, k.backgroundImage, &size))
        {
            if (PushRoundedClip(dr, box, k.radius))
            {
                DrawImage(dr, bitmap, FitImage({size.width, size.height}, box, k.backgroundFit));
                // A veil of the surface at (1 - opacity), as the page does.
                FillRect(dr, box, WithAlpha(k.bg, 1.0f - k.backgroundOpacity));
                target->PopLayer();
            }
        }
    }
    const bool clipped = k.clip && PushRoundedClip(dr, box, k.radius);

    const D2D1_COLOR_F candText = Has(k.candText) ? k.candText : k.text;
    if (m.options.preedit)
    {
        const RectF row = {x0 + g.pinyinRow.x, y0 + g.pinyinRow.y, g.pinyinRow.width, g.pinyinRow.height};
        if (Has(k.preeditBg))
            FillRounded(dr, row, k.itemRadius, k.preeditBg);
        if (Has(k.preeditDivider))
            FillRect(dr, {row.x, row.y + row.height - CssBorder(), row.width, CssBorder()}, k.preeditDivider);
        const D2D1_COLOR_F pinyinColor = Has(k.preeditText) ? k.preeditText : k.text;
        DrawRun(dr, m.pinyin, x0 + g.pinyinTextX, y0 + g.pinyinBaseline, pinyinColor);
        // .cursor: 1.5px x 1.2em after 1px, shifted down 10% of its height.
        const float cx = x0 + g.pinyinTextX + m.pinyin.width + 1.0f;
        const float cy = y0 + g.pinyinBaseline - m.cursorHeight + m.cursorHeight * 0.1f;
        FillRect(dr, {cx, cy, 1.5f, m.cursorHeight}, k.accent);
    }

    const size_t count = g.items.size();
    for (size_t i = 0; i < count; ++i)
    {
        const auto &item = g.items[i];
        const RectF r = {x0 + item.box.x, y0 + item.box.y, item.box.width, item.box.height};
        D2D1_COLOR_F fill = item.first ? k.selected : (i == m.hovered ? k.hover : None());
        if (Has(fill))
        {
            float radius = item.first ? k.firstRadius : k.itemRadius;
            float tl = radius, tr = radius, br = radius, bl = radius;
            if (m.options.horizontal && k.edgeRadius > 0.0f)
            {
                if (item.first)
                    bl = k.edgeRadius;
                if (item.last)
                    br = k.edgeRadius;
            }
            if (!m.options.horizontal && k.lastBottomRadius && item.last)
                br = bl = k.edgeRadius;
            FillCorners(dr, r, tl, tr, br, bl, fill);
        }
        if (item.first && k.showBar)
        {
            // ::before: centered on the wrapper (vertical) or on .first (horizontal).
            const float barX = r.x;
            const float barY = r.y + r.height * k.barTop - k.barHeight * 0.5f;
            FillRounded(dr, {barX, barY, k.barWidth, k.barHeight}, std::min(k.barWidth * 0.5f, 8.0f), k.bar);
        }
        const D2D1_COLOR_F textColor = item.first && Has(k.selectedText) ? k.selectedText : candText;
        const D2D1_COLOR_F numColor = item.first && Has(k.selectedNum) ? k.selectedNum : k.num;
        DrawRun(dr, m.numbers[i], x0 + item.textX, y0 + item.baseline, numColor);
        DrawRun(dr, m.contents[i], x0 + item.textX + m.numbers[i].width + k.numMarginRight, y0 + item.baseline,
                textColor);
    }

    if (g.arrows)
    {
        const float w = (g.arrowWidth - k.arrowGap) * 0.5f;
        const float h = g.arrowHeight;
        if (g.arrowDividerX >= 0.0f)
        {
            // --ms-divider
            FillRect(dr, {x0 + g.arrowDividerX, y0 + g.arrowDividerTop, CssBorder(), g.arrowDividerHeight},
                     k.light ? Hex(0xE0E0E0) : Hex(0x3D3D3D));
        }
        for (int a = 0; a < 2; ++a)
        {
            const float ax = x0 + g.arrowOrigin.x + a * (w + k.arrowGap);
            const float ay = y0 + g.arrowOrigin.y;
            const bool disabled = a == 0;
            const PointF center = {ax + w * 0.5f, ay + h * 0.5f};
            if (k.triangleArrows)
            {
                D2D1_COLOR_F color = WithAlpha(candText, disabled ? 0.25f : 1.0f);
                ID2D1SolidColorBrush *brush = dr.GetSolidColorBrush(color);
                ComPtr<ID2D1Factory> factory;
                target->GetFactory(factory.GetAddressOf());
                ComPtr<ID2D1PathGeometry> path;
                ComPtr<ID2D1GeometrySink> sink;
                if (brush && factory && SUCCEEDED(factory->CreatePathGeometry(path.GetAddressOf())) &&
                    SUCCEEDED(path->Open(sink.GetAddressOf())))
                {
                    // The skin's 10x12 triangle path, fitted into 0.44em x 0.52em.
                    const float tw = 0.44f * f, th = 0.52f * f;
                    const float sx = tw / 10.0f, sy = th / 12.0f;
                    const float left = center.x - tw * 0.5f, top = center.y - th * 0.5f;
                    auto P = [&](float px, float py) {
                        const float lx = a == 0 ? 10.0f - px : px;
                        return D2D1::Point2F(left + lx * sx, top + py * sy);
                    };
                    sink->BeginFigure(P(6.14f, 3.69f), D2D1_FIGURE_BEGIN_FILLED);
                    sink->AddLine(P(3.86f, 2.31f));
                    sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(P(0.0f, 0.0f), P(0.0f, 4.5f)));
                    sink->AddLine(P(0.0f, 7.5f));
                    sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(P(0.0f, 12.0f), P(3.86f, 9.69f)));
                    sink->AddLine(P(6.14f, 8.31f));
                    sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(P(10.0f, 6.0f), P(6.14f, 3.69f)));
                    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                    sink->Close();
                    target->FillGeometry(path.Get(), brush);
                }
            }
            else
            {
                // A 0.28em box with left/bottom borders, rotated 45deg (prev) or -135deg (next).
                const D2D1_COLOR_F color = WithAlpha(k.num, disabled ? 0.3f : 1.0f);
                const float side = 0.28f * f;
                const float stroke = std::max(1.2f, 0.075f * f);
                ID2D1SolidColorBrush *brush = dr.GetSolidColorBrush(color);
                if (brush)
                {
                    D2D1_MATRIX_3X2_F previous;
                    target->GetTransform(&previous);
                    const float shift = (a == 0 ? 0.07f : -0.07f) * f;
                    target->SetTransform(D2D1::Matrix3x2F::Rotation(a == 0 ? 45.0f : -135.0f) *
                                         D2D1::Matrix3x2F::Translation(center.x + shift, center.y) * previous);
                    const float half = side * 0.5f;
                    target->FillRectangle(D2D1::RectF(-half, -half, -half + stroke, half), brush);
                    target->FillRectangle(D2D1::RectF(-half + stroke, half - stroke, half, half), brush);
                    target->SetTransform(previous);
                }
            }
        }
    }

    if (clipped)
        target->PopLayer();
    if (Has(k.border) && k.borderWidth > 0.0f)
        StrokeRounded(dr, box, k.radius, k.border, k.borderWidth);
}

bool CandidatePreview::OnMouseMove(const POINT &point, WPARAM)
{
    if (!window_)
        return false;
    const PointF p = window_->ClientPixelsToDips(point);
    size_t hot = static_cast<size_t>(-1);
    const auto &items = impl_->geometry.items;
    for (size_t i = 0; i < items.size(); ++i)
    {
        const RectF r = {impl_->origin.x + items[i].box.x, impl_->origin.y + items[i].box.y, items[i].box.width,
                         items[i].box.height};
        if (Contains(r, p))
            hot = i;
    }
    if (hot != impl_->hovered)
    {
        impl_->hovered = hot;
        InvalidateVisual();
    }
    return true;
}

void CandidatePreview::OnMouseLeave()
{
    if (impl_->hovered != static_cast<size_t>(-1))
    {
        impl_->hovered = static_cast<size_t>(-1);
        InvalidateVisual();
    }
}
} // namespace skinpreview
} // namespace native_settings
