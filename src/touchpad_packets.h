#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace glide::touchpad
{

    inline constexpr std::size_t MaxContacts = 5;
    inline constexpr std::size_t MaxRawBytes = 65536;
    inline constexpr std::size_t MaxReportBytes = 4096;
    inline constexpr std::size_t MaxReportsPerPacket = 64;

    struct Contact
    {
        std::uint32_t id = 0;
        double x = 0, y = 0; // Descriptor-normalized touchpad coordinates, not pixels.
        bool tip = false, confidence = false, valid = true;
    };

    struct Report
    {
        std::uintptr_t device = 0;
        std::uint16_t scan = 0;
        unsigned total = 0; // Nonzero on the first report of a hybrid frame.
        unsigned slots = 0;
        std::array<Contact, MaxContacts> contacts{};
        bool button = false;
    };

    struct Frame
    {
        std::uintptr_t device = 0;
        std::uint16_t scan = 0;
        unsigned count = 0; // Includes tip-up contacts, per the PTP protocol.
        std::array<Contact, MaxContacts> contacts{};
        bool button = false;
    };

    inline bool ValidContact(const Contact& contact) noexcept
    {
        return contact.valid && std::isfinite(contact.x) && std::isfinite(contact.y) &&
               contact.x >= 0 && contact.x <= 1 && contact.y >= 0 && contact.y <= 1;
    }

    // Shared by the native variable-length RAWHID reader and hostile-packet tests.
    // Kernel-provided alignment padding after the report payload is permitted.
    inline constexpr bool ValidBatch(std::size_t bytes, std::size_t offset, std::size_t reportBytes, std::size_t reports) noexcept
    {
        return bytes <= MaxRawBytes && offset <= bytes && reportBytes > 0 &&
               reportBytes <= MaxReportBytes && reports > 0 && reports <= MaxReportsPerPacket &&
               reports <= (bytes - offset) / reportBytes;
    }

    // HidP_GetUsageValue returns unsigned, unscaled bits. Sign extension is explicit
    // when a descriptor has a negative logical minimum; arithmetic uses 64 bits.
    inline bool Normalize(std::uint32_t bits, unsigned bitSize, std::int32_t minimum, std::int32_t maximum, double& normalized) noexcept
    {
        if (!bitSize || bitSize > 32 || maximum <= minimum)
            return false;
        if (bitSize < 32 && (bits >> bitSize))
            return false;
        std::int64_t value = bits;
        if (minimum < 0 && (bits & (std::uint32_t{ 1 } << (bitSize - 1))))
        {
            value -= std::int64_t{ 1 } << bitSize;
        }
        if (value < minimum || value > maximum)
            return false;
        normalized = static_cast<double>(value - minimum) /
                     static_cast<double>(static_cast<std::int64_t>(maximum) - minimum);
        return std::isfinite(normalized) && normalized >= 0 && normalized <= 1;
    }

    struct AssemblyResult
    {
        bool complete = false;
        bool invalid = false;
        Frame frame{};
    };

    // One bounded frame at a time. Interleaved devices or incomplete hybrid frames
    // cancel instead of combining fingers from different scans/devices.
    class Assembler
    {
    public:
        AssemblyResult Push(const Report& report, std::uint64_t now) noexcept
        {
            if (!report.device || report.total > MaxContacts || !report.slots || report.slots > MaxContacts)
                return Bad();
            if (pending_ && (now < startedAt_ || now - startedAt_ > TimeoutMs))
                return Bad();
            if (pending_)
            {
                if (report.device != frame_.device || report.scan != frame_.scan || report.total)
                    return Bad();
            }
            else
            {
                frame_ = {};
                frame_.device = report.device;
                frame_.scan = report.scan;
                frame_.button = report.button;
                expected_ = report.total;
                startedAt_ = now;
                pending_ = expected_ != 0;
                if (!expected_)
                {
                    // A count-zero packet may be an orphaned hybrid continuation.
                    // Never let one fabricate a lift from still-down contacts.
                    for (unsigned i = 0; i < report.slots; ++i)
                    {
                        if (!ValidContact(report.contacts[i]) || report.contacts[i].tip)
                            return Bad();
                    }
                    return Complete();
                }
            }
            frame_.button = frame_.button || report.button;
            const unsigned remaining = expected_ - frame_.count;
            const unsigned take = remaining < report.slots ? remaining : report.slots;
            if (!take)
                return Bad();
            for (unsigned i = 0; i < take; ++i)
            {
                const auto& contact = report.contacts[i];
                if (!ValidContact(contact))
                    return Bad();
                for (unsigned j = 0; j < frame_.count; ++j)
                {
                    if (frame_.contacts[j].id == contact.id)
                        return Bad();
                }
                frame_.contacts[frame_.count++] = contact;
            }
            if (frame_.count != expected_)
                return {};
            return Complete();
        }

        bool Expire(std::uint64_t now) noexcept
        {
            if (!pending_ || (now >= startedAt_ && now - startedAt_ <= TimeoutMs))
                return false;
            Reset();
            return true;
        }
        void Reset() noexcept
        {
            pending_ = false;
            expected_ = 0;
            frame_ = {};
            startedAt_ = 0;
            activeCount_ = 0;
            previousDevice_ = 0;
        }
        bool Pending() const noexcept { return pending_; }
        bool Invariant() const noexcept
        {
            return expected_ <= MaxContacts && frame_.count <= MaxContacts && activeCount_ <= MaxContacts &&
                   (!pending_ || (frame_.device && frame_.count < expected_));
        }

    private:
        AssemblyResult Complete() noexcept
        {
            // PTP releases are explicit tip-up contact records. A previously down
            // ID disappearing without that record means data was lost; fail closed.
            // This also rejects orphan count-zero continuations containing only
            // tip-up slots while some other previously known contact remains down.
            if (previousDevice_ == frame_.device)
            {
                for (unsigned i = 0; i < activeCount_; ++i)
                {
                    bool present = false;
                    for (unsigned j = 0; j < frame_.count; ++j)
                        present = present || frame_.contacts[j].id == activeIds_[i];
                    if (!present)
                        return Bad();
                }
            }
            previousDevice_ = frame_.device;
            activeCount_ = 0;
            for (unsigned i = 0; i < frame_.count; ++i)
                if (frame_.contacts[i].tip)
                    activeIds_[activeCount_++] = frame_.contacts[i].id;
            pending_ = false;
            return { true, false, frame_ };
        }
        AssemblyResult Bad() noexcept
        {
            Reset();
            return { false, true, {} };
        }
        static constexpr std::uint64_t TimeoutMs = 50;
        Frame frame_{};
        unsigned expected_ = 0;
        std::uint64_t startedAt_ = 0;
        std::uintptr_t previousDevice_ = 0;
        std::array<std::uint32_t, MaxContacts> activeIds_{};
        unsigned activeCount_ = 0;
        bool pending_ = false;
    };

} // namespace glide::touchpad
