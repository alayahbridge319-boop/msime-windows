#include "msimeui/DeviceResources.h"

#include "msimeui/Fonts.h"

#include <algorithm>
#include <cmath>
#include <d2d1_3.h>
#include <functional>
#include <future>
#include <iterator>
#include <mutex>
#include <shlwapi.h>
#include <windows.h>

namespace msimeui
{
namespace
{
UINT AlignSwapExtent(UINT value)
{
    constexpr UINT kBucket = 64;
    value = (std::max)(value, 1U);
    return ((value + kBucket - 1U) / kBucket) * kBucket;
}

uint64_t QuantizeChannel(float value)
{
    return static_cast<uint64_t>(std::lround(std::clamp(value, 0.0f, 1.0f) * 65535.0f));
}

RectF IntersectRect(const RectF &lhs, const RectF &rhs)
{
    const float left = (std::max)(lhs.x, rhs.x);
    const float top = (std::max)(lhs.y, rhs.y);
    const float right = (std::min)(lhs.x + lhs.width, rhs.x + rhs.width);
    const float bottom = (std::min)(lhs.y + lhs.height, rhs.y + rhs.height);
    return {left, top, (std::max)(right - left, 0.0f), (std::max)(bottom - top, 0.0f)};
}

Microsoft::WRL::ComPtr<ID3D11Device> CreateD3DDevice()
{
    const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr, 0, D3D11_SDK_VERSION,
                                 device.GetAddressOf(), nullptr, nullptr)))
    {
        device.Reset();
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, nullptr, 0, D3D11_SDK_VERSION,
                                     device.GetAddressOf(), nullptr, nullptr)))
        {
            device.Reset();
        }
    }
    return device;
}

// A Direct3D 11 device is free-threaded, so one created on the prewarm thread can be
// used by the window thread.
std::mutex g_prewarmMutex;
std::future<Microsoft::WRL::ComPtr<ID3D11Device>> g_prewarmedDevice;

Microsoft::WRL::ComPtr<ID3D11Device> TakePrewarmedDevice()
{
    std::future<Microsoft::WRL::ComPtr<ID3D11Device>> pending;
    {
        std::lock_guard lock(g_prewarmMutex);
        pending = std::move(g_prewarmedDevice);
    }
    return pending.valid() ? pending.get() : nullptr;
}
} // namespace

void DeviceResources::PrewarmComposition()
{
    std::lock_guard lock(g_prewarmMutex);
    if (!g_prewarmedDevice.valid())
    {
        g_prewarmedDevice = std::async(std::launch::async, &CreateD3DDevice);
    }
}

uint64_t DeviceResources::ColorKey(const D2D1_COLOR_F &color)
{
    return (QuantizeChannel(color.r) << 48) | (QuantizeChannel(color.g) << 32) | (QuantizeChannel(color.b) << 16) |
           QuantizeChannel(color.a);
}

void DeviceResources::BeginFrame()
{
    ++frame_;
    clipStack_.clear();
    TrimBrushCache();
}

void DeviceResources::TrimBrushCache()
{
    if (brushCache_.size() <= kBrushCacheSoftLimit)
    {
        return;
    }
    // Only brushes untouched in the current frame go: a caller may still hold one it
    // fetched a moment ago.
    for (auto it = brushCache_.begin(); it != brushCache_.end();)
    {
        it = it->second.lastFrame + 1 < frame_ ? brushCache_.erase(it) : std::next(it);
    }
}

void DeviceResources::PushClip(const RectF &rect, D2D1_ANTIALIAS_MODE mode)
{
    if (ID2D1RenderTarget *target = GetRenderTarget())
    {
        target->PushAxisAlignedClip(D2D1::RectF(rect.x, rect.y, rect.x + rect.width, rect.y + rect.height), mode);
    }
    clipStack_.push_back(clipStack_.empty() ? rect : IntersectRect(clipStack_.back(), rect));
}

void DeviceResources::PopClip()
{
    if (clipStack_.empty())
    {
        return;
    }
    clipStack_.pop_back();
    if (ID2D1RenderTarget *target = GetRenderTarget())
    {
        target->PopAxisAlignedClip();
    }
}

bool DeviceResources::IsVisible(const RectF &rect) const
{
    if (clipStack_.empty())
    {
        return true;
    }
    const RectF &clip = clipStack_.back();
    return rect.x < clip.x + clip.width && rect.x + rect.width > clip.x && rect.y < clip.y + clip.height &&
           rect.y + rect.height > clip.y;
}

FLOAT DeviceResources::DpiForHwnd() const
{
    // The override wins over the system window DPI: window sizing and
    // rendering must share one scale even when GetDpiForWindow disagrees
    // with the content's real scale (RDP client-scaling sync).
    if (dpiOverride_ > 0.0f)
    {
        return dpiOverride_;
    }
    if (!hwnd_)
    {
        return 96.0f;
    }
    const UINT dpi = GetDpiForWindow(hwnd_);
    return dpi > 0 ? static_cast<FLOAT>(dpi) : 96.0f;
}

void DeviceResources::SetDpiOverride(FLOAT dpi)
{
    const FLOAT normalized = dpi > 0.0f ? dpi : 0.0f;
    if (normalized == dpiOverride_)
    {
        return;
    }
    dpiOverride_ = normalized;
    // EnsureForComposition early-returns when the existing swap chain already
    // covers the requested size, so a live target would keep the stale DPI.
    // Push the new DPI into live targets right away; freshly created targets
    // read it through DpiForHwnd() instead.
    const FLOAT effective = DpiForHwnd();
    if (hwndRenderTarget_)
    {
        hwndRenderTarget_->SetDpi(effective, effective);
    }
    if (deviceContext_)
    {
        deviceContext_->SetDpi(effective, effective);
    }
}

bool DeviceResources::EnsureFactories()
{
    if (!d2dFactory_)
    {
        Microsoft::WRL::ComPtr<ID2D1Factory1> factory1;
        if (SUCCEEDED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory1.GetAddressOf())))
        {
            d2dFactory1_ = factory1;
            d2dFactory_ = factory1;
        }
        else if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2dFactory_.GetAddressOf())))
        {
            return false;
        }
    }

    if (!dwriteFactory_)
    {
        if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                       reinterpret_cast<IUnknown **>(dwriteFactory_.GetAddressOf()))))
        {
            return false;
        }
    }

    if (!wicFactory_)
    {
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(wicFactory_.GetAddressOf()))))
        {
            return false;
        }
    }
    return true;
}

bool DeviceResources::EnsureForWindow(HWND hwnd)
{
    hwnd_ = hwnd;
    composition_ = false;
    if (!EnsureFactories())
    {
        return false;
    }
    if (hwndRenderTarget_)
    {
        const FLOAT dpi = DpiForHwnd();
        hwndRenderTarget_->SetDpi(dpi, dpi);
        return true;
    }

    DiscardTarget();
    RECT rc = {};
    GetClientRect(hwnd, &rc);
    pixelWidth_ = static_cast<UINT>((std::max)(rc.right, 1L));
    pixelHeight_ = static_cast<UINT>((std::max)(rc.bottom, 1L));
    const auto size = D2D1::SizeU(pixelWidth_, pixelHeight_);
    if (FAILED(d2dFactory_->CreateHwndRenderTarget(D2D1::RenderTargetProperties(),
                                                   D2D1::HwndRenderTargetProperties(hwnd, size),
                                                   hwndRenderTarget_.GetAddressOf())))
    {
        return false;
    }

    const FLOAT dpi = DpiForHwnd();
    hwndRenderTarget_->SetDpi(dpi, dpi);
    return true;
}

bool DeviceResources::BindCompositionSurface()
{
    if (!swapChain_ || !deviceContext_)
    {
        return false;
    }

    deviceContext_->SetTarget(nullptr);
    dxgiBitmap_.Reset();

    Microsoft::WRL::ComPtr<IDXGISurface> surface;
    if (FAILED(swapChain_->GetBuffer(0, IID_PPV_ARGS(surface.GetAddressOf()))))
    {
        return false;
    }

    const FLOAT dpi = DpiForHwnd();
    const D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                          compositionOpaque_ ? D2D1_ALPHA_MODE_IGNORE : D2D1_ALPHA_MODE_PREMULTIPLIED),
        dpi, dpi);
    if (FAILED(deviceContext_->CreateBitmapFromDxgiSurface(surface.Get(), &props, dxgiBitmap_.GetAddressOf())))
    {
        return false;
    }
    deviceContext_->SetTarget(dxgiBitmap_.Get());
    deviceContext_->SetDpi(dpi, dpi);
    return true;
}

void DeviceResources::SetCompositionOpaque(bool opaque)
{
    if (compositionOpaque_ == opaque)
    {
        return;
    }
    compositionOpaque_ = opaque;
    if (composition_)
    {
        DiscardTarget();
    }
}

bool DeviceResources::EnsureForComposition(HWND hwnd)
{
    hwnd_ = hwnd;
    if (!EnsureFactories() || !d2dFactory1_ || !hwnd)
    {
        return false;
    }

    RECT rc = {};
    GetClientRect(hwnd, &rc);
    const UINT width = AlignSwapExtent(static_cast<UINT>((std::max)(rc.right, 1L)));
    const UINT height = AlignSwapExtent(static_cast<UINT>((std::max)(rc.bottom, 1L)));

    if (composition_ && deviceContext_ && swapChain_ && pixelWidth_ >= width && pixelHeight_ >= height)
    {
        const FLOAT dpi = DpiForHwnd();
        deviceContext_->SetDpi(dpi, dpi);
        return true;
    }

    if (!composition_ || !d3dDevice_ || !swapChain_)
    {
        DiscardTarget();
        composition_ = true;

        d3dDevice_ = TakePrewarmedDevice();
        if (!d3dDevice_)
        {
            d3dDevice_ = CreateD3DDevice();
        }
        if (!d3dDevice_)
        {
            composition_ = false;
            return false;
        }
        if (FAILED(d3dDevice_.As(&dxgiDevice_)))
        {
            composition_ = false;
            return false;
        }
        if (FAILED(d2dFactory1_->CreateDevice(dxgiDevice_.Get(), d2dDevice_.GetAddressOf())) ||
            FAILED(d2dDevice_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, deviceContext_.GetAddressOf())))
        {
            composition_ = false;
            return false;
        }

        Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
        Microsoft::WRL::ComPtr<IDXGIFactory2> factory;
        if (FAILED(dxgiDevice_->GetAdapter(adapter.GetAddressOf())) ||
            FAILED(adapter->GetParent(IID_PPV_ARGS(factory.GetAddressOf()))))
        {
            composition_ = false;
            return false;
        }

        DXGI_SWAP_CHAIN_DESC1 desc = {};
        desc.Width = width;
        desc.Height = height;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        desc.AlphaMode = compositionOpaque_ ? DXGI_ALPHA_MODE_IGNORE : DXGI_ALPHA_MODE_PREMULTIPLIED;
        desc.Scaling = DXGI_SCALING_STRETCH;
        if (FAILED(factory->CreateSwapChainForComposition(d3dDevice_.Get(), &desc, nullptr, swapChain_.GetAddressOf())))
        {
            composition_ = false;
            return false;
        }

        if (FAILED(DCompositionCreateDevice(dxgiDevice_.Get(), IID_PPV_ARGS(dcompDevice_.GetAddressOf()))) ||
            FAILED(dcompDevice_->CreateTargetForHwnd(hwnd, TRUE, dcompTarget_.GetAddressOf())) ||
            FAILED(dcompDevice_->CreateVisual(dcompVisual_.GetAddressOf())) ||
            FAILED(dcompVisual_->SetContent(swapChain_.Get())) || FAILED(dcompTarget_->SetRoot(dcompVisual_.Get())))
        {
            composition_ = false;
            return false;
        }
        pixelWidth_ = width;
        pixelHeight_ = height;
        if (!BindCompositionSurface())
        {
            composition_ = false;
            return false;
        }
        dcompDevice_->Commit();
        return true;
    }

    const UINT newWidth = (std::max)(pixelWidth_, width);
    const UINT newHeight = (std::max)(pixelHeight_, height);
    pixelWidth_ = newWidth;
    pixelHeight_ = newHeight;
    deviceContext_->SetTarget(nullptr);
    dxgiBitmap_.Reset();
    if (FAILED(swapChain_->ResizeBuffers(0, newWidth, newHeight, DXGI_FORMAT_B8G8R8A8_UNORM, 0)))
    {
        return false;
    }
    return BindCompositionSurface();
}

void DeviceResources::Resize(UINT width, UINT height)
{
    width = (std::max)(width, 1U);
    height = (std::max)(height, 1U);
    if (hwndRenderTarget_)
    {
        hwndRenderTarget_->Resize(D2D1::SizeU(width, height));
        pixelWidth_ = width;
        pixelHeight_ = height;
        return;
    }
    if (composition_ && swapChain_ && deviceContext_)
    {
        const UINT neededW = AlignSwapExtent(width);
        const UINT neededH = AlignSwapExtent(height);
        if (pixelWidth_ >= neededW && pixelHeight_ >= neededH)
        {
            return;
        }
        pixelWidth_ = (std::max)(pixelWidth_, neededW);
        pixelHeight_ = (std::max)(pixelHeight_, neededH);
        deviceContext_->SetTarget(nullptr);
        dxgiBitmap_.Reset();
        if (SUCCEEDED(swapChain_->ResizeBuffers(0, pixelWidth_, pixelHeight_, DXGI_FORMAT_B8G8R8A8_UNORM, 0)))
        {
            BindCompositionSurface();
        }
    }
}

void DeviceResources::DiscardTarget()
{
    if (deviceContext_)
    {
        deviceContext_->SetTarget(nullptr);
    }
    dxgiBitmap_.Reset();
    hwndRenderTarget_.Reset();
    deviceContext_.Reset();
    d2dDevice_.Reset();
    swapChain_.Reset();
    dcompVisual_.Reset();
    dcompTarget_.Reset();
    dcompDevice_.Reset();
    dxgiDevice_.Reset();
    d3dDevice_.Reset();
    brushCache_.clear();
    bitmapCache_.clear();
    svgCache_.clear();
    clipStack_.clear();
    composition_ = false;
}

HRESULT DeviceResources::Present()
{
    if (!swapChain_)
    {
        return S_OK;
    }
    const HRESULT hr = swapChain_->Present(0, 0);
    if (dcompDevice_)
    {
        dcompDevice_->Commit();
    }
    return hr;
}

ID2D1RenderTarget *DeviceResources::GetRenderTarget() const
{
    if (deviceContext_)
    {
        return deviceContext_.Get();
    }
    return hwndRenderTarget_.Get();
}

ID2D1DeviceContext *DeviceResources::GetDeviceContext() const
{
    return deviceContext_.Get();
}

bool DeviceResources::UsesComposition() const
{
    return composition_;
}

IDWriteFactory *DeviceResources::GetDWriteFactory() const
{
    return dwriteFactory_.Get();
}

ID2D1SolidColorBrush *DeviceResources::GetSolidColorBrush(const D2D1_COLOR_F &color)
{
    ID2D1RenderTarget *target = GetRenderTarget();
    if (!target)
    {
        return nullptr;
    }

    const uint64_t key = ColorKey(color);
    auto found = brushCache_.find(key);
    if (found != brushCache_.end() && found->second.brush)
    {
        found->second.lastFrame = frame_;
        return found->second.brush.Get();
    }

    BrushCacheEntry entry;
    entry.lastFrame = frame_;
    if (FAILED(target->CreateSolidColorBrush(color, entry.brush.GetAddressOf())))
    {
        return nullptr;
    }

    ID2D1SolidColorBrush *brush = entry.brush.Get();
    brushCache_[key] = std::move(entry);
    return brush;
}

IDWriteTextFormat *DeviceResources::GetTextFormat(const std::wstring &fontFamily, float fontSize,
                                                  DWRITE_FONT_WEIGHT fontWeight, DWRITE_TEXT_ALIGNMENT textAlignment,
                                                  DWRITE_PARAGRAPH_ALIGNMENT paragraphAlignment,
                                                  DWRITE_WORD_WRAPPING wordWrapping)
{
    if (!dwriteFactory_)
    {
        return nullptr;
    }

    for (auto &entry : textFormatCache_)
    {
        if (entry.fontFamily == fontFamily && entry.fontSize == fontSize && entry.fontWeight == fontWeight &&
            entry.textAlignment == textAlignment && entry.paragraphAlignment == paragraphAlignment &&
            entry.wordWrapping == wordWrapping && entry.format)
        {
            return entry.format.Get();
        }
    }

    TextFormatCacheEntry entry;
    entry.fontFamily = fontFamily;
    entry.fontSize = fontSize;
    entry.fontWeight = fontWeight;
    entry.textAlignment = textAlignment;
    entry.paragraphAlignment = paragraphAlignment;
    entry.wordWrapping = wordWrapping;
    if (FAILED(dwriteFactory_->CreateTextFormat(entry.fontFamily.c_str(), nullptr, entry.fontWeight,
                                                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, entry.fontSize,
                                                L"", entry.format.GetAddressOf())))
    {
        if (FAILED(dwriteFactory_->CreateTextFormat(L"Microsoft YaHei", nullptr, entry.fontWeight,
                                                    DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                                    entry.fontSize, L"", entry.format.GetAddressOf())))
        {
            return nullptr;
        }
    }

    entry.format->SetTextAlignment(entry.textAlignment);
    entry.format->SetParagraphAlignment(entry.paragraphAlignment);
    entry.format->SetWordWrapping(entry.wordWrapping);
    ApplyUiFontFallback(dwriteFactory_.Get(), entry.format.Get());
    textFormatCache_.push_back(std::move(entry));
    return textFormatCache_.back().format.Get();
}

ID2D1Bitmap *DeviceResources::GetBitmapFromFile(const std::wstring &filePath, D2D1_SIZE_F *size)
{
    ID2D1RenderTarget *target = GetRenderTarget();
    if (!target || !wicFactory_ || filePath.empty())
    {
        return nullptr;
    }

    for (auto it = bitmapCache_.begin(); it != bitmapCache_.end(); ++it)
    {
        if (it->filePath == filePath && it->bitmap)
        {
            // 命中的挪到末尾，末尾即最近使用；淘汰从头部开始。
            if (std::next(it) != bitmapCache_.end())
            {
                std::rotate(it, std::next(it), bitmapCache_.end());
            }
            if (size)
            {
                *size = bitmapCache_.back().size;
            }
            return bitmapCache_.back().bitmap.Get();
        }
    }

    Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(wicFactory_->CreateDecoderFromFilename(filePath.c_str(), nullptr, GENERIC_READ,
                                                      WICDecodeMetadataCacheOnLoad, decoder.GetAddressOf())))
    {
        return nullptr;
    }

    Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, frame.GetAddressOf())))
    {
        return nullptr;
    }

    Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
    if (FAILED(wicFactory_->CreateFormatConverter(converter.GetAddressOf())) ||
        FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                     WICBitmapPaletteTypeMedianCut)))
    {
        return nullptr;
    }

    BitmapCacheEntry entry;
    entry.filePath = filePath;
    if (FAILED(target->CreateBitmapFromWicBitmap(converter.Get(), nullptr, entry.bitmap.GetAddressOf())))
    {
        return nullptr;
    }
    entry.size = entry.bitmap->GetSize();
    // 位图按原始分辨率解码成 32bpp，调用方每帧按路径重新取、不持有指针，所以超出上限时
    // 丢掉最久没用的只是下次重新解码。不设上限的话，换过的皮肤图会一直留到设备重建。
    if (bitmapCache_.size() >= kMaxCachedBitmaps)
    {
        bitmapCache_.erase(bitmapCache_.begin());
    }
    bitmapCache_.push_back(std::move(entry));
    if (size)
    {
        *size = bitmapCache_.back().size;
    }
    return bitmapCache_.back().bitmap.Get();
}

ID2D1Bitmap *DeviceResources::GetSvgBitmap(const std::string &markup, const SizeF &sizeDips)
{
    ID2D1RenderTarget *target = GetRenderTarget();
    if (!target || markup.empty() || sizeDips.width <= 0.0f || sizeDips.height <= 0.0f)
    {
        return nullptr;
    }

    float dpiX = 96.0f;
    float dpiY = 96.0f;
    target->GetDpi(&dpiX, &dpiY);
    const size_t hash = std::hash<std::string>{}(markup);
    for (auto it = svgCache_.begin(); it != svgCache_.end(); ++it)
    {
        if (it->markupHash == hash && it->dpi == dpiX && it->size.width == sizeDips.width &&
            it->size.height == sizeDips.height && it->markup == markup && it->bitmap)
        {
            if (std::next(it) != svgCache_.end())
            {
                std::rotate(it, std::next(it), svgCache_.end());
            }
            return svgCache_.back().bitmap.Get();
        }
    }

    // Rasterize once into a bitmap of the target's own pixel format and DPI; drawing a
    // bitmap afterwards is far cheaper than walking the SVG tree every frame, and it can
    // take an opacity like any other bitmap. The pixel size is rounded rather than left to
    // D2D, which rounds up: a box of 27.0001 px would get a 28 px bitmap squeezed into it.
    const D2D1_SIZE_U pixelSize =
        D2D1::SizeU(static_cast<UINT32>(std::max(std::lround(sizeDips.width * dpiX / 96.0f), 1L)),
                    static_cast<UINT32>(std::max(std::lround(sizeDips.height * dpiY / 96.0f), 1L)));
    Microsoft::WRL::ComPtr<ID2D1BitmapRenderTarget> raster;
    if (FAILED(target->CreateCompatibleRenderTarget(D2D1::SizeF(sizeDips.width, sizeDips.height), pixelSize,
                                                    raster.GetAddressOf())))
    {
        return nullptr;
    }
    Microsoft::WRL::ComPtr<ID2D1DeviceContext5> context;
    if (FAILED(raster.As(&context)))
    {
        return nullptr;
    }
    Microsoft::WRL::ComPtr<IStream> stream;
    stream.Attach(SHCreateMemStream(reinterpret_cast<const BYTE *>(markup.data()), static_cast<UINT>(markup.size())));
    Microsoft::WRL::ComPtr<ID2D1SvgDocument> document;
    if (!stream || FAILED(context->CreateSvgDocument(stream.Get(), D2D1::SizeF(sizeDips.width, sizeDips.height),
                                                     document.GetAddressOf())))
    {
        return nullptr;
    }
    // Scale the drawing to the requested box like an <img> does: the root's own
    // width/height would otherwise draw it at its intrinsic size inside the viewport.
    Microsoft::WRL::ComPtr<ID2D1SvgElement> root;
    document->GetRoot(root.GetAddressOf());
    if (root)
    {
        if (!root->IsAttributeSpecified(L"viewBox"))
        {
            D2D1_SVG_LENGTH width = {sizeDips.width, D2D1_SVG_LENGTH_UNITS_NUMBER};
            D2D1_SVG_LENGTH height = {sizeDips.height, D2D1_SVG_LENGTH_UNITS_NUMBER};
            root->GetAttributeValue(L"width", D2D1_SVG_ATTRIBUTE_POD_TYPE_LENGTH, &width, sizeof(width));
            root->GetAttributeValue(L"height", D2D1_SVG_ATTRIBUTE_POD_TYPE_LENGTH, &height, sizeof(height));
            const D2D1_SVG_VIEWBOX viewBox = {0.0f, 0.0f, width.value, height.value};
            root->SetAttributeValue(L"viewBox", D2D1_SVG_ATTRIBUTE_POD_TYPE_VIEWBOX, &viewBox, sizeof(viewBox));
        }
        const D2D1_SVG_LENGTH width = {sizeDips.width, D2D1_SVG_LENGTH_UNITS_NUMBER};
        const D2D1_SVG_LENGTH height = {sizeDips.height, D2D1_SVG_LENGTH_UNITS_NUMBER};
        root->SetAttributeValue(L"width", D2D1_SVG_ATTRIBUTE_POD_TYPE_LENGTH, &width, sizeof(width));
        root->SetAttributeValue(L"height", D2D1_SVG_ATTRIBUTE_POD_TYPE_LENGTH, &height, sizeof(height));
    }

    raster->BeginDraw();
    raster->Clear(D2D1::ColorF(0, 0.0f));
    context->DrawSvgDocument(document.Get());
    SvgCacheEntry entry;
    if (FAILED(raster->EndDraw()) || FAILED(raster->GetBitmap(entry.bitmap.GetAddressOf())))
    {
        return nullptr;
    }
    entry.markupHash = hash;
    entry.markup = markup;
    entry.size = sizeDips;
    entry.dpi = dpiX;
    if (svgCache_.size() >= kMaxCachedSvgBitmaps)
    {
        svgCache_.erase(svgCache_.begin());
    }
    svgCache_.push_back(std::move(entry));
    return svgCache_.back().bitmap.Get();
}
} // namespace msimeui
