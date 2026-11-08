#pragma once

#include <stdexcept>
#include "../GlideZoom/touchpad_hid.h"
#include "../GlideZoom/touchpad_state.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <vector>

// These shims return controlled API contracts to the actual production reader.
// They do not reimplement Microsoft's binary HID descriptor parser or drivers.
namespace mock {
using namespace glide::touchpad;
constexpr UINT Error = static_cast<UINT>(-1);
constexpr std::size_t Offset = offsetof(RAWINPUT, data) + offsetof(RAWHID, bRawData);
std::uint64_t assertions=0, cases=0;
void Check(bool value,const char* message) {
    ++assertions;
    if(!value) { std::cerr<<"FAIL: "<<message<<'\n'; throw std::runtime_error("HID mock contract assertion failed"); }
}
struct Binding { USAGE page,usage; USHORT link; UCHAR report; unsigned offset,width; };
struct Device {
    HIDP_CAPS caps{};
    std::vector<HIDP_VALUE_CAPS> values;
    std::vector<HIDP_BUTTON_CAPS> buttons;
    std::vector<Binding> bindings;
    unsigned slots=0, loads=0, valueCalls=0;
    int preparsedFault=0, capsFault=0, valuesFault=0, buttonsFault=0, valueFault=0, usagesFault=0;
};
std::array<Device,20> devices{};
HANDLE Handle(unsigned id) { return reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(id)); }
Device& Get(HANDLE handle) {
    const auto id=reinterpret_cast<std::uintptr_t>(handle);
    Check(id>0 && id<devices.size(),"mock device handle valid");
    return devices[id];
}
Device& Get(PHIDP_PREPARSED_DATA data) {
    Check(data && data->device>0 && data->device<devices.size(),"mock preparsed token valid");
    return devices[data->device];
}
void Value(Device& device,USAGE page,USAGE usage,USHORT link,UCHAR report,unsigned offset,
           unsigned width,LONG maximum) {
    HIDP_VALUE_CAPS caps{};
    caps.UsagePage=page; caps.NotRange.Usage=usage; caps.LinkCollection=link; caps.ReportID=report;
    caps.BitSize=static_cast<USHORT>(width*8); caps.ReportCount=1; caps.IsAbsolute=true;
    caps.LogicalMin=0; caps.LogicalMax=maximum;
    caps.NotRange.DataIndex=static_cast<USHORT>((report-1)*32+
        (link?2+(link-1)*5+(usage==0x51?0:usage==0x30?1:2):usage==0x54?0:1));
    device.values.push_back(caps);
    device.bindings.push_back({page,usage,link,report,offset,width});
}
void Format(Device& device,UCHAR report,unsigned slots) {
    Value(device,0x0D,0x54,0,report,1,1,5);
    Value(device,0x0D,0x56,0,report,2,2,65535);
    for(unsigned i=0;i<slots;++i) {
        const auto link=static_cast<USHORT>(i+1);
        const unsigned base=4+i*6;
        // Deliberately put X/Y before ID in the capabilities array.
        Value(device,1,0x30,link,report,base+1,2,1000);
        Value(device,1,0x31,link,report,base+3,2,2000);
        Value(device,0x0D,0x51,link,report,base,1,255);
        for(USAGE usage : {USAGE{0x42},USAGE{0x47}}) {
            HIDP_BUTTON_CAPS button{};
            button.UsagePage=0x0D; button.NotRange.Usage=usage; button.LinkCollection=link; button.ReportID=report;
            device.buttons.push_back(button);
        }
    }
    HIDP_BUTTON_CAPS button{};
    button.UsagePage=9; button.NotRange.Usage=1; button.ReportID=report;
    device.buttons.push_back(button);
    device.caps.NumberInputValueCaps=static_cast<USHORT>(device.values.size());
    device.caps.NumberInputButtonCaps=static_cast<USHORT>(device.buttons.size());
}
Device& Standard(unsigned id=1,unsigned slots=3) {
    auto& device=devices[id];
    device={}; device.slots=slots;
    device.caps.UsagePage=0x0D; device.caps.Usage=5;
    device.caps.InputReportByteLength=static_cast<USHORT>(5+slots*6);
    Format(device,1,slots);
    return device;
}
struct Packet {
    std::vector<ULONG> words;
    UINT bytes=0;
    int fault=0;
    unsigned reads=0;
    unsigned char* Data() { return reinterpret_cast<unsigned char*>(words.data()); }
    unsigned char* Report(unsigned index=0) {
        DWORD size=0; std::memcpy(&size,Data()+sizeof(RAWINPUTHEADER),sizeof(size));
        return Data()+Offset+index*size;
    }
    void Field(std::size_t offset,DWORD value) { std::memcpy(Data()+offset,&value,sizeof(value)); }
    void DeviceHandle(HANDLE handle) { std::memcpy(Data()+offsetof(RAWINPUTHEADER,hDevice),&handle,sizeof(handle)); }
};
void Word(unsigned char* data,unsigned value) { data[0]=static_cast<BYTE>(value); data[1]=static_cast<BYTE>(value>>8); }
Packet Input(unsigned id=1,unsigned reports=1,UCHAR reportId=1) {
    const auto& device=devices[id];
    Packet packet;
    packet.bytes=static_cast<UINT>(Offset+device.caps.InputReportByteLength*reports);
    packet.words.resize((packet.bytes+3)/4);
    const RAWINPUTHEADER header{RIM_TYPEHID,packet.bytes,Handle(id),0};
    std::memcpy(packet.Data(),&header,sizeof(header));
    packet.Field(sizeof(RAWINPUTHEADER),device.caps.InputReportByteLength);
    packet.Field(sizeof(RAWINPUTHEADER)+sizeof(DWORD),reports);
    for(unsigned r=0;r<reports;++r) {
        auto* data=packet.Report(r);
        data[0]=reportId; data[1]=static_cast<BYTE>(device.slots); Word(data+2,100+r);
        for(unsigned i=0;i<device.slots;++i) {
            const unsigned base=4+6*i;
            data[base]=static_cast<BYTE>(10+i); Word(data+base+1,200+i*100); Word(data+base+3,1000);
            data[base+5]=3; // Tip + Confidence.
        }
    }
    return packet;
}
struct Result { DecodeResult status; std::vector<Report> reports; };
Result Read(HidReader& reader,Packet& packet) {
    Result result{};
    result.status=reader.Read(&packet,[&](const Report& report){ result.reports.push_back(report); });
    return result;
}
void Expect(DecodeResult expected,Packet& packet,const char* message) {
    HidReader reader;
    const auto result=Read(reader,packet);
    Check(result.status==expected,message);
    if(expected!=DecodeResult::Parsed) Check(result.reports.empty(),"rejected packet emits no contacts");
}
} // namespace mock

UINT GetRawInputData(HRAWINPUT handle,UINT command,void* output,UINT* bytes,UINT headerSize) {
    using namespace mock;
    Check(command==RID_INPUT && headerSize==sizeof(RAWINPUTHEADER),"raw API contract arguments");
    auto& packet=*static_cast<Packet*>(handle);
    if(!output) {
        if(packet.fault==1) return Error;
        *bytes=packet.fault==2?static_cast<UINT>(Offset-1):packet.fault==3?65537:packet.bytes;
        return 0;
    }
    ++packet.reads;
    if(packet.fault==4 || *bytes<packet.bytes) return Error;
    std::memcpy(output,packet.words.data(),packet.bytes);
    if(packet.fault==5) return packet.bytes-1;
    if(packet.fault==6) --*bytes;
    return packet.bytes;
}
UINT GetRawInputDeviceInfoW(HANDLE handle,UINT command,void* output,UINT* bytes) {
    using namespace mock;
    Check(command==RIDI_PREPARSEDDATA,"only read-only preparsed-data query used");
    auto& device=Get(handle);
    if(!output) {
        ++device.loads;
        if(device.preparsedFault==1) return Error;
        *bytes=device.preparsedFault==2?0:device.preparsedFault==3?65537:sizeof(_HIDP_PREPARSED_DATA);
        return 0;
    }
    if(device.preparsedFault==4) return Error;
    const _HIDP_PREPARSED_DATA token{static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(handle))};
    std::memcpy(output,&token,sizeof(token));
    if(device.preparsedFault==5) return sizeof(token)-1;
    if(device.preparsedFault==6) --*bytes;
    return sizeof(token);
}
NTSTATUS HidP_GetCaps(PHIDP_PREPARSED_DATA data,HIDP_CAPS* caps) {
    auto& device=mock::Get(data);
    if(device.capsFault) return HIDP_STATUS_FAILURE;
    *caps=device.caps; return HIDP_STATUS_SUCCESS;
}
NTSTATUS HidP_GetValueCaps(HIDP_REPORT_TYPE type,HIDP_VALUE_CAPS* values,USHORT* count,PHIDP_PREPARSED_DATA data) {
    auto& device=mock::Get(data);
    mock::Check(type==HidP_Input,"value caps read input only");
    if(device.valuesFault==1) return HIDP_STATUS_FAILURE;
    if(device.valuesFault==2) { *count=257; return HIDP_STATUS_SUCCESS; }
    if(*count<device.values.size()) return HIDP_STATUS_FAILURE;
    std::copy(device.values.begin(),device.values.end(),values);
    *count=static_cast<USHORT>(device.values.size()); return HIDP_STATUS_SUCCESS;
}
NTSTATUS HidP_GetButtonCaps(HIDP_REPORT_TYPE type,HIDP_BUTTON_CAPS* buttons,USHORT* count,PHIDP_PREPARSED_DATA data) {
    auto& device=mock::Get(data);
    mock::Check(type==HidP_Input,"button caps read input only");
    if(device.buttonsFault==1) return HIDP_STATUS_FAILURE;
    if(device.buttonsFault==2) { *count=129; return HIDP_STATUS_SUCCESS; }
    if(*count<device.buttons.size()) return HIDP_STATUS_FAILURE;
    std::copy(device.buttons.begin(),device.buttons.end(),buttons);
    *count=static_cast<USHORT>(device.buttons.size()); return HIDP_STATUS_SUCCESS;
}
NTSTATUS HidP_GetUsageValue(HIDP_REPORT_TYPE type,USAGE page,USHORT link,USAGE usage,ULONG* value,
                          PHIDP_PREPARSED_DATA data,PCHAR report,ULONG bytes) {
    auto& device=mock::Get(data);
    ++device.valueCalls;
    mock::Check(type==HidP_Input && bytes==device.caps.InputReportByteLength,"value read size/type contract");
    if(device.valueFault) return HIDP_STATUS_FAILURE;
    for(const auto& binding:device.bindings) if(binding.page==page && binding.usage==usage &&
        binding.link==link && binding.report==static_cast<UCHAR>(report[0])) {
        mock::Check(binding.offset+binding.width<=bytes,"mock binding within report");
        *value=0;
        for(unsigned i=0;i<binding.width;++i) *value|=static_cast<ULONG>(static_cast<BYTE>(report[binding.offset+i]))<<(8*i);
        return HIDP_STATUS_SUCCESS;
    }
    return HIDP_STATUS_FAILURE;
}
NTSTATUS HidP_GetUsages(HIDP_REPORT_TYPE type,USAGE page,USHORT link,USAGE* usages,ULONG* count,
                      PHIDP_PREPARSED_DATA data,PCHAR report,ULONG bytes) {
    auto& device=mock::Get(data);
    mock::Check(type==HidP_Input && bytes==device.caps.InputReportByteLength && *count==128,
                "button read bounded input contract");
    if(device.usagesFault==1) return HIDP_STATUS_FAILURE;
    if(device.usagesFault==2) { *count=129; return HIDP_STATUS_SUCCESS; }
    *count=0;
    if(page==9 && !link) {
        if(report[bytes-1]&1) usages[(*count)++]=1;
    } else if(page==0x0D && link && link<=device.slots) {
        const auto flags=static_cast<BYTE>(report[4+(link-1)*6+5]);
        if(flags&1) usages[(*count)++]=0x42;
        if(flags&2) usages[(*count)++]=0x47;
    } else return HIDP_STATUS_FAILURE;
    return HIDP_STATUS_SUCCESS;
}

