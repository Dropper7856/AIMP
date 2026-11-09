#include "aimp/AISyncExecutionCodec.h"
#include <algorithm>
#include <bit>
#include <iostream>
#include <stdexcept>

namespace {
using namespace aimp::ai_sync;
void Check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
native_intent::Payload Intent(std::uint32_t id) {
    native_intent::Payload p{}; auto& w=p.words; w[0]=id;
    if (id==8 || id==10) {w[1]=1;w[2]=0xFEDCBA98;}
    if (id==9 || id==12) {w[1]=0x11;w[2]=0x89ABCDEF;w[3]=0xFEDCBA98;}
    if (id==13) {w[1]=1;w[2]=std::bit_cast<std::uint32_t>(-0.0F);}
    if (id>=15 && id<=17) {w[1]=2;w[4]=0x11223344;w[6]=0x55667788;}
    if (id==19) {w[1]=4;w[8]=std::bit_cast<std::uint32_t>(-1.5F);w[11]=std::bit_cast<std::uint32_t>(1.F);}
    if (id==20) {
        w[21]=0x11223344;w[22]=0x55667788;w[23]=w[24]=w[25]=0xFFFFFFFF;
        w[26]=std::bit_cast<std::uint32_t>(64.F);w[27]=std::bit_cast<std::uint32_t>(60.F);
        w[29]=0xFFFFFFFF;w[30]=std::bit_cast<std::uint32_t>(-1.F);
        w[31]=std::bit_cast<std::uint32_t>(60.F);w[32]=std::bit_cast<std::uint32_t>(-100000.F);w[33]=0x0302BE7F;
    }
    if (id==21) {w[1]=4;w[8]=std::bit_cast<std::uint32_t>(-1.5F);w[9]=0x80000000U;w[10]=std::bit_cast<std::uint32_t>(3.25F);}
    if (id==23) {w[1]=1;w[2]=0x80000000U;}
    if (id==25) {w[1]=0x14;w[3]=2;w[8]=std::bit_cast<std::uint32_t>(-1.5F);w[9]=0x80000000U;w[10]=std::bit_cast<std::uint32_t>(3.25F);}
    return p;
}
transactions::Record Record() {
    transactions::Record r{};r.scope.session[0]=1;r.scope.authorityEpoch=2;r.scope.worldEpoch=3;r.scope.consumerEpoch=4;
    r.owner={1,2,1,1,transactions::Capability::Movement,1};
    r.commandId=r.globalSeq=r.ownerSeq=r.parameterRevision=1;r.nativeKind=native_intent::kKind;r.admission=1;r.intent=Intent(9);
    return r;
}
void PayloadDomain() {
    native_intent::CanonicalPayload bytes{};native_intent::Payload decoded{};
    Check(native_intent::Encode({},bytes) && native_intent::Decode(bytes,decoded) && native_intent::Empty(decoded),"empty payload");
    for (std::uint32_t id=1;id<=24;++id) {
        const auto p=Intent(id);const bool supported=id<=23&&id!=11&&id!=14&&id!=18;
        Check(native_intent::Validate(p)==supported,"contract domain");
        if (!supported) continue;
        Check(native_intent::Encode(p,bytes) && native_intent::Decode(bytes,decoded) && decoded==p,"payload exact round trip");
        for (std::size_t n=0;n<bytes.size();++n) Check(!native_intent::Decode(std::span(bytes).first(n),decoded),"truncated payload");
        auto invalid=p;invalid.words[35]=1;Check(!native_intent::Validate(invalid),"reserved intent word");
    }
    const auto p=Intent(9);Check(native_intent::Encode(p,bytes),"scalar encode");
    Check(bytes[8]==0xEF&&bytes[9]==0xCD&&bytes[10]==0xAB&&bytes[11]==0x89,"little-endian scalar");
    // Neither selector is a native address. Preserve None/1/other bit patterns;
    // a pointer/vector carrier must never be admitted for ACT_STUN_DAMAGE.
    for(auto first : {0U,1U,0xFFFFFFFFU}) for(auto second : {0U,1U,0xFFFFFFFFU}) {
        auto selectors=Intent(12);selectors.words[2]=first;selectors.words[3]=second;
        Check(native_intent::Encode(selectors,bytes)&&native_intent::Decode(bytes,decoded)&&decoded==selectors,"stun selectors lost");
        selectors.words[1]=0x22;Check(!native_intent::Validate(selectors),"stun selector interpreted as pointer");
    }
    for (auto id : {15U,16U,17U}) {
        auto identity=Intent(id);identity.words[4]=0;Check(native_intent::Validate(identity),"identity B component rejected");
        identity.words[5]=1;Check(!native_intent::Validate(identity),"identity second argument accepted");
        identity=Intent(id);identity.words[1]=5;Check(!native_intent::Validate(identity),"null with identity data accepted");
        identity.words[4]=identity.words[6]=0;
        Check(native_intent::Validate(identity)==(id==16),"null target does not match native constructor contract");
        identity.words[1]=2;Check(!native_intent::Validate(identity),"empty identity accepted");
    }
    for (auto id : {13U,19U}) for (auto bits : {0x7F800000U,0xFF800000U,0x7FC00000U}) {
        auto invalid=Intent(id);invalid.words[id==13?2:8]=bits;Check(!native_intent::Validate(invalid),"nonfinite argument");
    }
    // The failing v318 capture had refs0/3 and flagsCC0D2880. Exercise the
    // production capture normalizer, preserving identities and defined bits.
    auto referenced=Intent(20);referenced.words[12]=0x11223344;referenced.words[13]=0x55667788;
    referenced.words[18]=0x99AABBCC;referenced.words[19]=0xDDEEFF00;
    referenced.words[33]=0xCC0D2880;
    native_intent::Payload captured{};
    Check(native_intent::CaptureScripted(std::span(referenced.words).subspan(12,22),0,captured),"referenced native script refused");
    Check(captured.words[33]==0x000D2880&&captured.words[12]==referenced.words[12]&&
        captured.words[19]==referenced.words[19],"reference/flag normalization changed semantic fields");
    captured.words[1]=9U; // Only refs0/3 were resolved on the Host.
    Check(native_intent::Encode(captured,bytes)&&native_intent::Decode(bytes,decoded)&&decoded==captured,"script reference wire loss");
    for(std::size_t i=12;i<20;i+=2) {
        auto all=Intent(20);all.words[i]=0x10203040;all.words[i+1]=0x50607080;
        Check(native_intent::Encode(all,bytes)&&native_intent::Decode(bytes,decoded)&&all==decoded,"script reference pair lost");
        all.words[i]=0;Check(!native_intent::Validate(all),"hierarchy without entity admitted");
    }
    for (auto index : {1U,2U,4U,6U,8U,20U,34U,35U}) {
        auto invalid=Intent(20);invalid.words[index]=1;Check(!native_intent::Validate(invalid),"unsupported scripted field was stripped");
    }
    for (auto index : {21U,22U}) {auto invalid=Intent(20);invalid.words[index]=0;Check(!native_intent::Validate(invalid),"missing scripted resource");}
    for (auto index : {23U,24U,25U}) {auto invalid=Intent(20);invalid.words[index]=0xFFFFFFFE;Check(!native_intent::Validate(invalid),"scripted integer lower bound");}
    for (auto index : {26U,27U,28U,30U,31U,32U}) {auto invalid=Intent(20);invalid.words[index]=0x7FC00000;Check(!native_intent::Validate(invalid),"nonfinite script scalar");}
    {auto timed=Intent(20);timed.words[28]=0x3F000000;Check(native_intent::Validate(timed),"Host scripted +40 scalar refused");}
    auto invalid=Intent(20);invalid.words[33]|=0x80000000;Check(!native_intent::Validate(invalid),"unsupported native flag");
    invalid=Intent(8);invalid.words[1]=15;Check(!native_intent::Validate(invalid),"Refused kind admitted");
}
void NewContracts() {
    native_intent::CanonicalPayload bytes{};native_intent::Payload decoded{};
    Check(native_intent::CompletesAtConstruction(Intent(22)),"native immediate 4D completion treated as rejection");
    for(const float duration : {-1.0F,-0.0F,0.0F,0.5F}) {
        auto timed=Intent(23);timed.words[2]=std::bit_cast<std::uint32_t>(duration);
        Check(native_intent::CompletesAtConstruction(timed)==(duration<=0.0F),"native duration completion contract");
    }
    Check(!native_intent::CompletesAtConstruction(Intent(21)),"passage initialization failure hidden");
    Check(!native_intent::CompletesAtConstruction(Intent(25)),"passage76 construction mistaken for completion");
    for (auto id : {21U,22U,23U,25U}) {
        const auto p=Intent(id);
        Check(native_intent::Encode(p,bytes)&&native_intent::Decode(bytes,decoded)&&decoded==p,"new contract round trip");
        if(id==21||id==25) Check(bytes[34]==0xC0&&bytes[35]==0xBF&&bytes[39]==0x80&&
            bytes[42]==0x50&&bytes[43]==0x40,"XYZ order or signed zero lost");
        if(id==23) Check(bytes[11]==0x80,"duration signed zero lost");
        // Every unused word, including the fourth vector lane, is canonical
        // integer zero. Negative float zero is also forbidden in padding.
        for(std::size_t i=1;i<36;++i) {
            if(((id==21||id==25)&&(i==1||(i>=8&&i<=10)))||(id==25&&i==3)||(id==23&&(i==1||i==2)))continue;
            for(auto bits : {1U,0x80000000U}) {
                auto invalid=p;invalid.words[i]=bits;
                Check(!native_intent::Validate(invalid)&&!native_intent::Encode(invalid,bytes),"new reserved word admitted");
                Check(native_intent::Encode(p,bytes),"new contract reencode");bytes[i*4+(bits==1U?0:3)]=bits==1U?1:0x80;
                Check(!native_intent::Decode(bytes,decoded),"new reserved wire word admitted");
            }
        }
    }
    for(auto id : {21U,23U,25U}) {
        for(auto kind : {0U,1U,2U,4U,5U,0x11U,0x14U,0x24U}) {
            if(kind==(id==25?0x14U:id==21?4U:1U))continue;
            auto invalid=Intent(id);invalid.words[1]=kind;Check(!native_intent::Validate(invalid),"new wrong argument carrier");
            Check(native_intent::Encode(Intent(id),bytes),"argument carrier wire setup");
            bytes[4]=static_cast<std::uint8_t>(kind);
            Check(!native_intent::Decode(bytes,decoded),"new wrong argument carrier decoded");
        }
        const std::size_t begin=id==23?2:8,end=id==23?3:11;
        for(auto i=begin;i<end;++i) {
            for(auto bits : {0U,0x80000000U,0x7F7FFFFFU,0xFF7FFFFFU,1U,0x80000001U,0x11223344U}) {
                auto finite=Intent(id);finite.words[i]=bits;
                Check(native_intent::Encode(finite,bytes)&&native_intent::Decode(bytes,decoded)&&decoded==finite,"finite float bits changed");
            }
            for(auto bits : {0x7F800000U,0xFF800000U,0x7FC00000U,0x7F800001U}) {
                auto invalid=Intent(id);invalid.words[i]=bits;Check(!native_intent::Validate(invalid),"new nonfinite argument");
                Check(native_intent::Encode(Intent(id),bytes),"finite wire setup");
                for(std::size_t b=0;b<4;++b)bytes[i*4+b]=static_cast<std::uint8_t>(bits>>(b*8));
                Check(!native_intent::Decode(bytes,decoded),"new nonfinite wire argument");
            }
        }
    }
    for(auto selector : {0U,2U}) {
        auto passage=Intent(25);passage.words[3]=selector;
        Check(native_intent::Encode(passage,bytes)&&native_intent::Decode(bytes,decoded)&&decoded==passage,"passage selector lost");
        passage.words[8]=passage.words[9]=passage.words[10]=0;
        Check(native_intent::Encode(passage,bytes)&&native_intent::Decode(bytes,decoded)&&decoded==passage,"passage origin refused");
    }
    for(auto selector : {1U,3U,0xFFFFFFFFU,0x80000000U,0x11223344U,0x3F800000U}) {
        auto invalid=Intent(25);invalid.words[3]=selector;
        Check(!native_intent::Validate(invalid)&&!native_intent::Encode(invalid,bytes),"unsupported passage speed selector admitted");
        Check(native_intent::Encode(Intent(25),bytes),"passage selector wire setup");
        for(std::size_t b=0;b<4;++b)bytes[12+b]=static_cast<std::uint8_t>(selector>>(b*8));
        Check(!native_intent::Decode(bytes,decoded),"unsupported passage selector decoded");
    }
    for(auto id : {24U,26U,0xFFFFFFFFU}) Check(!native_intent::Validate(Intent(id)),"future contract admitted");
}
void ObserverScriptIsolation() {
    // Observed vent-kill constructors used resolved refs0/1 and flagsD2B80
    // (then D2D80). Their local aliases/resources remain valid actor inputs,
    // but their scene context must never bind to the observer's THE_PLAYER.
    for (const auto flags : {0x000D2B80U, 0x000D2D80U, native_intent::kScriptedFlags}) {
        auto source = Intent(20);
        source.words[1] = 15U;
        for (std::size_t i=12U;i<20U;++i) source.words[i]=0x10203040U+static_cast<std::uint32_t>(i);
        source.words[28] = std::bit_cast<std::uint32_t>(0.5F);
        source.words[33] = flags;
        const auto original = source;
        native_intent::Payload isolated{};
        Check(native_intent::IsolateObserverScripted(source,isolated),"observer scripted isolation refused valid input");
        Check(source==original,"observer isolation changed receipt/wire identity");
        Check(isolated.words[1]==0U&&isolated.words[33]==0U,"observer retained scene flags or resolved reference evidence");
        for (std::size_t i=12U;i<=20U;++i) Check(isolated.words[i]==0U,"observer retained foreign scene reference");
        for (std::size_t i=21U;i<=28U;++i) Check(isolated.words[i]==source.words[i],"observer lost actor clip/timeline parameters");
        Check(isolated.words[29]==0U&&isolated.words[30]==std::bit_cast<std::uint32_t>(-1.F)&&
            isolated.words[31]==0U&&isolated.words[32]==0U,"observer retained external timeline/placement controls");
        native_intent::Payload twice{};
        Check(native_intent::IsolateObserverScripted(isolated,twice)&&twice==isolated,"observer isolation is not idempotent");
    }
    native_intent::Payload output=Intent(9);const auto before=output;
    Check(!native_intent::IsolateObserverScripted(Intent(9),output)&&output==before,"observer isolation accepted other constructor");
    auto invalid=Intent(20);invalid.words[21]=0U;
    Check(!native_intent::IsolateObserverScripted(invalid,output)&&output==before,"observer isolation repaired invalid wire input");
}
void WireAndReceipts() {
    auto r=Record();execution::Batch batch{};batch.scope=r.scope;batch.batchId=5;batch.count=2;batch.records[0]=r;
    auto scalar=r;scalar.nativeKind=7;scalar.admission=0;scalar.intent={};batch.records[1]=scalar;
    std::vector<std::uint8_t> bytes;Check(execution::EncodeBatch(batch,bytes),"v3 mixed encode");
    Check(bytes[4]==3&&bytes.size()==execution::kBatchHeaderSize+2*execution::kNativeIntentRecordSize,"v3 fixed stride");
    const auto emptyAt=execution::kBatchHeaderSize+execution::kNativeIntentRecordSize+execution::kExtendedRecordSize;
    Check(std::all_of(bytes.begin()+emptyAt,bytes.begin()+emptyAt+native_intent::kBytes,[](auto b){return b==0;}),"nonintent v3 extension");
    execution::Batch copy{};Check(execution::DecodeBatch(bytes,r.scope,copy)&&copy.records[0].intent==r.intent&&native_intent::Empty(copy.records[1].intent),"mixed payload decode");
    auto invalid=bytes;invalid[emptyAt]=1;Check(!execution::DecodeBatch(invalid,r.scope,copy),"nonintent unexpected payload");
    for(std::size_t n=0;n<bytes.size();++n)Check(!execution::DecodeBatch(std::span(bytes).first(n),r.scope,copy),"truncated v3 batch");
    execution::Receipt receipt{r.scope,5,r,execution::Stage::Waiting,transactions::Result::Ok};
    Check(execution::EncodeReceipt(receipt,bytes)&&bytes[4]==3&&bytes.size()==execution::kMaximumReceiptBytes,"v3 receipt encode");
    execution::Receipt echoed{};Check(execution::DecodeReceipt(bytes,r.scope,echoed)&&echoed.operation.intent==r.intent,"receipt exact intent");
    bytes[execution::kHeaderSize+execution::kExtendedRecordSize+8]^=1;
    Check(execution::DecodeReceipt(bytes,r.scope,echoed)&&echoed.operation.intent!=r.intent,"receipt changed scalar lost");
    for (auto op : {transactions::Op::Update,transactions::Op::Cancel,transactions::Op::Stop}) {auto bad=r;bad.opcode=op;Check(!execution::ValidateExecutable(bad),"unsupported intent operation");}
    for (auto op : {transactions::Op::Complete,transactions::Op::Release}) {
        auto terminal=r;terminal.opcode=op;Check(!execution::ValidateExecutable(terminal),"terminal retained intent");
        terminal.intent={};Check(execution::ValidateExecutable(terminal),"canonical terminal intent");
        batch.count=1;batch.records[0]=terminal;Check(execution::EncodeBatch(batch,bytes)&&bytes[4]==3,"terminal must use v3");
    }
    auto bad=r;bad.intent={};Check(!execution::ValidateExecutable(bad),"missing begin payload");
    bad=r;bad.admission=0;Check(!execution::ValidateExecutable(bad),"intent nonappend");
    bad=r;bad.scalarRetention=2;Check(!execution::ValidateExecutable(bad),"intent retention");
    bad=r;bad.aux=-0.F;Check(!execution::ValidateExecutable(bad),"intent unused negative zero");
    scalar.intent=r.intent;Check(!execution::ValidateExecutable(scalar),"nonintent unexpected payload");
    // Historical scalar envelopes must remain v1, and an extended scalar v2.
    batch.count=1;scalar.intent={};batch.records[0]=scalar;
    Check(execution::EncodeBatch(batch,bytes)&&bytes[4]==1&&execution::DecodeBatch(bytes,r.scope,copy),"v1 regression");
    batch.records[0].admission=1;
    Check(execution::EncodeBatch(batch,bytes)&&bytes[4]==2&&execution::DecodeBatch(bytes,r.scope,copy),"v2 regression");
    receipt.operation=batch.records[0];Check(execution::EncodeReceipt(receipt,bytes)&&bytes[4]==2&&execution::DecodeReceipt(bytes,r.scope,echoed),"v2 receipt regression");
    execution::Control control{r.scope,execution::ControlKind::Offer,execution::kProfileNativeIntent};std::array<std::uint8_t,execution::kHeaderSize> offer{};
    Check(execution::EncodeControl(control,offer)&&execution::ProfileForKind(native_intent::kKind)==128,"intent capability");
    for(auto id : {21U,22U,23U,25U}) {
        auto operation=Record();operation.intent=Intent(id);batch.count=1;batch.records[0]=operation;
        Check(execution::EncodeBatch(batch,bytes)&&bytes[4]==3&&bytes.size()==execution::kBatchHeaderSize+execution::kNativeIntentRecordSize&&
            execution::DecodeBatch(bytes,r.scope,copy)&&copy.records[0].intent==operation.intent,"new contract batch envelope");
        receipt.operation=operation;
        Check(execution::EncodeReceipt(receipt,bytes)&&bytes[4]==3&&bytes.size()==execution::kMaximumReceiptBytes&&
            execution::DecodeReceipt(bytes,r.scope,echoed)&&echoed.operation.intent==operation.intent,"new contract receipt envelope");
    }
    for(auto selector : {0U,2U}) {
        auto passage=Record();passage.intent=Intent(25);passage.intent.words[3]=selector;
        batch.count=1;batch.records[0]=passage;
        Check(execution::EncodeBatch(batch,bytes)&&execution::DecodeBatch(bytes,r.scope,copy)&&copy.records[0].intent==passage.intent,"passage batch selector/XYZ loss");
        receipt.operation=passage;
        Check(execution::EncodeReceipt(receipt,bytes)&&execution::DecodeReceipt(bytes,r.scope,echoed)&&echoed.operation.intent==passage.intent,"passage receipt selector/XYZ loss");
        auto marker=passage;marker.nativeKind=execution::kMarkerKind;
        Check(!execution::ValidateExecutable(marker),"passage contract admitted as level marker");
    }
}
}
int main() { PayloadDomain();NewContracts();ObserverScriptIsolation();WireAndReceipts();std::cout<<"AISync native intent codec PASS (offline)\n"; }
