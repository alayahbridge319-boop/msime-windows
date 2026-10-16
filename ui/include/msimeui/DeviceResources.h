#pragma once

#include <d2d1.h>
#include <d2d1_1.h>
#include <d3d11.h>
#include <dcomp.h>
#include <dxgi1_2.h>
#include <dwrite.h>
#include <wincodec.h>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>
#include <wrl/client.h>

#include "Types.h"

namespace msimeui
{
class DeviceResources
{
  public:
    // Marks the start of a frame. Brushes not used since the previous frame become
    // evictable, and the clip stack used for culling starts over.
    void BeginFrame();

    // Axis-aligned clip that also feeds IsVisible(), so containers can skip children
    // that cannot reach the screen. Pair every PushClip with a PopClip.
    void PushClip(const RectF &rect, D2D1_ANTIALIAS_MODE mode = D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    void PopClip();
    // False only when `rect` is entirely outside the current clip.
    bool IsVisible(const RectF &rect) const;

    bool EnsureFactories();
    bool EnsureForWindow(HWND hwnd);
    bool EnsureForComposition(HWND hwnd);
    // Starts creating the Direct3D device on a background thread; the next composition
    // target created in the process takes it instead of creating its own. Bringing up
    // the GPU driver takes a few hundred milliseconds, so a window that calls this early
    // gets that time back on its first frame.
    static void PrewarmComposition();
    // An opaque composition surface ignores alpha, which lets Direct2D draw ClearType
    // text. Takes effect when the composition surface is next created.
    void SetCompositionOpaque(bool opaque);
    void Resize(UINT width, UINT height);
    void DiscardTarget();
    HRESULT Present();

    /// Rendering DPI override for windows whose system DPI diverges from the
    /// content's true scale (e.g. an RDP session syncs the client's display
    /// scaling into the session's DPI metadata while the focused application
    /// still renders at 96 DPI). Pass 0 (or any non-positive value) to clear
    /// the override and fall back to GetDpiForWindow(hwnd). While set, the
    /// override is the single DPI source so window sizing and rendering stay
    /// on the same scale; a live render target is re-DPI'd immediately
    /// because target recreation can be skipped for same-size compositions.
    void SetDpiOverride(FLOAT dpi);
    ID2D1RenderTarget *GetRenderTarget() const;
    ID2D1DeviceContext *GetDeviceContext() const;
    IDWriteFactory *GetDWriteFactory() const;
    ID2D1SolidColorBrush *GetSolidColorBrush(const D2D1_COLOR_F &color);
    IDWriteTextFormat *GetTextFormat(const std::wstring &fontFamily, float fontSize, DWRITE_FONT_WEIGHT fontWeight,
                                     DWRITE_TEXT_ALIGNMENT textAlignment, DWRITE_PARAGRAPH_ALIGNMENT paragraphAlignment,
                                     DWRITE_WORD_WRAPPING wordWrapping);
    ID2D1Bitmap *GetBitmapFromFile(const std::wstring &filePath, D2D1_SIZE_F *size = nullptr);
    // Rasterizes an SVG document (UTF-8 markup) at `sizeDips` for the current DPI and
    // caches the bitmap. Needs ID2D1DeviceContext5 (Windows 10 1703+); null otherwise.
    ID2D1Bitmap *GetSvgBitmap(const std::string &markup, const SizeF &sizeDips);
    bool UsesComposition() const;

  private:
    struct BrushCacheEntry
    {
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
        uint64_t lastFrame = 0;
    };

    struct SvgCacheEntry
    {
        size_t markupHash = 0;
        std::string markup;
        SizeF size = {};
        float dpi = 0.0f;
        Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap;
    };

    struct TextFormatCacheEntry
    {
        std::wstring fontFamily;
        float fontSize = 0.0f;
        DWRITE_FONT_WEIGHT fontWeight = DWRITE_FONT_WEIGHT_NORMAL;
        DWRITE_TEXT_ALIGNMENT textAlignment = DWRITE_TEXT_ALIGNMENT_LEADING;
        DWRITE_PARAGRAPH_ALIGNMENT paragraphAlignment = DWRITE_PARAGRAPH_ALIGNMENT_NEAR;
        DWRITE_WORD_WRAPPING wordWrapping = DWRITE_WORD_WRAPPING_WRAP;
        Microsoft::WRL::ComPtr<IDWriteTextFormat> format;
    };

    struct BitmapCacheEntry
    {
        std::wstring filePath;
        D2D1_SIZE_F size = {};
        Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap;
    };

    static constexpr size_t kMaxCachedBitmaps = 4;
    static constexpr size_t kMaxCachedSvgBitmaps = 96;
    // Animated colors produce a new brush every frame; past this many, brushes left
    // over from earlier frames are dropped.
    static constexpr size_t kBrushCacheSoftLimit = 256;

    static uint64_t ColorKey(const D2D1_COLOR_F &color);
    void TrimBrushCache();
    bool BindCompositionSurface();
    FLOAT DpiForHwnd() const;

    FLOAT dpiOverride_ = 0.0f;

    HWND hwnd_ = nullptr;
    UINT pixelWidth_ = 1;
    UINT pixelHeight_ = 1;
    bool composition_ = false;
    bool compositionOpaque_ = false;

    Microsoft::WRL::ComPtr<ID2D1Factory> d2dFactory_;
    Microsoft::WRL::ComPtr<ID2D1Factory1> d2dFactory1_;
    Microsoft::WRL::ComPtr<IDWriteFactory> dwriteFactory_;
    Microsoft::WRL::ComPtr<IWICImagingFactory> wicFactory_;
    Microsoft::WRL::ComPtr<ID2D1HwndRenderTarget> hwndRenderTarget_;
    Microsoft::WRL::ComPtr<ID3D11Device> d3dDevice_;
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice_;
    Microsoft::WRL::ComPtr<IDXGISwapChain1> swapChain_;
    Microsoft::WRL::ComPtr<ID2D1Device> d2dDevice_;
    Microsoft::WRL::ComPtr<ID2D1DeviceContext> deviceContext_;
    Microsoft::WRL::ComPtr<ID2D1Bitmap1> dxgiBitmap_;
    Microsoft::WRL::ComPtr<IDCompositionDevice> dcompDevice_;
    Microsoft::WRL::ComPtr<IDCompositionTarget> dcompTarget_;
    Microsoft::WRL::ComPtr<IDCompositionVisual> dcompVisual_;
    std::unordered_map<uint64_t, BrushCacheEntry> brushCache_;
    uint64_t frame_ = 0;
    std::vector<TextFormatCacheEntry> textFormatCache_;
    std::vector<BitmapCacheEntry> bitmapCache_;
    std::vector<SvgCacheEntry> svgCache_;
    std::vector<RectF> clipStack_;
};
} // namespace msimeui
