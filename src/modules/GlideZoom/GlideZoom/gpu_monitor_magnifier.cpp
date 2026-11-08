#include "gpu_monitor_magnifier.h"
#include "gpu_frame_policy.h"
#include "gpu_cursor_pixels.h"
#include "gpu_queue_policy.h"
#include "gpu_shaders.h"
#include "overlay_order.h"
#include "cursor_coverage.h"
#include "window_class_lease.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <new>
#include <vector>

#include <d3d11_4.h>
#include <d3dcompiler.h>
#include <dcomp.h>
#include <dwmapi.h>
#include <dxgi1_6.h>
#include <roapi.h>
#include <wrl.h>
#include <winstring.h>

// The LLVM-MinGW 20261020 generated windows.foundation.h declares both
// IReference<BYTE> and IReference<boolean>, although boolean is a BYTE there.
// Those are duplicate C++ template specializations. None of this renderer's
// interfaces uses IReference<boolean>; suppress only that unused declaration
// in this translation unit instead of changing the SDK or hand-writing COM
// layouts. The interfaces below otherwise come directly from the SDK headers.
#if defined(__MINGW32__) && !defined(____FIReference_1_boolean_INTERFACE_DEFINED__)
#define GLIDE_MINGW_BOOLEAN_REFERENCE_GUARD
#define ____FIReference_1_boolean_INTERFACE_DEFINED__
#endif
#include <windows.graphics.capture.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#if defined(GLIDE_MINGW_BOOLEAN_REFERENCE_GUARD)
#undef GLIDE_MINGW_BOOLEAN_REFERENCE_GUARD
#endif

namespace
{
    using Microsoft::WRL::ComPtr;
    namespace Capture = ABI::Windows::Graphics::Capture;
    namespace Foundation = ABI::Windows::Foundation;
    namespace Graphics = ABI::Windows::Graphics;
    namespace DirectX = ABI::Windows::Graphics::DirectX;
    namespace Direct3D = ABI::Windows::Graphics::DirectX::Direct3D11;
    using SurfaceAccess = Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess;

    constexpr wchar_t kClass[] = L"GlideZoom.GpuMonitor.1";
    constexpr UINT kMaximumTexture = 16384;
    constexpr UINT kMaximumCursor = glide::gpu::MaximumCursorDimension;
    constexpr DWORD kExcludeFromCapture = 0x00000011;
    constexpr DWORD kCursorSuppressed = 2;

    HRESULT NativeError() noexcept
    {
        const DWORD error = GetLastError();
        return HRESULT_FROM_WIN32(error ? error : ERROR_GEN_FAILURE);
    }

    template<class T>
    HRESULT CloseObject(ComPtr<T>& object) noexcept
    {
        if (!object)
            return S_OK;
        ComPtr<Foundation::IClosable> close;
        const HRESULT query = object.As(&close);
        HRESULT result = query;
        if (SUCCEEDED(query))
            result = close->Close();
        object.Reset();
        return result;
    }

    template<class T>
    HRESULT Factory(const wchar_t* name, ComPtr<T>& result) noexcept
    {
        HSTRING_HEADER header{};
        HSTRING string = nullptr;
        const auto length = static_cast<UINT32>(std::wcslen(name));
        HRESULT status = WindowsCreateStringReference(name, length, &header, &string);
        if (FAILED(status))
            return status;
        return RoGetActivationFactory(string, __uuidof(T), reinterpret_cast<void**>(result.ReleaseAndGetAddressOf()));
    }

    bool SameRect(const RECT& a, const RECT& b) noexcept
    {
        return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
    }

    struct DrawConstants
    {
        float source[4];
        float rectangle[4];
        float viewport[4];
        float color[4];
    };
    static_assert(sizeof(DrawConstants) == 64, "GPU constant buffer alignment");

    // Both source crop and zoom are submitted as one set of shader constants.
    // The captured desktop stays on the GPU; only the small cursor shape uses GDI.
    constexpr auto& kShader = glide::gpu::MagnifierShader;

    struct IconCopy
    {
        HICON icon = nullptr;
        ICONINFO information{};
        ~IconCopy() noexcept
        {
            if (information.hbmColor)
                DeleteObject(information.hbmColor);
            if (information.hbmMask)
                DeleteObject(information.hbmMask);
            if (icon)
                DestroyIcon(icon);
        }
    };

    // The bitmap is never screen capture: this rasterizes only one cursor icon.
    bool DrawCursorBitmap(HICON icon, UINT width, UINT height, bool white, std::vector<std::uint32_t>& output)
    {
        const auto count = static_cast<std::size_t>(width) * height;
        // Any throwing allocation happens before native GDI ownership is acquired.
        output.resize(count);
        HDC dc = CreateCompatibleDC(nullptr);
        if (!dc)
            return false;
        BITMAPINFO information{};
        information.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        information.bmiHeader.biWidth = static_cast<LONG>(width);
        information.bmiHeader.biHeight = -static_cast<LONG>(height);
        information.bmiHeader.biPlanes = 1;
        information.bmiHeader.biBitCount = 32;
        information.bmiHeader.biCompression = BI_RGB;
        void* bytes = nullptr;
        HBITMAP bitmap = CreateDIBSection(dc, &information, DIB_RGB_COLORS, &bytes, nullptr, 0);
        if (!bitmap || !bytes)
        {
            if (bitmap)
                DeleteObject(bitmap);
            DeleteDC(dc);
            return false;
        }
        std::memset(bytes, white ? 255 : 0, count * sizeof(std::uint32_t));
        HGDIOBJ old = SelectObject(dc, bitmap);
        bool okay = old && old != HGDI_ERROR &&
                    DrawIconEx(dc, 0, 0, icon, static_cast<int>(width), static_cast<int>(height), 0, nullptr, DI_NORMAL | DI_NOMIRROR) != FALSE && GdiFlush() != FALSE;
        if (okay)
        {
            std::memcpy(output.data(), bytes, count * sizeof(std::uint32_t));
        }
        if (old && old != HGDI_ERROR)
            SelectObject(dc, old);
        DeleteObject(bitmap);
        DeleteDC(dc);
        return okay;
    }

    // Only generated numeric HUD text is rasterized here. No desktop pixels,
    // window titles or foreign application content pass through this bitmap.
    struct ScopeTextBitmap
    {
        static constexpr UINT Width = 384, Height = 48;
        HDC dc = nullptr;
        HBITMAP bitmap = nullptr;
        HFONT font = nullptr;
        HGDIOBJ oldBitmap = nullptr, oldFont = nullptr;
        void* bytes = nullptr;

        ~ScopeTextBitmap() noexcept { Close(); }
        void Close() noexcept
        {
            if (oldFont && oldFont != HGDI_ERROR)
                SelectObject(dc, oldFont);
            if (oldBitmap && oldBitmap != HGDI_ERROR)
                SelectObject(dc, oldBitmap);
            if (font)
                DeleteObject(font);
            if (bitmap)
                DeleteObject(bitmap);
            if (dc)
                DeleteDC(dc);
            dc = nullptr;
            bitmap = nullptr;
            font = nullptr;
            oldBitmap = oldFont = nullptr;
            bytes = nullptr;
        }
        bool Draw(const wchar_t* text) noexcept
        {
            if (!dc)
            {
                dc = CreateCompatibleDC(nullptr);
                BITMAPINFO information{};
                information.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                information.bmiHeader.biWidth = static_cast<LONG>(Width);
                information.bmiHeader.biHeight = -static_cast<LONG>(Height);
                information.bmiHeader.biPlanes = 1;
                information.bmiHeader.biBitCount = 32;
                information.bmiHeader.biCompression = BI_RGB;
                if (dc)
                    bitmap = CreateDIBSection(dc, &information, DIB_RGB_COLORS, &bytes, nullptr, 0);
                font = CreateFontW(-16, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
                if (bitmap && font)
                {
                    oldBitmap = SelectObject(dc, bitmap);
                    oldFont = SelectObject(dc, font);
                }
                if (!bytes || !oldBitmap || oldBitmap == HGDI_ERROR || !oldFont || oldFont == HGDI_ERROR)
                {
                    Close();
                    return false;
                }
            }
            std::memset(bytes, 0, static_cast<std::size_t>(Width) * Height * sizeof(std::uint32_t));
            if (!SetBkMode(dc, TRANSPARENT) || SetTextColor(dc, RGB(255, 255, 255)) == CLR_INVALID)
                return false;
            RECT rectangle{ 8, 3, static_cast<LONG>(Width) - 8, static_cast<LONG>(Height) - 3 };
            return DrawTextW(dc, text, -1, &rectangle, DT_LEFT | DT_TOP | DT_NOPREFIX) != 0 && GdiFlush() != FALSE;
        }
    };
} // namespace

struct GpuMonitorMagnifier::Impl
{
    explicit Impl(GpuMonitorMagnifier& value) noexcept :
        owner(value) {}
    GpuMonitorMagnifier& owner;
    HINSTANCE instance = nullptr;
    HWND host = nullptr;
    HMONITOR monitor = nullptr;
    RECT destination{};
    glide::monitor::Frame frame{};
    glide::monitor::Frame submittedFrame{};
    glide::gpu::SamplingRect sampling{};
    glide::gpu::FrameGate gate;
    glide::gpu::PresentationQueue presentationQueue;
    glide::overlay::OrderGuard orderGuard;
    std::uint64_t epoch = 1;
    bool initialized = false, roInitialized = false, classReady = false;
    bool windowConfigured = false, pipelineReady = false;
    bool prepared = false, pending = false, layingOut = false, geometryInvalid = false;
    bool staged = false, stagedCursor = false, stagedReused = false;
    bool stagedCursorEnabled = true, hasSubmittedFrame = false;
    bool cursorHiddenByUs = false, cursorMonochrome = false;
    glide::cursor::Presentation submittedCursor;
    HCURSOR stagedCursorShape = nullptr;
    std::uint64_t stagedIndicatorRevision = 0;
    HCURSOR lastCursor = nullptr;
    ULONGLONG cursorSampleTime = 0;
    UINT cursorWidth = 0, cursorHeight = 0, hotspotX = 0, hotspotY = 0;
    std::uint64_t cursorHash = 0;
    HANDLE frameLatency = nullptr;

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGIFactory2> dxgiFactory;
    ComPtr<IDXGISwapChain1> swap;
    ComPtr<ID3D11RenderTargetView> target;
    ComPtr<ID3D11VertexShader> vertex;
    ComPtr<ID3D11PixelShader> desktopShader, cursorShader;
    ComPtr<ID3D11PixelShader> overviewShader, solidShader, textShader;
    ComPtr<ID3D11Buffer> constants;
    ComPtr<ID3D11SamplerState> linearSampler, pointSampler;
    ComPtr<ID3D11BlendState> cursorBlend;
    ComPtr<ID3D11RasterizerState> rasterizer;
    ComPtr<ID3D11Texture2D> captured;
    ComPtr<ID3D11ShaderResourceView> capturedView;
    ComPtr<ID3D11Texture2D> cursor;
    ComPtr<ID3D11ShaderResourceView> cursorView;
    ComPtr<ID3D11Texture2D> scopeText;
    ComPtr<ID3D11ShaderResourceView> scopeTextView;
    ScopeTextBitmap scopeTextBitmap;
    std::array<wchar_t, 128> scopeTextValue{};
    ComPtr<IDCompositionDevice> composition;
    ComPtr<IDCompositionTarget> compositionTarget;
    ComPtr<IDCompositionVisual> compositionVisual;
    ComPtr<Direct3D::IDirect3DDevice> captureDevice;
    ComPtr<Capture::IGraphicsCaptureItem> captureItem;
    ComPtr<Capture::IDirect3D11CaptureFramePool> pool;
    ComPtr<Capture::IGraphicsCaptureSession> session;

    ~Impl() noexcept
    {
        Cleanup();
        if (host)
        {
            SetWindowLongPtrW(host, GWLP_USERDATA, 0);
            DestroyWindow(host);
            host = nullptr;
        }
        if (classReady)
        {
            ClassBackend backend{ instance };
            SharedClass().Release(backend);
            classReady = false;
        }
        if (roInitialized)
            RoUninitialize();
    }

    bool Fail(HRESULT status, const wchar_t* reason) noexcept
    {
        owner.Hide();
        return owner.Error(status, reason);
    }

    struct ClassBackend
    {
        HINSTANCE instance;
        bool Register(bool& owned) const noexcept
        {
            owned = false;
            WNDCLASSEXW klass{};
            klass.cbSize = sizeof(klass);
            klass.hInstance = instance;
            klass.lpfnWndProc = WindowProc;
            klass.lpszClassName = kClass;
            if (RegisterClassExW(&klass))
            {
                owned = true;
                return true;
            }
            WNDCLASSEXW existing{};
            existing.cbSize = sizeof(existing);
            return GetLastError() == ERROR_CLASS_ALREADY_EXISTS &&
                   GetClassInfoExW(instance, kClass, &existing) && existing.lpfnWndProc == WindowProc;
        }
        bool Unregister() const noexcept { return UnregisterClassW(kClass, instance) != FALSE; }
    };

    static glide::window::ClassLease& SharedClass() noexcept
    {
        static glide::window::ClassLease registration;
        return registration;
    }

    bool EnsureOrder(bool show, const RECT* beforeLayout = nullptr) noexcept
    {
        struct Backend
        {
            const Impl& renderer;
            HWND window;
            const std::uint64_t operation;
            const RECT bounds;
            const bool requirePrepared;
            HRESULT error = E_ABORT;

            bool Current() const noexcept
            {
                return renderer.host == window && renderer.epoch == operation &&
                       (!requirePrepared || (renderer.prepared && !renderer.geometryInvalid)) && IsWindow(window) &&
                       glide::OnInputDesktop();
            }
            bool Visible() const noexcept { return IsWindowVisible(window) != FALSE; }
            glide::overlay::Window Overlay() const noexcept { return reinterpret_cast<glide::overlay::Window>(window); }
            bool Previous(glide::overlay::Window from, glide::overlay::Window& previous) noexcept
            {
                const HWND observed = reinterpret_cast<HWND>(from);
                SetLastError(ERROR_SUCCESS);
                const HWND above = GetWindow(observed, GW_HWNDPREV);
                const DWORD status = GetLastError();
                if ((!above && status != ERROR_SUCCESS) || !IsWindow(observed))
                {
                    error = HRESULT_FROM_WIN32(status ? status : ERROR_INVALID_WINDOW_HANDLE);
                    return false;
                }
                previous = reinterpret_cast<glide::overlay::Window>(above);
                return true;
            }
            bool Inspect(glide::overlay::Window candidate, bool& overlaps, bool& protectedWindow) noexcept
            {
                const HWND observed = reinterpret_cast<HWND>(candidate);
                overlaps = protectedWindow = false;
                if (!IsWindow(observed))
                {
                    error = HRESULT_FROM_WIN32(ERROR_INVALID_WINDOW_HANDLE);
                    return false;
                }
                if (IsWindowVisible(observed) && !IsIconic(observed))
                {
                    DWORD cloaked = 0;
                    const HRESULT status = DwmGetWindowAttribute(observed, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
                    if (FAILED(status))
                    {
                        error = status;
                        return false;
                    }
                    if (!cloaked)
                    {
                        RECT rectangle{};
                        if (!GetWindowRect(observed, &rectangle))
                        {
                            error = NativeError();
                            return false;
                        }
                        overlaps = rectangle.right > rectangle.left && rectangle.bottom > rectangle.top &&
                                   rectangle.left < bounds.right && rectangle.right > bounds.left &&
                                   rectangle.top < bounds.bottom && rectangle.bottom > bounds.top;
                        if (overlaps)
                        {
                            SetLastError(ERROR_SUCCESS);
                            const auto style = GetWindowLongPtrW(observed, GWL_EXSTYLE);
                            const DWORD styleError = GetLastError();
                            if (!style && styleError != ERROR_SUCCESS)
                            {
                                error = HRESULT_FROM_WIN32(styleError);
                                return false;
                            }
                            // This query is documented for foreign layered
                            // windows. A failure there is uncertainty, not proof
                            // that captured pixels contain the original popup.
                            DWORD affinity = 0;
                            const bool queried = GetWindowDisplayAffinity(observed, &affinity) != FALSE;
                            const auto protection = glide::overlay::ObserveCaptureProtection(
                                (style & WS_EX_LAYERED) != 0, queried, affinity);
                            if (protection == glide::overlay::CaptureProtection::Unavailable)
                            {
                                error = NativeError();
                                return false;
                            }
                            protectedWindow = protection == glide::overlay::CaptureProtection::Detected;
                        }
                    }
                }
                if (!IsWindow(observed))
                {
                    error = HRESULT_FROM_WIN32(ERROR_INVALID_WINDOW_HANDLE);
                    return false;
                }
                return true;
            }
            bool Raise(bool show) noexcept
            {
                if (!Current())
                    return false;
                if (!SetWindowPos(window, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER | (show ? SWP_SHOWWINDOW : 0)))
                {
                    error = NativeError();
                    return false;
                }
                return true;
            }
        } backend{ *this, host, epoch, beforeLayout ? *beforeLayout : destination, beforeLayout == nullptr };
        // Querying predecessors is bounded, cycle checked, and followed by a
        // second observation after a conditional own-window raise. Higher-band
        // system UI and a continuously reclaimed topmost position fail closed.
        switch (orderGuard.Ensure(backend, GetTickCount64(), show))
        {
        case glide::overlay::OrderResult::Ready:
            return true;
        case glide::overlay::OrderResult::Protected:
            return Fail(E_ACCESSDENIED, L"A capture-protected window is above the zoom view. Zoom was released.");
        case glide::overlay::OrderResult::Contended:
            return Fail(E_ACCESSDENIED, L"Another window is repeatedly reclaiming the topmost position. Zoom was released.");
        case glide::overlay::OrderResult::Blocked:
            return Fail(E_ACCESSDENIED, L"Windows could not place the zoom view above an overlapping window. Zoom was released.");
        default:
            return Fail(backend.error, L"The zoom view's window order could not be verified. Zoom was released.");
        }
    }

    void StopCapture() noexcept
    {
        pending = false;
        staged = stagedCursor = stagedReused = false;
        stagedCursorShape = nullptr;
        gate.Close();
        // An abandoned readiness grant or unacknowledged first submission may
        // never progress after its HWND is hidden. Recreate that swap chain on
        // restart; resetting only its logical policy cannot restore a signal.
        if (presentationQueue.HasUnsubmittedGrant() ||
            (presentationQueue.HasSubmittedFrame() && presentationQueue.Cold()))
            pipelineReady = false;
        presentationQueue.Reset();
        // Close before release. There are no application FrameArrived handlers
        // to race this operation or to call back into a destroyed renderer.
        CloseObject(session);
        CloseObject(pool);
        captureItem.Reset();
    }

    void Cleanup() noexcept
    {
        StopCapture();
        if (compositionTarget)
            compositionTarget->SetRoot(nullptr);
        if (composition)
            composition->Commit();
        compositionVisual.Reset();
        compositionTarget.Reset();
        composition.Reset();
        if (context)
        {
            context->ClearState();
            context->Flush();
        }
        target.Reset();
        if (frameLatency)
        {
            CloseHandle(frameLatency);
            frameLatency = nullptr;
        }
        swap.Reset();
        capturedView.Reset();
        captured.Reset();
        cursorView.Reset();
        cursor.Reset();
        scopeTextView.Reset();
        scopeText.Reset();
        scopeTextBitmap.Close();
        scopeTextValue = {};
        constants.Reset();
        vertex.Reset();
        desktopShader.Reset();
        cursorShader.Reset();
        overviewShader.Reset();
        solidShader.Reset();
        textShader.Reset();
        linearSampler.Reset();
        pointSampler.Reset();
        cursorBlend.Reset();
        rasterizer.Reset();
        captureDevice.Reset();
        context.Reset();
        device.Reset();
        dxgiFactory.Reset();
        monitor = nullptr;
        destination = {};
        lastCursor = nullptr;
        cursorWidth = cursorHeight = 0;
        cursorHash = 0;
        submittedCursor.Clear();
        hasSubmittedFrame = false;
        submittedFrame = {};
        prepared = false;
        pipelineReady = false;
    }

    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) noexcept
    {
        auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE)
        {
            self = static_cast<Impl*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
            SetLastError(ERROR_SUCCESS);
            if (!SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self)) &&
                GetLastError() != ERROR_SUCCESS)
                return FALSE;
        }
        if (self && message == WM_NCDESTROY)
        {
            if (self->host == window)
            {
                self->host = nullptr;
                self->prepared = false;
                self->submittedCursor.Clear();
                self->hasSubmittedFrame = false;
                self->submittedFrame = {};
                self->staged = self->stagedCursor = self->stagedReused = false;
                self->stagedCursorShape = nullptr;
                self->windowConfigured = false;
                self->geometryInvalid = true;
                ++self->epoch;
            }
            SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        }
        if (self && !self->layingOut && message == WM_WINDOWPOSCHANGED && lParam)
        {
            const auto* position = reinterpret_cast<const WINDOWPOS*>(lParam);
            if (!(position->flags & SWP_NOMOVE) || !(position->flags & SWP_NOSIZE))
            {
                self->geometryInvalid = true;
                ++self->epoch;
            }
        }
        if (self && ((message == WM_DPICHANGED && !self->layingOut) || message == WM_DISPLAYCHANGE))
        {
            self->geometryInvalid = true;
            ++self->epoch;
        }
        if (message == WM_NCHITTEST)
            return HTTRANSPARENT;
        if (message == WM_MOUSEACTIVATE)
            return MA_NOACTIVATE;
        if (message == WM_ERASEBKGND)
            return 1;
        if (message == WM_PAINT)
        {
            PAINTSTRUCT paint{};
            BeginPaint(window, &paint);
            EndPaint(window, &paint);
            return 0;
        }
        if (message == WM_CLOSE)
            return 0; // The application owns this auxiliary window.
        return DefWindowProcW(window, message, wParam, lParam);
    }

    bool EnsureWindow() noexcept
    {
        if (host && IsWindow(host) && windowConfigured)
            return true;
        if (host && IsWindow(host))
        {
            owner.Destroy();
            if (host && IsWindow(host))
                return Fail(E_UNEXPECTED, L"A partially configured magnifier window could not be removed.");
        }
        windowConfigured = false;
        if (!classReady)
        {
            ClassBackend backend{ instance };
            if (!SharedClass().Acquire(reinterpret_cast<std::uintptr_t>(instance), backend))
                return Fail(NativeError(), L"Could not register the GPU magnifier window.");
            classReady = true;
        }
        host = CreateWindowExW(WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT |
                                   WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_NOREDIRECTIONBITMAP,
                               kClass,
                               L"GlideZoom GPU magnified view",
                               WS_POPUP,
                               0,
                               0,
                               1,
                               1,
                               nullptr,
                               nullptr,
                               instance,
                               this);
        if (!host)
            return Fail(NativeError(), L"Could not create the GPU magnifier window.");
        if (!SetLayeredWindowAttributes(host, 0, 255, LWA_ALPHA))
            return Fail(NativeError(), L"Could not configure the non-interactive magnifier window.");
        if (!SetWindowDisplayAffinity(host, kExcludeFromCapture))
            return Fail(NativeError(), L"Windows could not exclude the magnifier from its own capture.");
        DWORD affinity = 0;
        if (!GetWindowDisplayAffinity(host, &affinity) || affinity != kExcludeFromCapture)
            return Fail(HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED),
                        L"This Windows session cannot verify capture exclusion for the magnifier.");
        windowConfigured = true;
        return true;
    }

    bool CreateDevice(HMONITOR selected) noexcept
    {
        HRESULT status = CreateDXGIFactory1(__uuidof(IDXGIFactory2),
                                            reinterpret_cast<void**>(dxgiFactory.ReleaseAndGetAddressOf()));
        if (FAILED(status))
            return Fail(status, L"Could not create the DXGI graphics factory.");
        ComPtr<IDXGIAdapter1> selectedAdapter;
        for (UINT adapterIndex = 0; adapterIndex != 64 && !selectedAdapter; ++adapterIndex)
        {
            ComPtr<IDXGIAdapter1> adapter;
            status = dxgiFactory->EnumAdapters1(adapterIndex, &adapter);
            if (status == DXGI_ERROR_NOT_FOUND)
                break;
            if (FAILED(status))
                return Fail(status, L"Could not enumerate graphics adapters.");
            for (UINT outputIndex = 0; outputIndex != 64; ++outputIndex)
            {
                ComPtr<IDXGIOutput> output;
                status = adapter->EnumOutputs(outputIndex, &output);
                if (status == DXGI_ERROR_NOT_FOUND)
                    break;
                if (FAILED(status))
                    return Fail(status, L"Could not enumerate monitor outputs.");
                DXGI_OUTPUT_DESC description{};
                status = output->GetDesc(&description);
                if (FAILED(status))
                    return Fail(status, L"Could not read a monitor output.");
                if (description.Monitor == selected)
                {
                    selectedAdapter = adapter;
                    break;
                }
            }
        }
        if (!selectedAdapter)
            return Fail(HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED),
                        L"The selected monitor has no usable Direct3D output in this session.");
        const D3D_FEATURE_LEVEL levels[]{ D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
        D3D_FEATURE_LEVEL actual{};
        status = D3D11CreateDevice(selectedAdapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 2, D3D11_SDK_VERSION, &device, &actual, &context);
        if (FAILED(status))
            return Fail(status, L"Could not create the Direct3D 11 GPU renderer.");
        ComPtr<ID3D11Multithread> multithread;
        status = context.As(&multithread);
        if (FAILED(status))
            return Fail(status, L"Could not protect the capture device's graphics context.");
        multithread->SetMultithreadProtected(TRUE);
        ComPtr<IDXGIDevice> dxgiDevice;
        status = device.As(&dxgiDevice);
        if (FAILED(status))
            return Fail(status, L"Could not obtain the capture device.");
        ComPtr<IInspectable> inspectable;
        status = CreateDirect3D11DeviceFromDXGIDevice(
            dxgiDevice.Get(), &inspectable);
        if (FAILED(status) || FAILED(status = inspectable.As(&captureDevice)))
            return Fail(status, L"Could not create the Windows Graphics Capture device.");
        return true;
    }

    bool CreatePipeline(UINT width, UINT height) noexcept
    {
        DXGI_SWAP_CHAIN_DESC1 description{};
        description.Width = width;
        description.Height = height;
        description.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        description.SampleDesc.Count = 1;
        description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        description.BufferCount = 2;
        description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        description.Scaling = DXGI_SCALING_STRETCH;
        description.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
        description.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
        HRESULT status = dxgiFactory->CreateSwapChainForComposition(device.Get(), &description, nullptr, &swap);
        if (FAILED(status))
            return Fail(status, L"Could not create the GPU presentation buffers.");
        ComPtr<IDXGISwapChain2> swap2;
        status = swap.As(&swap2);
        if (FAILED(status) || FAILED(status = swap2->SetMaximumFrameLatency(1)))
            return Fail(status, L"Could not configure bounded GPU frame queuing.");
        frameLatency = swap2->GetFrameLatencyWaitableObject();
        if (!frameLatency)
            return Fail(E_FAIL, L"The graphics driver did not provide frame pacing.");
        ComPtr<IDXGISwapChain3> swap3;
        status = swap.As(&swap3);
        if (FAILED(status))
            return Fail(status, L"The graphics driver cannot select a display color space.");
        UINT colorSupport = 0;
        status = swap3->CheckColorSpaceSupport(DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709, &colorSupport);
        if (FAILED(status) || !(colorSupport & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT))
            return Fail(FAILED(status) ? status : DXGI_ERROR_UNSUPPORTED,
                        L"The selected display cannot present the required scRGB GPU surface.");
        status = swap3->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709);
        if (FAILED(status))
            return Fail(status, L"Could not configure the magnifier's display color space.");
        ComPtr<ID3D11Texture2D> backBuffer;
        status = swap->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(backBuffer.GetAddressOf()));
        if (FAILED(status) || FAILED(status = device->CreateRenderTargetView(backBuffer.Get(), nullptr, &target)))
            return Fail(status, L"Could not create the magnifier render target.");

        ComPtr<IDXGIDevice> dxgiDevice;
        status = device.As(&dxgiDevice);
        if (FAILED(status))
            return Fail(status, L"Could not obtain the composition device.");
        status = DCompositionCreateDevice(dxgiDevice.Get(), __uuidof(IDCompositionDevice), reinterpret_cast<void**>(composition.ReleaseAndGetAddressOf()));
        if (FAILED(status) || FAILED(status = composition->CreateTargetForHwnd(host, TRUE, &compositionTarget)) ||
            FAILED(status = composition->CreateVisual(&compositionVisual)) ||
            FAILED(status = compositionVisual->SetContent(swap.Get())) ||
            FAILED(status = compositionTarget->SetRoot(compositionVisual.Get())) ||
            FAILED(status = composition->Commit()))
            return Fail(status, L"Could not attach the GPU surface to the magnifier window.");

        ComPtr<ID3DBlob> vertexCode, desktopCode, cursorCode, errors;
        const UINT compile = D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3;
        status = D3DCompile(kShader, sizeof(kShader) - 1, "GlideZoom magnifier", nullptr, nullptr, "VertexMain", "vs_5_0", compile, 0, &vertexCode, &errors);
        if (FAILED(status))
            return Fail(status, L"Could not compile the magnifier vertex shader.");
        status = D3DCompile(kShader, sizeof(kShader) - 1, "GlideZoom magnifier", nullptr, nullptr, "DesktopMain", "ps_5_0", compile, 0, &desktopCode, &errors);
        if (FAILED(status))
            return Fail(status, L"Could not compile the magnifier desktop shader.");
        status = D3DCompile(kShader, sizeof(kShader) - 1, "GlideZoom magnifier", nullptr, nullptr, "CursorMain", "ps_5_0", compile, 0, &cursorCode, &errors);
        if (FAILED(status))
            return Fail(status, L"Could not compile the magnifier cursor shader.");
        if (FAILED(status = device->CreateVertexShader(vertexCode->GetBufferPointer(),
                                                       vertexCode->GetBufferSize(),
                                                       nullptr,
                                                       &vertex)) ||
            FAILED(status = device->CreatePixelShader(desktopCode->GetBufferPointer(),
                                                      desktopCode->GetBufferSize(),
                                                      nullptr,
                                                      &desktopShader)) ||
            FAILED(status = device->CreatePixelShader(cursorCode->GetBufferPointer(),
                                                      cursorCode->GetBufferSize(),
                                                      nullptr,
                                                      &cursorShader)))
            return Fail(status, L"Could not create the magnifier GPU shaders.");
        const auto compileIndicator = [&](const char* entry, ComPtr<ID3D11PixelShader>& shader) noexcept {
            ComPtr<ID3DBlob> code;
            status = D3DCompile(kShader, sizeof(kShader) - 1, "GlideZoom indicator", nullptr, nullptr, entry, "ps_5_0", compile, 0, &code, &errors);
            return SUCCEEDED(status) && SUCCEEDED(status = device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &shader));
        };
        if (!compileIndicator("OverviewMain", overviewShader) || !compileIndicator("SolidMain", solidShader) ||
            !compileIndicator("TextMain", textShader))
            return Fail(status, L"Could not create the view indicator GPU shaders.");
        D3D11_BUFFER_DESC buffer{};
        buffer.ByteWidth = sizeof(DrawConstants);
        buffer.Usage = D3D11_USAGE_DEFAULT;
        buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (FAILED(status = device->CreateBuffer(&buffer, nullptr, &constants)))
            return Fail(status, L"Could not create the magnifier shader constants.");
        D3D11_SAMPLER_DESC sampler{};
        sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler.MaxAnisotropy = 1;
        sampler.ComparisonFunc = D3D11_COMPARISON_NEVER;
        sampler.MaxLOD = D3D11_FLOAT32_MAX;
        if (FAILED(status = device->CreateSamplerState(&sampler, &linearSampler)))
            return Fail(status, L"Could not configure the magnifier image sampler.");
        sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        if (FAILED(status = device->CreateSamplerState(&sampler, &pointSampler)))
            return Fail(status, L"Could not configure the magnifier cursor sampler.");
        D3D11_BLEND_DESC blend{};
        blend.RenderTarget[0].BlendEnable = TRUE;
        blend.RenderTarget[0].SrcBlend = blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        blend.RenderTarget[0].DestBlend = blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        blend.RenderTarget[0].BlendOp = blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        if (FAILED(status = device->CreateBlendState(&blend, &cursorBlend)))
            return Fail(status, L"Could not configure cursor compositing.");
        D3D11_RASTERIZER_DESC raster{};
        raster.FillMode = D3D11_FILL_SOLID;
        raster.CullMode = D3D11_CULL_NONE;
        raster.DepthClipEnable = TRUE;
        if (FAILED(status = device->CreateRasterizerState(&raster, &rasterizer)))
            return Fail(status, L"Could not configure the magnifier rasterizer.");

        D3D11_TEXTURE2D_DESC texture{};
        texture.Width = width;
        texture.Height = height;
        texture.MipLevels = texture.ArraySize = 1;
        texture.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        texture.SampleDesc.Count = 1;
        texture.Usage = D3D11_USAGE_DEFAULT;
        texture.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(status = device->CreateTexture2D(&texture, nullptr, &captured)) ||
            FAILED(status = device->CreateShaderResourceView(captured.Get(), nullptr, &capturedView)))
            return Fail(status, L"Could not allocate the captured monitor texture.");
        pipelineReady = true;
        return true;
    }

    bool StartCapture() noexcept
    {
        ComPtr<IGraphicsCaptureItemInterop> interop;
        HRESULT status = Factory(RuntimeClass_Windows_Graphics_Capture_GraphicsCaptureItem, interop);
        if (FAILED(status))
            return Fail(status, L"Windows Graphics Capture is not available.");
        status = interop->CreateForMonitor(monitor, __uuidof(Capture::IGraphicsCaptureItem), reinterpret_cast<void**>(captureItem.ReleaseAndGetAddressOf()));
        if (FAILED(status))
            return Fail(status, L"Windows could not capture the selected monitor.");
        Graphics::SizeInt32 size{};
        status = captureItem->get_Size(&size);
        if (FAILED(status) || size.Width != destination.right - destination.left ||
            size.Height != destination.bottom - destination.top)
            return Fail(FAILED(status) ? status : E_INVALIDARG,
                        L"The captured monitor size changed. Reset zoom and try again.");
        ComPtr<Capture::IDirect3D11CaptureFramePoolStatics2> statics;
        status = Factory(RuntimeClass_Windows_Graphics_Capture_Direct3D11CaptureFramePool, statics);
        if (FAILED(status))
            return Fail(status, L"Background monitor capture is unavailable on this Windows version.");
        status = statics->CreateFreeThreaded(captureDevice.Get(),
                                             DirectX::DirectXPixelFormat_R16G16B16A16Float,
                                             3,
                                             size,
                                             &pool);
        if (FAILED(status) || FAILED(status = pool->CreateCaptureSession(captureItem.Get(), &session)))
            return Fail(status, L"Could not start the monitor capture frame pool.");
        ComPtr<Capture::IGraphicsCaptureSession2> cursorControl;
        status = session.As(&cursorControl);
        if (FAILED(status) || FAILED(status = cursorControl->put_IsCursorCaptureEnabled(false)))
            return Fail(status, L"Windows cannot disable the captured cursor; this version is unsupported.");
        // Respect Windows' capture indicator. Borderless capture requires a
        // separate consent/capability flow and is not silently requested here.
        if (!gate.Begin(size.Width, size.Height, GetTickCount64()))
            return Fail(E_UNEXPECTED, L"The monitor capture generation could not be started.");
        status = session->StartCapture();
        if (FAILED(status))
            return Fail(status, L"Windows could not start monitor capture.");
        return true;
    }

    bool PollCapture() noexcept
    {
        if (!pool || !captured)
            return Fail(E_UNEXPECTED, L"The monitor capture resources were lost.");
        if (gate.State(GetTickCount64()) == glide::gpu::FrameState::TimedOut)
            return Fail(HRESULT_FROM_WIN32(WAIT_TIMEOUT), L"Windows did not deliver the first monitor frame within two seconds.");
        const auto generation = gate.Generation();
        const auto operation = epoch;
        // At most the pool's three buffers. Empty is normal for static content.
        for (unsigned count = 0; count != 3; ++count)
        {
            ComPtr<Capture::IDirect3D11CaptureFrame> next;
            HRESULT status = pool->TryGetNextFrame(&next);
            if (FAILED(status))
                return Fail(status, L"Windows monitor capture stopped delivering frames.");
            if (!next)
                break;
            struct ReturnFrame
            {
                ComPtr<Capture::IDirect3D11CaptureFrame>& value;
                ~ReturnFrame() noexcept { CloseObject(value); }
            } returnFrame{ next };
            Graphics::SizeInt32 size{};
            ComPtr<Direct3D::IDirect3DSurface> surface;
            ComPtr<SurfaceAccess> access;
            ComPtr<ID3D11Texture2D> texture;
            if (FAILED(status = next->get_ContentSize(&size)) ||
                FAILED(status = next->get_Surface(&surface)) ||
                FAILED(status = surface.As(&access)) ||
                FAILED(status = access->GetInterface(__uuidof(ID3D11Texture2D),
                                                     reinterpret_cast<void**>(texture.GetAddressOf()))))
                return Fail(status, L"Could not read a captured monitor frame.");
            D3D11_TEXTURE2D_DESC description{};
            texture->GetDesc(&description);
            if (operation != epoch || size.Width != destination.right - destination.left ||
                size.Height != destination.bottom - destination.top ||
                description.Width < static_cast<UINT>(size.Width) ||
                description.Height < static_cast<UINT>(size.Height) ||
                description.Format != DXGI_FORMAT_R16G16B16A16_FLOAT ||
                description.SampleDesc.Count != 1 || description.ArraySize != 1 ||
                gate.State(GetTickCount64()) == glide::gpu::FrameState::TimedOut)
                return Fail(E_INVALIDARG, L"The captured monitor geometry or pixel format changed.");
            ID3D11ShaderResourceView* empty[2]{};
            context->PSSetShaderResources(0, 2, empty);
            const D3D11_BOX box{ 0, 0, 0, static_cast<UINT>(size.Width), static_cast<UINT>(size.Height), 1 };
            context->CopySubresourceRegion(captured.Get(), 0, 0, 0, 0, texture.Get(), 0, &box);
            status = device->GetDeviceRemovedReason();
            if (FAILED(status))
                return Fail(status, L"The graphics device was removed during monitor capture.");
            if (!gate.Accept(generation, size.Width, size.Height))
                return Fail(E_UNEXPECTED, L"A cancelled monitor capture frame was rejected.");
        }
        pending = !gate.Ready();
        if (pending)
        {
            owner.Error(HRESULT_FROM_WIN32(ERROR_IO_PENDING), L"Waiting for the first captured monitor frame.");
            return false;
        }
        return true;
    }

    bool CursorTexture(HCURSOR shape)
    {
        const auto now = GetTickCount64();
        if (shape == lastCursor && cursorView && now >= cursorSampleTime && now - cursorSampleTime < 100)
            return true;
        IconCopy copy;
        copy.icon = CopyIcon(reinterpret_cast<HICON>(shape));
        if (!copy.icon || !GetIconInfo(copy.icon, &copy.information))
            return Fail(NativeError(), L"Could not read the current cursor shape.");
        BITMAP bitmap{};
        const bool monochrome = !copy.information.hbmColor;
        HBITMAP source = monochrome ? copy.information.hbmMask : copy.information.hbmColor;
        if (!source || GetObjectW(source, sizeof(bitmap), &bitmap) != static_cast<int>(sizeof(bitmap)))
            return Fail(NativeError(), L"Could not inspect the current cursor bitmap.");
        const LONG height = monochrome ? bitmap.bmHeight / 2 : bitmap.bmHeight;
        if (bitmap.bmWidth <= 0 || height <= 0 || bitmap.bmWidth > static_cast<LONG>(kMaximumCursor) ||
            height > static_cast<LONG>(kMaximumCursor) || (monochrome && bitmap.bmHeight != height * 2) ||
            copy.information.xHotspot >= static_cast<DWORD>(bitmap.bmWidth) ||
            copy.information.yHotspot >= static_cast<DWORD>(height))
            return Fail(E_INVALIDARG, L"The current cursor bitmap is outside supported bounds.");
        const UINT width = static_cast<UINT>(bitmap.bmWidth);
        const UINT tall = static_cast<UINT>(height);
        std::vector<std::uint32_t> pixels, background;
        if (monochrome)
        {
            const auto count = static_cast<std::size_t>(width) * tall;
            background.resize(count * 2);
            BITMAPINFO information{};
            information.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            information.bmiHeader.biWidth = bitmap.bmWidth;
            information.bmiHeader.biHeight = -bitmap.bmHeight;
            information.bmiHeader.biPlanes = 1;
            information.bmiHeader.biBitCount = 32;
            information.bmiHeader.biCompression = BI_RGB;
            HDC dc = CreateCompatibleDC(nullptr);
            if (!dc)
                return Fail(NativeError(), L"Could not read a monochrome cursor.");
            const int rows = GetDIBits(dc, source, 0, static_cast<UINT>(bitmap.bmHeight), background.data(), &information, DIB_RGB_COLORS);
            DeleteDC(dc);
            if (rows != bitmap.bmHeight)
                return Fail(NativeError(), L"Could not read a monochrome cursor.");
            if (!glide::gpu::BuildCursorPixels(true, width, tall, { background.data(), count }, { background.data() + count, count }, pixels))
                return Fail(E_FAIL, L"Could not convert the monochrome cursor bitmap.");
        }
        else
        {
            if (!DrawCursorBitmap(copy.icon, width, tall, false, pixels) ||
                !DrawCursorBitmap(copy.icon, width, tall, true, background))
                return Fail(NativeError(), L"Could not rasterize the current cursor.");
            if (!glide::gpu::BuildCursorPixels(false, width, tall, { pixels.data(), pixels.size() }, { background.data(), background.size() }, pixels))
                return Fail(E_FAIL, L"Could not convert the color cursor bitmap.");
        }
        std::uint64_t hash = 1469598103934665603ull;
        for (const auto pixel : pixels)
        {
            hash ^= pixel;
            hash *= 1099511628211ull;
        }
        if (!cursorView || cursorHash != hash || width != cursorWidth || tall != cursorHeight ||
            monochrome != cursorMonochrome)
        {
            D3D11_TEXTURE2D_DESC description{};
            description.Width = width;
            description.Height = tall;
            description.MipLevels = description.ArraySize = 1;
            description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            description.SampleDesc.Count = 1;
            description.Usage = D3D11_USAGE_IMMUTABLE;
            description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            const D3D11_SUBRESOURCE_DATA data{ pixels.data(), width * 4, 0 };
            ComPtr<ID3D11Texture2D> next;
            ComPtr<ID3D11ShaderResourceView> nextView;
            HRESULT status = device->CreateTexture2D(&description, &data, &next);
            if (FAILED(status) || FAILED(status = device->CreateShaderResourceView(next.Get(), nullptr, &nextView)))
                return Fail(status, L"Could not upload the cursor image to the GPU.");
            cursor = next;
            cursorView = nextView;
            cursorHash = hash;
        }
        cursorWidth = width;
        cursorHeight = tall;
        hotspotX = copy.information.xHotspot;
        hotspotY = copy.information.yHotspot;
        cursorMonochrome = monochrome;
        cursorSampleTime = now;
        lastCursor = shape;
        return true;
    }

    bool Projected(glide::Vec2 point) const noexcept
    {
        return glide::cursor::HotspotVisible(frame, point);
    }

    bool UpdateScopeText() noexcept
    {
        std::array<wchar_t, 128> value{};
        const int length = swprintf_s(value.data(), value.size(), L"X %d   Y %d\n%.0f x %.0f px   |   %.2fx", static_cast<int>(frame.source.left), static_cast<int>(frame.source.top), static_cast<double>(frame.source.right) - frame.source.left, static_cast<double>(frame.source.bottom) - frame.source.top, frame.transform.zoom);
        if (length <= 0)
            return false;
        if (scopeTextView && value == scopeTextValue)
            return true;
        if (!scopeTextBitmap.Draw(value.data()))
            return false;
        const void* pixels = scopeTextBitmap.bytes;
        if (!pixels)
            return false;
        if (!scopeTextView)
        {
            D3D11_TEXTURE2D_DESC description{};
            description.Width = ScopeTextBitmap::Width;
            description.Height = ScopeTextBitmap::Height;
            description.MipLevels = description.ArraySize = 1;
            description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            description.SampleDesc.Count = 1;
            description.Usage = D3D11_USAGE_DEFAULT;
            description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            if (FAILED(device->CreateTexture2D(&description, nullptr, &scopeText)) ||
                FAILED(device->CreateShaderResourceView(scopeText.Get(), nullptr, &scopeTextView)))
            {
                scopeTextView.Reset();
                scopeText.Reset();
                return false;
            }
        }
        context->UpdateSubresource(scopeText.Get(), 0, nullptr, pixels, ScopeTextBitmap::Width * 4u, 0);
        scopeTextValue = value;
        return true;
    }

    void RenderIndicator(DrawConstants data) noexcept
    {
        glide::scope::Layout layout;
        if (!glide::scope::MakeLayout(frame, owner.indicator_, layout) ||
            ((!layout.showOverview || layout.overviewAlpha <= 0) && (!layout.showScreenOutline || layout.alpha <= 0)))
            return;
        const double width = data.viewport[0], height = data.viewport[1];
        ID3D11ShaderResourceView* resources[]{ capturedView.Get(), nullptr };
        context->PSSetShaderResources(0, 2, resources);
        context->OMSetBlendState(cursorBlend.Get(), nullptr, 0xffffffffu);
        const auto draw = [&](glide::Rect rectangle, ID3D11PixelShader* shader, double r, double g, double b, double alpha) noexcept {
            if (alpha <= 0 || rectangle.right <= rectangle.left || rectangle.bottom <= rectangle.top)
                return;
            data.rectangle[0] = static_cast<float>(rectangle.left / width);
            data.rectangle[1] = static_cast<float>(rectangle.top / height);
            data.rectangle[2] = static_cast<float>(rectangle.right / width);
            data.rectangle[3] = static_cast<float>(rectangle.bottom / height);
            data.color[0] = static_cast<float>(r * alpha);
            data.color[1] = static_cast<float>(g * alpha);
            data.color[2] = static_cast<float>(b * alpha);
            data.color[3] = static_cast<float>(alpha);
            context->UpdateSubresource(constants.Get(), 0, nullptr, &data, 0, 0);
            context->PSSetShader(shader, nullptr, 0);
            context->Draw(6, 0);
        };
        const auto stroke = [&](glide::Rect rectangle, double border, double r, double g, double b, double alpha) noexcept {
            for (const auto edge : glide::scope::Stroke(rectangle, border))
                draw(edge, solidShader.Get(), r, g, b, alpha);
        };
        const auto outlined = [&](glide::Rect rectangle, double alpha) noexcept {
            const glide::Rect shadow{ rectangle.left - 2, rectangle.top - 2, rectangle.right + 2, rectangle.bottom + 2 };
            stroke(shadow, layout.border + 4, 0, 0, 0, alpha);
            stroke(rectangle, layout.border, 0.05, 0.75, 1, alpha);
        };
        // Draw the on-screen source frame beneath the overview so a source
        // region crossing the chosen corner cannot obscure the miniature.
        if (layout.showScreenOutline)
            outlined(layout.screenCrop, layout.alpha);
        if (layout.showOverview && layout.overviewAlpha > 0)
        {
            // This samples the complete raw monitor texture, never the zoomed
            // back buffer or another host. WDA exclusion remains unchanged.
            draw(layout.overview, overviewShader.Get(), 1, 1, 1, layout.overviewAlpha);
            outlined(layout.overview, layout.overviewAlpha);
            outlined(layout.crop, layout.overviewAlpha);
        }
        const double detailsAlpha = layout.showOverview ? layout.overviewAlpha : layout.alpha;
        if (layout.showDetails && detailsAlpha > 0 && UpdateScopeText())
        {
            draw(layout.details, solidShader.Get(), 0, 0, 0, detailsAlpha * 0.85);
            resources[1] = scopeTextView.Get();
            context->PSSetShaderResources(0, 2, resources);
            draw(layout.details, textShader.Get(), 1, 1, 1, detailsAlpha);
        }
    }

    bool ReadyForRender() noexcept
    {
        // First Present accepts a submission; it does not prove that DWM has
        // completed startup. Poll through the first post-submission signal so
        // the owner thread keeps servicing controls, reset and the watchdog.
        // Probe before altering the last displayed frame's metadata.
        const bool coldQueue = presentationQueue.Cold();
        const DWORD wait = WaitForSingleObject(frameLatency, presentationQueue.WaitMilliseconds(owner.passiveRefresh_));
        const HRESULT waitError = wait == WAIT_TIMEOUT ? HRESULT_FROM_WIN32(WAIT_TIMEOUT) :
                                                         (wait == WAIT_FAILED ? NativeError() : E_UNEXPECTED);
        if (wait != WAIT_OBJECT_0)
        {
            const HRESULT removed = device->GetDeviceRemovedReason();
            if (FAILED(removed))
            {
                pipelineReady = false;
                return Fail(removed, L"The graphics device was removed while waiting for presentation.");
            }
        }
        const auto decision = presentationQueue.Observe(
            wait == WAIT_OBJECT_0 ? glide::gpu::QueueSignal::Ready :
                                    (wait == WAIT_TIMEOUT ? glide::gpu::QueueSignal::Busy : glide::gpu::QueueSignal::Failed),
            GetTickCount64(),
            owner.passiveRefresh_);
        if (decision == glide::gpu::QueueDecision::Pending)
        {
            pending = true;
            return owner.Error(HRESULT_FROM_WIN32(ERROR_IO_PENDING),
                               L"Waiting for the GPU presentation queue to become ready.");
        }
        if (decision != glide::gpu::QueueDecision::Render)
        {
            pipelineReady = false;
            return Fail(wait == WAIT_OBJECT_0 ? HRESULT_FROM_WIN32(WAIT_TIMEOUT) : waitError,
                        wait != WAIT_OBJECT_0 && wait != WAIT_TIMEOUT ? L"Windows could not wait for the GPU presentation queue." : (coldQueue ? L"The GPU presentation queue did not finish starting within two seconds." : L"The GPU presentation queue stopped responding."));
        }
        return true;
    }

    bool Render(bool& drewCursor, HCURSOR& cursorShape)
    {
        drewCursor = false;
        cursorShape = nullptr;
        if (!presentationQueue.HasUnsubmittedGrant())
            return Fail(E_UNEXPECTED, L"The GPU frame has no presentation readiness grant.");
        const float width = static_cast<float>(destination.right - destination.left);
        const float height = static_cast<float>(destination.bottom - destination.top);
        DrawConstants data{ { static_cast<float>(sampling.left), static_cast<float>(sampling.top), static_cast<float>(sampling.right), static_cast<float>(sampling.bottom) },
                            { 0, 0, 1, 1 },
                            { width, height, 0, 0 },
                            { 0, 0, 0, 0 } };
        const D3D11_VIEWPORT viewport{ 0, 0, width, height, 0, 1 };
        ID3D11RenderTargetView* targets[]{ target.Get() };
        ID3D11Buffer* buffers[]{ constants.Get() };
        ID3D11SamplerState* samplers[]{ linearSampler.Get(), pointSampler.Get() };
        ID3D11ShaderResourceView* resources[]{ capturedView.Get(), nullptr };
        context->OMSetRenderTargets(1, targets, nullptr);
        context->RSSetViewports(1, &viewport);
        context->RSSetState(rasterizer.Get());
        context->IASetInputLayout(nullptr);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(vertex.Get(), nullptr, 0);
        context->VSSetConstantBuffers(0, 1, buffers);
        context->PSSetConstantBuffers(0, 1, buffers);
        context->PSSetSamplers(0, 2, samplers);
        context->PSSetShaderResources(0, 2, resources);
        context->UpdateSubresource(constants.Get(), 0, nullptr, &data, 0, 0);
        context->OMSetBlendState(nullptr, nullptr, 0xffffffffu);
        context->PSSetShader(desktopShader.Get(), nullptr, 0);
        context->Draw(6, 0);

        // HUD precedes the replacement cursor. Its opaque portions can never
        // cover the cursor pixels used to justify the global visibility lease.
        RenderIndicator(data);

        CURSORINFO information{};
        information.cbSize = sizeof(information);
        POINT point{};
        if (owner.cursorEnabled_ && (!GetCursorInfo(&information) || !GetPhysicalCursorPos(&point)))
            return Fail(NativeError(), L"Could not locate the pointer for magnifier presentation.");
        const glide::Vec2 sourcePoint{ static_cast<double>(point.x), static_cast<double>(point.y) };
        const bool visible = owner.cursorEnabled_ && information.hCursor && !(information.flags & kCursorSuppressed) &&
                             ((information.flags & CURSOR_SHOWING) || cursorHiddenByUs) &&
                             MonitorFromPoint(point, MONITOR_DEFAULTTONULL) == monitor &&
                             Projected(sourcePoint);
        if (visible)
        {
            if (!CursorTexture(information.hCursor))
                return false;
            glide::Vec2 drawnPoint = sourcePoint;
            if (owner.cursorResolver_)
            {
                glide::Vec2 precise{};
                const glide::Rect bounds{ static_cast<double>(destination.left), static_cast<double>(destination.top), static_cast<double>(destination.right), static_cast<double>(destination.bottom) };
                if (owner.cursorResolver_(owner.cursorResolverContext_, sourcePoint, frame.transform, bounds, precise) &&
                    glide::detail::Finite(precise) && std::abs(precise.x - sourcePoint.x) <= 0.500001 &&
                    std::abs(precise.y - sourcePoint.y) <= 0.500001 && Projected(precise))
                    drawnPoint = precise;
            }
            const auto screen = glide::SourceToScreen(drawnPoint, frame.transform);
            const double zoom = frame.transform.zoom;
            const double left = screen.x - destination.left - hotspotX * zoom;
            const double top = screen.y - destination.top - hotspotY * zoom;
            data.rectangle[0] = static_cast<float>(left / width);
            data.rectangle[1] = static_cast<float>(top / height);
            data.rectangle[2] = static_cast<float>((left + cursorWidth * zoom) / width);
            data.rectangle[3] = static_cast<float>((top + cursorHeight * zoom) / height);
            data.viewport[2] = cursorMonochrome ? 1.0f : 0.0f;
            context->UpdateSubresource(constants.Get(), 0, nullptr, &data, 0, 0);
            resources[1] = cursorView.Get();
            context->PSSetShaderResources(0, 2, resources);
            context->PSSetShader(cursorShader.Get(), nullptr, 0);
            context->OMSetBlendState(cursorBlend.Get(), nullptr, 0xffffffffu);
            context->Draw(6, 0);
            drewCursor = true;
            cursorShape = information.hCursor;
        }
        ID3D11ShaderResourceView* empty[2]{};
        context->PSSetShaderResources(0, 2, empty);
        const HRESULT status = device->GetDeviceRemovedReason();
        if (FAILED(status))
            return Fail(status, L"The graphics device was removed while rendering zoom.");
        return true;
    }
};

GpuMonitorMagnifier::GpuMonitorMagnifier() noexcept = default;
GpuMonitorMagnifier::~GpuMonitorMagnifier() noexcept
{
    Destroy();
}

bool GpuMonitorMagnifier::Error(HRESULT result, const wchar_t* reason) noexcept
{
    result_ = result;
    reason_ = reason;
    SetLastError(HRESULT_FACILITY(result) == FACILITY_WIN32 ? HRESULT_CODE(result) : ERROR_GEN_FAILURE);
    return false;
}

bool GpuMonitorMagnifier::Initialize(HINSTANCE instance, MagnificationApi& api) noexcept
{
    if (!instance || !api.SetInputTransform || !api.ShowSystemCursor)
        return Error(E_INVALIDARG, L"The magnifier input and cursor APIs are unavailable.");
    if (!impl_)
        impl_.reset(new (std::nothrow) Impl(*this));
    if (!impl_)
        return Error(E_OUTOFMEMORY, L"Could not allocate the GPU magnifier.");
    auto& state = *impl_;
    if (state.initialized)
        return state.instance == instance;
    OSVERSIONINFOEXW version{};
    version.dwOSVersionInfoSize = sizeof(version);
    version.dwMajorVersion = 10;
    version.dwBuildNumber = 19041;
    ULONGLONG condition = VerSetConditionMask(0, VER_MAJORVERSION, VER_GREATER_EQUAL);
    condition = VerSetConditionMask(condition, VER_MINORVERSION, VER_GREATER_EQUAL);
    condition = VerSetConditionMask(condition, VER_BUILDNUMBER, VER_GREATER_EQUAL);
    if (!VerifyVersionInfoW(&version, VER_MAJORVERSION | VER_MINORVERSION | VER_BUILDNUMBER, condition))
        return Error(HRESULT_FROM_WIN32(ERROR_OLD_WIN_VERSION),
                     L"GPU monitor zoom requires Windows 10 version 2004 or newer.");
    BOOL composed = FALSE;
    HRESULT status = DwmIsCompositionEnabled(&composed);
    if (FAILED(status) || !composed)
        return Error(FAILED(status) ? status : HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED),
                     L"GPU monitor zoom requires Windows desktop composition.");
    if (!state.roInitialized)
    {
        status = RoInitialize(RO_INIT_MULTITHREADED);
        if (status == RPC_E_CHANGED_MODE)
            status = RoInitialize(RO_INIT_SINGLETHREADED);
        if (FAILED(status))
            return Error(status, L"Could not initialize Windows Graphics Capture.");
        state.roInitialized = true;
    }
    ComPtr<Capture::IGraphicsCaptureSessionStatics> statics;
    status = Factory(RuntimeClass_Windows_Graphics_Capture_GraphicsCaptureSession, statics);
    boolean supported = false;
    if (FAILED(status) || FAILED(status = statics->IsSupported(&supported)) || !supported)
        return Error(FAILED(status) ? status : HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED),
                     L"Windows Graphics Capture is not supported by this desktop session.");
    state.instance = instance;
    state.initialized = true;
    result_ = S_OK;
    reason_ = L"";
    return true;
}

bool GpuMonitorMagnifier::Prepare(const glide::monitor::Frame& frame) noexcept
{
    if (!impl_ || !impl_->initialized)
        return Error(E_UNEXPECTED, L"The GPU magnifier was not initialized.");
    auto& state = *impl_;
    // Verify the old visible image before preparing a new image or input map.
    if (Active() && !state.EnsureOrder(false))
        return false;
    state.pending = false;
    if (!glide::OnInputDesktop())
        return state.Fail(HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED), L"The input desktop is not available.");
    glide::gpu::SamplingRect sampling;
    if (!glide::gpu::MakeSamplingRect(frame, sampling))
        return state.Fail(E_INVALIDARG, L"The requested magnifier geometry is invalid.");
    const RECT destination{ frame.destination.left, frame.destination.top, frame.destination.right, frame.destination.bottom };
    const UINT width = static_cast<UINT>(destination.right - destination.left);
    const UINT height = static_cast<UINT>(destination.bottom - destination.top);
    if (width > kMaximumTexture || height > kMaximumTexture)
        return state.Fail(E_INVALIDARG, L"The monitor dimensions exceed the GPU texture limit.");
    const HMONITOR selected = MonitorFromRect(&destination, MONITOR_DEFAULTTONULL);
    MONITORINFO information{};
    information.cbSize = sizeof(information);
    if (!selected || !GetMonitorInfoW(selected, &information) || !SameRect(information.rcMonitor, destination))
        return state.Fail(E_INVALIDARG, L"The selected monitor changed. Reset zoom and try again.");
    const auto preparation = glide::gpu::PrepareDecision(state.staged, state.presentationQueue.HasUnsubmittedGrant(), Matches(frame) && state.pipelineReady && state.monitor == selected && SameRect(state.destination, destination));
    if (preparation == glide::gpu::Preparation::Reuse)
    {
        const auto operation = state.epoch;
        RECT actual{};
        if (!GetWindowRect(state.host, &actual) || !SameRect(actual, destination))
            return state.Fail(E_ABORT, L"The staged magnifier window changed before reuse.");
        if (!state.EnsureOrder(false))
            return false;
        if (state.epoch != operation || !Matches(frame) || !glide::OnInputDesktop())
            return state.Fail(E_ABORT, L"GPU frame reuse was cancelled by a desktop or window change.");
        // Show refreshes cursor/HUD pixels using this same grant. The camera is
        // unchanged, but a deferred hook acknowledgement may have taken time.
        state.stagedReused = true;
        result_ = S_OK;
        reason_ = L"";
        return true;
    }
    if (preparation == glide::gpu::Preparation::Rebuild)
    {
        // Never reset only the queue bookkeeping after consuming a native
        // signal. A new geometry needs a new native presentation queue too.
        Hide();
        state.pipelineReady = false;
        if (Active())
            return state.Fail(E_ABORT, L"The abandoned staged magnifier view could not be hidden.");
    }
    state.staged = state.stagedCursor = state.stagedReused = false;
    state.stagedCursorShape = nullptr;
    if (!state.EnsureWindow())
        return false;
    // The hidden initial host must not skip protection checks or appear before
    // the owner commits its matching input map. Use the requested monitor bounds
    // while its native layout and prepared image have not been established yet.
    if (!state.EnsureOrder(false, &destination))
        return false;
    if (state.monitor != selected || !SameRect(state.destination, destination) ||
        state.geometryInvalid || !state.pipelineReady)
    {
        Hide();
        state.Cleanup();
        state.geometryInvalid = false;
        const auto layoutEpoch = state.epoch;
        state.layingOut = true;
        const bool positioned = SetWindowPos(state.host, HWND_TOPMOST, destination.left, destination.top, static_cast<int>(width), static_cast<int>(height), SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOREDRAW) != FALSE;
        state.layingOut = false;
        if (!positioned)
            return state.Fail(NativeError(), L"Could not position the magnifier on the selected monitor.");
        RECT actual{};
        if (!GetWindowRect(state.host, &actual) || !SameRect(actual, destination) ||
            state.epoch != layoutEpoch || state.geometryInvalid)
            return state.Fail(E_UNEXPECTED, L"Windows moved or resized the magnifier unexpectedly.");
        state.destination = destination;
        state.monitor = selected;
        if (!state.CreateDevice(selected) || !state.CreatePipeline(width, height))
            return false;
    }
    // Hide/retarget intentionally advanced the epoch above. Once resources are
    // established, guard the calls that can service native cancellation.
    const auto preparedEpoch = state.epoch;
    if (!state.session)
    {
        if (!state.StartCapture())
            return false;
        if (!state.host || state.geometryInvalid || state.epoch != preparedEpoch || !glide::OnInputDesktop())
            return state.Fail(E_ABORT, L"Monitor capture startup was cancelled by a desktop or window change.");
        // Device/shader startup can take longer than the input watchdog. Give
        // the UI owner one deterministic pending boundary to discard stale
        // input and rebase its clock before acquiring any global mapping.
        // Retained screenshot sessions do not enter this branch.
        state.pending = true;
        return Error(HRESULT_FROM_WIN32(ERROR_IO_PENDING), L"Waiting for the first captured monitor frame.");
    }
    if (!state.PollCapture())
        return false;
    if (!state.host || state.geometryInvalid || state.epoch != preparedEpoch || !glide::OnInputDesktop())
        return state.Fail(E_ABORT, L"Magnifier preparation was cancelled by a desktop or window change.");
    if (!state.ReadyForRender())
        return false;
    if (!state.host || state.geometryInvalid || state.epoch != preparedEpoch || !glide::OnInputDesktop())
        return state.Fail(E_ABORT, L"GPU readiness was cancelled before preparing the next frame.");
    state.prepared = false;
    state.frame = frame;
    state.sampling = sampling;
    bool cursorDrawn = false;
    HCURSOR cursorShape = nullptr;
    try
    {
        // Prepare the complete next image after its bounded pacing check,
        // before the caller changes the global input mapping. Show then only
        // presents this staged image. This narrows, but cannot atomically erase,
        // the separate Windows input-mapping/compositor presentation interval.
        if (!state.Render(cursorDrawn, cursorShape))
            return false;
    }
    catch (...)
    {
        return state.Fail(E_OUTOFMEMORY, L"Could not allocate the magnifier cursor image.");
    }
    if (!state.host || state.geometryInvalid || state.epoch != preparedEpoch || !glide::OnInputDesktop())
        return state.Fail(E_ABORT, L"GPU frame preparation was cancelled before changing input mapping.");
    state.staged = true;
    state.stagedCursor = cursorDrawn;
    state.stagedCursorEnabled = cursorEnabled_;
    state.stagedIndicatorRevision = indicatorRevision_;
    state.stagedCursorShape = cursorShape;
    state.prepared = true;
    state.pending = false;
    if (!state.EnsureOrder(false))
        return false;
    result_ = S_OK;
    reason_ = L"";
    return true;
}

bool GpuMonitorMagnifier::Show() noexcept
{
    if (!impl_)
        return Error(E_UNEXPECTED, L"The GPU magnifier was not initialized.");
    auto& state = *impl_;
    state.pending = false;
    const auto operation = state.epoch;
    if (!state.prepared || !state.host || !state.swap || state.geometryInvalid ||
        !state.gate.Ready() || !glide::OnInputDesktop())
        return state.Fail(E_ABORT, L"The prepared GPU view is no longer valid.");
    if (!state.EnsureOrder(false))
        return false;
    try
    {
        bool cursorDrawn = state.stagedCursor;
        HCURSOR cursorShape = state.stagedCursorShape;
        if (!state.staged)
        {
            // The exact input mapping is unchanged on this fast path. Pull
            // fresh content and cursor pixels without another input write.
            if (!state.PollCapture())
                return false;
            if (!state.ReadyForRender())
                return false;
            if (!state.Render(cursorDrawn, cursorShape))
                return false;
        }
        else if (state.stagedReused || state.stagedCursorEnabled != cursorEnabled_ || state.stagedIndicatorRevision != indicatorRevision_)
        {
            // Ownership or fade alpha may change after Prepare. Reuse its
            // existing grant without consuming a second readiness signal.
            if (state.stagedReused && !state.PollCapture())
                return false;
            if (!state.Render(cursorDrawn, cursorShape))
                return false;
        }
        state.staged = state.stagedCursor = state.stagedReused = false;
        state.stagedCursorShape = nullptr;
        if (state.epoch != operation || !state.prepared || !glide::OnInputDesktop())
            return state.Fail(E_ABORT, L"GPU presentation was cancelled before display.");
        // Readiness polling can defer or wait. Recheck immediately before the
        // next submission, as well as before entering that polling above.
        if (!state.EnsureOrder(false))
            return false;
        // Present a complete desktop+cursor image. No HWND source/scale changes
        // or forced WM_PAINT occur during ordinary zoom and pan.
        const HRESULT status = state.swap->Present(1, 0);
        if (status != S_OK)
            return state.Fail(status, status == DXGI_STATUS_OCCLUDED ? L"Windows reported the magnifier presentation as occluded. Zoom was released." : L"Windows could not present the magnified GPU frame.");
        if (state.epoch != operation || !state.prepared || !state.host || !glide::OnInputDesktop())
            return state.Fail(E_ABORT, L"GPU presentation was cancelled by a desktop change.");
        if (!state.presentationQueue.Submitted(GetTickCount64()))
            return state.Fail(E_UNEXPECTED, L"The GPU presentation readiness grant was cancelled.");
        if (!state.EnsureOrder(true))
            return false;
        if (state.epoch != operation || !state.prepared || state.geometryInvalid ||
            !state.host || !IsWindowVisible(state.host))
            return state.Fail(E_ABORT, L"The magnifier window was closed during presentation.");
        state.submittedCursor.Commit(state.frame, cursorDrawn ? reinterpret_cast<std::uintptr_t>(cursorShape) : 0);
        state.submittedFrame = state.frame;
        state.hasSubmittedFrame = true;
        result_ = S_OK;
        reason_ = L"";
        return true;
    }
    catch (...)
    {
        return state.Fail(E_OUTOFMEMORY, L"Could not allocate the magnifier cursor image.");
    }
}

GpuMonitorMagnifier::VisualRefresh GpuMonitorMagnifier::RefreshPassive(const glide::monitor::Frame& frame) noexcept
{
    const bool previous = passiveRefresh_;
    passiveRefresh_ = true;
    const auto result = glide::monitor::RefreshPassive(*this, frame);
    passiveRefresh_ = previous;
    return result;
}

void GpuMonitorMagnifier::SetCursorEnabled(bool enabled) noexcept
{
    cursorEnabled_ = enabled;
}

void GpuMonitorMagnifier::SetScopeIndicator(const ScopeIndicator& options) noexcept
{
    const auto sanitized = glide::scope::Sanitize(options);
    if (!glide::scope::Same(indicator_, sanitized))
    {
        indicator_ = sanitized;
        ++indicatorRevision_;
    }
}

bool GpuMonitorMagnifier::TryGetPresentedFrame(glide::monitor::Frame& frame) const noexcept
{
    if (!impl_ || !impl_->hasSubmittedFrame || !impl_->pipelineReady || impl_->geometryInvalid ||
        !impl_->gate.Ready() || !Active() || !glide::OnInputDesktop())
        return false;
    frame = impl_->submittedFrame;
    return true;
}

bool GpuMonitorMagnifier::CanRetainVisual(const glide::monitor::Frame& frame) noexcept
{
    if (!impl_)
        return false;
    auto& state = *impl_;
    const auto operation = state.epoch;
    const auto eligible = [&]() noexcept {
        return state.hasSubmittedFrame && state.prepared && !state.staged && state.pipelineReady &&
               state.gate.Ready() && !state.geometryInvalid &&
               state.presentationQueue.HasSubmittedFrame() && !state.presentationQueue.HasUnsubmittedGrant() &&
               glide::monitor::SameFrame(frame, state.submittedFrame) &&
               glide::monitor::SameFrame(state.frame, state.submittedFrame) &&
               (cursorEnabled_ || !state.submittedCursor.HasCursor()) &&
               Active() && glide::OnInputDesktop() && state.epoch == operation;
    };
    return eligible() && state.EnsureOrder(false) && eligible();
}

void GpuMonitorMagnifier::Hide(bool preserveCapture) noexcept
{
    if (!impl_)
        return;
    auto& state = *impl_;
    ++state.epoch;
    state.orderGuard.Reset();
    state.prepared = state.pending = false;
    state.submittedCursor.Clear();
    state.hasSubmittedFrame = false;
    state.submittedFrame = {};
    state.staged = state.stagedCursor = state.stagedReused = false;
    state.stagedCursorShape = nullptr;
    if (state.host)
    {
        const HWND old = state.host;
        ShowWindow(old, SW_HIDE);
        if (IsWindowVisible(old))
        {
            // Never knowingly retain a visible stale surface while the owner
            // releases its input mapping. A surviving HWND is retained so the
            // application can detect Active() and terminate/recover safely.
            if (!DestroyWindow(old) && IsWindowVisible(old))
                Error(NativeError(), L"Windows could not hide or destroy the magnifier window.");
            else
            {
                state.host = nullptr;
                state.windowConfigured = false;
                state.geometryInvalid = true;
                preserveCapture = false;
            }
        }
    }
    if (!preserveCapture)
        state.StopCapture();
}

void GpuMonitorMagnifier::Destroy() noexcept
{
    if (!impl_)
        return;
    Hide();
    auto& state = *impl_;
    state.Cleanup();
    if (state.host)
    {
        const HWND old = state.host;
        if (DestroyWindow(old) || !IsWindow(old))
            state.host = nullptr;
    }
}

bool GpuMonitorMagnifier::Active() const noexcept
{
    return impl_ && impl_->host && IsWindowVisible(impl_->host);
}

bool GpuMonitorMagnifier::Matches(const glide::monitor::Frame& frame) const noexcept
{
    return impl_ && impl_->prepared && !impl_->geometryInvalid && impl_->gate.Ready() &&
           impl_->host && glide::monitor::SameFrame(frame, impl_->frame);
}

bool GpuMonitorMagnifier::Pending() const noexcept
{
    return impl_ && impl_->pending;
}

bool GpuMonitorMagnifier::CanRetain(const glide::session::Snapshot& snapshot) noexcept
{
    if (!impl_)
        return false;
    auto& state = *impl_;
    const auto operation = state.epoch;
    const auto frame = state.frame;
    const glide::session::Snapshot displayed{ { 1, 0, 0 },
                                              { true, state.frame.source, state.frame.destination } };
    const auto eligible = [&]() noexcept {
        return state.pending && state.prepared && !state.staged && state.pipelineReady &&
               state.gate.Ready() && !state.geometryInvalid &&
               state.presentationQueue.Cold() && state.presentationQueue.HasSubmittedFrame() &&
               !state.presentationQueue.HasUnsubmittedGrant() && snapshot == displayed &&
               Active() && glide::OnInputDesktop() && state.epoch == operation;
    };
    // An old image/input pair is retainable only while its view is still clear.
    // Never show a hidden host or keep mapping beneath a newly arrived popup.
    return eligible() && CanRetainVisual(frame) && eligible();
}

bool GpuMonitorMagnifier::ReplacementVisibleAt(glide::Vec2 source) const noexcept
{
    return impl_ && impl_->Projected(source) && ReplacementVisibleNow();
}

bool GpuMonitorMagnifier::ReplacementVisibleNow() const noexcept
{
    if (!cursorEnabled_ || !impl_ || !impl_->prepared || impl_->staged || impl_->geometryInvalid || !impl_->gate.Ready() ||
        !Active() || !glide::OnInputDesktop())
        return false;
    const auto& state = *impl_;
    const auto operation = state.epoch;
    CURSORINFO information{};
    information.cbSize = sizeof(information);
    POINT physical{};
    if (!GetCursorInfo(&information) || !GetPhysicalCursorPos(&physical) ||
        MonitorFromPoint(physical, MONITOR_DEFAULTTONULL) != state.monitor)
        return false;
    const glide::Vec2 point{ static_cast<double>(physical.x), static_cast<double>(physical.y) };
    return state.submittedCursor.Covers(state.frame, point, reinterpret_cast<std::uintptr_t>(information.hCursor), (information.flags & kCursorSuppressed) != 0) &&
           state.epoch == operation && state.prepared && !state.staged &&
           Active() && glide::OnInputDesktop();
}

void GpuMonitorMagnifier::SetSystemCursorHiddenByUs(bool hidden) noexcept
{
    if (impl_)
        impl_->cursorHiddenByUs = hidden;
}

void GpuMonitorMagnifier::SetCursorPositionResolver(CursorPositionResolver resolver, void* context) noexcept
{
    cursorResolver_ = resolver;
    cursorResolverContext_ = context;
}
