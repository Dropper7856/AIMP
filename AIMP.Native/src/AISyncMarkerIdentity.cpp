#include "aimp/AISyncMarkerIdentity.h"
#include "aimp/HookManager.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

namespace aimp::ai_sync::marker_identity {
static_assert(sizeof(std::uintptr_t) == 4, "Marker adapter requires the certified x86 image ABI");
namespace {
bool LevelValid(const std::array<char, kMaxLevelBytes>& level) noexcept {
    bool ended = false;
    std::size_t length = 0;
    for (const auto c : level) {
        if (c == 0) { ended = true; continue; }
        if (ended || !((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '_' || c == '-')) return false;
        ++length;
    }
    return ended && length != 0;
}
struct Cursor {
    std::span<const std::byte> bytes;
    std::size_t offset{};
    bool Skip(std::size_t count) noexcept {
        if (count > bytes.size() - offset) return false;
        offset += count;
        return true;
    }
    bool Read(std::size_t count, std::uint32_t& value) noexcept {
        value = 0;
        if (count > bytes.size() - offset) return false;
        for (std::size_t i = 0; i < count; ++i)
            value |= std::uint32_t(std::to_integer<std::uint8_t>(bytes[offset + i])) << (8 * i);
        offset += count;
        return true;
    }
};

// AI.exe.c 00743CA0 + 0073C540: 10-byte header; 64 serialized
// bytes per cover (NOT the 0x60 runtime stride); u32 marker total;
// per-cover u16 count and 18 serialized bytes per marker (NOT 0x24);
// u16 grid dimensions + three u32 words; per-cell u16 count + u16 entries.
// Entire grammar is checked before accepting even a marker near the beginning.
Result Parse(std::span<const std::byte> bytes, std::uint32_t cover,
    std::uint32_t marker, std::uint32_t& flat) noexcept {
    if (bytes.size() > kMaxContentBytes) return Result::ContentTooLarge;
    Cursor c{bytes};
    std::uint32_t magic{}, version{}, covers{}, total{};
    if (!c.Read(4, magic) || magic != 0x32727663U || !c.Read(4, version))
        return Result::InvalidContent;
    if (version != kCoverVersion) return Result::UnsupportedVersion;
    if (!c.Read(2, covers) || !c.Skip(std::size_t(covers) * 64) || !c.Read(4, total))
        return Result::InvalidContent;
    std::uint32_t sum = 0;
    bool found = false;
    for (std::uint32_t i = 0; i < covers; ++i) {
        std::uint32_t count{};
        if (!c.Read(2, count) || sum > total || count > total - sum ||
            !c.Skip(std::size_t(count) * 18)) return Result::InvalidContent;
        if (i + 1 == cover && marker < count) { flat = sum + marker; found = true; }
        sum += count;
    }
    if (sum != total) return Result::InvalidContent;
    std::uint32_t width{}, height{};
    if (!c.Read(2, width) || !c.Read(2, height) || !c.Skip(12)) return Result::InvalidContent;
    // Bound cell loop using the minimum two serialized bytes per cell.
    const auto cells = std::uint64_t(width) * height;
    if (cells > (bytes.size() - c.offset) / 2) return Result::InvalidContent;
    for (std::uint64_t i = 0; i < cells; ++i) {
        std::uint32_t count{};
        if (!c.Read(2, count) || !c.Skip(std::size_t(count) * 2)) return Result::InvalidContent;
    }
    if (c.offset != bytes.size()) return Result::InvalidContent;
    return found ? Result::Ok : Result::InvalidOrdinal;
}
} // namespace

Result Validate(const Key& key) noexcept {
    if (key.coverOrdinal == 0 || key.coverOrdinal > 65535U || key.markerOrdinal >= 65535U ||
        std::all_of(key.contentSha256.begin(), key.contentSha256.end(),
            [](auto byte) { return byte == 0; })) return Result::InvalidKey;
    return Result::Ok;
}

Result HashImmutableContent(std::span<const std::byte> bytes, Digest& output) noexcept {
    output = {};
    if (bytes.size() > kMaxContentBytes) return Result::ContentTooLarge;
    BCRYPT_ALG_HANDLE algorithm{};
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        return Result::HashUnavailable;
    BCRYPT_HASH_HANDLE hash{};
    if (BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) < 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return Result::HashUnavailable;
    }
    Digest digest{};
    const auto updated = bytes.empty() ? 0 : BCryptHashData(hash,
        reinterpret_cast<PUCHAR>(const_cast<std::byte*>(bytes.data())),
        static_cast<ULONG>(bytes.size()), 0);
    const auto finished = updated < 0 ? updated :
        BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0);
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (finished < 0) return Result::HashUnavailable;
    output = digest;
    return Result::Ok;
}

Result DescribeSerializedMarker(std::span<const std::byte> bytes,
    const std::array<char, kMaxLevelBytes>& level, std::uint32_t state,
    std::uint32_t coverOrdinal, std::uint32_t markerOrdinal, Key& output) noexcept {
    output = {};
    if (!LevelValid(level)) return Result::InvalidKey;
    std::uint32_t ignored{};
    const auto parsed = Parse(bytes, coverOrdinal, markerOrdinal, ignored);
    if (parsed != Result::Ok) return parsed;
    Key candidate{{}, coverOrdinal, markerOrdinal};
    Digest content{};
    const auto hashed = HashImmutableContent(bytes, content);
    if (hashed != Result::Ok) return hashed;
    // Domain separated full digest, canonical namespace and LE state. No path
    // hash, timestamp, native bucket hash or partial content fingerprint.
    constexpr char domain[] = "AIMP.CVR2.IDENTITY.V1";
    std::array<std::byte, sizeof(domain) + 4 + kMaxLevelBytes + 32> qualified{};
    std::memcpy(qualified.data(), domain, sizeof(domain));
    for (std::size_t i = 0; i < 4; ++i)
        qualified[sizeof(domain) + i] = static_cast<std::byte>((state >> (8 * i)) & 255);
    std::memcpy(qualified.data() + sizeof(domain) + 4, level.data(), level.size());
    std::memcpy(qualified.data() + sizeof(domain) + 4 + level.size(), content.data(), content.size());
    const auto qualifiedHash = HashImmutableContent(qualified, candidate.contentSha256);
    if (qualifiedHash != Result::Ok) return qualifiedHash;
    output = candidate;
    return Result::Ok;
}

Result FindSerializedMarker(std::span<const std::byte> bytes,
    const std::array<char, kMaxLevelBytes>& level, std::uint32_t state, const Key& key,
    std::uint32_t& flatOrdinal) noexcept {
    flatOrdinal = 0xffffffffU;
    if (Validate(key) != Result::Ok) return Result::InvalidKey;
    std::uint32_t flat{};
    const auto parsed = Parse(bytes, key.coverOrdinal, key.markerOrdinal, flat);
    if (parsed != Result::Ok) return parsed;
    Key candidate{};
    const auto hashed = DescribeSerializedMarker(bytes, level, state, key.coverOrdinal,
        key.markerOrdinal, candidate);
    if (hashed != Result::Ok) return hashed;
    if (candidate.contentSha256 != key.contentSha256) return Result::ContentMismatch;
    flatOrdinal = flat;
    return Result::Ok;
}

namespace {
// Exact x86 layouts below are field reads, never host C++ ABI overlays.
struct CoverRange { std::uint32_t first{}, count{}; std::size_t file{}; };
struct Snapshot {
    std::vector<std::byte> bytes;
    std::vector<CoverRange> covers;
    Digest digest{};
    std::uint32_t markers{}, markerTable{}, coverTable{};
};
struct Entry {
    std::uintptr_t world{};
    std::uint64_t load{};
    bool loading{};
    std::shared_ptr<Snapshot> snapshot;
};
struct Frame {
    std::uintptr_t world{}, stream{};
    std::uint32_t bufferBegin{}, bufferEnd{};
    std::uint64_t load{};
    std::uint32_t state{};
    std::array<char, kMaxLevelBytes> level{};
    std::shared_ptr<Snapshot> snapshot;
    bool complete{};
};
std::recursive_mutex gGate;
std::array<Entry, 64> gEntries{};
std::uintptr_t gBase{}, gGenerationWorld{};
std::uint64_t gSerial{}, gGeneration{};
EngineThread gEngineThread{};
bool gInstalled{}, gRetired{};
thread_local std::array<Frame, 8> gFrames{};
thread_local std::size_t gDepth{};
std::atomic<void*> gLoadOriginal{}, gFreeOriginal{}, gDestroyOriginal{}, gResetOriginal{};
void *gLoadTrampoline{}, *gFreeTrampoline{}, *gDestroyTrampoline{}, *gResetTrampoline{};
using NativeLoad = void(__thiscall*)(void*, std::uint32_t);
using NativeWorld = void(__thiscall*)(void*);
using NativePath = void(__thiscall*)(void*, char*, std::uint32_t);
using NativeOpen = std::uint8_t(__thiscall*)(void*, const char*);
using NativeClose = void(__thiscall*)(void*);

bool Copy(std::uintptr_t source, void* output, std::size_t size) noexcept {
    SIZE_T copied{};
    return source != 0 && ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(source),
        output, size, &copied) != FALSE && copied == size;
}
template<class T> bool Read(std::uintptr_t at, T& output) noexcept {
    return Copy(at, &output, sizeof(output));
}
std::uint32_t U32(std::span<const std::byte> b, std::size_t at) noexcept {
    Cursor cursor{b, at}; std::uint32_t v{};
    static_cast<void>(cursor.Read(4, v)); return v;
}
std::uint32_t U16(std::span<const std::byte> b, std::size_t at) noexcept {
    Cursor cursor{b, at}; std::uint32_t v{};
    static_cast<void>(cursor.Read(2, v)); return v;
}
Entry* Find(std::uintptr_t world) noexcept {
    for (auto& entry : gEntries) if (entry.world == world) return &entry;
    return nullptr;
}
void Revoke(std::uintptr_t world) noexcept {
    const std::lock_guard lock(gGate);
    if (auto* entry = Find(world)) { entry->snapshot.reset(); entry->loading = false; }
}
std::uint64_t Begin(std::uintptr_t world) noexcept {
    const std::lock_guard lock(gGate);
    auto* entry = Find(world);
    if (!entry) for (auto& slot : gEntries) if (!slot.loading && !slot.snapshot) { entry = &slot; break; }
    if (!entry || gSerial == UINT64_MAX) return 0;
    *entry = {world, ++gSerial, true, {}};
    return entry->load;
}
std::shared_ptr<Snapshot> Content(std::span<const std::byte> bytes,
    const std::array<char, kMaxLevelBytes>& level, std::uint32_t state) noexcept {
    std::uint32_t ignored{};
    const auto parsed = Parse(bytes, 1, 0, ignored);
    if (parsed != Result::Ok && parsed != Result::InvalidOrdinal) return {};
    try {
        auto snapshot = std::make_shared<Snapshot>();
        snapshot->bytes.assign(bytes.begin(), bytes.end());
        const auto covers = U16(bytes, 8);
        auto at = std::size_t{10} + covers * 64;
        snapshot->markers = U32(bytes, at); at += 4;
        if (snapshot->markers == 0 || snapshot->markers > INT32_MAX) return {};
        snapshot->covers.reserve(covers);
        std::uint32_t first = 0, nonempty = 0;
        for (std::uint32_t i = 0; i < covers; ++i) {
            const auto count = U16(bytes, at); at += 2;
            snapshot->covers.push_back({first, count, at});
            if (count != 0 && nonempty == 0) nonempty = i + 1;
            first += count; at += std::size_t(count) * 18;
        }
        Key key{};
        if (DescribeSerializedMarker(bytes, level, state, nonempty, 0, key) != Result::Ok) return {};
        snapshot->digest = key.contentSha256;
        return snapshot;
    } catch (...) { return {}; }
}
bool Header(std::uintptr_t world, const Snapshot& snapshot, bool bound,
    std::uint32_t& markers, std::uint32_t& covers) noexcept {
    std::uint32_t markerCount{}, coverCount{}, owner{}, grid{}, traversal{};
    std::uint8_t ready{};
    return Read(world + 0x23D8, markers) && markers != 0 &&
        Read(world + 0x23DC, markerCount) && markerCount == snapshot.markers &&
        Read(world + 0x23E0, owner) && owner == world &&
        Read(world + 0x23E4, covers) && covers != 0 &&
        Read(world + 0x23E8, coverCount) && coverCount == snapshot.covers.size() &&
        Read(world + 0x23F4, ready) && (ready & 1) != 0 &&
        Read(world + 0x23F8, grid) && grid != 0 &&
        Read(world + 0x23FC, traversal) && traversal != 0 &&
        markers <= UINT32_MAX - std::uint64_t(snapshot.markers) * 0x24 &&
        covers <= UINT32_MAX - std::uint64_t(snapshot.covers.size()) * 0x60 &&
        (!bound || (markers == snapshot.markerTable && covers == snapshot.coverTable));
}
bool MarkerMatches(const Snapshot& snapshot, std::uint32_t markers, std::uint32_t covers,
    std::uint32_t cover, std::uint32_t ordinal) noexcept {
    if (cover == 0 || cover > snapshot.covers.size()) return false;
    const auto& range = snapshot.covers[cover - 1];
    if (ordinal >= range.count) return false;
    std::uint16_t count{}, selector{}, tag{};
    std::uint32_t first{}, value{};
    const auto resource = std::uintptr_t(covers) + std::size_t(cover - 1) * 0x60;
    const auto marker = std::uintptr_t(markers) + std::size_t(range.first + ordinal) * 0x24;
    const auto file = range.file + std::size_t(ordinal) * 18;
    if (!Read(resource + 0x58, count) || count != range.count ||
        !Read(resource + 0x5C, first) || first != range.first ||
        !Read(marker + 0x18, selector) || selector != cover ||
        !Read(marker + 0x1A, tag) || tag != U16(snapshot.bytes, file + 12)) return false;
    constexpr std::array<std::size_t, 4> native{8, 0, 4, 0x1C};
    constexpr std::array<std::size_t, 4> serialized{0, 4, 8, 14};
    for (std::size_t i = 0; i < native.size(); ++i)
        if (!Read(marker + native[i], value) || value != U32(snapshot.bytes, file + serialized[i])) return false;
    // Deliberately excludes runtime marker +0C/+10/+14 ownership and cover
    // +54 availability. Neither contributes to portable content identity.
    return true;
}
bool Commit(std::uintptr_t world, std::uint64_t load, const std::shared_ptr<Snapshot>& snapshot) noexcept {
    const std::lock_guard lock(gGate);
    auto* entry = Find(world);
    if (!snapshot || !entry || !entry->loading || entry->load != load || gRetired) return false;
    entry->loading = false;
    std::uint32_t markers{}, covers{};
    if (!Header(world, *snapshot, false, markers, covers)) return false;
    // Reconcile EVERY authored marker with the actual completed allocation.
    for (std::uint32_t i = 0; i < snapshot->covers.size(); ++i) {
        const auto& range = snapshot->covers[i];
        std::uint16_t count{}; std::uint32_t first{};
        const auto resource = std::uintptr_t(covers) + i * 0x60;
        if (!Read(resource + 0x58, count) || count != range.count ||
            !Read(resource + 0x5C, first) || first != range.first) return false;
        for (std::uint32_t j = 0; j < range.count; ++j)
            if (!MarkerMatches(*snapshot, markers, covers, i + 1, j)) return false;
    }
    snapshot->markerTable = markers; snapshot->coverTable = covers;
    entry->snapshot = snapshot;
    return true;
}
bool CanonicalLevel(const char* output, std::uint32_t bytes,
    std::array<char, kMaxLevelBytes>& level) noexcept {
    if (bytes == 0 || bytes > 260) return false;
    std::array<char, 260> text{};
    if (!Copy(reinterpret_cast<std::uintptr_t>(output), text.data(), bytes)) return false;
    const auto end = std::find(text.begin(), text.begin() + bytes, '\0');
    if (end == text.begin() + bytes) return false;
    for (auto it = text.begin(); it != end; ++it) {
        if (*it == '\\') *it = '/';
        if (*it >= 'a' && *it <= 'z') *it = static_cast<char>(*it - 'a' + 'A');
    }
    constexpr char prefix[] = "DATA/ENV/PRODUCTION/";
    constexpr char suffix[] = "/WORLD";
    const auto size = static_cast<std::size_t>(end - text.begin());
    if (size <= sizeof(prefix) + sizeof(suffix) - 2 ||
        !std::equal(std::begin(prefix), std::end(prefix) - 1, text.begin()) ||
        !std::equal(std::begin(suffix), std::end(suffix) - 1, end - (sizeof(suffix) - 1))) return false;
    const auto length = size - (sizeof(prefix) - 1) - (sizeof(suffix) - 1);
    if (length >= level.size()) return false;
    std::array<char, kMaxLevelBytes> candidate{};
    std::copy_n(text.begin() + sizeof(prefix) - 1, length, candidate.begin());
    if (!LevelValid(candidate)) return false;
    level = candidate;
    return true;
}
Frame* ActiveFrame() noexcept {
    return gDepth && gDepth <= gFrames.size() ? &gFrames[gDepth - 1] : nullptr;
}
void __fastcall PathDetour(void* owner, void*, char* output, std::uint32_t bytes) {
    reinterpret_cast<NativePath>(gBase + 0x38BEB0U)(owner, output, bytes);
    if (auto* frame = ActiveFrame(); frame && frame->load)
        static_cast<void>(CanonicalLevel(output, bytes, frame->level));
}
void SnapshotStream(Frame& frame, void* stream) noexcept {
    if (!frame.load || !LevelValid(frame.level)) return;
    const auto address = reinterpret_cast<std::uintptr_t>(stream);
    std::uint32_t cursor{}, begin{}, end{}, error{};
    if (!Read(address, cursor) || !Read(address + 4, begin) || !Read(address + 8, end) ||
        !Read(address + 12, error) || error != 0 || cursor != begin || end < begin ||
        end - begin > kMaxContentBytes) return;
    try {
        std::vector<std::byte> bytes(end - begin);
        if (Copy(begin, bytes.data(), bytes.size())) {
            frame.snapshot = Content(bytes, frame.level, frame.state);
            frame.stream = address;
            frame.bufferBegin = begin; frame.bufferEnd = end;
        }
    } catch (...) { frame.snapshot.reset(); }
}
std::uint8_t __fastcall OpenDetour(void* stream, void*, const char* path) {
    const auto status = reinterpret_cast<NativeOpen>(gBase + 0x46F610U)(stream, path);
    if (auto* frame = ActiveFrame(); frame && status) SnapshotStream(*frame, stream);
    return status;
}
void ConsumedStream(Frame& frame, void* stream) noexcept {
    if (frame.load && frame.snapshot && frame.stream == reinterpret_cast<std::uintptr_t>(stream)) {
        std::uint32_t cursor{}, begin{}, end{}, error{};
        frame.complete = Read(frame.stream, cursor) && Read(frame.stream + 8, end) &&
            Read(frame.stream + 4, begin) && begin == frame.bufferBegin && end == frame.bufferEnd &&
            Read(frame.stream + 12, error) && error == 0 && cursor == end;
        // Confirm the buffer consumed was still this exact original snapshot
        // before its native destructor frees it. Runtime arrays are never hashed.
        std::array<std::byte, 4096> chunk{};
        for (std::size_t at = 0; frame.complete && at < frame.snapshot->bytes.size(); at += chunk.size()) {
            const auto count = (std::min)(chunk.size(), frame.snapshot->bytes.size() - at);
            frame.complete = Copy(std::uintptr_t(begin) + at, chunk.data(), count) &&
                std::equal(chunk.begin(), chunk.begin() + count, frame.snapshot->bytes.begin() + at);
        }
    }
}
void __fastcall CloseDetour(void* stream, void*) {
    if (auto* frame = ActiveFrame()) ConsumedStream(*frame, stream);
    reinterpret_cast<NativeClose>(gBase + 0x463770U)(stream);
}
bool EmptyForLoad(std::uintptr_t world) noexcept {
    std::uint32_t value{}; std::uint8_t flags{};
    if (!Read(world + 0x23F4, flags) || (flags & 1)) return false;
    for (const auto offset : {0x23E8U, 0x23F0U, 0x23F8U, 0x23FCU})
        if (!Read(world + offset, value) || value) return false;
    return true;
}
void __fastcall LoadDetour(void* world, void*, std::uint32_t state) {
    const auto original = reinterpret_cast<NativeLoad>(gLoadOriginal.load(std::memory_order_acquire));
    if (!original) return;
    const auto address = reinterpret_cast<std::uintptr_t>(world);
    const auto at = gDepth++;
    struct Scope {
        std::size_t at;
        ~Scope() { --gDepth; if (at < gFrames.size()) gFrames[at] = {}; }
    } scope{at};
    if (at >= gFrames.size()) { Revoke(address); original(world, state); return; }
    auto& frame = gFrames[at]; frame = {}; frame.world = address; frame.state = state;
    // Early-return calls preserve an existing binding, but cannot inherit an
    // outer frame. The retail loader owns its new allocations on THIS thread;
    // observing them must not depend on a dispatcher certified only later.
    if (EmptyForLoad(address)) {
        Revoke(address);
        frame.load = Begin(address);
    }
    original(world, state);
    if (frame.load && frame.complete)
        static_cast<void>(Commit(address, frame.load, frame.snapshot));
    else if (frame.load) Revoke(address);
}
void __fastcall FreeDetour(void* world, void*) {
    Revoke(reinterpret_cast<std::uintptr_t>(world));
    if (const auto original = reinterpret_cast<NativeWorld>(gFreeOriginal.load(std::memory_order_acquire))) original(world);
}
void __fastcall DestroyDetour(void* world, void*) {
    Revoke(reinterpret_cast<std::uintptr_t>(world));
    if (const auto original = reinterpret_cast<NativeWorld>(gDestroyOriginal.load(std::memory_order_acquire))) original(world);
}
void __fastcall ResetDetour(void* world, void*) {
    Revoke(reinterpret_cast<std::uintptr_t>(world));
    if (const auto original = reinterpret_cast<NativeWorld>(gResetOriginal.load(std::memory_order_acquire))) original(world);
}
Result Context(std::uintptr_t base, std::uintptr_t& world, std::uint64_t generation,
    Entry*& entry) noexcept {
    if (!gInstalled || gRetired || base != gBase) return Result::NativeLoaderBindingRequired;
    if (!gEngineThread || !gEngineThread()) return Result::WrongThread;
    if (!generation || generation < gGeneration) return Result::InvalidGeneration;
    std::uint32_t root{}, current{};
    if (!Read(base + 0x12F0C88U, root) || !root || !Read(root + 0x20, current) || !current ||
        (world && world != current)) return Result::WrongWorld;
    world = current;
    if (gGenerationWorld && world != gGenerationWorld && generation <= gGeneration) return Result::InvalidGeneration;
    gGeneration = generation; gGenerationWorld = world;
    entry = Find(world);
    if (!entry || entry->loading || !entry->snapshot) return Result::MissingResource;
    return Result::Ok;
}
Result ResolveLocked(std::uintptr_t base, std::uintptr_t world, std::uint64_t generation,
    const Key& key, Mapping& output) noexcept {
    output = {};
    Entry* entry{};
    const auto context = Context(base, world, generation, entry);
    if (context != Result::Ok) return context;
    if (Validate(key) != Result::Ok) return Result::InvalidKey;
    const auto& snapshot = *entry->snapshot;
    if (key.contentSha256 != snapshot.digest) return Result::ContentMismatch;
    if (key.coverOrdinal > snapshot.covers.size()) return Result::InvalidOrdinal;
    const auto& range = snapshot.covers[key.coverOrdinal - 1];
    if (key.markerOrdinal >= range.count) return Result::InvalidOrdinal;
    std::uint32_t markers{}, covers{};
    if (!Header(world, snapshot, true, markers, covers) ||
        !MarkerMatches(snapshot, markers, covers, key.coverOrdinal, key.markerOrdinal)) return Result::NativeMismatch;
    output = {key, world, generation, entry->load, static_cast<std::int32_t>(range.first + key.markerOrdinal)};
    return Result::Ok;
}
} // namespace

Result Capture(std::uintptr_t base, std::uintptr_t world, std::uint64_t generation,
    std::int32_t marker, Key& output) noexcept {
    output = {};
    const std::lock_guard lock(gGate);
    Entry* entry{};
    const auto context = Context(base, world, generation, entry);
    if (context != Result::Ok) return context;
    const auto& snapshot = *entry->snapshot;
    if (marker < 0 || static_cast<std::uint32_t>(marker) >= snapshot.markers) return Result::InvalidOrdinal;
    std::uint32_t markers{}, covers{};
    if (!Header(world, snapshot, true, markers, covers)) return Result::NativeMismatch;
    for (std::uint32_t i = 0; i < snapshot.covers.size(); ++i) {
        const auto& range = snapshot.covers[i];
        const auto index = static_cast<std::uint32_t>(marker);
        if (index < range.first || index - range.first >= range.count) continue;
        if (!MarkerMatches(snapshot, markers, covers, i + 1, index - range.first)) return Result::NativeMismatch;
        output = {snapshot.digest, i + 1, index - range.first};
        return Result::Ok;
    }
    return Result::InvalidOrdinal;
}
Result Resolve(std::uintptr_t base, std::uintptr_t world, std::uint64_t generation,
    const Key& key, Mapping& output) noexcept {
    const std::lock_guard lock(gGate);
    return ResolveLocked(base, world, generation, key, output);
}
Result Resolve(std::uintptr_t base, std::uintptr_t world, std::uint64_t generation,
    const Key& key, std::int32_t& marker) noexcept {
    marker = -1; Mapping output{};
    const auto result = Resolve(base, world, generation, key, output);
    if (result == Result::Ok) marker = output.marker;
    return result;
}
Result Revalidate(std::uintptr_t base, const Mapping& mapping) noexcept {
    const std::lock_guard lock(gGate);
    Mapping current{};
    const auto result = ResolveLocked(base, mapping.world, mapping.generation, mapping.key, current);
    if (result != Result::Ok) return result;
    return current.load == mapping.load && current.marker == mapping.marker ? Result::Ok : Result::StaleBinding;
}
Result UseResolved(std::uintptr_t base, std::uintptr_t world, std::uint64_t generation,
    const Key& key, ResolvedUse use, void* context) noexcept {
    const std::lock_guard lock(gGate);
    Mapping mapping{};
    const auto result = ResolveLocked(base, world, generation, key, mapping);
    if (result != Result::Ok) return result;
    return use && use(mapping, context) ? Result::Ok : Result::NativeUseRejected;
}
void Retire() noexcept {
    const std::lock_guard lock(gGate);
    gRetired = true;
    for (auto& entry : gEntries) { entry.snapshot.reset(); entry.loading = false; }
}
bool Install(std::uintptr_t base, HookManager& hooks, EngineThread engine, std::string& error) {
    if (gInstalled || !base || !engine || !hooks.ExactBuildValidated() || !hooks.MutationAllowed()) {
        error = "Marker identity requires an unused exact-build hook manager and engine-thread guard"; return false;
    }
    gBase = base; gEngineThread = engine; gRetired = false;
    const auto before = hooks.InstalledCount();
    const auto hook = [&](const char* name, std::uintptr_t rva, void* detour,
        std::vector<std::uint8_t> expected, void** trampoline, std::atomic<void*>* published) {
        HookRequest request{};
        request.name = name; request.target = reinterpret_cast<void*>(base + rva); request.detour = detour;
        request.patchLength = expected.size(); request.expected.bytes = std::move(expected);
        request.expected.mask.assign(request.patchLength, 'x'); request.relocationSafePrefix = true;
        request.trampolineOut = trampoline; request.publishedTrampolineOut = published;
        return hooks.Install(request, error);
    };
    const auto redirect = [&](const char* name, std::uintptr_t site, std::uintptr_t target, void* detour) {
        return hooks.InstallCallRedirect({name, reinterpret_cast<void*>(base + site),
            reinterpret_cast<void*>(base + target), detour}, error);
    };
    const auto handler = static_cast<std::uint32_t>(base + 0xD2B6B4U);
    // All prologues are whole instructions with no relative operands. The
    // destructor SEH handler immediate is rebased and checked byte-for-byte.
    if (!hook("AISync marker pre-free", 0x341C10U, reinterpret_cast<void*>(&FreeDetour),
            {0x53,0x56,0x8B,0xF1,0x57}, &gFreeTrampoline, &gFreeOriginal) ||
        !hook("AISync marker destructor", 0x33E350U, reinterpret_cast<void*>(&DestroyDetour),
            {0x6A,0xFF,0x68,static_cast<std::uint8_t>(handler),static_cast<std::uint8_t>(handler >> 8),
                static_cast<std::uint8_t>(handler >> 16),static_cast<std::uint8_t>(handler >> 24)},
            &gDestroyTrampoline, &gDestroyOriginal) ||
        !hook("AISync marker reset", 0x33E2E0U, reinterpret_cast<void*>(&ResetDetour),
            {0x8B,0xC1,0x56,0x8D,0x90,0x08,0x21,0,0}, &gResetTrampoline, &gResetOriginal) ||
        !hook("AISync marker loader", 0x343CA0U, reinterpret_cast<void*>(&LoadDetour),
            {0x55,0x8B,0xEC,0x83,0xE4,0xF0}, &gLoadTrampoline, &gLoadOriginal) ||
        !redirect("AISync marker path", 0x343D1BU, 0x38BEB0U, reinterpret_cast<void*>(&PathDetour)) ||
        !redirect("AISync marker consumed stream", 0x343D5AU, 0x46F610U, reinterpret_cast<void*>(&OpenDetour)) ||
        !redirect("AISync marker consumed end", 0x3444A2U, 0x463770U, reinterpret_cast<void*>(&CloseDetour))) {
        Retire(); std::string rollback;
        if (!hooks.UninstallFrom(before, rollback)) error += "; " + rollback;
        return false;
    }
    { const std::lock_guard lock(gGate); gInstalled = true; }
    return true;
}
#ifdef AIMP_MARKER_IDENTITY_TESTING
namespace testing {
void Configure(std::uintptr_t base, EngineThread engine) noexcept {
    const std::lock_guard lock(gGate);
    gBase = base; gEngineThread = engine; gInstalled = base != 0; gRetired = false;
    gGeneration = 0; gGenerationWorld = 0;
    for (auto& entry : gEntries) entry = {};
}
bool Load(std::uintptr_t world, std::uint32_t state,
    const std::array<char, kMaxLevelBytes>& level, std::span<const std::byte> bytes,
    std::uint32_t consumedBytes, std::uint32_t streamError) noexcept {
    Revoke(world);
    Frame frame{}; frame.world = world; frame.state = state; frame.level = level; frame.load = Begin(world);
    const auto begin = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(bytes.data()));
    std::array<std::uint32_t, 5> stream{begin, begin, begin + static_cast<std::uint32_t>(bytes.size()), 0, 1};
    SnapshotStream(frame, stream.data());
    stream[0] += consumedBytes == UINT32_MAX ? static_cast<std::uint32_t>(bytes.size()) : consumedBytes;
    stream[3] = streamError;
    ConsumedStream(frame, stream.data());
    if (!frame.complete) { Revoke(world); return false; }
    return Commit(world, frame.load, frame.snapshot);
}
void Unload(std::uintptr_t world) noexcept { Revoke(world); }
}
#endif
} // namespace aimp::ai_sync::marker_identity
