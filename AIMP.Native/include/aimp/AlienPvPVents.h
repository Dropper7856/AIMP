#pragma once
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
namespace aimp::alien_pvp::vents {
using Point=std::array<float,3>;
using Reader=bool(*)(std::uintptr_t,void*,std::size_t) noexcept;
struct Table {std::uintptr_t manager{},entries{};std::uint32_t count{};};
inline bool ReadTable(std::uintptr_t alien,Reader read,Table& table) noexcept {
    std::uintptr_t adapter{};
    return alien && read(alien+0x34F0U,&adapter,sizeof(adapter)) && adapter &&
        read(adapter+0x24U,&table.manager,sizeof(table.manager)) && table.manager &&
        read(table.manager+8U,&table.entries,sizeof(table.entries)) && table.entries &&
        read(table.manager+12U,&table.count,sizeof(table.count)) && table.count<=4096U;
}
inline bool ReadPoint(const Table& table,std::uint32_t index,Reader read,Point& point) noexcept {
    std::uintptr_t descriptor{};
    if(index>=table.count || !read(table.entries+index*0x30U+0x28U,&descriptor,sizeof(descriptor)) || !descriptor ||
        !read(descriptor,point.data(),sizeof(point)))return false;
    for(float v:point)if(!std::isfinite(v) || std::abs(v)>100000.0F)return false;
    return true;
}
inline bool WithinReach(const Point& body,const Point& vent) noexcept {
    return std::hypot(body[0]-vent[0],body[2]-vent[2])<=2.75F &&
        vent[1]-body[1]>=-1.5F && vent[1]-body[1]<=4.5F;
}
} // namespace aimp::alien_pvp::vents
