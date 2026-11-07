// AI-Playable-Alien control.cpp::MainTick / ActionMoveInDirection::Start.
// MIT (c) 2026 pirletik; see third_party/AI-Playable-Alien/LICENSE.
// Included in pvp_actions after Live(). Reuse AIMP's typed native request
// adapter for the donor's kind9 request and its strong Ref lifetime.
#ifndef AIMP_ALIEN_ACTION_TEST
struct DonorMovement final {
    std::unique_ptr<execution::NativeRequestAdapter> adapter{};
    ap::Context context{};
    transactions::Record command{};
    bool used{},fault{};
};
std::array<DonorMovement,4> donorMovements{};
bool DonorPermission(void* opaque,const transactions::Record& r) noexcept {
    const auto& d=*static_cast<DonorMovement*>(opaque);
    const bool cleanup=r.opcode==transactions::Op::Cancel || r.opcode==transactions::Op::Release;
    return Live(d.context) && r.scope==d.command.scope && r.owner==d.command.owner &&
        r.commandId==d.command.commandId && r.nativeKind==9U &&
        (cleanup || (PvPLeaseScopeCurrent() && !gPvPLease.releasing));
}
bool DonorResolve(void* opaque,const transactions::OwnerKey& owner,execution::LocalActorBinding& actor) noexcept {
    const auto& d=*static_cast<DonorMovement*>(opaque);
    if(!Live(d.context) || owner!=d.command.owner)return false;
    actor={d.context.character,gPvPLease.runtimeId,true,gPvPLease.ticket.serial};return true;
}
DonorMovement* DonorFor(const ap::Context& c) noexcept {
    for(auto& d:donorMovements)
        if(d.used && d.context.character==c.character && d.context.scope==c.scope)return &d;
    return nullptr;
}
bool StopDonor(const ap::Context& c) noexcept {
    auto* d=DonorFor(c);if(!d)return true;
    if(!Live(c) || d->fault)return false; // retain strong Ref on unknown lifetime
    auto cleanup=d->command;
    cleanup.flags=cleanup.mode=cleanup.gait=0U;
    cleanup.goalX=cleanup.goalY=cleanup.goalZ=cleanup.distance=cleanup.stopDistance=cleanup.aux=0.0F;
    cleanup.opcode=transactions::Op::Cancel;
    if(d->adapter->Invoke(cleanup)!=transactions::NativeOutcome::Admitted)return false;
    cleanup.opcode=transactions::Op::Release;
    if(d->adapter->Invoke(cleanup)!=transactions::NativeOutcome::Admitted)return false;
    d->used=false;return true;
}
bool DonorGait(const ap::Context& c,std::uint32_t gait) noexcept {
    // Exact writes performed by donor ActionMoveInDirection::Start; do not
    // force effective gait +198 or bypass the native transition animation.
    AIMP_ACTION_TRY {
        const auto loco=*reinterpret_cast<const std::uintptr_t*>(c.character+0x42A8U);
        if(!loco || gait>3U)return false;
        auto& requested=*reinterpret_cast<std::uint32_t*>(loco+0x194U);
        if(requested!=gait){requested=gait;*reinterpret_cast<std::uint32_t*>(loco+0x190U)=0U;}
        return true;
    } AIMP_ACTION_CATCH {return false;}
}
bool DriveDonor(const ap::Context& c,const ap::Frame* input) noexcept {
    if(!input || (input->forward==0.0F && input->strafe==0.0F))return StopDonor(c);
    if(!Live(c) || !PvPLeaseScopeCurrent() || gPvPLease.releasing || input->gait>3U ||
        !std::isfinite(input->yaw) || !std::isfinite(input->forward) || !std::isfinite(input->strafe) ||
        std::abs(input->forward)>1.0F || std::abs(input->strafe)>1.0F)return false;
    auto* d=DonorFor(c);
    if(!d) {
        for(auto& candidate:donorMovements)
            if(!candidate.used && !candidate.fault && (!candidate.adapter || candidate.adapter->ActiveCount()==0U)){d=&candidate;break;}
        if(!d)return false;
        d->context=c;d->command={};
        auto& r=d->command;
        std::memcpy(r.scope.session.data(),&c.scope.cookie,sizeof(c.scope.cookie));
        r.scope.authorityEpoch=c.scope.ownerPeer;r.scope.worldEpoch=c.scope.worldGeneration;
        r.scope.consumerEpoch=c.scope.nativeGeneration;
        r.owner={c.scope.entityA,c.scope.entityB,gPvPLease.runtimeId,1U,
            transactions::Capability::Movement,c.scope.incarnation};
        r.commandId=r.globalSeq=r.ownerSeq=1U;r.parameterRevision=1U;r.nativeKind=9U;
        if(!d->adapter)d->adapter=std::unique_ptr<execution::NativeRequestAdapter>(new(std::nothrow) execution::NativeRequestAdapter);
        if(!d->adapter || !d->adapter->Initialize(c.base,true,&DonorPermission,&DonorResolve,d))return false;
    }
    if(d->fault)return false;
    if(!DonorGait(c,input->gait))return false;
    auto& r=d->command;
    r.opcode=d->used?transactions::Op::Update:transactions::Op::Submit;
    if(d->used){++r.parameterRevision;++r.globalSeq;++r.ownerSeq;}
    // Donor ReadInput worldYaw; native request +20 is pi/2 (not a speed),
    // +24 is refreshed to 10 metres and +34 rebases at the current body.
    r.aux=ap::WrapYaw(input->yaw+std::atan2(input->strafe,input->forward));
    r.distance=1.57079632679F;r.stopDistance=10.0F;
    d->used=true; // retain a partial Submit rather than resubmit it
    const auto outcome=d->adapter->Invoke(r);
    d->fault=outcome==transactions::NativeOutcome::Unknown;
    // A resource check can defer before the factory runs. It created no
    // retained request, so the next fresh input must retry Submit, not Update.
    if(!d->fault && d->adapter->ActiveCount()==0U)d->used=false;
    return outcome==transactions::NativeOutcome::Admitted;
}
void DonorDestroyed(std::uintptr_t character,std::uint32_t runtime,std::uint64_t serial,std::uint32_t generation) noexcept {
    for(auto& d:donorMovements) {
        if(!d.used || !d.adapter || d.context.character!=character || d.command.owner.birth!=runtime ||
            d.context.scope.incarnation!=serial || d.context.scope.nativeGeneration!=generation)continue;
        d.adapter->ConfirmActorDestroyed({character,runtime,true,serial});
        if(!d.adapter->BeginRetirement(d.command.scope))continue;
        const auto retired=d.adapter->DrainRetirement(
            [](void*,const execution::RetirementIdentity&) noexcept {return execution::Readiness::Rejected;},nullptr);
        if(retired.complete) {
            // Exact destructor return + zero retained Refs permits recycling
            // this adapter's storage; no calls target the destroyed actor.
            d.adapter.reset();d.used=d.fault=false;
        }
    }
}
#else
bool StopDonor(const ap::Context&) noexcept {return true;}
#endif
