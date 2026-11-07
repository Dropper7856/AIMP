#pragma once
#include "aimp/AlienPvPWire.h"
#include <array>
#include <cstddef>
#include <cstdint>

namespace aimp::alien_pvp {
// Local IPC is reliable only up to its socket. The engine pump and managed
// worker both had bounded lossy queues. This journal bridges those queues.
// Stop-and-wait applies ONLY to discrete control data, not motion/pose traffic.
inline constexpr std::uint64_t kDeliveryRetryMs=75U;
inline constexpr std::uint64_t kDeliveryDeadlineMs=2000U;
inline bool Reliable(const Frame& f) noexcept {
    return f.kind==Kind::Activate || f.kind==Kind::Stop || f.kind==Kind::Resolution ||
        f.kind==Kind::ActionResult ||
        ((f.kind==Kind::Input || f.kind==Kind::Commit) && f.action!=Action::None);
}
inline Frame Acknowledgement(const Frame& f) noexcept {
    Frame ack{};ack.kind=Kind::Acknowledge;ack.scope=f.scope;ack.epoch=f.epoch;ack.deliveryId=f.deliveryId;return ack;
}
// An ACK means accepted into the scoped delivery stream, NEVER native entry,
// animation completion, damage, or a successful attack.
class DeliveryInbox final {
public:
    enum class Verdict { Next, Duplicate, Gap };
    Verdict Inspect(std::uint64_t id) const noexcept {
        if(!id || (id>>32U)!=(last_>>32U))return Verdict::Gap;
        if(id<=last_)return Verdict::Duplicate;
        return last_!=UINT64_MAX && id==last_+1U?Verdict::Next:Verdict::Gap;
    }
    bool Commit(std::uint64_t id) noexcept {
        if(Inspect(id)!=Verdict::Next)return false;
        last_=id;return true;
    }
    void Clear(std::uint64_t prefix=0U) noexcept {last_=prefix;}
private:
    std::uint64_t last_{};
};
template<std::size_t Capacity=64> class DeliveryOutbox final {
public:
    bool Push(Frame f,std::uint64_t now) noexcept {
        if(!Reliable(f) || !Valid(f) || count_==Capacity || (next_&UINT32_MAX)==UINT32_MAX)return false;
        f.deliveryId=++next_;
        items_[(read_+count_)%Capacity]={f,now,0U,false};++count_;return true;
    }
    bool Due(std::uint64_t now,Frame& f) noexcept {
        if(!count_)return false;
        auto& h=items_[read_];
        if(h.sent && now>=h.attempt && now-h.attempt<kDeliveryRetryMs)return false;
        h.attempt=now;h.sent=true;f=h.frame;return true;
    }
    bool Acknowledge(const Frame& f) noexcept {
        if(!count_ || f.kind!=Kind::Acknowledge)return false;
        const auto& h=items_[read_].frame;
        if(f.scope!=h.scope || f.epoch!=h.epoch || f.deliveryId!=h.deliveryId)return false;
        items_[read_]={};read_=(read_+1U)%Capacity;--count_;return true;
    }
    bool Expired(std::uint64_t now) const noexcept {
        return count_ && (now<items_[read_].created || now-items_[read_].created>kDeliveryDeadlineMs);
    }
    bool Has(Kind kind) const noexcept {
        for(std::size_t i=0;i<count_;++i)if(items_[(read_+i)%Capacity].frame.kind==kind)return true;
        return false;
    }
    std::size_t Size() const noexcept {return count_;}
    void Clear(std::uint64_t prefix=0U) noexcept {*this={};next_=prefix;}
private:
    static_assert(Capacity>0U);
    struct Entry {Frame frame{};std::uint64_t created{},attempt{};bool sent{};};
    std::array<Entry,Capacity> items_{};std::size_t read_{},count_{};std::uint64_t next_{};
};
} // namespace aimp::alien_pvp
