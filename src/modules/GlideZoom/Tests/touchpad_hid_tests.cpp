#include <stdexcept>
#include "HidTestApi.h"

namespace {
using namespace mock;
void NativeContractMatrix() {
    for(unsigned slots=1;slots<=5;++slots) for(unsigned reports:{1u,2u,64u}) for(unsigned id:{1u,2u,17u}) {
        ++cases;
        Standard(id,slots); auto packet=Input(id,reports); HidReader reader;
        const auto result=Read(reader,packet);
        Check(result.status==DecodeResult::Parsed && result.reports.size()==reports,"bounded batch decoded");
        for(unsigned r=0;r<reports;++r) {
            const auto& frame=result.reports[r];
            Check(frame.device==id && frame.total==slots && frame.slots==slots && frame.scan==100+r,"metadata decode exact");
            for(unsigned i=0;i<slots;++i) {
                const auto& contact=frame.contacts[i];
                Check(contact.valid && contact.tip && contact.confidence && contact.id==10+i,"contact ID and flags exact");
                Check(std::abs(contact.x-(0.2+i*0.1))<1e-9 && contact.y==0.5,"descriptor normalization exact");
            }
        }
        Check(devices[id].loads==1,"descriptor loaded once per batch");
    }
}
void RawFailures() {
    for(int fault=1;fault<=6;++fault) {
        ++cases; Standard(); auto packet=Input(); packet.fault=fault;
        Expect(DecodeResult::Invalid,packet,"raw query/read failure rejected");
    }
    for(unsigned fault=0;fault<10;++fault) {
        ++cases; Standard(); auto packet=Input();
        switch(fault) {
        case 0: packet.Field(offsetof(RAWINPUTHEADER,dwType),0); break;
        case 1: packet.Field(offsetof(RAWINPUTHEADER,dwSize),packet.bytes-1); break;
        case 2: packet.Field(sizeof(RAWINPUTHEADER),0); break;
        case 3: packet.Field(sizeof(RAWINPUTHEADER),4097); break;
        case 4: packet.Field(sizeof(RAWINPUTHEADER)+4,0); break;
        case 5: packet.Field(sizeof(RAWINPUTHEADER)+4,65); break;
        case 6: packet.Field(sizeof(RAWINPUTHEADER)+4,2); break;
        case 7: packet.Field(sizeof(RAWINPUTHEADER),devices[1].caps.InputReportByteLength-1); break;
        case 8: packet.DeviceHandle(nullptr); break;
        default: packet.Report()[0]=99; break;
        }
        Expect(fault==8?DecodeResult::Unsupported:fault==9?DecodeResult::Ignored:DecodeResult::Invalid,
               packet,"malformed/unknown raw header/report rejected or ignored");
    }
}
void DescriptorFailures() {
    for(unsigned fault=0;fault<39;++fault) {
        ++cases; auto& device=Standard(); auto packet=Input();
        switch(fault) {
        case 0: case 1: case 2: case 3: case 4: case 5: device.preparsedFault=static_cast<int>(fault)+1; break;
        case 6: device.capsFault=1; break;
        case 7: device.caps.UsagePage=1; break;
        case 8: device.caps.Usage=2; break;
        case 9: device.caps.InputReportByteLength=0; break;
        case 10: device.caps.InputReportByteLength=4097; break;
        case 11: device.caps.NumberInputValueCaps=0; break;
        case 12: device.caps.NumberInputValueCaps=257; break;
        case 13: device.caps.NumberInputButtonCaps=0; break;
        case 14: device.caps.NumberInputButtonCaps=129; break;
        case 15: device.valuesFault=1; break;
        case 16: device.valuesFault=2; break;
        case 17: device.buttonsFault=1; break;
        case 18: device.buttonsFault=2; break;
        case 19: device.values[4].IsAlias=true; break;
        case 20: device.values[4].IsAbsolute=false; break;
        case 21: device.values[4].BitSize=0; break;
        case 22: device.values[4].BitSize=33; break;
        case 23: device.values[4].ReportCount=2; break;
        case 24: device.values[4].LinkCollection=0; break;
        case 25: device.values[0].NotRange.Usage=0x99; break;
        case 26: device.values[1].NotRange.Usage=0x99; break;
        case 27: device.values[2].LogicalMax=device.values[2].LogicalMin; break;
        case 28: device.values[3].NotRange.Usage=0x99; break;
        case 29: device.buttons[0].NotRange.Usage=0x99; break;
        case 30: device.buttons[1].NotRange.Usage=0x99; break;
        case 31: device.values.push_back(device.values[4]); ++device.caps.NumberInputValueCaps; break;
        case 32: device.values[7].NotRange.DataIndex=device.values[4].NotRange.DataIndex; break;
        case 33:
            device.values[4].IsRange=true; device.values[4].Range={0x51,0x51,2,3}; break;
        case 34:
            device.values[2].IsRange=true; device.values[2].Range={0x30,0x31,10,9}; break;
        case 35: // This range plus the existing Y capability is ambiguous.
            device.values[2].IsRange=true; device.values[2].Range={0x30,0x31,3,4}; break;
        case 36:
            for(auto& value:device.values) if(value.UsagePage==0x0D && value.NotRange.Usage==0x51) value.NotRange.Usage=0x99;
            break;
        case 37: Standard(1,1); for(unsigned id=2;id<=17;++id) Format(device,static_cast<UCHAR>(id),1); break;
        default: Standard(1,6); break;
        }
        Expect(DecodeResult::Unsupported,packet,"descriptor API fault/ambiguous capability rejected");
    }
}
void PayloadFailuresAndCache() {
    for(unsigned fault=0;fault<9;++fault) {
        ++cases; auto& device=Standard(); auto packet=Input();
        switch(fault) {
        case 0: device.valueFault=1; break;
        case 1: device.usagesFault=1; break;
        case 2: device.usagesFault=2; break;
        case 3: packet.Report()[1]=6; break;
        case 4: Word(packet.Report()+5,1001); break;
        case 5: Word(packet.Report()+7,2001); break;
        case 6: packet.Report()[9]=1; break; // no Confidence
        case 7: packet.Report()[9]=2; break; // tip-up
        default: packet.Report()[device.caps.InputReportByteLength-1]=1; break;
        }
        HidReader reader; const auto result=Read(reader,packet);
        if(fault<=3) Check(result.status==DecodeResult::Invalid && result.reports.empty(),"top-level decoder error rejected");
        else {
            Check(result.status==DecodeResult::Parsed && result.reports.size()==1,"per-contact validation delegated explicitly");
            const auto& report=result.reports[0];
            if(fault==4 || fault==5) Check(!report.contacts[0].valid,"out-of-range contact is invalid");
            if(fault==6) Check(report.contacts[0].tip && !report.contacts[0].confidence,"palm confidence retained");
            if(fault==7) Check(!report.contacts[0].tip && report.contacts[0].confidence,"explicit tip-up retained");
            if(fault==8) Check(report.button,"physical button retained for cancellation");
        }
    }
    HidReader reader;
    for(unsigned id=1;id<=5;++id) { Standard(id); auto packet=Input(id); Check(Read(reader,packet).status==DecodeResult::Parsed,"cache fill"); }
    auto second=Input(2); Read(reader,second); Check(devices[2].loads==1,"cache hit does not reload descriptor");
    auto first=Input(1); Read(reader,first); Check(devices[1].loads==2,"bounded LRU eviction reloads oldest");
    reader.Removed(Handle(1)); Read(reader,first); Check(devices[1].loads==3,"device removal invalidates cache");
    reader.Reset(); Read(reader,first); Check(devices[1].loads==4,"reset invalidates cache");
    Standard(1).capsFault=1; reader.Reset();
    Check(Read(reader,first).status==DecodeResult::Unsupported,"unsupported descriptor cached");
    devices[1].capsFault=0;
    Check(Read(reader,first).status==DecodeResult::Unsupported && devices[1].loads==1,"negative cache does not repeatedly probe");
    reader.Removed(Handle(1));
    Check(Read(reader,first).status==DecodeResult::Parsed && devices[1].loads==2,"device-change permits descriptor recovery");
}
void FormatsAndComposition() {
    auto& device=Standard(); Format(device,2,3);
    auto packet=Input(1,2); packet.Report(1)[0]=2;
    HidReader reader; auto result=Read(reader,packet);
    Check(result.status==DecodeResult::Parsed && result.reports.size()==2,"two report IDs supported in one TLC batch");
    Standard(); reader.Reset(); packet=Input(); packet.Report()[1]=1;
    for(unsigned i=1;i<3;++i) Word(packet.Report()+4+i*6+1,65535);
    result=Read(reader,packet);
    Check(result.status==DecodeResult::Parsed && result.reports[0].contacts[0].valid && !result.reports[0].contacts[1].valid,
          "unused invalid parallel slots retained but do not poison valid slot");
    Assembler assembler;
    Check(assembler.Push(result.reports[0],1).complete,"only declared parallel slots assembled");

    auto& reordered=Standard(); reader.Reset();
    std::reverse(reordered.values.begin(),reordered.values.end());
    packet=Input(); packet.Report()[1]=1;
    for(unsigned i=1;i<3;++i) Word(packet.Report()+4+i*6+1,65535);
    result=Read(reader,packet);
    assembler.Reset();
    Check(result.status==DecodeResult::Parsed && assembler.Push(result.reports[0],1).complete &&
          result.reports[0].contacts[0].id==10,
          "capability enumeration order does not replace the wire's first valid contact slot");

    auto& ranged=Standard(1,1); reader.Reset();
    ranged.values[2].IsRange=true; ranged.values[2].Range={0x30,0x31,3,4};
    ranged.values[2].ReportCount=2;
    ranged.values.erase(ranged.values.begin()+3); --ranged.caps.NumberInputValueCaps;
    ranged.buttons[0].IsRange=true; ranged.buttons[0].Range={0x42,0x47};
    ranged.buttons.erase(ranged.buttons.begin()+1); --ranged.caps.NumberInputButtonCaps;
    packet=Input(); result=Read(reader,packet);
    Check(result.status==DecodeResult::Parsed && result.reports[0].contacts[0].valid &&
          result.reports[0].contacts[0].y==1 && result.reports[0].contacts[0].confidence,
          "bounded X/Y usage range and Tip/Confidence range decoded");

    for(unsigned slots:{1u,2u,3u,5u}) {
        ++cases; Standard(1,slots); reader.Reset(); Assembler frames; State state; state.SetEnabled(true);
        std::uint64_t now=1; unsigned scan=1;
        auto send=[&](unsigned total,bool tip,unsigned y) {
            const unsigned reports=total?(total+slots-1)/slots:1;
            auto input=Input(1,reports); Effect effect;
            for(unsigned r=0;r<reports;++r) {
                auto* data=input.Report(r); data[1]=static_cast<BYTE>(r?0:total); Word(data+2,scan);
                for(unsigned i=0;i<slots;++i) { data[4+i*6]=static_cast<BYTE>(10+r*slots+i); Word(data+7+i*6,y); data[9+i*6]=tip?3:2; }
            }
            const auto status=reader.Read(&input,[&](const Report& report) {
                const auto assembled=frames.Push(report,now);
                Check(!assembled.invalid,"native mock reports assemble");
                if(assembled.complete) effect=state.Push(assembled.frame,now);
            });
            Check(status==DecodeResult::Parsed,"native mock packet parsed"); ++scan; now+=10; return effect;
        };
        send(0,false,1000); send(3,true,1000); send(3,false,1000);
        Check(send(3,true,1000).begin,"HidP shim -> real decoder -> assembler -> exact held second tap");
        Check(std::abs(send(3,true,980).delta+10)<1e-9,"native decoded centroid produces fractional zoom delta");
        Check(send(3,false,980).end,"native explicit release ends gesture");
    }
}
} // namespace

int Run_touchpad_hid() {
    NativeContractMatrix(); RawFailures(); DescriptorFailures(); PayloadFailuresAndCache(); FormatsAndComposition();
    std::cout<<"PASS: "<<mock::assertions<<" native HID boundary assertions; "<<mock::cases<<" matrix cases\n";
    return 0;
}
