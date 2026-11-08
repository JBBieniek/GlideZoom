#include "zoom_capture_win32.h"

#include <dwmapi.h>
#include <new>

namespace glide::capture
{
    namespace
    {
        struct Enumeration
        {
            Topology topology{};
            DWORD error = ERROR_SUCCESS;
        };

        BOOL CALLBACK AddMonitor(HMONITOR, HDC, LPRECT bounds, LPARAM context) noexcept
        {
            auto& enumeration = *reinterpret_cast<Enumeration*>(context);
            if (!bounds || enumeration.topology.count >= kMaxMonitors)
            {
                enumeration.error = ERROR_NOT_SUPPORTED;
                return FALSE;
            }
            const PixelRect rectangle{ bounds->left, bounds->top, bounds->right, bounds->bottom };
            if (!ValidRect(rectangle))
            {
                enumeration.error = ERROR_INVALID_DATA;
                return FALSE;
            }
            enumeration.topology.monitors[enumeration.topology.count++] = rectangle;
            return TRUE;
        }

        DWORD NativeError(DWORD fallback) noexcept
        {
            const DWORD error = GetLastError();
            return error == ERROR_SUCCESS ? fallback : error;
        }

        struct ClipboardBackend
        {
            HWND owner;
            HGLOBAL& data;
            DWORD& error;

            bool Open() noexcept
            {
                SetLastError(ERROR_SUCCESS);
                if (OpenClipboard(owner))
                    return true;
                error = NativeError(ERROR_BUSY);
                return false;
            }
            bool Empty() noexcept
            {
                SetLastError(ERROR_SUCCESS);
                if (EmptyClipboard())
                    return true;
                error = NativeError(ERROR_GEN_FAILURE);
                return false;
            }
            bool Set() noexcept
            {
                SetLastError(ERROR_SUCCESS);
                if (!SetClipboardData(CF_DIBV5, data))
                {
                    error = NativeError(ERROR_GEN_FAILURE);
                    return false;
                }
                // Ownership transferred. Never unlock, write or free this handle again.
                data = nullptr;
                return true;
            }
            bool Close() noexcept
            {
                SetLastError(ERROR_SUCCESS);
                if (CloseClipboard())
                    return true;
                error = NativeError(ERROR_GEN_FAILURE);
                return false;
            }
        };
    } // namespace

    bool ReadTopology(Topology& output) noexcept
    {
        Enumeration enumeration;
        SetLastError(ERROR_SUCCESS);
        if (!EnumDisplayMonitors(nullptr, nullptr, AddMonitor, reinterpret_cast<LPARAM>(&enumeration)))
        {
            SetLastError(enumeration.error ? enumeration.error : NativeError(ERROR_GEN_FAILURE));
            return false;
        }
        if (!Canonicalize(enumeration.topology))
        {
            SetLastError(ERROR_NOT_SUPPORTED);
            return false;
        }
        output = enumeration.topology;
        return true;
    }

    NativeImage::~NativeImage() noexcept
    {
        Reset();
    }

    void NativeImage::Reset() noexcept
    {
        horizontal_.reset();
        output_ = nullptr;
        if (clipboardData_)
            GlobalUnlock(clipboard_);
        clipboardData_ = nullptr;
        if (clipboard_)
            GlobalFree(clipboard_);
        clipboard_ = nullptr;
        if (memory_ && previous_ && previous_ != HGDI_ERROR)
            SelectObject(memory_, previous_);
        // Destroying our private DC also releases its selection if restoring the
        // old stock bitmap happened to fail. We never delete a selected bitmap.
        if (memory_)
            DeleteDC(memory_);
        memory_ = nullptr;
        previous_ = nullptr;
        if (bitmap_)
            DeleteObject(bitmap_);
        bitmap_ = nullptr;
        source_ = nullptr;
        prepared_ = captured_ = rendered_ = false;
        plan_ = {};
    }

    bool NativeImage::Failure(DWORD fallback) noexcept
    {
        error_ = NativeError(fallback);
        return false;
    }

    bool NativeImage::Prepare(const Plan& plan) noexcept
    {
        Reset();
        error_ = ERROR_SUCCESS;
        if (!ValidPlan(plan))
        {
            error_ = ERROR_INVALID_PARAMETER;
            return false;
        }
        plan_ = plan;
        const auto abort = [&](DWORD error) noexcept {
            Failure(error);
            Reset();
            return false;
        };
        SetLastError(ERROR_SUCCESS);
        memory_ = CreateCompatibleDC(nullptr);
        if (!memory_)
            return abort(ERROR_NOT_ENOUGH_MEMORY);
        BITMAPINFO information{};
        information.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        information.bmiHeader.biWidth = static_cast<LONG>(plan.sourceWidth);
        information.bmiHeader.biHeight = -static_cast<LONG>(plan.sourceHeight);
        information.bmiHeader.biPlanes = 1;
        information.bmiHeader.biBitCount = 32;
        information.bmiHeader.biCompression = BI_RGB;
        information.bmiHeader.biSizeImage = static_cast<DWORD>(plan.sourcePixels * 4);
        void* source = nullptr;
        SetLastError(ERROR_SUCCESS);
        bitmap_ = CreateDIBSection(memory_, &information, DIB_RGB_COLORS, &source, nullptr, 0);
        if (!bitmap_ || !source)
            return abort(ERROR_NOT_ENOUGH_MEMORY);
        source_ = static_cast<std::uint32_t*>(source);
        SetLastError(ERROR_SUCCESS);
        previous_ = SelectObject(memory_, bitmap_);
        if (!previous_ || previous_ == HGDI_ERROR)
            return abort(ERROR_GEN_FAILURE);
        SetLastError(ERROR_SUCCESS);
        clipboard_ = GlobalAlloc(GMEM_MOVEABLE, sizeof(BITMAPV5HEADER) + static_cast<SIZE_T>(plan.outputPixels * 4));
        if (!clipboard_)
            return abort(ERROR_NOT_ENOUGH_MEMORY);
        SetLastError(ERROR_SUCCESS);
        clipboardData_ = GlobalLock(clipboard_);
        if (!clipboardData_)
            return abort(ERROR_NOT_ENOUGH_MEMORY);
        horizontal_.reset(new (std::nothrow) AxisSample[plan.width]);
        if (!horizontal_)
        {
            SetLastError(ERROR_NOT_ENOUGH_MEMORY);
            return abort(ERROR_NOT_ENOUGH_MEMORY);
        }
        BITMAPV5HEADER header{};
        header.bV5Size = sizeof(header);
        header.bV5Width = static_cast<LONG>(plan.width);
        header.bV5Height = -static_cast<LONG>(plan.height);
        header.bV5Planes = 1;
        header.bV5BitCount = 32;
        header.bV5Compression = BI_BITFIELDS;
        header.bV5SizeImage = static_cast<DWORD>(plan.outputPixels * 4);
        header.bV5RedMask = 0x00ff0000;
        header.bV5GreenMask = 0x0000ff00;
        header.bV5BlueMask = 0x000000ff;
        header.bV5AlphaMask = 0xff000000;
        header.bV5CSType = LCS_sRGB;
        header.bV5Intent = LCS_GM_IMAGES;
        *static_cast<BITMAPV5HEADER*>(clipboardData_) = header;
        output_ = reinterpret_cast<std::uint32_t*>(static_cast<unsigned char*>(clipboardData_) + sizeof(header));
        // Never expose uninitialized desktop gaps. Filling before the transaction
        // also keeps the temporary normal-view interval shorter.
        std::fill(source_, source_ + static_cast<std::size_t>(plan.sourcePixels), 0xff000000u);
        prepared_ = true;
        return true;
    }

    bool NativeImage::Capture(Heartbeat heartbeat) noexcept
    {
        captured_ = rendered_ = false;
        if (!prepared_ || !source_ || !memory_)
        {
            error_ = ERROR_INVALID_STATE;
            return false;
        }
        if (!heartbeat.Continue())
        {
            error_ = ERROR_CANCELLED;
            return false;
        }
        // This waits for our queued compositor work, not an atomic whole-session
        // frame fence. Native validation is still required for compositor timing.
        const HRESULT flushed = DwmFlush();
        if (FAILED(flushed))
        {
            error_ = static_cast<DWORD>(flushed);
            return false;
        }
        if (!heartbeat.Continue())
        {
            error_ = ERROR_CANCELLED;
            return false;
        }
        SetLastError(ERROR_SUCCESS);
        HDC desktop = GetDC(nullptr);
        if (!desktop)
            return Failure(ERROR_GEN_FAILURE);
        bool success = true;
        for (std::size_t i = 0; i < plan_.topology.count; ++i)
        {
            const auto piece = Intersection(plan_.source, plan_.topology.monitors[i]);
            if (piece.right <= piece.left || piece.bottom <= piece.top)
                continue;
            if (!heartbeat.Continue())
            {
                error_ = ERROR_CANCELLED;
                success = false;
                break;
            }
            SetLastError(ERROR_SUCCESS);
            if (!BitBlt(memory_, piece.left - plan_.source.left, piece.top - plan_.source.top, piece.right - piece.left, piece.bottom - piece.top, desktop, piece.left, piece.top, SRCCOPY | CAPTUREBLT))
            {
                Failure(ERROR_GEN_FAILURE);
                success = false;
                break;
            }
        }
        SetLastError(ERROR_SUCCESS);
        if (!GdiFlush())
        {
            if (success)
                Failure(ERROR_GEN_FAILURE);
            success = false;
        }
        ReleaseDC(nullptr, desktop);
        if (success && !heartbeat.Continue())
        {
            error_ = ERROR_CANCELLED;
            success = false;
        }
        captured_ = success;
        return success;
    }

    bool NativeImage::Render(Heartbeat heartbeat) noexcept
    {
        rendered_ = false;
        if (!prepared_ || !captured_ || !source_ || !output_ || !horizontal_)
        {
            error_ = ERROR_INVALID_STATE;
            return false;
        }
        rendered_ = capture::Render(plan_, source_, static_cast<std::size_t>(plan_.sourcePixels), output_, static_cast<std::size_t>(plan_.outputPixels), horizontal_.get(), plan_.width, heartbeat);
        if (!rendered_)
            error_ = ERROR_CANCELLED;
        return rendered_;
    }

    ClipboardResult NativeImage::Publish(HWND owner) noexcept
    {
        DWORD process = 0;
        const DWORD thread = owner ? GetWindowThreadProcessId(owner, &process) : 0;
        if (!prepared_ || !captured_ || !rendered_ || !clipboard_ ||
            thread != GetCurrentThreadId() || process != GetCurrentProcessId())
        {
            error_ = ERROR_INVALID_STATE;
            return ClipboardResult::Invalid;
        }
        if (clipboardData_)
        {
            SetLastError(ERROR_SUCCESS);
            if (!GlobalUnlock(clipboard_) && GetLastError() != ERROR_SUCCESS)
            {
                Failure(ERROR_GEN_FAILURE);
                return ClipboardResult::Invalid;
            }
            clipboardData_ = nullptr;
            output_ = nullptr;
        }
        ClipboardBackend backend{ owner, clipboard_, error_ };
        const auto result = PublishPrepared(backend);
        if (!clipboard_)
            rendered_ = false; // Handle belongs to Windows, even if Close failed.
        if (result == ClipboardResult::Copied)
            error_ = ERROR_SUCCESS;
        return result;
    }

} // namespace glide::capture
