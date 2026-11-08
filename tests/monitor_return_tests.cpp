#include "../src/input_routing.h"
#include "../src/monitor_return.h"

#include <array>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace {
void CheckReturn(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}
void CheckEntry(bool condition, const char *message, glide::Rect bounds,
                glide::Transform parked, glide::Vec2 entry) {
  if (condition)
    return;
  std::ostringstream detail;
  detail << std::setprecision(17) << message << "; bounds=[" << bounds.left
         << ',' << bounds.top << ',' << bounds.right << ',' << bounds.bottom
         << "]; zoom=" << parked.zoom << "; offset=[" << parked.offset.x << ','
         << parked.offset.y << "]; entry=[" << entry.x << ',' << entry.y << ']';
  std::cerr << detail.str() << '\n';
  throw std::runtime_error(detail.str());
}
struct ReturnBackend {
  glide::session::Snapshot current{};
  unsigned inputWrites = 0;
  bool Read(glide::session::Snapshot &output) const {
    output = current;
    return true;
  }
  bool SetVisual(glide::session::Visual value) {
    current.visual = value;
    return true;
  }
  bool SetInput(glide::session::Input value) {
    current.input = value;
    ++inputWrites;
    return true;
  }
};
struct ReturnJournal {
  void Publish(glide::session::RecoveryRecord value) {
    CheckReturn(glide::session::Valid(value), "mapping journal stays valid");
  }
  void Clear() {}
};
} // namespace

int main() {
  using namespace glide;
  using namespace glide::input::routing;
  unsigned entries = 0;
  // Regression: at 1.01x a 1080-pixel display can expose source y1079
  // while its quantized visible bottom is only 1079.3069. Rounding that
  // exclusive input edge to 1079 discards a pixel still present in the image.
  for (const Rect bounds :
       {Rect{0, 0, 1920, 1080}, Rect{-1920, -1080, 0, 0},
        Rect{-2560, -400, 0, 1040}, Rect{1920, -300, 5760, 1860}})
    for (const double zoom : {1.0001, 1.01, 1.05, 1.99999, 2.0, 7.5, 16.0}) {
      const Transform parked{
          zoom, ClampOffset({bounds.left, bounds.top}, zoom, bounds)};
      for (const Vec2 entry :
           {Vec2{bounds.right - 1, (bounds.top + bounds.bottom) / 2},
            Vec2{(bounds.left + bounds.right) / 2, bounds.bottom - 1},
            Vec2{bounds.right - 1, bounds.bottom - 1}}) {
        Transform revealed{};
        CheckEntry(monitor_return::RevealEntry(parked, bounds, entry, revealed),
                   "fractional far-edge entry can reveal its retained camera",
                   bounds, parked, entry);
        monitor::Frame frame{}, replay{};
        CheckEntry(
            monitor::MakeFrame(bounds, revealed, frame) &&
                monitor::Contains(monitor::AsRect(frame.source), entry) &&
                monitor::Contains(bounds,
                                  SourceToScreen(entry, frame.transform)),
            "far-edge entry is present in both the displayed image and its "
            "input crop",
            bounds, parked, entry);
        CheckEntry(
            monitor::MakeFrame(bounds, frame.transform, replay) &&
                monitor::SameFrame(frame, replay),
            "far-edge crop retains exact frame replay and mapping ownership",
            bounds, parked, entry);
        Vec2 mapped{};
        CheckEntry(
            monitor::MapInput(frame, SourceToScreen(entry, frame.transform),
                              mapped) &&
                std::abs(mapped.x - entry.x) < 1 &&
                std::abs(mapped.y - entry.y) < 1,
            "far-edge input mapping error remains below one source pixel",
            bounds, parked, entry);
        ++entries;
      }
    }
  for (const Rect bounds : {Rect{0, 0, 100, 100}, Rect{-100, -100, 0, 0}}) {
    const Transform exact{2, {bounds.right / 2 - 1, bounds.bottom / 2 - 1}};
    monitor::Frame frame{}, replay{};
    CheckReturn(monitor::MakeFrame(bounds, exact, frame) &&
                    frame.source.right == bounds.right - 1 &&
                    frame.source.bottom == bounds.bottom - 1,
                "an exact visible-high boundary does not include the unshown "
                "final physical pixel");
    CheckReturn(monitor::MakeFrame(bounds, frame.transform, replay) &&
                    monitor::SameFrame(frame, replay),
                "the strict far-edge threshold also retains identical replay");
  }
  for (const Rect bounds : {Rect{-2560, -400, 0, 1040}, Rect{0, 0, 1920, 1080},
                            Rect{1920, -300, 5760, 1860}})
    for (const double zoom : {1.0, 1.01, 2.0, 4.0, 8.0, 16.0}) {
      const Transform parked{
          zoom, ClampOffset({bounds.left, bounds.top}, zoom, bounds)};
      const double middleX = (bounds.left + bounds.right) / 2;
      const double middleY = (bounds.top + bounds.bottom) / 2;
      for (const Vec2 entry :
           {Vec2{bounds.left, bounds.top}, Vec2{bounds.right - 1, bounds.top},
            Vec2{bounds.left, bounds.bottom - 1},
            Vec2{bounds.right - 1, bounds.bottom - 1}, Vec2{middleX, middleY},
            Vec2{bounds.left, middleY}, Vec2{bounds.right - 1, middleY}}) {
        Transform revealed{};
        CheckEntry(monitor_return::RevealEntry(parked, bounds, entry, revealed),
                   "every physical monitor entry can reveal a retained camera",
                   bounds, parked, entry);
        monitor::Frame frame{};
        CheckEntry(revealed.zoom == parked.zoom &&
                       monitor::MakeFrame(bounds, revealed, frame) &&
                       monitor::Contains(monitor::AsRect(frame.source), entry),
                   "return retains zoom and makes the entering pointer "
                   "reachable in the committed input crop",
                   bounds, parked, entry);
        monitor::Frame previous{};
        CheckReturn(monitor::MakeFrame(bounds, parked, previous),
                    "parked frame is valid");
        if (monitor::Contains(monitor::AsRect(previous.source), entry))
          CheckReturn(revealed.offset.x == parked.offset.x &&
                          revealed.offset.y == parked.offset.y,
                      "already reachable source positions retain the exact "
                      "parked camera");
        ++entries;
      }
    }

  const Rect first{0, 0, 1920, 1080}, second{1920, 0, 4480, 1440};
  const Transform parked{2, {0, 0}};
  const Vec2 entry{1800, 900}; // Outside the parked 0..960 by 0..540 crop.
  monitor::Frame oldFrame{}, returned{};
  CheckReturn(monitor::MakeFrame(first, parked, oldFrame),
              "old parked camera is valid");
  Transform camera{};
  CheckReturn(monitor_return::RevealEntry(parked, first, entry, camera) &&
                  monitor::MakeFrame(first, camera, returned),
              "outside-crop reentry rebases before input commit");
  Snapshot publication;
  publication.enabled = true;
  publication.generation = 1;
  publication.epoch = 7;
  publication.activeId = 2;
  publication.count = 2;
  publication.routes[0] = {1, first, monitor::AsRect(oldFrame.source), true};
  publication.routes[1] = {2, second, second, false};
  State routing;
  routing.Observe(publication, true, 7);
  routing.ObservePoint(entry);
  CheckReturn(routing.Report(true).targetId == 1 && routing.BlockNewButton(),
              "any point in the physical monitor requests return but blocks an "
              "unsafe first click");
  CheckReturn(routing.Button(0, true, false),
              "first unready DOWN remains consumed");
  publication.busy = true;
  publication.generation = 2;
  routing.Observe(publication, true, 7);
  const auto barrier = routing.Report(true, 1000);
  CheckReturn(CommitReady(barrier, 2, 7, 1000),
              "handoff waits for a fresh idle routing barrier");
  CheckReturn(!monitor::Contains(monitor::AsRect(oldFrame.source), entry) &&
                  monitor::Contains(monitor::AsRect(returned.source), entry),
              "regression: only the rebased camera covers the return point");
  ReturnBackend native;
  ReturnJournal journal;
  session::Session mapping;
  CheckReturn(
      mapping.Apply({{1, 0, 0}, {true, returned.source, returned.destination}},
                    native, journal) == session::Result::Applied,
      "the new reachable mapping is verified before routing is admitted");
  CheckReturn(native.inputWrites == 1 && mapping.OwnsState(),
              "camera return changes only the owned input mapping");
  publication.routes[0].source = monitor::AsRect(returned.source);
  publication.busy = false;
  publication.generation = 3;
  publication.activeId = 1;
  publication.ackSerial = barrier.requestSerial;
  routing.Observe(publication, true, 7);
  CheckReturn(
      routing.ZoomReady() && !routing.BlockNewButton(),
      "clicks become reachable only after the new frame and mapping commit");
  CheckReturn(routing.Button(0, false, false),
              "the blocked first DOWN keeps its matching consumed UP");
  CheckReturn(
      !routing.Button(0, true, false) && !routing.Button(0, false, true),
      "a fresh DOWN is forwarded and its UP cannot be stolen by gesture state");

  State dragging;
  dragging.Observe(publication, true, 7);
  dragging.ObservePoint(entry);
  CheckReturn(!dragging.Button(0, true, false),
              "application owns its original drag DOWN");
  dragging.ObservePoint({2500, 900});
  publication.busy = true;
  publication.generation = 4;
  dragging.Observe(publication, true, 7);
  CheckReturn(
      !dragging.Report(true, 1100).canCommit &&
          dragging.Report(true).targetId == 1,
      "physical drag ownership pins its monitor and prevents a return handoff");
  CheckReturn(!dragging.Button(0, false, true),
              "forwarded drag UP is always delivered");
  dragging.ObservePoint({2500, 900});
  CheckReturn(dragging.Report(true, 1200).canCommit &&
                  dragging.Report(true).targetId == 2,
              "handoff is eligible again only after the physical drag release");

  const Transform sentinel{3, {17, 19}};
  for (const Vec2 invalid :
       {Vec2{-1, 50}, Vec2{1920, 50}, Vec2{50, 1080},
        Vec2{std::numeric_limits<double>::quiet_NaN(), 50}}) {
    auto output = sentinel;
    CheckReturn(!monitor_return::RevealEntry(parked, first, invalid, output) &&
                    output.zoom == sentinel.zoom &&
                    output.offset.x == sentinel.offset.x &&
                    output.offset.y == sentinel.offset.y,
                "invalid or departed entry cannot change a camera");
  }
  std::cout << "PASS: " << entries
            << " monitor entries, retained zoom, verified crop/mapping and "
               "button-pair handoff\n";
  return 0;
}
