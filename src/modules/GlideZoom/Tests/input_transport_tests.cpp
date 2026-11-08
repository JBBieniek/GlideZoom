#include <stdexcept>
#include "../GlideZoom/input_transport.h"

#include <cstdlib>
#include <iostream>
#include <thread>

namespace {
void Check(bool ok, const char* message) {
    if (!ok) { std::cerr << "FAIL: " << message << '\n'; throw std::runtime_error("GlideZoom assertion failed; see captured output"); }
}

struct Packet {
    std::uint64_t sequence;
    std::uint64_t inverse;
};
} // namespace

int Run_input_transport() {
    using namespace glide::input;
    SpscQueue<Packet, 8> small;
    Packet packet{};
    Check(!small.Pop(packet), "new queue empty");
    for (std::uint64_t cycle = 0; cycle < 10000; ++cycle) {
        for (std::uint64_t i = 0; i < 7; ++i) {
            Check(small.Push({cycle * 7 + i, ~(cycle * 7 + i)}), "available slot accepts packet");
        }
        Check(!small.Push({0xBAD, 0}), "full queue rejects without overwriting");
        for (std::uint64_t i = 0; i < 7; ++i) {
            Check(small.Pop(packet) && packet.sequence == cycle * 7 + i &&
                  packet.inverse == ~packet.sequence, "wrap preserves order and payload");
        }
        Check(!small.Pop(packet), "drained queue empty");
    }
    small.Push({1, ~std::uint64_t{1}});
    small.Reset();
    Check(!small.Pop(packet), "stopped queue reset discards old stream");

    SpscQueue<Packet, 64> queue;
    constexpr std::uint64_t packets = 1000000;
    std::thread producer([&] {
        for (std::uint64_t i = 0; i < packets; ++i) {
            while (!queue.Push({i, ~i})) std::this_thread::yield();
        }
    });
    for (std::uint64_t expected = 0; expected < packets; ++expected) {
        while (!queue.Pop(packet)) std::this_thread::yield();
        Check(packet.sequence == expected && packet.inverse == ~expected,
              "concurrent queue preserves exact FIFO payload across wrap");
    }
    producer.join();
    Check(!queue.Pop(packet), "no duplicate packets after concurrent drain");

    SnapshotChannel channel;
    channel.Publish({0, 0, 0});
    std::atomic<bool> done{false};
    std::uint64_t accepted = 0, inconsistent = 0;
    std::thread writer([&] {
        for (std::uint64_t i = 1; i <= packets; ++i) channel.Publish({i * 3, i, static_cast<double>(i)});
        done.store(true);
    });
    do {
        Snapshot value;
        if (channel.Read(value)) {
            ++accepted;
            Check(value.options == value.epoch * 3 && value.zoom == static_cast<double>(value.epoch),
                  "accepted snapshot always has matching options, epoch and zoom");
        } else {
            ++inconsistent;
        }
    } while (!done.load());
    writer.join();
    Snapshot final;
    Check(channel.Read(final) && final.epoch == packets && final.options == packets * 3 &&
          final.zoom == static_cast<double>(packets), "final committed snapshot available");
    std::cout << "Input transport: 1,070,000 ordered packets; " << accepted
              << " coherent concurrent snapshots; " << inconsistent
              << " in-progress snapshots rejected without waiting.\n";
    return 0;
}
