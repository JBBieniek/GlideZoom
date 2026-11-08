#include <stdexcept>
#include "../GlideZoom/touchpad_packets.h"
#include "../GlideZoom/touchpad_state.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>

namespace {
using namespace glide::touchpad;
std::uint64_t assertions=0, matrix=0;
void Check(bool value, const char* message) {
    ++assertions;
    if (!value) { std::cerr << "FAIL: " << message << '\n'; throw std::runtime_error("GlideZoom assertion failed; see captured output"); }
}
Contact Finger(unsigned id) { return {id, 0.2+id*0.02, 0.5, true, true, true}; }
Report Packet(unsigned count, unsigned slots, unsigned start, std::uint16_t scan=10, std::uintptr_t device=1) {
    Report report;
    report.device=device; report.scan=scan; report.total=count; report.slots=slots;
    for (unsigned i=0;i<slots && i<MaxContacts;++i) report.contacts[i]=Finger(start+i);
    return report;
}

void Bounds() {
    Check(ValidBatch(64,32,8,4), "exact raw HID payload fits");
    Check(ValidBatch(72,32,8,4), "kernel alignment padding permitted");
    for (std::size_t bytes=0; bytes<256; ++bytes) for (std::size_t offset : {0u,8u,24u,32u,128u,256u})
    for (std::size_t size : {0u,1u,4u,16u,4096u,4097u}) for (std::size_t count : {0u,1u,2u,64u,65u}) {
        ++matrix;
        const bool expected=size>0 && size<=4096 && count>0 && count<=64 && offset<=bytes && size*count<=bytes-offset;
        Check(ValidBatch(bytes,offset,size,count)==expected, "variable raw packet bounds matrix");
    }
    Check(!ValidBatch(std::numeric_limits<std::size_t>::max(),32,8,1), "oversized raw input rejected");
    Check(!ValidBatch(64,32,std::numeric_limits<std::size_t>::max(),1), "size overflow rejected");
    Check(!ValidBatch(64,32,1,std::numeric_limits<std::size_t>::max()), "count overflow rejected");
    double value=0;
    Check(Normalize(0,12,0,4095,value) && value==0, "minimum normalization");
    Check(Normalize(4095,12,0,4095,value) && value==1, "maximum normalization");
    Check(Normalize(0x8000,16,-32768,32767,value) && value==0, "negative signed minimum");
    Check(Normalize(0x7FFF,16,-32768,32767,value) && value==1, "positive signed maximum");
    Check(Normalize(0x80000000u,32,std::numeric_limits<std::int32_t>::min(),std::numeric_limits<std::int32_t>::max(),value) && value==0,
          "32-bit sign extension avoids shift overflow");
    for (unsigned bits=0;bits<=33;++bits) for (std::uint32_t raw : {0u,1u,255u,4095u,0xFFFFFFFFu}) {
        const bool good=Normalize(raw,bits,0,4095,value);
        if (good) Check(std::isfinite(value) && value>=0 && value<=1, "normalized values always bounded");
        if (!bits || bits>32) Check(!good, "invalid descriptor bit sizes rejected");
    }
    Check(!Normalize(1,8,100,100,value), "zero logical span rejected");
    Check(!Normalize(1,8,200,100,value), "reversed logical bounds rejected");
    Check(!Normalize(256,8,0,300,value), "bits outside declared width rejected");
}

void AssemblyMatrix() {
    for (unsigned total=1;total<=5;++total) for (unsigned slots=1;slots<=5;++slots)
    for (const std::uint16_t scan : std::array<std::uint16_t, 3>{0, 1, 65535})
    for (std::uintptr_t device : {1u,2u,0xABCDEFu})
    for (bool reverse : {false,true}) {
        ++matrix;
        Assembler assembler;
        unsigned sent=0;
        AssemblyResult result;
        while (sent<total) {
            auto report=Packet(sent==0?total:0,slots,sent+1,scan,device);
            const unsigned take=std::min(slots,total-sent);
            if (reverse) std::reverse(report.contacts.begin(),report.contacts.begin()+take);
            // Unused slots may contain arbitrary invalid data; Count owns validity.
            for (unsigned i=take;i<slots;++i) report.contacts[i].valid=false;
            result=assembler.Push(report,100+sent);
            sent+=take;
            Check(!result.invalid && result.complete==(sent==total), "parallel/hybrid completes only at total");
            Check(assembler.Invariant(), "assembler invariant under hybrid sequence");
        }
        Check(result.frame.count==total && result.frame.device==device && result.frame.scan==scan,
              "assembled metadata matches one device/frame");
        bool ids[6]{};
        for (unsigned i=0;i<total;++i) ids[result.frame.contacts[i].id]=true;
        for (unsigned id=1;id<=total;++id) Check(ids[id], "every contact carried exactly once");
        Check(!assembler.Pending(), "completed frame leaves no pending assembly");
    }
}

void Faults() {
    for (unsigned fault=0;fault<12;++fault) {
        ++matrix;
        Assembler assembler;
        Check(!assembler.Push(Packet(3,1,1),100).complete, "partial frame retained until complete");
        auto report=Packet(0,1,2);
        std::uint64_t now=101;
        switch(fault) {
        case 0: report.device=2; break;
        case 1: report.scan=11; break;
        case 2: report.total=3; break;
        case 3: report.total=6; break;
        case 4: report.slots=0; break;
        case 5: report.slots=6; break;
        case 6: report.contacts[0].id=1; break;
        case 7: report.contacts[0].valid=false; break;
        case 8: report.contacts[0].x=std::numeric_limits<double>::quiet_NaN(); break;
        case 9: report.contacts[0].y=1.01; break;
        case 10: now=151; break;
        default: now=99; break;
        }
        const auto result=assembler.Push(report,now);
        Check(result.invalid && !result.complete && !assembler.Pending() && assembler.Invariant(),
              "malformed/interleaved/incomplete input cancels pending frame");
    }
    Assembler assembler;
    assembler.Push(Packet(3,1,1),10);
    Check(!assembler.Expire(60) && assembler.Expire(61), "hybrid timeout is bounded and inclusive");
    auto up=Packet(3,3,1,65535);
    for(auto& contact:up.contacts) contact.tip=false;
    auto result=assembler.Push(up,100);
    Check(result.complete && result.frame.count==3 && !result.frame.contacts[0].tip,
          "tip-up contact count preserved for recognizer");
    result=assembler.Push(Packet(3,3,1,0),110);
    Check(result.complete && !result.invalid, "scan-time rollover accepted between complete frames");
    auto empty=Packet(0,5,1,1);
    for(auto& contact:empty.contacts) contact.tip=false;
    result=assembler.Push(empty,120);
    Check(result.invalid, "count-zero cannot silently remove previously down contacts");
    result=assembler.Push(up,130);
    Check(result.complete, "explicit tip-up records can establish all-up after reset");
    result=assembler.Push(empty,140);
    Check(result.complete && result.frame.count==0, "validated empty frame accepted after explicit release");
    empty.contacts[0].valid=false;
    Check(assembler.Push(empty,150).invalid, "invalid zero-count slots cannot manufacture an all-up boundary");
}

void Composition() {
    for(unsigned slots=1;slots<=5;++slots) {
        Assembler assembler;
        State state;
        state.SetEnabled(true);
        std::uint64_t now=1;
        std::uint16_t scan=0;
        auto send=[&](unsigned count,double y,bool tip=true) {
            Effect effect;
            unsigned sent=0;
            do {
                auto report=Packet(sent==0?count:0,slots,sent+1,scan);
                for(auto& contact:report.contacts) { contact.y=y; contact.tip=count!=0 && tip; }
                const auto result=assembler.Push(report,now);
                Check(!result.invalid,"composed hybrid report valid");
                if(result.complete) effect=state.Push(result.frame,now);
                sent+=std::min(slots,count-sent);
            } while(sent<count);
            now+=10; ++scan;
            return effect;
        };
        send(0,0.5); send(3,0.5); send(3,0.5,false);
        Check(send(3,0.5).begin,"hybrid frames activate exact second touchdown");
        const auto move=send(3,0.49);
        Check(std::abs(move.delta+10)<1e-9,"hybrid frames drive proportional zoom");
        Check(send(3,0.49,false).end,"hybrid explicit tip-up records end hold");
        Check(state.Invariant() && assembler.Invariant(),"packet+recognizer composition invariant");
    }
}

void LostHybridFirstReport() {
    // Exercise the actual packet assembler + recognizer. When the nonzero-count
    // first report is lost, a count-zero continuation MUST NOT become a tap lift.
    for (unsigned slots : {1u,2u}) for (bool tip : {false,true}) for (bool holding : {false,true}) {
        ++matrix;
        Assembler assembler;
        State state;
        state.SetEnabled(true);
        std::uint64_t now=1;
        std::uint16_t scan=1;
        auto process=[&](const Report& report) {
            const auto result=assembler.Push(report,now++);
            if(result.invalid) return state.Cancel();
            return result.complete ? state.Push(result.frame,now) : Effect{};
        };
        auto full=[&](bool down) {
            Effect effect;
            for(unsigned sent=0;sent<3;sent+=slots) {
                auto report=Packet(sent==0?3:0,slots,sent+1,scan);
                for(auto& contact:report.contacts) contact.tip=down;
                effect=process(report);
            }
            ++scan;
            return effect;
        };
        auto empty=Packet(0,slots,1,scan++);
        for(auto& contact:empty.contacts) contact.tip=false;
        process(empty);
        Check(!full(true).begin && state.Current()==State::Phase::First,"first physical tap tracked");
        if(holding) {
            full(false);
            Check(full(true).begin,"real explicit release permits second held touchdown");
        }
        auto orphan=Packet(0,slots,1,scan++);
        for(auto& contact:orphan.contacts) contact.tip=tip;
        const auto effect=process(orphan);
        Check(!effect.begin && !state.Active() && state.Current()==State::Phase::Blocked,
              "orphan continuation cannot fabricate all-up whether its slots are tip-up or down");
        Check(effect.end==holding && (!holding || effect.cancel),"orphan cancels an existing hold");
        Check(!full(true).begin && !state.Active(),"following held frame cannot start without real tap pair");
        Check(state.Invariant() && assembler.Invariant(),"lost-first-report composition invariant");
    }

    Assembler assembler;
    Check(assembler.Push(Packet(3,3,1),1).complete,"track three down contacts");
    auto partialUp=Packet(3,3,1,11);
    partialUp.contacts[0].tip=false;
    Check(assembler.Push(partialUp,2).complete,"explicit staggered first release accepted");
    Check(assembler.Push(Packet(2,2,2,12),3).complete,"previously released ID may disappear");
    Check(assembler.Push(Packet(1,1,3,13),4).invalid,"unreleased disappearing ID rejected");
}
} // namespace

int Run_touchpad_packets() {
    Bounds(); AssemblyMatrix(); Faults(); Composition(); LostHybridFirstReport();
    std::cout<<"PASS: "<<assertions<<" touchpad packet assertions; "<<matrix<<" matrix cases\n";
    return 0;
}
