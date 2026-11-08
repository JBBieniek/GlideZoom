#include <stdexcept>
#include "../GlideZoom/zoom_capture.h"
#include "../GlideZoom/monitor_geometry.h"
#include "../GlideZoom/runtime_state.h"

#include <array>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <vector>

namespace {
namespace cap = glide::capture;
namespace ses = glide::session;
std::uint64_t assertions = 0, imageCases = 0, imagePixels = 0, policyCases = 0;
std::uint64_t geometryCases = 0, emptySourceCases = 0, clipboardCases = 0, fusionCases = 0, boundaries = 0;
std::uint64_t leaseCases = 0;
std::uint64_t reentrantCases = 0;
std::uint64_t observeCases = 0;

void Check(bool condition, const char* message) {
    ++assertions;
    if (!condition) {
        std::cerr << "FAIL: " << message << " [image=" << imageCases << ", policy=" << policyCases
                  << ", fusion=" << fusionCases << "]\n";
        throw std::runtime_error("GlideZoom assertion failed; see captured output");
    }
}
bool Success(ses::Result result) noexcept {
    return result == ses::Result::Applied || result == ses::Result::Unchanged;
}
cap::Topology Displays(std::initializer_list<cap::PixelRect> rectangles) {
    cap::Topology result;
    for (const auto rectangle : rectangles) result.monitors[result.count++] = rectangle;
    Check(cap::Canonicalize(result), "fixture topology canonicalizes");
    return result;
}
bool Physical(std::int32_t x, std::int32_t y, const cap::Topology& topology) noexcept {
    for (std::size_t i = 0; i < topology.count; ++i) {
        const auto r = topology.monitors[i];
        if (x >= r.left && x < r.right && y >= r.top && y < r.bottom) return true;
    }
    return false;
}
std::uint32_t Picture(std::int32_t x, std::int32_t y, const cap::Topology& topology) noexcept {
    if (!Physical(x, y, topology)) return 0xff000000;
    const auto red = static_cast<std::uint32_t>(x * 29 + y * 13) & 255u;
    const auto green = static_cast<std::uint32_t>(x * 3 - y * 17) & 255u;
    const auto blue = static_cast<std::uint32_t>(x * 11 + y * 5) & 255u;
    // Deliberately vary undefined source alpha, as a GDI DIB can do.
    const auto alpha = static_cast<std::uint32_t>(x - y) & 255u;
    return (alpha << 24) | (red << 16) | (green << 8) | blue;
}

// Independent long-double reference. It samples a conceptual desktop rather
// than the production crop/axis map and uses floating-point channel weights,
// so an incorrect crop, signed origin, integer stretch or packed blend fails.
std::uint32_t Oracle(const cap::Plan& plan, std::uint32_t x, std::uint32_t y) noexcept {
    const auto screenX = plan.output.left + static_cast<std::int32_t>(x);
    const auto screenY = plan.output.top + static_cast<std::int32_t>(y);
    if (!Physical(screenX, screenY, plan.topology)) return 0xff000000;
    const auto position = [](long double screen, long double offset, long double zoom,
                             std::int32_t low, std::int32_t high) noexcept {
        return std::max<long double>(low, std::min<long double>(high - 1,
            offset + (screen + 0.5L) / zoom - 0.5L));
    };
    const long double sx = position(screenX, plan.transform.offset.x, plan.transform.zoom,
                                    plan.output.left, plan.output.right);
    const long double sy = position(screenY, plan.transform.offset.y, plan.transform.zoom,
                                    plan.output.top, plan.output.bottom);
    const auto x0 = static_cast<std::int32_t>(std::floor(sx));
    const auto y0 = static_cast<std::int32_t>(std::floor(sy));
    const auto x1 = std::min(x0 + 1, plan.output.right - 1);
    const auto y1 = std::min(y0 + 1, plan.output.bottom - 1);
    const auto a = Picture(x0, y0, plan.topology), b = Picture(x1, y0, plan.topology);
    const auto c = Picture(x0, y1, plan.topology), d = Picture(x1, y1, plan.topology);
    const long double wx = sx - x0, wy = sy - y0;
    std::uint32_t result = 0xff000000;
    for (unsigned shift : {0u, 8u, 16u}) {
        const long double top = ((a >> shift) & 255u) * (1 - wx) + ((b >> shift) & 255u) * wx;
        const long double bottom = ((c >> shift) & 255u) * (1 - wx) + ((d >> shift) & 255u) * wx;
        result |= static_cast<std::uint32_t>(std::llround(top * (1 - wy) + bottom * wy)) << shift;
    }
    return result;
}

struct Pulse {
    unsigned calls = 0, stop = 0;
    static bool Poll(void* context) noexcept {
        auto& self = *static_cast<Pulse*>(context);
        ++self.calls;
        return !self.stop || self.calls < self.stop;
    }
    cap::Heartbeat AsHeartbeat() noexcept { return {Poll, this}; }
};

void CheckImage(const cap::Plan& plan) {
    ++imageCases;
    Check(cap::ValidPlan(plan), "production plan is self-consistent");
    std::vector<std::uint32_t> source(static_cast<std::size_t>(plan.sourcePixels));
    for (std::uint32_t y = 0; y < plan.sourceHeight; ++y)
        for (std::uint32_t x = 0; x < plan.sourceWidth; ++x)
            source[static_cast<std::size_t>(y) * plan.sourceWidth + x] =
                Picture(plan.source.left + static_cast<std::int32_t>(x),
                        plan.source.top + static_cast<std::int32_t>(y), plan.topology);
    const auto original = source;
    std::vector<std::uint32_t> output(static_cast<std::size_t>(plan.outputPixels) + 2, 0x12345678);
    std::vector<cap::AxisSample> axis(plan.width);
    Pulse heartbeat;
    Check(cap::Render(plan, source.data(), source.size(), output.data() + 1,
                      static_cast<std::size_t>(plan.outputPixels), axis.data(), axis.size(),
                      heartbeat.AsHeartbeat()), "bounded reconstruction succeeds");
    Check(output.front() == 0x12345678 && output.back() == 0x12345678,
          "render leaves guards outside output unchanged");
    Check(source == original, "source snapshot remains immutable");
    Check(heartbeat.calls >= 3, "render gives begin/row/end heartbeat opportunities");
    for (std::uint32_t y = 0; y < plan.height; ++y)
        for (std::uint32_t x = 0; x < plan.width; ++x) {
            ++imagePixels;
            const auto actual = output[1 + static_cast<std::size_t>(y) * plan.width + x];
            const auto expected = Oracle(plan, x, y);
            Check((actual >> 24) == 255, "undefined source alpha cannot make screenshot transparent");
            for (unsigned shift : {0u, 8u, 16u}) {
                const int error = static_cast<int>((actual >> shift) & 255) -
                                  static_cast<int>((expected >> shift) & 255);
                Check(std::abs(error) <= 1, "fixed-point bilinear image matches independent desktop oracle");
                if (plan.transform.zoom == 1) Check(error == 0, "identity copy is exact in every channel");
            }
        }
}

void ImageMatrix() {
    const std::array<cap::Topology, 6> layouts{
        Displays({{0, 0, 24, 12}}), Displays({{-24, -12, 0, 0}}),
        Displays({{-24, 0, 0, 12}, {0, 0, 24, 12}, {24, 0, 48, 12}}),
        Displays({{0, 0, 20, 10}, {30, -8, 38, 20}, {-16, 10, 0, 24}}),
        Displays({{-8, -20, 0, 0}, {0, -2, 18, 10}, {22, 4, 40, 13}}),
        Displays({{0, 0, 1, 1}})};
    constexpr std::array<double, 10> scales{1, 1.0001, 1.001, 1.05, 1.3, 1.999, 2, 3.7, 16, 4096};
    const std::array<glide::Vec2, 5> anchors{{{0, 0}, {0.5, 0.5}, {1, 1}, {0, 1}, {1, 0}}};
    for (const auto& topology : layouts)
        for (std::size_t selection = 0; selection <= topology.count; ++selection)
            for (double scale : scales) for (auto relative : anchors) {
                const auto scope = selection == topology.count ? cap::Scope::Desktop : cap::Scope::Monitor;
                const auto bounds = selection == topology.count ? cap::DesktopBounds(topology) : topology.monitors[selection];
                const auto rectangle = cap::AsRect(bounds);
                const glide::Vec2 anchor{rectangle.left + relative.x * (rectangle.right - rectangle.left),
                                         rectangle.top + relative.y * (rectangle.bottom - rectangle.top)};
                auto transform = glide::AnchorZoom({}, scale, anchor, rectangle);
                if (scope == cap::Scope::Monitor) {
                    glide::monitor::Frame frame;
                    Check(glide::monitor::MakeFrame(rectangle, transform, frame), "native-monitor geometry fixture");
                    transform = frame.transform;
                } else {
                    transform.zoom = static_cast<float>(transform.zoom);
                    transform.offset = {static_cast<double>(std::lround(transform.offset.x)),
                                        static_cast<double>(std::lround(transform.offset.y))};
                }
                cap::Plan plan;
                ++geometryCases;
                // With an artificial 1px screen or 4096x zoom on a tiny screen,
                // native integer panning can round the whole source past an
                // edge. Refuse that empty capture instead of inventing pixels.
                const bool entirelyOutside = transform.zoom != 1 &&
                    (transform.offset.x + bounds.left / transform.zoom >= bounds.right ||
                     transform.offset.y + bounds.top / transform.zoom >= bounds.bottom ||
                     transform.offset.x + bounds.right / transform.zoom <= bounds.left ||
                     transform.offset.y + bounds.bottom / transform.zoom <= bounds.top);
                const bool accepted = cap::MakePlan(scope, rectangle, transform, topology, plan);
                Check(accepted != entirelyOutside, "normal tuples accepted; entirely off-desktop source explicitly rejected");
                if (!accepted) { ++emptySourceCases; continue; }
                Check(plan.output == bounds, "output extent follows chosen scope exactly");
                Check(plan.transform.zoom == transform.zoom, "saved uniform scale is retained, never rounded to crop stretch");
                Check(plan.source.left >= bounds.left && plan.source.top >= bounds.top &&
                      plan.source.right <= bounds.right && plan.source.bottom <= bounds.bottom,
                      "captured source is bounded inside selected scope");
                CheckImage(plan);
            }
    Check(geometryCases == 900 && imageCases + emptySourceCases == geometryCases,
          "every declared geometry/scaling tuple ran with an explicit outcome");
}

void BoundsAndCancellation() {
    const auto triple = Displays({{-3840, 0, 0, 2160}, {0, 0, 3840, 2160}, {3840, 0, 7680, 2160}});
    cap::Plan plan;
    Check(cap::MakePlan(cap::Scope::Desktop, {}, {}, triple, plan) &&
          plan.width == 11520 && plan.height == 2160 && plan.outputPixels == 24883200,
          "triple 4K extended desktop fits the explicit capture bound");
    const auto maximum = Displays({{0, 0, 32768, 1024}});
    Check(cap::MakePlan(cap::Scope::Desktop, {}, {}, maximum, plan) && plan.outputPixels == cap::kMaxPixels,
          "exact 32Mi-pixel boundary is allowed");
    const auto reject = [&](cap::Scope scope, glide::Rect selected, glide::Transform transform,
                            const cap::Topology& topology) {
        cap::Plan unchanged;
        unchanged.width = 12345;
        Check(!cap::MakePlan(scope, selected, transform, topology, unchanged) && unchanged.width == 12345,
              "invalid or oversized geometry fails without modifying output");
    };
    reject(cap::Scope::Desktop, {}, {}, Displays({{0, 0, 32768, 1025}}));
    reject(cap::Scope::Desktop, {}, {}, Displays({{0, 0, 32769, 1}}));
    reject(cap::Scope::Desktop, {}, {}, Displays({{0, 0, 7680, 4320}, {7680, 0, 15360, 4320}, {15360, 0, 23040, 4320}}));
    const auto far = Displays({{-262144, -20, -262120, 0}, {262120, 0, 262144, 20}});
    reject(cap::Scope::Desktop, {}, {}, far);
    Check(cap::MakePlan(cap::Scope::Monitor, cap::AsRect(far.monitors[0]), {}, far, plan),
          "a small selected screen can be copied even when virtual desktop bounds are enormous");
    const auto small = Displays({{-12, -6, 0, 0}, {0, 0, 12, 6}});
    for (double bad : {0.0, 0.999, -1.0, 4096.1, std::numeric_limits<double>::quiet_NaN(),
                       std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity()})
        reject(cap::Scope::Desktop, {}, {bad, {}}, small);
    for (double bad : {262145.0, -262145.0, std::numeric_limits<double>::quiet_NaN(),
                       std::numeric_limits<double>::infinity()})
        reject(cap::Scope::Desktop, {}, {2, {bad, 0}}, small);
    reject(cap::Scope::Monitor, {-12.1, -6, 0, 0}, {}, small);
    reject(cap::Scope::Monitor, {-12, -6, 1, 0}, {}, small);
    reject(static_cast<cap::Scope>(99), {}, {}, small);
    reject(cap::Scope::Desktop, {}, {2, {100, 100}}, small);
    auto malformed = small;
    malformed.count = cap::kMaxMonitors + 1;
    reject(cap::Scope::Desktop, {}, {}, malformed);
    malformed.count = 0;
    reject(cap::Scope::Desktop, {}, {}, malformed);
    malformed = small;
    malformed.monitors[0].left = -262145;
    reject(cap::Scope::Desktop, {}, {}, malformed);
    malformed = small;
    malformed.monitors[0] = {-12, -6, 12, 6};
    reject(cap::Scope::Desktop, {}, {}, malformed);
    auto reordered = small;
    std::swap(reordered.monitors[0], reordered.monitors[1]);
    reordered.monitors[2] = reordered.monitors[1];
    reordered.count = 3;
    Check(cap::SameTopology(small, reordered), "order and exact cloned rectangles do not create false topology changes");
    Check(cap::Canonicalize(reordered) && reordered.count == 2,
          "duplicates are removed before rendering or bounding work");

    Check(cap::MakePlan(cap::Scope::Desktop, {}, {}, small, plan), "guard fixture plan");
    std::vector<std::uint32_t> source(static_cast<std::size_t>(plan.sourcePixels), 0x00778899);
    std::vector<std::uint32_t> output(static_cast<std::size_t>(plan.outputPixels), 0x12345678);
    std::vector<cap::AxisSample> axis(plan.width);
    const auto render = [&](const cap::Plan& value, const std::uint32_t* input, std::size_t count,
                            std::uint32_t* destination, std::size_t outCount, cap::AxisSample* map,
                            std::size_t maps, cap::Heartbeat pulse = {}) {
        return cap::Render(value, input, count, destination, outCount, map, maps, pulse);
    };
    Check(!render(plan, source.data(), source.size() - 1, output.data(), output.size(), axis.data(), axis.size()), "short source rejected");
    Check(!render(plan, source.data(), source.size(), output.data(), output.size() - 1, axis.data(), axis.size()), "short output rejected");
    Check(!render(plan, source.data(), source.size(), output.data(), output.size(), axis.data(), axis.size() - 1), "short axis map rejected");
    Check(!render(plan, nullptr, source.size(), output.data(), output.size(), axis.data(), axis.size()), "null source rejected");
    Check(!render(plan, source.data(), source.size(), source.data(), source.size(), axis.data(), axis.size()), "aliasing source/output rejected");
    Check(!render(plan, source.data(), source.size(), output.data(), output.size(),
                  reinterpret_cast<cap::AxisSample*>(output.data()), axis.size()), "aliasing workspace/output rejected");
    auto tampered = plan;
    tampered.source.left--;
    Check(!render(tampered, source.data(), source.size(), output.data(), output.size(), axis.data(), axis.size()), "tampered crop rejected");
    tampered = plan;
    tampered.outputPixels++;
    Check(!cap::ValidPlan(tampered), "tampered count rejected before allocation");
    tampered = plan;
    tampered.topology.monitors[2] = tampered.topology.monitors[0];
    tampered.topology.count = 3;
    Check(!cap::ValidPlan(tampered), "noncanonical duplicate topology cannot amplify render work");
    Check(std::all_of(output.begin(), output.end(), [](auto p) { return p == 0x12345678; }),
          "all precondition failures leave output untouched");
    for (unsigned stop : {1u, 2u, 3u}) {
        Pulse pulse{0, stop};
        Check(!render(plan, source.data(), source.size(), output.data(), output.size(), axis.data(), axis.size(), pulse.AsHeartbeat()),
              "heartbeat cancellation suppresses unpublished partial image");
        Check(pulse.calls == stop, "cancellation happens at requested reached boundary");
    }
}

enum class Call : unsigned { Ready, Prepare, Quiesce, Enter, Before, Capture, After, Restore, Render, Recheck, Publish, Finish, Count };
struct PolicyHost {
    unsigned mask = 0;
    cap::ClipboardResult clipboard = cap::ClipboardResult::Copied;
    std::array<unsigned, static_cast<unsigned>(Call::Count)> calls{};
    unsigned identityChecks = 0;
    bool busy = false, idle = false, restored = false;
    bool Allowed(unsigned bit) const noexcept { return (mask & (1u << bit)) != 0; }
    void Count(Call call) noexcept { ++calls[static_cast<unsigned>(call)]; }
    unsigned Counted(Call call) const noexcept { return calls[static_cast<unsigned>(call)]; }
    bool Ready() noexcept { Count(Call::Ready); return Allowed(0); }
    bool Prepare() noexcept { Count(Call::Prepare); Check(!busy, "allocation precedes quiescing zoom"); return Allowed(1); }
    void Quiesce() noexcept { Count(Call::Quiesce); busy = true; }
    bool EnterIdentity() noexcept { Count(Call::Enter); idle = Allowed(2); return idle; }
    bool IdentityMatches() noexcept {
        const bool before = identityChecks++ == 0;
        Count(before ? Call::Before : Call::After);
        return Allowed(before ? 3 : 5);
    }
    bool Capture() noexcept {
        Count(Call::Capture);
        Check(busy && idle && Allowed(3), "source capture requires a quiesced verified baseline");
        return Allowed(4);
    }
    bool RestoreView() noexcept { Count(Call::Restore); restored = Allowed(6); return restored; }
    bool Render() noexcept {
        Count(Call::Render);
        Check(restored && Allowed(3) && Allowed(4) && Allowed(5), "expensive render begins only after successful capture and view restore");
        return Allowed(7);
    }
    bool RestoredMatches() noexcept { Count(Call::Recheck); return Allowed(8); }
    cap::ClipboardResult Publish() noexcept {
        Count(Call::Publish);
        Check((mask & 511u) == 511u && restored && busy, "no clipboard mutation before every capture/restoration precondition");
        return clipboard;
    }
    void Finish() noexcept { Count(Call::Finish); busy = false; }
};

void PolicyMatrix() {
    constexpr std::array<cap::ClipboardResult, 6> clipboardResults{
        cap::ClipboardResult::Copied, cap::ClipboardResult::Busy, cap::ClipboardResult::EmptyFailed,
        cap::ClipboardResult::SetFailed, cap::ClipboardResult::CloseFailed, cap::ClipboardResult::Invalid};
    for (unsigned mask = 0; mask < 512; ++mask) for (auto clipboard : clipboardResults) {
        ++policyCases;
        PolicyHost host;
        host.mask = mask; host.clipboard = clipboard;
        const auto result = cap::Run(host);
        Check(!host.busy, "all policy exits release capture quiescence");
        Check(host.Counted(Call::Finish) == host.Counted(Call::Quiesce), "finish runs exactly once iff quiesced");
        const bool entered = host.Counted(Call::Enter) && host.Allowed(2);
        Check(host.Counted(Call::Restore) == (entered ? 1u : 0u),
              "every entered identity attempts conditional restoration even after capture failure");
        Check(host.Counted(Call::Publish) == (mask == 511 ? 1u : 0u),
              "all combinations of early failures protect clipboard publication");
        if (entered && !host.Allowed(6)) Check(result == cap::Result::RestoreFailed,
              "restoration failure is never hidden by a capture/render status");
        if (host.Counted(Call::Publish)) Check(result == cap::ClipboardOutcome(clipboard),
              "all clipboard boundary outcomes remain distinguishable");
    }
    Check(policyCases == 3072, "entire nine-gate plus six-clipboard-result matrix ran");
}

struct Clipboard {
    unsigned mask = 0, opens = 0, empties = 0, sets = 0, closes = 0;
    bool opened = false, oldData = true, ownsHandle = true, windowsOwnsHandle = false;
    bool firstCloseFails = false;
    bool Open() noexcept { ++opens; opened = (mask & 1u) != 0; return opened; }
    bool Empty() noexcept {
        ++empties; Check(opened, "clipboard empty only while opened");
        if (!(mask & 2u)) return false;
        oldData = false;
        return true;
    }
    bool Set() noexcept {
        ++sets; Check(opened && !oldData && ownsHandle, "clipboard set only with prepared owned handle after empty");
        if (!(mask & 4u)) return false;
        ownsHandle = false; windowsOwnsHandle = true;
        return true;
    }
    bool Close() noexcept {
        ++closes; Check(opened, "close paired with successful open");
        if (firstCloseFails && closes == 1) return false;
        if (!(mask & 8u)) return false;
        opened = false;
        return true;
    }
};
void ClipboardMatrix() {
    for (unsigned mask = 0; mask < 16; ++mask) {
        ++clipboardCases;
        Clipboard backend; backend.mask = mask;
        const auto result = cap::PublishPrepared(backend);
        Check(backend.closes == ((mask & 1u) ? ((mask & 8u) ? 1u : 2u) : 0u),
              "every successful open closes with at most one retry on all outcomes");
        Check(backend.sets == ((mask & 3u) == 3u ? 1u : 0u), "Set never called on failed Open or failed Empty");
        const bool transferred = (mask & 7u) == 7u;
        Check(backend.windowsOwnsHandle == transferred && backend.ownsHandle != transferred,
              "handle transfers only after successful Set, including failed Close");
        Check(backend.oldData == ((mask & 3u) != 3u), "old clipboard persists exactly up to the Empty boundary");
        if (!(mask & 1u)) Check(result == cap::ClipboardResult::Busy, "clipboard contention is nonmutating");
        else if (!(mask & 8u)) Check(result == cap::ClipboardResult::CloseFailed, "close failure is observable even after Set");
        else if (!(mask & 2u)) Check(result == cap::ClipboardResult::EmptyFailed, "empty failure preserves old contents");
        else if (!(mask & 4u)) Check(result == cap::ClipboardResult::SetFailed && !backend.oldData,
                                  "post-empty failure honestly records loss of old contents");
        else Check(result == cap::ClipboardResult::Copied, "completed clipboard transfer succeeds");
    }
    for (unsigned mask = 0; mask < 8; ++mask) {
        ++clipboardCases;
        Clipboard backend; backend.mask = mask | 8u; backend.firstCloseFails = true;
        const auto result = cap::PublishPrepared(backend);
        Check(backend.closes == ((mask & 1u) ? 2u : 0u) && !backend.opened,
              "single close retry releases a transient failure without looping");
        const auto expected = !(mask & 1u) ? cap::ClipboardResult::Busy :
            !(mask & 2u) ? cap::ClipboardResult::EmptyFailed :
            !(mask & 4u) ? cap::ClipboardResult::SetFailed : cap::ClipboardResult::Copied;
        Check(result == expected, "successful close retry preserves original Empty/Set outcome");
        Check(backend.windowsOwnsHandle == ((mask & 7u) == 7u), "retry never changes handle-transfer ownership");
    }
}

struct Journal {
    ses::RecoveryRecord record{};
    bool armed = false;
    void Publish(const ses::RecoveryRecord& value) noexcept {
        Check(ses::Valid(value), "fusion writes publish valid recovery before native mutation");
        record = value; armed = true;
    }
    void Clear() noexcept { armed = false; }
};
enum class NativeFault { None, Read, Reject, FalseAfter, ForeignRead, ForeignAfter };
struct Backend {
    Journal& journal;
    ses::Snapshot state{};
    NativeFault fault = NativeFault::None;
    unsigned faultAt = 0, reads = 0, writes = 0, reached = 0;
    void (*callback)(void*) noexcept = nullptr;
    void* callbackContext = nullptr;
    unsigned callbackAt = 0, callbackCalls = 0, setterDepth = 0, maximumDepth = 0;
    bool callbackAfter = false;
    bool foreign = false;
    static ses::Snapshot Foreign() noexcept { return {{7, 19, -4}, {true, {0, 0, 6, 4}, {0, 0, 24, 12}}}; }
    void MakeForeign() noexcept { state = Foreign(); foreign = true; }
    bool Read(ses::Snapshot& output) noexcept {
        ++reads;
        if (reads == faultAt && (fault == NativeFault::Read || fault == NativeFault::ForeignRead)) {
            ++reached;
            if (fault == NativeFault::Read) return false;
            MakeForeign();
        }
        output = state; return true;
    }
    bool Write(bool visual, ses::Snapshot next) noexcept {
        Check(journal.armed && ses::Valid(journal.record), "every real Session setter has an armed journal");
        Check(!foreign, "observable foreign ownership is never overwritten during capture transaction");
        ++writes;
        ++setterDepth;
        maximumDepth = std::max(maximumDepth, setterDepth);
        struct Depth {
            unsigned& depth;
            ~Depth() noexcept { --depth; }
        } depth{setterDepth};
        const auto interrupt = [&]() noexcept {
            if (callback && callbackAt == writes) { ++callbackCalls; callback(callbackContext); }
        };
        if (!callbackAfter) interrupt();
        if (writes == faultAt && fault == NativeFault::Reject) { ++reached; return false; }
        if (visual) state.visual = next.visual; else state.input = next.input;
        if (callbackAfter) interrupt();
        if (writes == faultAt && fault == NativeFault::FalseAfter) { ++reached; return false; }
        if (writes == faultAt && fault == NativeFault::ForeignAfter) { ++reached; MakeForeign(); }
        return true;
    }
    bool SetVisual(ses::Visual visual) noexcept { auto next = state; next.visual = visual; return Write(true, next); }
    bool SetInput(ses::Input input) noexcept { auto next = state; next.input = input; return Write(false, next); }
};
enum class Outside { None, Foreign, DesktopLost, TopologyChanged, GuardianLost, Pause, EmergencyReset };

// An explicitly modeled App host, not Windows. Actual production capture Run,
// lifecycle, Session and recovery journal are composed. Cutpoints model native
// API observations and asynchronous outside changes; no claim of atomic OS CAS.
struct FusionHost {
    Journal journal;
    Backend backend{journal};
    ses::Session owner;
    glide::runtime::State lifecycle;
    cap::Lease lease;
    ses::Snapshot saved{};
    bool monitor = false, overlay = false, input = true, gesture = false, busy = false;
    bool desktop = true, topology = true, guardian = true, restored = false;
    bool allocation = true, capture = true, render = true;
    bool leaseActive = true;
    bool deferredReset = false;
    unsigned emergencyIntents = 0, drainedResets = 0;
    unsigned stages = 0, at = 0, reached = 0, captures = 0, restores = 0, publishes = 0;
    std::uint64_t beforeEpoch = 0, duringEpoch = 0;
    Outside outside = Outside::None;
    explicit FusionHost(unsigned state, bool held) noexcept : monitor(state == 1), gesture(held) {
        Check(lifecycle.PrepareEnable() && lifecycle.Activate(), "fusion lifecycle ready");
        if (state == 1) saved = {{}, {true, {3, 2, 15, 8}, {0, 0, 24, 12}}};
        if (state == 2) saved = {{2, 3, 2}, {true, {3, 2, 15, 8}, {0, 0, 24, 12}}};
        if (state) Check(Success(owner.Apply(saved, backend, journal)), "initial saved zoom acquired through real Session");
        overlay = monitor;
        backend.reads = backend.writes = 0;
        beforeEpoch = lifecycle.Epoch();
        Check(lease.Acquire(beforeEpoch), "freeze capture generation before dispatch-capable preparation");
    }
    void Stage() noexcept {
        ++stages;
        if (stages != at || outside == Outside::None) return;
        ++reached;
        switch (outside) {
        case Outside::Foreign: backend.MakeForeign(); break;
        case Outside::DesktopLost: desktop = false; break;
        case Outside::TopologyChanged: topology = false; break;
        case Outside::GuardianLost: guardian = false; break;
        case Outside::Pause: lifecycle.Pause(); break;
        case Outside::EmergencyReset: EmergencyIntent(this); break;
        default: break;
        }
    }
    static void EmergencyIntent(void* context) noexcept {
        auto& host = *static_cast<FusionHost*>(context);
        const auto writes = host.backend.writes;
        const auto observed = host.backend.state;
        ++host.emergencyIntents;
        host.deferredReset = true;
        host.lifecycle.RequestRestore();
        host.lifecycle.Cancel();
        host.gesture = host.input = false;
        Check(host.backend.writes == writes && host.backend.state == observed,
              "nested emergency handling only invalidates epoch/latches intent, never reenters native Session");
    }
    bool Environment() const noexcept {
        return desktop && topology && guardian && lifecycle.Ready() &&
               (!leaseActive || lease.Matches(lifecycle.Epoch()));
    }
    bool Identity() noexcept {
        ses::Snapshot observed;
        return Environment() && !overlay && backend.Read(observed) && ses::IsIdentity(observed);
    }
    bool Ready() noexcept {
        Stage();
        if (!Environment()) return false;
        const auto result = owner.Check(backend, journal);
        ses::Snapshot observed;
        return Success(result) && backend.Read(observed) && observed == saved;
    }
    bool Prepare() noexcept { Stage(); return allocation && lease.Matches(lifecycle.Epoch()); }
    void Quiesce() noexcept {
        Check(lease.AdvanceOne(lifecycle.Epoch()), "intentional Quiesce predicts exactly one cancellation");
        Stage(); busy = true; input = gesture = false; lifecycle.Cancel(); duringEpoch = lifecycle.Epoch();
    }
    bool EnterIdentity() noexcept {
        Stage();
        if (!Environment()) return false;
        if (!lease.AdvanceOne(lifecycle.Epoch())) return false;
        lifecycle.Cancel(); // App::Reset always cancels input once before restoring owned state.
        overlay = false;
        lifecycle.RequestRestore();
        const auto result = owner.Reset(backend, journal);
        if (!Success(result)) {
            if (result == ses::Result::Conflict) lifecycle.RestoreConflict();
            return false;
        }
        lifecycle.RestoreComplete();
        return true;
    }
    bool IdentityMatches() noexcept { Stage(); return Identity(); }
    bool Capture() noexcept {
        Stage(); ++captures;
        Check(busy && !input && !gesture, "capture never consumes live queued gesture input");
        return capture && Identity();
    }
    bool RestoreView() noexcept {
        Stage(); ++restores;
        if (!Identity()) return false;
        const auto result = owner.Apply(saved, backend, journal);
        if (!Success(result)) {
            if (result == ses::Result::Conflict) lifecycle.RestoreConflict();
            else lifecycle.RequestRestore();
            return false;
        }
        overlay = monitor;
        restored = true;
        return true;
    }
    bool Render() noexcept { Stage(); Check(restored && !input && busy, "render after view restore while input remains quiesced"); return render; }
    bool RestoredMatches() noexcept {
        Stage();
        if (!Environment() || !Success(owner.Check(backend, journal))) return false;
        ses::Snapshot observed;
        return backend.Read(observed) && observed == saved && overlay == monitor;
    }
    cap::ClipboardResult Publish() noexcept {
        Stage(); ++publishes;
        // The native final-check/publication gap is non-atomic. Outside faults
        // in this fixture are inserted only at observable cuts before publish.
        Check(restored && busy && !input, "publish only after restored view");
        return cap::ClipboardResult::Copied;
    }
    void Finish() noexcept {
        lifecycle.Cancel(); busy = false;
        lease.Disarm(); leaseActive = false;
        input = Environment();
        Check(!lifecycle.Accept(beforeEpoch) && !lifecycle.Accept(duringEpoch),
              "pre-capture and during-capture queued commands cannot replay after finish");
    }
    // Models App's outer request guard, which also runs when preparation or
    // Ready fails before capture::Run acquires its inner Finish obligation.
    void EndRequest() noexcept {
        lease.Disarm(); leaseActive = false;
        if (deferredReset) {
            Check(backend.setterDepth == 0, "deferred reset drains only after outer native setter returns");
            ++drainedResets;
            overlay = false;
            lifecycle.Cancel();
            const auto result = owner.Reset(backend, journal);
            if (Success(result)) lifecycle.RestoreComplete();
            else if (result == ses::Result::Conflict) lifecycle.RestoreConflict();
            deferredReset = false;
        }
        input = Environment();
    }
    void Recover() noexcept {
        const auto before = backend.state;
        const bool wasForeign = backend.foreign;
        backend.fault = NativeFault::None;
        if (owner.NeedsRecovery()) {
            // Acquisition marks a verified baseline owned before its first
            // write. A read failure before that write needs no armed guardian
            // journal because nothing has changed globally.
            if (!journal.armed) Check(ses::IsIdentity(backend.state),
                                      "unpublished acquisition failure leaves native identity unchanged");
            const auto record = journal.armed ? journal.record : owner.Record();
            const auto recovered = ses::Session::Recover(record, backend);
            if (wasForeign) Check(recovered == ses::Result::Conflict && backend.state == before,
                                  "guardian recovery recognizes and preserves foreign takeover");
            else Check(Success(recovered) && ses::IsIdentity(backend.state),
                       "retained owned partial state remains recoverable");
        } else Check(!journal.armed, "released ownership has no stale recovery record");
        if (wasForeign) Check(backend.state == before, "cleanup never resets another magnifier");
    }
};

cap::Result RunFusion(FusionHost& host) noexcept {
    const auto result = cap::Run(host);
    host.EndRequest();
    return result;
}

void FusionMatrix() {
    for (unsigned state = 0; state < 3; ++state) for (bool held : {false, true}) {
        FusionHost reference(state, held);
        Check(RunFusion(reference) == cap::Result::Copied, "idle/monitor/desktop happy capture transaction");
        Check(reference.backend.state == reference.saved && reference.overlay == reference.monitor,
              "saved applied view is restored, never the pending animation target");
        const unsigned stageCount = reference.stages - 1; // Publish is deliberately outside last observable verification.
        const unsigned readCount = reference.backend.reads, writeCount = reference.backend.writes;
        ++fusionCases;
        reference.Recover();
        for (auto outside : {Outside::Foreign, Outside::DesktopLost, Outside::TopologyChanged,
                             Outside::GuardianLost, Outside::Pause, Outside::EmergencyReset})
            for (unsigned cut = 1; cut <= stageCount; ++cut) {
                FusionHost host(state, held);
                host.outside = outside; host.at = cut;
                const auto result = RunFusion(host);
                ++fusionCases;
                Check(host.reached == 1, "every declared outside interference cut is actually reached");
                ++boundaries;
                Check(result != cap::Result::Copied && host.publishes == 0,
                      "observed foreign state, desktop loss, topology change, guardian loss or pause suppresses publication");
                Check(!host.busy, "interference cannot strand capture busy flag");
                if (host.backend.foreign) Check(host.backend.state == Backend::Foreign(),
                                                "foreign transform remains byte-for-byte untouched");
                if (outside == Outside::EmergencyReset) Check(ses::IsIdentity(host.backend.state) && !host.overlay,
                    "emergency reset at any observed cut is never undone by reapplying captured zoom");
                host.Recover();
            }
        for (auto fault : {NativeFault::Read, NativeFault::ForeignRead})
            for (unsigned ordinal = 1; ordinal <= readCount; ++ordinal) {
                FusionHost host(state, held);
                host.backend.fault = fault; host.backend.faultAt = ordinal;
                const auto result = RunFusion(host);
                ++fusionCases;
                Check(host.backend.reached == 1, "every native read failure/takeover boundary is reached");
                ++boundaries;
                Check(result != cap::Result::Copied && !host.publishes, "native observation failures cannot publish a misleading screenshot");
                host.Recover();
            }
        for (auto fault : {NativeFault::Reject, NativeFault::FalseAfter, NativeFault::ForeignAfter})
            for (unsigned ordinal = 1; ordinal <= writeCount; ++ordinal) {
                FusionHost host(state, held);
                host.backend.fault = fault; host.backend.faultAt = ordinal;
                const auto result = RunFusion(host);
                ++fusionCases;
                Check(host.backend.reached == 1, "every native setter boundary is reached");
                ++boundaries;
                if (fault == NativeFault::FalseAfter) Check(result == cap::Result::Copied,
                    "false native result after real effect is accepted only by verified production Session postconditions");
                else Check(result != cap::Result::Copied && !host.publishes,
                           "rejected/foreign partial setters prevent clipboard publication");
                host.Recover();
            }
        for (unsigned mask = 0; mask < 8; ++mask) {
            FusionHost host(state, held);
            host.allocation = (mask & 1u) != 0;
            host.capture = (mask & 2u) != 0;
            host.render = (mask & 4u) != 0;
            const auto result = RunFusion(host);
            ++fusionCases;
            Check((result == cap::Result::Copied) == (mask == 7), "mixed allocation/capture/render failures publish only complete image");
            Check(host.backend.state == host.saved && host.overlay == host.monitor,
                  "local capture pipeline failures preserve or restore the previously applied view");
            host.Recover();
        }
    }
}

void NativeSetterReentrancy() {
    for (unsigned state : {1u, 2u}) for (bool held : {false, true}) {
        FusionHost baseline(state, held);
        Check(RunFusion(baseline) == cap::Result::Copied, "native reentrancy baseline transaction");
        const auto nativeWrites = baseline.backend.writes;
        baseline.Recover();
        for (unsigned ordinal = 1; ordinal <= nativeWrites; ++ordinal) for (bool after : {false, true}) {
            ++reentrantCases;
            FusionHost host(state, held);
            host.backend.callback = FusionHost::EmergencyIntent;
            host.backend.callbackContext = &host;
            host.backend.callbackAt = ordinal;
            host.backend.callbackAfter = after;
            const auto result = RunFusion(host);
            Check(host.backend.callbackCalls == 1 && host.emergencyIntents == 1,
                  "each nested emergency callback reaches requested before/after-setter cut");
            Check(result != cap::Result::Copied && host.publishes == 0,
                  "native-call reentrant reset cancels screenshot even if outer setter subsequently succeeds");
            Check(host.backend.maximumDepth == 1 && host.backend.setterDepth == 0,
                  "same Session is never entered by a second native setter while outer setter is live");
            Check(host.drainedResets == 1 && !host.deferredReset && !host.owner.NeedsRecovery() && !host.journal.armed,
                  "outer request guard drains reset exactly once and releases recovery ownership");
            Check(ses::IsIdentity(host.backend.state) && !host.overlay && !host.busy,
                  "emergency intent wins over saved-view restoration before any capture request returns");
        }
    }
    Check(reentrantCases == 24, "all initial scope/held state/native setter/before-after tuples ran");
}

void LeaseRegressions() {
    {
        ++leaseCases;
        cap::Lease lease;
        Check(!lease.Armed() && !lease.Matches(0) && !lease.AdvanceOne(0), "default lease cannot authorize any generation");
        Check(!lease.Acquire(0) && !lease.Armed(), "zero generation is never a valid capture lease");
        Check(lease.Acquire(17) && lease.Matches(17), "capture freezes known existing generation");
        Check(lease.AdvanceOne(17) && !lease.Matches(17) && lease.Matches(18),
              "intentional one-step transition must happen exactly once");
        Check(!lease.AdvanceOne(19) && !lease.Armed() && !lease.Matches(18),
              "unexpected generation disarms rather than adopting changed state");
        Check(lease.Acquire(std::numeric_limits<std::uint64_t>::max()), "maximum generation can be represented");
        Check(!lease.AdvanceOne(std::numeric_limits<std::uint64_t>::max()) && !lease.Armed(),
              "epoch overflow cannot wrap and authorize old input");
    }
    for (unsigned cut = 0; cut < 4; ++cut) {
        ++leaseCases;
        glide::runtime::State state;
        Check(state.PrepareEnable() && state.Activate(), "lease regression starts Ready");
        cap::Lease lease;
        const auto saved = state.Epoch();
        Check(lease.Acquire(saved), "acquire lease at saved-view snapshot");
        if (cut == 0) state.Cancel(); // Reset while native allocation/preparation is in progress.
        if (cut == 0) {
            Check(state.Ready() && !lease.Matches(state.Epoch()),
                  "preparation completion refuses reentrant Reset even though lifecycle remains Ready");
            continue;
        }
        Check(lease.AdvanceOne(state.Epoch()), "predict intentional Quiesce");
        if (cut == 1) state.Cancel(); // Sent message during a native call inside the transition.
        state.Cancel();
        if (cut == 1) {
            Check(!lease.Matches(state.Epoch()), "extra cancellation cannot masquerade as expected Quiesce");
            continue;
        }
        Check(lease.Matches(state.Epoch()) && lease.AdvanceOne(state.Epoch()), "predict deliberate identity Reset");
        state.Cancel();
        if (cut == 2) state.Cancel(); // Emergency reset after source capture.
        if (cut == 3) state.Pause();  // A different lifecycle mutation also expires the lease.
        Check(!lease.Matches(state.Epoch()), "outside cancellation prevents subsequent saved-view reapply");
        Check(!lease.AdvanceOne(state.Epoch()) && !lease.Armed(), "failed lease never learns replacement epoch");
    }
}

void GetterLeaseRegressions() {
    // reader is a native-call boundary; State/Lease are production classes.
    // A true native result must not hide a sent message's cancellation.
    for (unsigned event = 0; event < 7; ++event) {
        ++observeCases;
        glide::runtime::State state;
        Check(state.PrepareEnable() && state.Activate(), "getter lease fixture starts Ready");
        cap::Lease lease;
        Check(lease.Acquire(state.Epoch()), "getter lease frozen before native call");
        unsigned calls = 0;
        const auto reader = [&]() noexcept {
            ++calls;
            switch (event) {
            case 1: state.Cancel(); break; // Emergency Reset can remain Ready.
            case 2: state.Pause(); break;
            case 3: state.Stop(); break;
            case 4: state.RequestRestore(); break; // Readiness can change without changing epoch.
            case 6: state.Cancel(); break;
            default: break;
            }
            return event != 5 && event != 6;
        };
        static_assert(noexcept(lease.Observe(state, reader)), "noexcept reader keeps observer nonthrowing");
        const bool observed = lease.Observe(state, reader);
        Check(calls == 1, "eligible getter is called exactly once");
        Check(observed == (event == 0), "successful getter authorizes only unchanged Ready generation");
        if (event == 1) Check(state.Ready(), "regression specifically detects reset that still reports Ready");
        if (event == 4) Check(lease.Matches(state.Epoch()), "pending restoration is rejected even with matching epoch");
    }
    for (unsigned invalid = 0; invalid < 5; ++invalid) {
        ++observeCases;
        glide::runtime::State state;
        Check(state.PrepareEnable() && state.Activate(), "initially invalid getter fixture starts Ready");
        cap::Lease lease;
        if (invalid == 1) lease.Acquire(state.Epoch() + 1);
        if (invalid >= 2) {
            if (invalid == 2) state.Pause();
            if (invalid == 3) state.Stop();
            if (invalid == 4) state.RequestRestore();
            lease.Acquire(state.Epoch());
        }
        unsigned calls = 0;
        Check(!lease.Observe(state, [&]() noexcept { ++calls; return true; }) && calls == 0,
              "disarmed/mismatched/not-Ready state prevents any native getter call");
    }
    Check(observeCases == 12, "every declared before/after getter lease case executed");
}
} // namespace

int Run_capture() {
    ImageMatrix();
    BoundsAndCancellation();
    PolicyMatrix();
    ClipboardMatrix();
    FusionMatrix();
    LeaseRegressions();
    NativeSetterReentrancy();
    GetterLeaseRegressions();
    std::cout << "Capture tests passed: " << assertions << " assertions; " << geometryCases
              << " geometry tuples (" << imageCases << " images, " << emptySourceCases
              << " empty-source rejections) / " << imagePixels << " oracle pixels; " << policyCases
              << " policy tuples; " << clipboardCases << " clipboard boundary tuples; "
              << fusionCases << " Session/lifecycle fusion tuples; " << boundaries
              << " reached injected boundaries; " << leaseCases << " epoch-lease regressions; "
              << reentrantCases << " nested-setter emergency regressions; "
              << observeCases << " getter-lease regressions.\n";
    return 0;
}
