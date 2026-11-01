#include "aimp/AISyncExecutionCodec.h"
#include <algorithm>
#include <bit>
#include <iostream>
#include <stdexcept>
using namespace aimp::ai_sync;
namespace {
void Check(bool ok) { if (!ok) throw std::runtime_error("marker codec assertion failed"); }
native_intent::Payload Payload() {
    native_intent::Payload p{}; auto& w=p.words; w[0]=24; w[1]=1;
    for (unsigned i=2;i<10;++i) w[i]=0x03020100U+(i-2)*0x04040404U;
    w[10]=1; w[11]=0; w[12]=1; w[13]=0; return p;
}
transactions::Record Record() {
    transactions::Record r{}; r.scope.session[0]=1; r.scope.authorityEpoch=2; r.scope.worldEpoch=3; r.scope.consumerEpoch=4;
    r.owner={1,2,1,1,transactions::Capability::Movement,1};
    r.commandId=r.globalSeq=r.ownerSeq=r.parameterRevision=1; r.nativeKind=2; r.intent=Payload();
    r.movement.version=r.movement.parts=1; r.movement.navigation=movement::Navigation::HostRoute;
    r.movement.before.goal={1,2,3}; r.movement.before.query[0]=std::bit_cast<float>(0x7FC01234U);
    r.movement.after=r.movement.before; r.movement.after.retained=1; return r;
}
void Domain() {
    auto p=Payload(); native_intent::CanonicalPayload bytes{}; native_intent::Payload copy{};
    Check(native_intent::Encode(p,bytes)&&native_intent::Decode(bytes,copy)&&copy==p);
    for(unsigned i=0;i<32;++i) Check(bytes[8+i]==i);
    Check(bytes[0]==24&&bytes[4]==1&&bytes[40]==1&&bytes[48]==1);
    for(unsigned i=56;i<144;++i)Check(bytes[i]==0);
    for (auto at : {1,10,11,12,13}) for(auto v : {0U,1U,2U,1000000U,1000001U,0xFFFFFFFFU}) {
        auto bad=p; bad.words[at]=v;
        bool valid=at==1?v==1:at==10?v>0&&v<=1000000:at==11?v<=1000000:v<=1;
        Check(native_intent::Validate(bad)==valid);
    }
    auto bad=p; std::fill(bad.words.begin()+2,bad.words.begin()+10,0); Check(!native_intent::Validate(bad));
    for(unsigned i=14;i<36;++i)for(auto v : {1U,0x80000000U}) {
        bad=p;bad.words[i]=v;Check(!native_intent::Validate(bad));
        auto wire=bytes;wire[i*4+(v==1?0:3)]=v==1?1:128;Check(!native_intent::Decode(wire,copy));
    }
}
void Operations() {
    auto r=Record();Check(execution::ValidateExecutable(r));
    for(auto op:{transactions::Op::Begin,transactions::Op::Submit,transactions::Op::Update}) {
        auto good=r;good.opcode=op;Check(execution::ValidateExecutable(good));
    }
    for(auto kind:{1U,3U,4U,5U,7U,8U,0x200U}) {auto b=r;b.nativeKind=kind;Check(!execution::ValidateExecutable(b));}
    auto constructor=r;constructor.nativeKind=0x200U;constructor.admission=1;constructor.movement={};
    Check(!execution::ValidateExecutable(constructor)); //24 alone cannot enter the constructor path.
    auto reject=[&](auto mutate){auto b=r;mutate(b);Check(!execution::ValidateExecutable(b));};
    reject([](auto& b){b.intent={};}); reject([](auto& b){b.intent.words[0]=1;});
    reject([](auto& b){b.flags=transactions::HasGoal;}); reject([](auto& b){b.mode=1;});
    reject([](auto& b){b.gait=1;});reject([](auto& b){b.scalarRetention=2;});
    reject([](auto& b){b.goalX=-0.F;});reject([](auto& b){b.goalY=-0.F;});reject([](auto& b){b.goalZ=-0.F;});
    reject([](auto& b){b.distance=-0.F;});reject([](auto& b){b.stopDistance=-0.F;});reject([](auto& b){b.aux=-0.F;});
    reject([](auto& b){b.movement={};});reject([](auto& b){b.movement.version=2;});
    reject([](auto& b){b.movement.parts=2;});reject([](auto& b){b.movement.primaryFlags=1;});
    reject([](auto& b){b.movement.navigation=movement::Navigation::LocalNative;});
    reject([](auto& b){b.movement.after.goal[0]=2;});reject([](auto& b){b.movement.after.query[0]=std::bit_cast<float>(0x7FC01235U);});
    reject([](auto& b){b.movement.after.stopDistance=-0.F;});
    for(auto op:{transactions::Op::Cancel,transactions::Op::Stop,transactions::Op::Complete,transactions::Op::Release}) {
        auto t=r;t.opcode=op;Check(!execution::ValidateExecutable(t));t.intent={};t.movement={};Check(execution::ValidateExecutable(t));
        execution::Batch batch{t.scope,5};batch.count=1;batch.records[0]=t;std::vector<std::uint8_t> bytes;
        execution::Batch decoded{};Check(execution::EncodeBatch(batch,bytes)&&bytes[4]==3&&execution::DecodeBatch(bytes,t.scope,decoded));
        execution::Receipt receipt{t.scope,5,t};execution::Receipt echo{};
        std::array<std::uint8_t,execution::kReceiptSize> legacy{};Check(!execution::EncodeReceipt(receipt,legacy));
        Check(execution::EncodeReceipt(receipt,bytes)&&bytes[4]==3&&execution::DecodeReceipt(bytes,t.scope,echo));
        const auto receiptBytes=bytes;
        // Forge correctly framed legacy v1/v2 terminal envelopes: both must refuse kind2.
        for(auto version:{1U,2U}) {
            auto stride=version==1?transactions::kRecordSize:execution::kExtendedRecordSize;
            auto old=receiptBytes;old.resize(64+stride+8);old[4]=static_cast<std::uint8_t>(version);
            auto size=static_cast<unsigned>(old.size());for(unsigned i=0;i<4;++i)old[8+i]=static_cast<std::uint8_t>(size>>(8*i));
            std::fill(old.end()-8,old.end(),std::uint8_t{});old[old.size()-8]=1;
            Check(!execution::DecodeReceipt(old,t.scope,echo));
            Check(execution::EncodeBatch(batch,bytes));
            auto oldBatch=bytes;oldBatch.resize(72+stride);oldBatch[4]=static_cast<std::uint8_t>(version);
            size=static_cast<unsigned>(oldBatch.size());for(unsigned i=0;i<4;++i)oldBatch[8+i]=static_cast<std::uint8_t>(size>>(8*i));
            Check(!execution::DecodeBatch(oldBatch,t.scope,decoded));
        }
    }
    execution::Batch batch{r.scope,5};batch.count=1;batch.records[0]=r;std::vector<std::uint8_t> bytes;
    execution::Batch decoded{};Check(execution::EncodeBatch(batch,bytes)&&bytes.size()==72+execution::kNativeIntentRecordSize&&bytes[4]==3);
    Check(execution::DecodeBatch(bytes,r.scope,decoded)&&decoded.records[0].intent==r.intent&&movement::Same(decoded.records[0].movement,r.movement));
    native_intent::CanonicalPayload payload{};Check(native_intent::Encode(r.intent,payload));
    Check(std::equal(payload.begin(),payload.end(),bytes.begin()+72+execution::kExtendedRecordSize));
    execution::Receipt receipt{r.scope,5,r};execution::Receipt echo{};Check(execution::EncodeReceipt(receipt,bytes)&&execution::DecodeReceipt(bytes,r.scope,echo));
    Check(echo.operation.intent==r.intent&&movement::Same(echo.operation.movement,r.movement));
    bytes[64+execution::kExtendedRecordSize+8]^=1;Check(execution::DecodeReceipt(bytes,r.scope,echo)&&echo.operation.intent!=r.intent);
    bytes[64+execution::kExtendedRecordSize+56]=1;Check(!execution::DecodeReceipt(bytes,r.scope,echo));
    Check(execution::ProfileForKind(2)==1024&&execution::kKnownProfiles==2047);
    execution::Control control{r.scope,execution::ControlKind::Offer,1024};std::array<std::uint8_t,64> offer{};Check(execution::EncodeControl(control,offer));
}
}
int main(){Domain();Operations();std::cout<<"marker codec PASS (offline only)\n";}
