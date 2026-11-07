#include "aimp/CharacterIncarnation.h"

namespace aimp::character_incarnations {
namespace {
Registry gNative;
bool Valid(std::uint64_t generation,std::uintptr_t character,std::uint32_t runtime) noexcept {
    return generation!=0U&&runtime!=0U&&character>=0x10000U&&character<=0xFFFFFFFFULL-0x50U;
}
void Put(std::span<std::uint8_t> bytes,std::size_t offset,std::uint64_t value,std::size_t count) noexcept {
    for(std::size_t i=0;i<count;++i)bytes[offset+i]=static_cast<std::uint8_t>(value>>(i*8U));
}
std::uint64_t Get(std::span<const std::uint8_t> bytes,std::size_t offset,std::size_t count) noexcept {
    std::uint64_t value{};for(std::size_t i=0;i<count;++i)value|=static_cast<std::uint64_t>(bytes[offset+i])<<(i*8U);return value;
}
bool LocalValid(const Ticket& ticket) noexcept {
    return ticket==Ticket{}||(Valid(ticket.generation,ticket.character,ticket.runtimeId)&&ticket.serial&&
        (ticket.origin==Origin::Created||ticket.origin==Origin::ExistingAtBinding));
}
}
Registry& NativeRegistry() noexcept{return gNative;}
bool EncodeLocalState(const Ticket& ticket,std::span<std::uint8_t> bytes) noexcept {
    if(bytes.size()!=kLocalStateBytes||!LocalValid(ticket))return false;
    Put(bytes,0,0x464C4941U,4);Put(bytes,4,1,4);Put(bytes,8,ticket.generation,8);Put(bytes,16,ticket.serial,8);
    Put(bytes,24,ticket.character,4);Put(bytes,28,ticket.runtimeId,4);Put(bytes,32,static_cast<std::uint8_t>(ticket.origin),4);
    Put(bytes,36,0,4);return true;
}
bool DecodeLocalState(std::span<const std::uint8_t> bytes,Ticket& ticket) noexcept {
    ticket={};
    if(bytes.size()!=kLocalStateBytes||Get(bytes,0,4)!=0x464C4941U||Get(bytes,4,4)!=1||Get(bytes,32,4)>1||Get(bytes,36,4))return false;
    const Ticket value{Get(bytes,8,8),Get(bytes,16,8),static_cast<std::uintptr_t>(Get(bytes,24,4)),
        static_cast<std::uint32_t>(Get(bytes,28,4)),static_cast<Origin>(Get(bytes,32,4))};
    if(!LocalValid(value))return false;
    ticket=value;
    return true;
}
Result Registry::WorldLocked(std::uint64_t generation) noexcept {
    if(!generation)return Result::Invalid;
    if(generation<generation_)return Result::WrongGeneration;
    if(generation>generation_){entries_={};local_={};generation_=generation;uncertain_=false;}
    return uncertain_?Result::Uncertain:Result::Ok;
}
Result Registry::AdvanceWorld(std::uint64_t generation) noexcept {
    const std::lock_guard lock(gate_);return WorldLocked(generation);
}
Result Registry::IssueLocked(std::uint64_t generation,std::uintptr_t character,std::uint32_t runtime,Origin origin,Ticket& out) noexcept {
    const auto world=WorldLocked(generation);if(world!=Result::Ok)return world;
    if(serial_==UINT64_MAX){uncertain_=true;return Result::Uncertain;}
    Entry* free{};
    for(auto& entry:entries_) {
        if(entry.ticket.character==character){free=&entry;break;}
        if(!entry.ticket.serial&&!free)free=&entry;
    }
    if(!free){uncertain_=true;return Result::Capacity;}
    free->ticket={generation,++serial_,character,runtime,origin};free->active=true;out=free->ticket;
    return Result::Ok;
}
Result Registry::Created(std::uint64_t generation,std::uintptr_t character,std::uint32_t runtime,Ticket& out) noexcept {
    out={};if(!Valid(generation,character,runtime))return Result::Invalid;
    const std::lock_guard lock(gate_);return IssueLocked(generation,character,runtime,Origin::Created,out);
}
Result Registry::BindExisting(std::uint64_t generation,std::uintptr_t character,std::uint32_t runtime,Ticket& out) noexcept {
    out={};if(!Valid(generation,character,runtime))return Result::Invalid;
    const std::lock_guard lock(gate_);
    const auto world=WorldLocked(generation);if(world!=Result::Ok)return world;
    for(const auto& entry:entries_)if(entry.active&&entry.ticket.character==character) {
        if(entry.ticket.runtimeId!=runtime){uncertain_=true;return Result::Uncertain;}
        out=entry.ticket;return Result::Duplicate;
    }
    // A binding request cannot resurrect a known retired address in this
    // world. Only a subsequently observed native construction may do so.
    for(const auto& entry:entries_)if(entry.ticket.character==character)return Result::Missing;
    return IssueLocked(generation,character,runtime,Origin::ExistingAtBinding,out);
}
Result Registry::Find(std::uint64_t generation,std::uintptr_t character,Ticket& out) const noexcept {
    out={};const std::lock_guard lock(gate_);
    if(generation!=generation_)return Result::WrongGeneration;
    if(uncertain_)return Result::Uncertain;
    for(const auto& entry:entries_)if(entry.active&&entry.ticket.character==character){out=entry.ticket;return Result::Ok;}
    return Result::Missing;
}
bool Registry::Current(const Ticket& ticket) const noexcept {
    Ticket current{};return ticket.serial!=0U&&Find(ticket.generation,ticket.character,current)==Result::Ok&&current==ticket;
}
Result Registry::PublishLocal(const Ticket& ticket) noexcept {
    const auto frozen=ticket;const std::lock_guard lock(gate_);
    if(frozen==Ticket{}){local_={};return Result::Ok;}
    if(frozen.generation!=generation_)return Result::WrongGeneration;
    if(uncertain_)return Result::Uncertain;
    for(const auto& entry:entries_)if(entry.active&&entry.ticket==frozen&&frozen.serial) {
        local_=frozen;return Result::Ok;
    }
    return Result::Missing;
}
Ticket Registry::Local() const noexcept {
    const std::lock_guard lock(gate_);
    if(!uncertain_&&local_.serial&&local_.generation==generation_)
        for(const auto& entry:entries_)if(entry.active&&entry.ticket==local_)return local_;
    return {};
}
Result Registry::Retire(const Ticket& ticket) noexcept {
    const auto frozen=ticket;const std::lock_guard lock(gate_);
    if(frozen.generation!=generation_)return Result::WrongGeneration;
    if(uncertain_)return Result::Uncertain;
    for(auto& entry:entries_)if(entry.ticket==frozen&&frozen.serial!=0U) {
        if(!entry.active)return Result::Duplicate;
        entry.active=false;return Result::Ok;
    }
    return Result::Missing;
}
}
