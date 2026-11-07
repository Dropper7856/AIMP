#pragma once
#include "aimp/AlienPvPWire.h"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace aimp::alien_pvp {
inline constexpr std::uint64_t kInputTimeoutMs=300U;
inline constexpr std::uint64_t kAuthorityTimeoutMs=750U;
inline constexpr std::uint64_t kMaximumQueueAgeMs=250U;
inline constexpr bool Fresh(std::uint64_t now,std::uint64_t then,std::uint64_t limit) noexcept {
    return now>=then && now-then<=limit;
}
// One incarnation, one Host-issued epoch, one monotonic stream. A repeated
// packet never renews a deadman's timer. Explicit revocation is irreversible
// inside its epoch; a new readiness barrier is required to start again.
class InputWindow final {
public:
    void Reset(const Scope& scope={}) noexcept { *this=InputWindow{};scope_=scope; }
    bool Accept(const Frame& frame,std::uint64_t receivedAt,std::uint64_t now) noexcept {
        if(!Valid(frame)||frame.scope!=scope_ || !Fresh(now,receivedAt,kMaximumQueueAgeMs))return false;
        if(frame.kind==Kind::Activate) {
            if(frame.epoch<=revoked_ || frame.epoch<epoch_)return false;
            if(frame.epoch!=epoch_){input_={};actions_={};lastHost_=lastInput_=lastPose_=lastPoseHost_=lastPoseInput_=0U;epoch_=frame.epoch;}
            active_=true;authorityAt_=receivedAt;return true;
        }
        if(frame.kind==Kind::Stop) {
            if(frame.epoch<epoch_)return false;
            revoked_=std::max(revoked_,frame.epoch);active_=false;return true;
        }
        if(!active_ || frame.epoch!=epoch_ || frame.epoch<=revoked_)return false;
        if(frame.kind==Kind::Commit) {
            if(frame.hostSequence<=lastHost_ || frame.sequence<=lastInput_)return false;
            if(frame.action!=Action::None && frame.action!=Action::Roar) {
                ActionEntry* slot{};
                for(auto& entry:actions_) {
                    if(!entry.command.hostSequence){slot=&entry;break;}
                    if(entry.last.phase!=ActionPhase::None && entry.last.phase!=ActionPhase::Started && entry.last.phase!=ActionPhase::Preparing &&
                        (!slot || entry.command.hostSequence<slot->command.hostSequence))slot=&entry;
                }
                if(!slot)return false;
                *slot={frame,{}};
            }
            lastHost_=frame.hostSequence;lastInput_=frame.sequence;input_=frame;
            inputAt_=receivedAt;authorityAt_=receivedAt;return true;
        }
        if(frame.kind==Kind::Resolution) {
            for(auto& entry:actions_)if(entry.command.hostSequence==frame.hostSequence &&
                entry.command.sequence==frame.sequence && entry.command.action==frame.action) {
                const auto& old=entry.last;
                if(frame.resolutionRevision!=old.resolutionRevision+1U)return false;
                if(old.phase==ActionPhase::None) {
                    if(frame.phase!=ActionPhase::Preparing && frame.phase!=ActionPhase::Started && frame.phase!=ActionPhase::Rejected && frame.phase!=ActionPhase::Failed)return false;
                } else if(old.phase==ActionPhase::Preparing || old.phase==ActionPhase::Started) {
                    if(old.phase==ActionPhase::Preparing && frame.phase==ActionPhase::Started) {
                        // Native availability has finished; the same target enters the pair.
                    } else
                    if(frame.phase!=ActionPhase::Completed && frame.phase!=ActionPhase::Failed && frame.phase!=ActionPhase::Cancelled && frame.phase!=ActionPhase::Rejected)return false;
                    if((old.phase==ActionPhase::Preparing && frame.phase==ActionPhase::Completed) ||
                        (old.phase==ActionPhase::Started && frame.phase==ActionPhase::Rejected))return false;
                    if(frame.targetPeer!=old.targetPeer || frame.targetA!=old.targetA || frame.targetB!=old.targetB ||
                        (old.phase!=ActionPhase::Preparing && frame.selector!=old.selector) || frame.x!=old.x || frame.y!=old.y || frame.z!=old.z ||
                        frame.resolvedX!=old.resolvedX || frame.resolvedY!=old.resolvedY || frame.resolvedZ!=old.resolvedZ)return false;
                } else return false;
                entry.last=frame;return true;
            }
            return false;
        }
        if(frame.kind==Kind::Correction) {
            if(frame.sequence<=lastPose_ || frame.hostSequence<lastPoseHost_ ||
                frame.acknowledgedInput<lastPoseInput_)return false;
            lastPose_=frame.sequence;lastPoseHost_=frame.hostSequence;
            lastPoseInput_=frame.acknowledgedInput;authorityAt_=receivedAt;return true;
        }
        return false;
    }
    void AcknowledgeLocalResolution(const Frame& frame,std::uint64_t now) noexcept {
        static_cast<void>(Accept(frame,now,now));
    }
    [[nodiscard]] bool Granted() const noexcept { return active_; }
    [[nodiscard]] bool ActiveAt(std::uint64_t now) const noexcept {
        return active_ && Fresh(now,authorityAt_,kAuthorityTimeoutMs);
    }
    [[nodiscard]] bool InputFresh(std::uint64_t now) const noexcept {
        return ActiveAt(now) && lastInput_ && Fresh(now,inputAt_,kInputTimeoutMs);
    }
    [[nodiscard]] const Frame& Input() const noexcept { return input_; }
    [[nodiscard]] std::uint64_t Epoch() const noexcept { return epoch_; }
    [[nodiscard]] std::uint64_t LastHost() const noexcept { return lastHost_; }
    [[nodiscard]] std::uint64_t LastInput() const noexcept { return lastInput_; }
private:
    struct ActionEntry final {Frame command{},last{};};
    std::array<ActionEntry,64> actions_{};
    Scope scope_{};Frame input_{};
    std::uint64_t epoch_{},revoked_{},lastHost_{},lastInput_{},lastPose_{},lastPoseHost_{},lastPoseInput_{};
    std::uint64_t inputAt_{},authorityAt_{};
    bool active_{};
};

// Fixed-capacity FIFO. Consecutive steering samples may coalesce, never
// discrete actions, Stop/Activate, or different incarnations/epochs.
struct TimedFrame final { Frame frame{};std::uint64_t at{}; };
template<std::size_t Capacity> class Mailbox final {
public:
    bool Push(const TimedFrame& value) noexcept {
        if(count_) {
            const auto i=(read_+count_-1U)%Capacity;
            const auto& previous=items_[i].frame;
            const auto& next=value.frame;
            const bool stream=next.kind==Kind::Input || next.kind==Kind::Commit ||
                next.kind==Kind::Pose || next.kind==Kind::Correction || next.kind==Kind::Status;
            if(stream && previous.kind==next.kind && previous.scope==next.scope &&
                previous.epoch==next.epoch && previous.action==Action::None && next.action==Action::None &&
                next.sequence>=previous.sequence && next.hostSequence>=previous.hostSequence) {
                items_[i]=value;return true;
            }
        }
        if(count_==Capacity)return false;
        items_[(read_+count_)%Capacity]=value;++count_;return true;
    }
    bool Pop(TimedFrame& out) noexcept {
        if(!count_)return false;
        out=items_[read_];read_=(read_+1U)%Capacity;--count_;return true;
    }
    void Clear() noexcept { read_=count_=0U; }
    [[nodiscard]] std::size_t Size() const noexcept { return count_; }
private:
    static_assert(Capacity>0U);
    std::array<TimedFrame,Capacity> items_{};
    std::size_t read_{},count_{};
};

// A pose at send time is paired with the input sequence. Client prediction is
// reconciled against the acknowledged sample, not against the current body:
// otherwise every round trip would pull the Alien backward by its latency.
class PredictionHistory final {
public:
    void Clear() noexcept { samples_={};next_=0U; }
    void Record(std::uint64_t sequence,float x,float y,float z) noexcept {
        samples_[next_]={sequence,x,y,z};next_=(next_+1U)%samples_.size();
    }
    bool Error(std::uint64_t acknowledged,float x,float y,float z,
        float& dx,float& dz) const noexcept {
        dx=dz=0.0F;
        if(!acknowledged)return false;
        for(const auto& s:samples_)if(s.sequence==acknowledged) {
            if(std::abs(y-s.y)>1.0F)return false;
            dx=x-s.x;dz=z-s.z;
            return std::isfinite(dx)&&std::isfinite(dz);
        }
        return false;
    }
private:
    struct Sample {std::uint64_t sequence{};float x{},y{},z{};};
    std::array<Sample,128> samples_{};std::size_t next_{};
};
inline float WrapYaw(float yaw) noexcept {
    return std::remainder(yaw,2.0F*kPi);
}
inline float ApproachYaw(float current,float wanted,float dt) noexcept {
    return WrapYaw(current+std::clamp(WrapYaw(wanted-current),-8.0F*dt,8.0F*dt));
}
} // namespace aimp::alien_pvp
