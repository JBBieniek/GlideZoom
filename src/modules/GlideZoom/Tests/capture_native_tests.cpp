#include <stdexcept>
#include "../GlideZoom/zoom_capture_win32.h"

#include <array>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <vector>

// Compile the real adapter against this target's isolated SDK stand-ins. These
// tests exercise its actual ownership/ordering code, not a reimplemented adapter.
#include "../GlideZoom/zoom_capture.cpp"

struct MockBitmap {
    int width = 0, height = 0;
    bool selected = false;
    std::vector<std::uint32_t> pixels;
};
struct MockDC { MockBitmap* selected = nullptr; bool desktop = false; };
struct MockGlobal {
    SIZE_T bytes = 0;
    unsigned locks = 0;
    bool windowsOwned = false;
    std::vector<std::uint64_t> storage;
};
struct MockWindow { DWORD process = 19, thread = 11; };

namespace {
namespace cap = glide::capture;
std::uint64_t assertions = 0, scenarios = 0, injected = 0;
void Check(bool condition, const char* message) {
    ++assertions;
    if (!condition) { std::cerr << "FAIL native capture: " << message << " [scenario=" << scenarios << "]\n"; throw std::runtime_error("GlideZoom assertion failed; see captured output"); }
}
enum class Op : unsigned { Enum, CreateDC, CreateDib, Select, Alloc, Lock, Unlock, Dwm, GetDC, Blt, Gdi, Open, Empty, Set, Close, Count };
struct Blit { MockBitmap* bitmap; int x, y, width, height, sourceX, sourceY; };
struct World {
    std::array<unsigned, static_cast<unsigned>(Op::Count)> calls{};
    Op fault = Op::Count;
    unsigned faultAt = 1, reached = 0;
    DWORD error = 0;
    unsigned liveDC = 0, liveBitmap = 0, liveGlobal = 0, screenRentals = 0;
    unsigned deletedSelected = 0, windowsFreedByApp = 0;
    bool clipboardOpen = false, oldClipboard = true;
    bool closeAlwaysFails = false;
    HGLOBAL published = nullptr;
    MockDC screen{nullptr, true};
    MockWindow window, foreign{77, 99};
    std::vector<RECT> displays{{-8, 0, 0, 8}, {0, -4, 8, 4}};
    std::vector<Blit> queued;
    bool Hit(Op operation) noexcept {
        const auto ordinal = ++calls[static_cast<unsigned>(operation)];
        if (operation == fault && ordinal == faultAt) {
            ++reached;
            error = ERROR_GEN_FAILURE;
            return true;
        }
        return false;
    }
    unsigned Calls(Op operation) const noexcept { return calls[static_cast<unsigned>(operation)]; }
    bool Physical(int x, int y) const noexcept {
        for (const auto r : displays) if (x >= r.left && x < r.right && y >= r.top && y < r.bottom) return true;
        return false;
    }
    std::uint32_t Picture(int x, int y) const noexcept {
        if (!Physical(x, y)) return 0xff000000;
        return (static_cast<std::uint32_t>(x * 3 + y) & 255) << 16 |
               (static_cast<std::uint32_t>(y * 9 - x) & 255) << 8 |
               (static_cast<std::uint32_t>(x + y * 7) & 255);
    }
    void Flush() noexcept {
        for (const auto blit : queued)
            for (int y = 0; y < blit.height; ++y)
                for (int x = 0; x < blit.width; ++x)
                    blit.bitmap->pixels[static_cast<std::size_t>(blit.y + y) * blit.bitmap->width + blit.x + x] =
                        Picture(blit.sourceX + x, blit.sourceY + y);
        queued.clear();
    }
    void ReleasePublished() noexcept {
        if (!published) return;
        Check(published->windowsOwned && published->locks == 0, "published data belongs to Windows and is unlocked");
        delete published;
        published = nullptr;
        --liveGlobal;
    }
    void Clean() noexcept {
        Check(liveDC == 0 && liveBitmap == 0 && screenRentals == 0 && queued.empty(),
              "all private DCs, DIBs, screen rentals and queued writes are released");
        Check(deletedSelected == 0 && windowsFreedByApp == 0, "no selected-object deletion or transferred-handle free");
        Check(liveGlobal == (published ? 1u : 0u), "only Windows-owned published memory can outlive the adapter");
        ReleasePublished();
        clipboardOpen = false; // Test cleanup of the explicitly injected Close failure.
    }
} world;
int stockBitmap = 0;

void Reset(Op fault = Op::Count, unsigned ordinal = 1) {
    world.Clean();
    world = World{};
    world.fault = fault; world.faultAt = ordinal;
    ++scenarios;
}
cap::Plan Plan() {
    cap::Topology topology;
    Check(cap::ReadTopology(topology), "native monitor enumeration succeeds");
    cap::Plan plan;
    Check(cap::MakePlan(cap::Scope::Desktop, {}, {}, topology, plan), "native identity fixture plan");
    return plan;
}
void CheckBitmap(const cap::Plan& plan) {
    Check(world.published && !world.oldClipboard, "successful publication replaces old clipboard");
    auto* header = reinterpret_cast<BITMAPV5HEADER*>(world.published->storage.data());
    Check(header->bV5Size == 124 && header->bV5Width == static_cast<LONG>(plan.width) &&
          header->bV5Height == -static_cast<LONG>(plan.height) && header->bV5Planes == 1 && header->bV5BitCount == 32,
          "clipboard uses bounded top-down 32-bit DIBV5 geometry");
    Check(header->bV5RedMask == 0x00ff0000 && header->bV5GreenMask == 0x0000ff00 &&
          header->bV5BlueMask == 0x000000ff && header->bV5AlphaMask == 0xff000000 &&
          header->bV5CSType == LCS_sRGB && header->bV5ProfileData == 0 &&
          header->bV5ProfileSize == 0 && header->bV5Reserved == 0,
          "clipboard masks/color metadata contain no undefined profile pointer or extra data");
    Check(world.published->bytes == 124 + plan.outputPixels * 4 && header->bV5SizeImage == plan.outputPixels * 4,
          "clipboard allocation matches exactly initialized header and pixels");
    const auto* pixels = reinterpret_cast<const std::uint32_t*>(reinterpret_cast<const unsigned char*>(header) + 124);
    for (std::uint32_t y = 0; y < plan.height; ++y)
        for (std::uint32_t x = 0; x < plan.width; ++x)
            Check(pixels[static_cast<std::size_t>(y) * plan.width + x] ==
                  (world.Picture(plan.output.left + static_cast<int>(x), plan.output.top + static_cast<int>(y)) | 0xff000000),
                  "actual native adapter captures negative-origin monitor pixels and black virtual gaps");
}
struct Pulse {
    unsigned calls = 0, stop = 0;
    static bool Poll(void* context) noexcept { auto& p = *static_cast<Pulse*>(context); return ++p.calls != p.stop; }
};
} // namespace

DWORD GetLastError() noexcept { return world.error; }
void SetLastError(DWORD error) noexcept { world.error = error; }
BOOL EnumDisplayMonitors(HDC dc, const RECT* clip, MONITORENUMPROC callback, LPARAM data) noexcept {
    Check(!dc && !clip, "full physical monitor enumeration requested");
    if (world.Hit(Op::Enum)) return FALSE;
    for (auto rectangle : world.displays)
        if (!callback(nullptr, nullptr, &rectangle, data)) return FALSE;
    return TRUE;
}
HDC CreateCompatibleDC(HDC dc) noexcept {
    Check(!dc, "private compatible memory DC");
    if (world.Hit(Op::CreateDC)) return nullptr;
    ++world.liveDC; return new MockDC;
}
HBITMAP CreateDIBSection(HDC dc, const BITMAPINFO* info, UINT usage, void** bits, HANDLE section, DWORD offset) noexcept {
    Check(dc && !dc->desktop && usage == DIB_RGB_COLORS && !section && !offset, "private unshared source DIB");
    *bits = nullptr;
    if (world.Hit(Op::CreateDib)) return nullptr;
    Check(info->bmiHeader.biSize == 40 && info->bmiHeader.biHeight < 0 && info->bmiHeader.biWidth > 0 &&
          info->bmiHeader.biBitCount == 32 && info->bmiHeader.biCompression == BI_RGB,
          "source DIB uses top-down 32-bit GDI layout");
    auto* bitmap = new MockBitmap;
    bitmap->width = info->bmiHeader.biWidth; bitmap->height = -info->bmiHeader.biHeight;
    bitmap->pixels.assign(static_cast<std::size_t>(bitmap->width) * bitmap->height, 0xdeadbeef);
    *bits = bitmap->pixels.data(); ++world.liveBitmap;
    return bitmap;
}
HGDIOBJ SelectObject(HDC dc, HGDIOBJ object) noexcept {
    Check(dc && !dc->desktop, "select only into private memory DC");
    if (world.Hit(Op::Select)) return HGDI_ERROR;
    HGDIOBJ previous = dc->selected ? static_cast<HGDIOBJ>(dc->selected) : &stockBitmap;
    if (dc->selected) dc->selected->selected = false;
    dc->selected = object == &stockBitmap ? nullptr : static_cast<MockBitmap*>(object);
    if (dc->selected) dc->selected->selected = true;
    return previous;
}
BOOL DeleteDC(HDC dc) noexcept {
    Check(dc && !dc->desktop, "delete only application-created DCs");
    world.Flush();
    if (dc->selected) dc->selected->selected = false;
    delete dc; --world.liveDC; return TRUE;
}
BOOL DeleteObject(HGDIOBJ object) noexcept {
    auto* bitmap = static_cast<MockBitmap*>(object);
    Check(bitmap != nullptr, "delete valid source bitmap");
    if (bitmap->selected) ++world.deletedSelected;
    Check(!bitmap->selected, "selected bitmap must be deselected before deletion");
    delete bitmap; --world.liveBitmap; return TRUE;
}
HGLOBAL GlobalAlloc(UINT flags, SIZE_T bytes) noexcept {
    Check(flags == GMEM_MOVEABLE, "clipboard memory is movable");
    if (world.Hit(Op::Alloc)) return nullptr;
    auto* memory = new MockGlobal;
    memory->bytes = bytes; memory->storage.assign((bytes + 7) / 8, 0xdeadbeefdeadbeefull);
    ++world.liveGlobal; return memory;
}
void* GlobalLock(HGLOBAL memory) noexcept {
    Check(memory && !memory->windowsOwned, "lock only owned clipboard allocation");
    if (world.Hit(Op::Lock)) return nullptr;
    ++memory->locks; return memory->storage.data();
}
BOOL GlobalUnlock(HGLOBAL memory) noexcept {
    Check(memory && !memory->windowsOwned && memory->locks > 0, "unlock only owned locked memory");
    if (world.Hit(Op::Unlock)) return FALSE;
    --memory->locks; SetLastError(ERROR_SUCCESS);
    return memory->locks ? TRUE : FALSE; // False + NO_ERROR is documented success.
}
HGLOBAL GlobalFree(HGLOBAL memory) noexcept {
    if (memory->windowsOwned) ++world.windowsFreedByApp;
    Check(!memory->windowsOwned, "transferred clipboard memory is never freed by app");
    Check(memory->locks == 0, "owned memory is unlocked before cleanup");
    delete memory; --world.liveGlobal; return nullptr;
}
HDC GetDC(HWND window) noexcept {
    Check(!window, "capture reads only desktop DC");
    if (world.Hit(Op::GetDC)) return nullptr;
    ++world.screenRentals; return &world.screen;
}
int ReleaseDC(HWND window, HDC dc) noexcept {
    Check(!window && dc == &world.screen && world.screenRentals > 0, "screen DC released to matching owner");
    --world.screenRentals; return 1;
}
BOOL BitBlt(HDC destination, int x, int y, int width, int height, HDC source, int sourceX, int sourceY, DWORD operation) noexcept {
    Check(destination && destination->selected && source == &world.screen,
          "BitBlt reads desktop into private selected source DIB");
    Check(operation == (SRCCOPY | CAPTUREBLT), "capture includes layered windows without raster transforms");
    if (world.Hit(Op::Blt)) return FALSE;
    Check(x >= 0 && y >= 0 && width > 0 && height > 0 &&
          x + width <= destination->selected->width && y + height <= destination->selected->height,
          "BitBlt target is inside preallocated bounded source");
    for (int row = 0; row < height; ++row) for (int col = 0; col < width; ++col)
        Check(world.Physical(sourceX + col, sourceY + row), "GDI never reads virtual desktop gaps");
    world.queued.push_back({destination->selected, x, y, width, height, sourceX, sourceY});
    return TRUE;
}
BOOL GdiFlush() noexcept {
    const bool failed = world.Hit(Op::Gdi);
    world.Flush(); // A failed native call can have partial/complete effects.
    return failed ? FALSE : TRUE;
}
HRESULT DwmFlush() noexcept { return world.Hit(Op::Dwm) ? static_cast<HRESULT>(0x80004005u) : 0; }
DWORD GetWindowThreadProcessId(HWND window, DWORD* process) noexcept {
    if (!window) { *process = 0; return 0; }
    *process = window->process; return window->thread;
}
DWORD GetCurrentThreadId() noexcept { return world.window.thread; }
DWORD GetCurrentProcessId() noexcept { return world.window.process; }
BOOL OpenClipboard(HWND owner) noexcept {
    Check(owner == &world.window, "clipboard owner is the application window");
    Check(!world.clipboardOpen, "clipboard is opened only once");
    if (world.Hit(Op::Open)) return FALSE;
    world.clipboardOpen = true; return TRUE;
}
BOOL EmptyClipboard() noexcept {
    Check(world.clipboardOpen, "EmptyClipboard requires ownership of open clipboard");
    if (world.Hit(Op::Empty)) return FALSE;
    world.ReleasePublished(); world.oldClipboard = false; return TRUE;
}
HANDLE SetClipboardData(UINT format, HANDLE handle) noexcept {
    auto* data = static_cast<HGLOBAL>(handle);
    Check(world.clipboardOpen && !world.oldClipboard && format == CF_DIBV5 && data && data->locks == 0,
          "fully rendered clipboard block is unlocked before SetClipboardData");
    if (world.Hit(Op::Set)) return nullptr;
    data->windowsOwned = true; world.published = data; return data;
}
BOOL CloseClipboard() noexcept {
    Check(world.clipboardOpen, "CloseClipboard pairs with successful open");
    if (world.Hit(Op::Close) || world.closeAlwaysFails) return FALSE;
    world.clipboardOpen = false; return TRUE;
}

int Run_capture_native() {
    Reset();
    {
        const auto plan = Plan();
        cap::NativeImage image;
        Check(!image.Capture() && !image.Render() && image.Publish(&world.window) == cap::ClipboardResult::Invalid,
              "unprepared methods do not touch GDI or clipboard");
        Check(image.Prepare(plan) && image.Capture() && image.Render(), "actual native adapter happy path");
        Check(world.Calls(Op::Gdi) == 1 && world.queued.empty(), "GDI drawing completes before source CPU reads");
        Check(image.Publish(&world.window) == cap::ClipboardResult::Copied, "actual native clipboard publication");
        CheckBitmap(plan);
        Check(image.Publish(&world.window) == cap::ClipboardResult::Invalid && world.Calls(Op::Set) == 1,
              "published handle cannot be transferred twice");
    }
    world.Clean();

    for (auto operation : {Op::CreateDC, Op::CreateDib, Op::Select, Op::Alloc, Op::Lock}) {
        Reset(operation);
        {
            cap::NativeImage image;
            Check(!image.Prepare(Plan()), "each allocation/selection failure is surfaced");
            Check(image.LastError() != ERROR_SUCCESS, "native failure code retained through cleanup");
            Check(!image.Capture() && !image.Render(), "failed preparation cannot be reused as a complete image");
            Check(image.Publish(&world.window) == cap::ClipboardResult::Invalid, "failed preparation never publishes");
        }
        Check(world.reached == 1 && world.oldClipboard && world.Calls(Op::Open) == 0, "allocation faults reached and clipboard untouched");
        ++injected;
        world.Clean();
    }
    for (auto operation : {Op::Dwm, Op::GetDC, Op::Blt, Op::Gdi}) {
        const unsigned repeats = operation == Op::Blt ? 2 : 1;
        for (unsigned ordinal = 1; ordinal <= repeats; ++ordinal) {
            Reset(operation, ordinal);
            {
                cap::NativeImage image;
                Check(image.Prepare(Plan()), "capture failure fixture prepared");
                Check(!image.Capture() && !image.Render(), "partial native capture cannot render or publish");
                Check(image.Publish(&world.window) == cap::ClipboardResult::Invalid, "failed source read cannot replace clipboard");
            }
            Check(world.reached == 1 && world.oldClipboard && world.Calls(Op::Open) == 0, "capture fault reached and clipboard untouched");
            ++injected;
            world.Clean();
        }
    }
    for (auto operation : {Op::Open, Op::Empty, Op::Set, Op::Close, Op::Unlock}) {
        Reset(operation);
        {
            cap::NativeImage image;
            const auto plan = Plan();
            Check(image.Prepare(plan) && image.Capture() && image.Render(), "clipboard fault fixture prepared");
            const auto result = image.Publish(&world.window);
            const auto expected = operation == Op::Open ? cap::ClipboardResult::Busy :
                operation == Op::Empty ? cap::ClipboardResult::EmptyFailed :
                operation == Op::Set ? cap::ClipboardResult::SetFailed :
                operation == Op::Close ? cap::ClipboardResult::Copied : cap::ClipboardResult::Invalid;
            Check(result == expected, "native clipboard failure classification is exact");
            Check(world.oldClipboard == (operation == Op::Open || operation == Op::Empty || operation == Op::Unlock),
                  "old clipboard only changes after actual successful Empty");
            Check((world.published != nullptr) == (operation == Op::Close),
                  "transient Close failure does not free successfully transferred memory");
            if (operation == Op::Close) {
                CheckBitmap(plan);
                Check(world.Calls(Op::Close) == 2 && !world.clipboardOpen,
                      "native Close retries once and releases clipboard after transient failure");
            }
            if (operation == Op::Open) {
                Check(image.Publish(&world.window) == cap::ClipboardResult::Copied,
                      "transient clipboard contention allows safe retry of the same prepared image");
                CheckBitmap(plan);
            }
        }
        Check(world.reached == 1, "every clipboard native failure actually reached");
        ++injected;
        world.Clean();
    }
    for (auto operation : {Op::Empty, Op::Set, Op::Count}) {
        Reset(operation);
        world.closeAlwaysFails = true;
        {
            cap::NativeImage image;
            const auto plan = Plan();
            Check(image.Prepare(plan) && image.Capture() && image.Render(), "permanent Close failure fixture");
            Check(image.Publish(&world.window) == cap::ClipboardResult::CloseFailed &&
                  world.Calls(Op::Close) == 2 && world.clipboardOpen,
                  "persistent clipboard lock gets bounded retries and fatal release-uncertainty status");
            Check(world.oldClipboard == (operation == Op::Empty), "permanent Close failure does not invent rollback of Empty");
            Check((world.published != nullptr) == (operation == Op::Count),
                  "ownership follows actual Set result independently of permanent Close failure");
            if (world.published) CheckBitmap(plan);
        }
        world.Clean();
    }
    Reset(Op::Select, 2);
    {
        cap::NativeImage image;
        Check(image.Prepare(Plan()), "cleanup select failure fixture");
    }
    Check(world.reached == 1, "cleanup stock-object restore failure reached");
    ++injected;
    world.Clean();

    for (unsigned stop : {1u, 2u, 3u, 4u, 5u}) {
        Reset();
        {
            cap::NativeImage image;
            Check(image.Prepare(Plan()), "heartbeat capture fixture");
            Pulse pulse{0, stop};
            Check(!image.Capture({Pulse::Poll, &pulse}) && pulse.calls == stop,
                  "capture cancellation reached before/after compositor, each copy, or completion");
            Check(!image.Render() && image.Publish(&world.window) == cap::ClipboardResult::Invalid,
                  "cancelled capture stays unpublished");
        }
        world.Clean();
    }
    for (unsigned stop : {1u, 2u, 3u}) {
        Reset();
        {
            cap::NativeImage image;
            Check(image.Prepare(Plan()) && image.Capture(), "render cancellation fixture");
            Pulse pulse{0, stop};
            Check(!image.Render({Pulse::Poll, &pulse}) && pulse.calls == stop,
                  "render cancellation reached before, during or after pixels");
            Check(image.Publish(&world.window) == cap::ClipboardResult::Invalid && world.oldClipboard,
                  "cancelled partially rendered image cannot publish");
        }
        world.Clean();
    }
    Reset();
    {
        cap::NativeImage image;
        const auto plan = Plan();
        Check(image.Prepare(plan) && image.Capture() && image.Render(), "owner validation fixture");
        Check(image.Publish(nullptr) == cap::ClipboardResult::Invalid &&
              image.Publish(&world.foreign) == cap::ClipboardResult::Invalid && world.Calls(Op::Open) == 0,
              "invalid or foreign window owner rejected before opening clipboard");
        auto corrupt = plan; corrupt.sourcePixels++;
        Check(!image.Prepare(corrupt), "reprepare invalid plan releases old private buffers");
        Check(!image.Capture() && !image.Render(), "invalid reprepare does not preserve stale prepared state");
    }
    world.Clean();

    for (unsigned kind = 0; kind < 5; ++kind) {
        Reset(kind == 0 ? Op::Enum : Op::Count);
        if (kind == 1) world.displays.clear();
        if (kind == 2) world.displays = {{0, 0, 24, 12}, {0, 0, 12, 12}};
        if (kind == 3) world.displays = {{0, 0, 0, 12}};
        if (kind == 4) world.displays.assign(cap::kMaxMonitors + 1, {0, 0, 24, 12});
        cap::Topology unchanged;
        unchanged.count = 19;
        Check(!cap::ReadTopology(unchanged) && unchanged.count == 19,
              "enumeration failure, empty/overlapping/invalid/too-many monitors leave output unchanged");
        if (kind == 0) { Check(world.reached == 1, "enumeration native failure reached"); ++injected; }
        world.Clean();
    }
    std::cout << "Native capture adapter mocks passed: " << assertions << " assertions; " << scenarios
              << " scenarios; " << injected << " reached native fault injections. No Windows compositor executed.\n";
    return 0;
}
