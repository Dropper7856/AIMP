#include "aimp/ExperimentalProtocol.h"
#include "aimp/NativeLimits.h"
#include "aimp/WorldPublicActions.h"
#include "aimp/PlayerTorchRuntime.h"
#include "aimp/PlayerHidingRuntime.h"
#include "aimp/PlayerCharacters.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cctype>
#include <limits>
#include <map>
#include <sstream>
#include <utility>

namespace aimp::experimental {
namespace {

enum class JsonType : std::uint8_t {
    String,
    Unsigned,
    Number,
    Boolean,
    Object,
};

struct JsonValue {
    JsonType type{JsonType::String};
    std::string stringValue;
    std::uint64_t unsignedValue{};
    double numberValue{};
    bool boolValue{};
    std::map<std::string, JsonValue, std::less<>> objectValue;
};

class Parser final {
public:
    explicit Parser(const std::span<const std::byte> payload) {
        text_.reserve(payload.size());
        for (const auto byte : payload) {
            text_.push_back(
                static_cast<char>(std::to_integer<std::uint8_t>(byte)));
        }
    }

    bool ParseObject(JsonValue& output) {
        Skip();
        if (!ParseValue(output, 0) ||
            output.type != JsonType::Object) {
            return false;
        }
        Skip();
        return cursor_ == text_.size();
    }

private:
    static bool AppendUtf8(
        std::string& output,
        const std::uint32_t codePoint) {
        if (codePoint <= 0x7FU) {
            output.push_back(static_cast<char>(codePoint));
        } else if (codePoint <= 0x7FFU) {
            output.push_back(static_cast<char>(
                0xC0U | (codePoint >> 6U)));
            output.push_back(static_cast<char>(
                0x80U | (codePoint & 0x3FU)));
        } else if (codePoint <= 0xFFFFU) {
            if (codePoint >= 0xD800U && codePoint <= 0xDFFFU) {
                return false;
            }
            output.push_back(static_cast<char>(
                0xE0U | (codePoint >> 12U)));
            output.push_back(static_cast<char>(
                0x80U | ((codePoint >> 6U) & 0x3FU)));
            output.push_back(static_cast<char>(
                0x80U | (codePoint & 0x3FU)));
        } else if (codePoint <= 0x10FFFFU) {
            output.push_back(static_cast<char>(
                0xF0U | (codePoint >> 18U)));
            output.push_back(static_cast<char>(
                0x80U | ((codePoint >> 12U) & 0x3FU)));
            output.push_back(static_cast<char>(
                0x80U | ((codePoint >> 6U) & 0x3FU)));
            output.push_back(static_cast<char>(
                0x80U | (codePoint & 0x3FU)));
        } else {
            return false;
        }
        return true;
    }

    void Skip() noexcept {
        while (cursor_ < text_.size() &&
               (text_[cursor_] == ' ' ||
                text_[cursor_] == '\t' ||
                text_[cursor_] == '\r' ||
                text_[cursor_] == '\n')) {
            ++cursor_;
        }
    }

    bool Consume(const char value) noexcept {
        if (cursor_ >= text_.size() || text_[cursor_] != value) {
            return false;
        }
        ++cursor_;
        return true;
    }

    bool Hex4(std::uint16_t& output) noexcept {
        if (cursor_ + 4 > text_.size()) {
            return false;
        }
        output = 0;
        for (std::size_t index = 0; index < 4; ++index) {
            const auto value =
                static_cast<unsigned char>(text_[cursor_++]);
            std::uint16_t digit = 0;
            if (value >= '0' && value <= '9') {
                digit = static_cast<std::uint16_t>(value - '0');
            } else if (value >= 'a' && value <= 'f') {
                digit = static_cast<std::uint16_t>(value - 'a' + 10);
            } else if (value >= 'A' && value <= 'F') {
                digit = static_cast<std::uint16_t>(value - 'A' + 10);
            } else {
                return false;
            }
            output = static_cast<std::uint16_t>((output << 4U) | digit);
        }
        return true;
    }

    bool String(std::string& output) {
        if (!Consume('"')) {
            return false;
        }
        output.clear();
        while (cursor_ < text_.size()) {
            const auto value =
                static_cast<unsigned char>(text_[cursor_++]);
            if (value == '"') {
                return true;
            }
            if (value < 0x20U) {
                return false;
            }
            if (value != '\\') {
                output.push_back(static_cast<char>(value));
                continue;
            }
            if (cursor_ >= text_.size()) {
                return false;
            }
            const auto escape = text_[cursor_++];
            switch (escape) {
            case '"':
            case '\\':
            case '/':
                output.push_back(escape);
                break;
            case 'b':
                output.push_back('\b');
                break;
            case 'f':
                output.push_back('\f');
                break;
            case 'n':
                output.push_back('\n');
                break;
            case 'r':
                output.push_back('\r');
                break;
            case 't':
                output.push_back('\t');
                break;
            case 'u': {
                std::uint16_t first = 0;
                if (!Hex4(first)) {
                    return false;
                }
                std::uint32_t codePoint = first;
                if (first >= 0xD800U && first <= 0xDBFFU) {
                    if (cursor_ + 2 > text_.size() ||
                        text_[cursor_] != '\\' ||
                        text_[cursor_ + 1] != 'u') {
                        return false;
                    }
                    cursor_ += 2;
                    std::uint16_t second = 0;
                    if (!Hex4(second) ||
                        second < 0xDC00U ||
                        second > 0xDFFFU) {
                        return false;
                    }
                    codePoint =
                        0x10000U +
                        ((static_cast<std::uint32_t>(first) -
                          0xD800U) << 10U) +
                        (static_cast<std::uint32_t>(second) -
                         0xDC00U);
                } else if (first >= 0xDC00U && first <= 0xDFFFU) {
                    return false;
                }
                if (!AppendUtf8(output, codePoint)) {
                    return false;
                }
                break;
            }
            default:
                return false;
            }
        }
        return false;
    }

    bool Number(JsonValue& output) {
        const auto begin = cursor_;
        if (cursor_ < text_.size() && text_[cursor_] == '-') {
            ++cursor_;
        }
        if (cursor_ >= text_.size()) {
            return false;
        }
        if (text_[cursor_] == '0') {
            ++cursor_;
            if (cursor_ < text_.size() &&
                std::isdigit(
                    static_cast<unsigned char>(text_[cursor_]))) {
                return false;
            }
        } else {
            if (!std::isdigit(
                    static_cast<unsigned char>(text_[cursor_]))) {
                return false;
            }
            while (cursor_ < text_.size() &&
                   std::isdigit(
                       static_cast<unsigned char>(text_[cursor_]))) {
                ++cursor_;
            }
        }
        bool integer = true;
        if (cursor_ < text_.size() && text_[cursor_] == '.') {
            integer = false;
            ++cursor_;
            const auto fraction = cursor_;
            while (cursor_ < text_.size() &&
                   std::isdigit(
                       static_cast<unsigned char>(text_[cursor_]))) {
                ++cursor_;
            }
            if (fraction == cursor_) {
                return false;
            }
        }
        if (cursor_ < text_.size() &&
            (text_[cursor_] == 'e' || text_[cursor_] == 'E')) {
            integer = false;
            ++cursor_;
            if (cursor_ < text_.size() &&
                (text_[cursor_] == '+' || text_[cursor_] == '-')) {
                ++cursor_;
            }
            const auto exponent = cursor_;
            while (cursor_ < text_.size() &&
                   std::isdigit(
                       static_cast<unsigned char>(text_[cursor_]))) {
                ++cursor_;
            }
            if (exponent == cursor_) {
                return false;
            }
        }

        const auto* first = text_.data() + begin;
        const auto* last = text_.data() + cursor_;
        if (integer && text_[begin] != '-') {
            std::uint64_t value = 0;
            const auto conversion = std::from_chars(first, last, value);
            if (conversion.ec == std::errc{} &&
                conversion.ptr == last) {
                output.type = JsonType::Unsigned;
                output.unsignedValue = value;
                output.numberValue = static_cast<double>(value);
                return true;
            }
        }
        double value = 0.0;
        const auto conversion = std::from_chars(first, last, value);
        if (conversion.ec != std::errc{} ||
            conversion.ptr != last ||
            !std::isfinite(value)) {
            return false;
        }
        output.type = JsonType::Number;
        output.numberValue = value;
        return true;
    }

    bool ParseValue(JsonValue& output, const std::uint32_t depth) {
        if (depth > 4) {
            return false;
        }
        Skip();
        if (cursor_ >= text_.size()) {
            return false;
        }
        if (text_[cursor_] == '"') {
            output.type = JsonType::String;
            return String(output.stringValue);
        }
        if (text_[cursor_] == '{') {
            output.type = JsonType::Object;
            output.objectValue.clear();
            ++cursor_;
            Skip();
            if (Consume('}')) {
                return true;
            }
            for (;;) {
                std::string key;
                if (!String(key)) {
                    return false;
                }
                Skip();
                if (!Consume(':')) {
                    return false;
                }
                JsonValue value;
                if (!ParseValue(value, depth + 1) ||
                    output.objectValue.contains(key)) {
                    return false;
                }
                output.objectValue.emplace(
                    std::move(key),
                    std::move(value));
                Skip();
                if (Consume('}')) {
                    return true;
                }
                if (!Consume(',')) {
                    return false;
                }
                Skip();
            }
        }
        if (text_.compare(cursor_, 4, "true") == 0) {
            cursor_ += 4;
            output.type = JsonType::Boolean;
            output.boolValue = true;
            return true;
        }
        if (text_.compare(cursor_, 5, "false") == 0) {
            cursor_ += 5;
            output.type = JsonType::Boolean;
            output.boolValue = false;
            return true;
        }
        return Number(output);
    }

    std::string text_;
    std::size_t cursor_{};
};

const JsonValue* Field(
    const JsonValue& object,
    const std::string_view key,
    const JsonType type) noexcept {
    if (object.type != JsonType::Object) {
        return nullptr;
    }
    const auto found = object.objectValue.find(key);
    if (found == object.objectValue.end() ||
        found->second.type != type) {
        return nullptr;
    }
    return &found->second;
}

bool Guid(const std::string& value) noexcept {
    if (value.size() != 36 ||
        value[8] != '-' ||
        value[13] != '-' ||
        value[18] != '-' ||
        value[23] != '-') {
        return false;
    }
    bool nonZero = false;
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (index == 8 || index == 13 ||
            index == 18 || index == 23) {
            continue;
        }
        const auto character =
            static_cast<unsigned char>(value[index]);
        if (!std::isxdigit(character)) {
            return false;
        }
        nonZero = nonZero || character != '0';
    }
    return nonZero;
}

bool PlayerName(const std::string& value) noexcept {
    if (value.empty() ||
        value.size() > 96 ||
        std::isspace(static_cast<unsigned char>(value.front())) ||
        std::isspace(static_cast<unsigned char>(value.back()))) {
        return false;
    }
    std::size_t cursor = 0;
    std::size_t codePoints = 0;
    while (cursor < value.size()) {
        const auto first = static_cast<std::uint8_t>(value[cursor]);
        std::uint32_t codePoint = 0;
        std::size_t continuation = 0;
        if (first <= 0x7FU) {
            codePoint = first;
        } else if ((first & 0xE0U) == 0xC0U) {
            codePoint = first & 0x1FU;
            continuation = 1;
        } else if ((first & 0xF0U) == 0xE0U) {
            codePoint = first & 0x0FU;
            continuation = 2;
        } else if ((first & 0xF8U) == 0xF0U) {
            codePoint = first & 0x07U;
            continuation = 3;
        } else {
            return false;
        }
        if (cursor + continuation >= value.size()) {
            return false;
        }
        for (std::size_t index = 1; index <= continuation; ++index) {
            const auto next =
                static_cast<std::uint8_t>(value[cursor + index]);
            if ((next & 0xC0U) != 0x80U) {
                return false;
            }
            codePoint = (codePoint << 6U) | (next & 0x3FU);
        }
        if ((continuation == 1 && codePoint < 0x80U) ||
            (continuation == 2 && codePoint < 0x800U) ||
            (continuation == 3 && codePoint < 0x10000U) ||
            codePoint < 0x20U ||
            (codePoint >= 0x7FU && codePoint <= 0x9FU) ||
            codePoint > 0x10FFFFU ||
            (codePoint >= 0xD800U && codePoint <= 0xDFFFU) ||
            (codePoint >= 0x200BU && codePoint <= 0x200FU) ||
            (codePoint >= 0x2028U && codePoint <= 0x202EU) ||
            (codePoint >= 0x2060U && codePoint <= 0x206FU) ||
            (codePoint >= 0xE000U && codePoint <= 0xF8FFU)) {
            return false;
        }
        cursor += continuation + 1;
        ++codePoints;
        if (codePoints > 24) {
            return false;
        }
    }
    return true;
}

bool CommandText(
    const std::string& value,
    const bool allowLineBreaks) noexcept {
    if (value.size() > 63U) {
        return false;
    }
    for (const auto character : value) {
        const auto byte = static_cast<unsigned char>(character);
        if (byte == 0U || byte == 0x7FU ||
            (byte < 0x20U &&
             !(allowLineBreaks &&
               (byte == '\r' || byte == '\n' || byte == '\t')))) {
            return false;
        }
    }
    return true;
}

bool Envelope(
    const JsonValue& root,
    RequestEnvelope& output) noexcept {
    const auto* schema = Field(root, "schemaVersion", JsonType::Unsigned);
    const auto* request = Field(root, "requestId", JsonType::String);
    const auto* generation =
        Field(root, "levelGeneration", JsonType::Unsigned);
    const auto* dummy =
        Field(root, "portableDummyId", JsonType::String);
    if (schema == nullptr || request == nullptr ||
        generation == nullptr || dummy == nullptr ||
        schema->unsignedValue != kSchemaVersion ||
        generation->unsignedValue >
            static_cast<std::uint64_t>(
                (std::numeric_limits<std::int32_t>::max)()) ||
        !Guid(request->stringValue) ||
        !Guid(dummy->stringValue)) {
        return false;
    }
    output.requestId = request->stringValue;
    output.levelGeneration =
        static_cast<std::uint32_t>(generation->unsignedValue);
    output.portableDummyId = dummy->stringValue;
    return true;
}

bool Parse(
    const std::span<const std::byte> payload,
    JsonValue& root) {
    if (payload.empty() || payload.size() > 64U * 1024U) {
        return false;
    }
    Parser parser(payload);
    return parser.ParseObject(root);
}

std::string Escape(const std::string_view value) {
    std::ostringstream output;
    output << '"';
    for (const auto character : value) {
        switch (character) {
        case '"':
            output << "\\\"";
            break;
        case '\\':
            output << "\\\\";
            break;
        case '\b':
            output << "\\b";
            break;
        case '\f':
            output << "\\f";
            break;
        case '\n':
            output << "\\n";
            break;
        case '\r':
            output << "\\r";
            break;
        case '\t':
            output << "\\t";
            break;
        default:
            if (static_cast<unsigned char>(character) < 0x20U) {
                output << "\\u00";
                static constexpr char kHex[] = "0123456789abcdef";
                output << kHex[
                    (static_cast<unsigned char>(character) >> 4U) & 0xFU];
                output << kHex[
                    static_cast<unsigned char>(character) & 0xFU];
            } else {
                output << character;
            }
            break;
        }
    }
    output << '"';
    return output.str();
}

std::string BoundedGameCommandMessage(const std::string_view value,const std::size_t maximumBytes=1000U) {
    // Launcher ModelValidation caps GameCommandResult.Message at 1024 UTF-16 code units.
    // Capping the UTF-8 payload to 1000 bytes is stricter and leaves headroom while
    // guaranteeing that diagnostics can never invalidate the IPC frame.
    const auto kMaximumBytes=maximumBytes;
    constexpr std::string_view kSuffix = "...[truncated]";
    if (value.size() <= kMaximumBytes) {
        return std::string(value);
    }

    std::size_t prefixBytes = kMaximumBytes - kSuffix.size();
    while (prefixBytes > 0U && prefixBytes < value.size() &&
           (static_cast<unsigned char>(value[prefixBytes]) & 0xC0U) == 0x80U) {
        --prefixBytes;
    }

    std::string output(value.substr(0U, prefixBytes));
    output.append(kSuffix);
    return output;
}

void Prefix(
    std::ostringstream& output,
    const RequestEnvelope& request) {
    output << "{\"schemaVersion\":" << kSchemaVersion
           << ",\"requestId\":" << Escape(request.requestId)
           << ",\"levelGeneration\":" << request.levelGeneration
           << ",\"portableDummyId\":" <<
        Escape(request.portableDummyId);
}

void StringArray(
    std::ostringstream& output,
    const std::vector<std::string>& values) {
    output << '[';
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index != 0) {
            output << ',';
        }
        output << Escape(values[index]);
    }
    output << ']';
}

std::vector<std::byte> Bytes(const std::string& json) {
    const auto* begin =
        reinterpret_cast<const std::byte*>(json.data());
    return {begin, begin + json.size()};
}

} // namespace

bool IsResolutionMode(const std::string_view value) noexcept {
    return value == "ExactProfile" ||
        value == "LocalDiscoveredProfile" ||
        value == "GenericDiscovery";
}

bool DecodeTestModeConfigure(
    const std::span<const std::byte> payload,
    TestModeConfigureRequest& output) noexcept {
    try {
        output = {};
        JsonValue root;
        if (!Parse(payload, root) ||
            root.objectValue.size() != 5 ||
            !Envelope(root, output)) {
            return false;
        }
        const auto* enabled = Field(root, "enabled", JsonType::Boolean);
        if (enabled == nullptr) {
            return false;
        }
        output.enabled = enabled->boolValue;
        return true;
    } catch (...) {
        output = {};
        return false;
    }
}

bool DecodeCapabilityQuery(
    const std::span<const std::byte> payload,
    CapabilityQueryRequest& output) noexcept {
    try {
        output = {};
        JsonValue root;
        return Parse(payload, root) &&
            root.objectValue.size() == 4 &&
            Envelope(root, output);
    } catch (...) {
        output = {};
        return false;
    }
}

bool DecodeDummyCreate(
    const std::span<const std::byte> payload,
    DummyCreateRequest& output) noexcept {
    try {
        output = {};
        JsonValue root;
        if (!Parse(payload, root) ||
            root.objectValue.size() != 6 ||
            !Envelope(root, output)) {
            return false;
        }
        const auto* identity = Field(root, "identity", JsonType::Object);
        const auto* offset = Field(root, "spawnOffset", JsonType::Object);
        if (identity == nullptr || offset == nullptr ||
            identity->objectValue.size() != 5 ||
            offset->objectValue.size() != 3) {
            return false;
        }
        const auto* dummy =
            Field(*identity, "portableDummyId", JsonType::String);
        const auto* peer =
            Field(*identity, "peerId", JsonType::Unsigned);
        const auto* launcher =
            Field(*identity, "launcherPlayerId", JsonType::String);
        const auto* binding =
            Field(*identity, "portableBindingId", JsonType::String);
        const auto* name =
            Field(*identity, "displayName", JsonType::String);
        const auto number = [&](const char* key, float& value) {
            const auto found = offset->objectValue.find(key);
            if (found == offset->objectValue.end() ||
                (found->second.type != JsonType::Unsigned &&
                 found->second.type != JsonType::Number)) {
                return false;
            }
            const auto numeric =
                found->second.type == JsonType::Unsigned
                    ? static_cast<double>(found->second.unsignedValue)
                    : found->second.numberValue;
            if (!std::isfinite(numeric) ||
                std::abs(numeric) > 10.0) {
                return false;
            }
            value = static_cast<float>(numeric);
            return true;
        };
        if (dummy == nullptr || peer == nullptr ||
            launcher == nullptr || binding == nullptr ||
            name == nullptr ||
            dummy->stringValue != output.portableDummyId ||
            peer->unsignedValue == 0 ||
            !Guid(dummy->stringValue) ||
            !Guid(launcher->stringValue) ||
            !Guid(binding->stringValue) ||
            !PlayerName(name->stringValue) ||
            !number("x", output.offsetX) ||
            !number("y", output.offsetY) ||
            !number("z", output.offsetZ)) {
            return false;
        }
        output.identity.portableDummyId = dummy->stringValue;
        output.identity.peerId = peer->unsignedValue;
        output.identity.launcherPlayerId = launcher->stringValue;
        output.identity.portableBindingId = binding->stringValue;
        output.identity.displayName = name->stringValue;
        return true;
    } catch (...) {
        output = {};
        return false;
    }
}

bool DecodeDummyRemove(
    const std::span<const std::byte> payload,
    DummyRemoveRequest& output) noexcept {
    try {
        output = {};
        JsonValue root;
        return Parse(payload, root) &&
            root.objectValue.size() == 4 &&
            Envelope(root, output);
    } catch (...) {
        output = {};
        return false;
    }
}

bool DecodeDummyMove(
    const std::span<const std::byte> payload,
    DummyMoveRequest& output) noexcept {
    try {
        output = {};
        JsonValue root;
        if (!Parse(payload, root) ||
            root.objectValue.size() != 7 ||
            !Envelope(root, output)) {
            return false;
        }
        const auto* command = Field(root, "command", JsonType::String);
        const auto* duration =
            Field(root, "durationMilliseconds", JsonType::Unsigned);
        const auto amount = root.objectValue.find("amount");
        if (command == nullptr || duration == nullptr ||
            amount == root.objectValue.end() ||
            (amount->second.type != JsonType::Unsigned &&
             amount->second.type != JsonType::Number) ||
            duration->unsignedValue > 10000) {
            return false;
        }
        static constexpr std::string_view kCommands[]{
            "Forward",
            "Backward",
            "RotateLeft",
            "RotateRight",
            "NearLocalPlayer",
            "MovementTest",
        };
        if (std::find(
                std::begin(kCommands),
                std::end(kCommands),
                command->stringValue) == std::end(kCommands)) {
            return false;
        }
        const auto numeric =
            amount->second.type == JsonType::Unsigned
                ? static_cast<double>(amount->second.unsignedValue)
                : amount->second.numberValue;
        if (!std::isfinite(numeric) || std::abs(numeric) > 180.0) {
            return false;
        }
        output.command = command->stringValue;
        output.amount = static_cast<float>(numeric);
        output.durationMilliseconds =
            static_cast<std::uint32_t>(duration->unsignedValue);
        return true;
    } catch (...) {
        output = {};
        return false;
    }
}

bool DecodeDiagnosticSnapshotRequest(
    const std::span<const std::byte> payload,
    DiagnosticSnapshotRequest& output) noexcept {
    try {
        output = {};
        JsonValue root;
        if (!Parse(payload, root) ||
            root.objectValue.size() != 5 ||
            !Envelope(root, output)) {
            return false;
        }
        const auto* flush = Field(root, "flush", JsonType::Boolean);
        if (flush == nullptr) {
            return false;
        }
        output.flush = flush->boolValue;
        return true;
    } catch (...) {
        output = {};
        return false;
    }
}

bool DecodeGameCommand(
    const std::span<const std::byte> payload,
    GameCommandRequest& output) noexcept {
    try {
        output = {};
        JsonValue root;
        if (!Parse(payload, root) ||
            root.objectValue.size() != 6U ||
            !Envelope(root, output)) {
            return false;
        }
        const auto* command = Field(root, "command", JsonType::String);
        const auto* arguments = Field(root, "arguments", JsonType::Object);
        if (command == nullptr || arguments == nullptr) {
            return false;
        }

        // Internal process setting on the existing command channel. It is
        // independent of a level and has an explicit, strictly parsed ON/OFF.
        if (command->stringValue == "WORLD_WS3_EMPTY_CALLBACK_SET") {
            if (output.levelGeneration != 0U || arguments->objectValue.size() != 1U)
                return false;
            const auto* enabled = Field(*arguments, "enabled", JsonType::String);
            if (enabled == nullptr || (enabled->stringValue != "0" && enabled->stringValue != "1"))
                return false;
            output.command = command->stringValue;
            output.worldSync3EmptyCallbackEnabled = enabled->stringValue == "1";
            return true;
        }

        if (command->stringValue == "SESSION_LOADING_HOLD") {
            if (output.levelGeneration != 0U || arguments->objectValue.size() != 3U) return false;
            const auto* active = Field(*arguments, "active", JsonType::String);
            const auto* session = Field(*arguments, "sessionId", JsonType::String);
            const auto* generation = Field(*arguments, "generation", JsonType::String);
            if (active == nullptr || session == nullptr || generation == nullptr ||
                (active->stringValue != "0" && active->stringValue != "1") ||
                session->stringValue.size() != 36U) return false;
            for (std::size_t i = 0U; i < 36U; ++i) {
                const char c = session->stringValue[i];
                if (i == 8U || i == 13U || i == 18U || i == 23U) { if (c != '-') return false; }
                else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
            }
            const auto& text = generation->stringValue;
            const auto parsed = std::from_chars(text.data(), text.data() + text.size(), output.sessionLoadingGeneration);
            if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
                output.sessionLoadingGeneration == 0U) return false;
            output.command = command->stringValue;
            output.sessionLoadingActive = active->stringValue == "1";
            output.sessionLoadingSessionId = session->stringValue;
            return true;
        }

        if (command->stringValue == "SESSION_ROUND_BOUNDARY") {
            if (output.levelGeneration != 0U || arguments->objectValue.size() != 1U) return false;
            const auto* session = Field(*arguments, "sessionId", JsonType::String);
            if (session == nullptr || session->stringValue.size() != 36U) return false;
            for (std::size_t i = 0U; i < 36U; ++i) {
                const char c = session->stringValue[i];
                if (i == 8U || i == 13U || i == 18U || i == 23U) { if (c != '-') return false; }
                else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
            }
            output.command = command->stringValue;
            output.sessionLoadingSessionId = session->stringValue;
            return true;
        }

        if (command->stringValue == "PERFORMANCE_ISOLATION_SET") {
            if (output.levelGeneration != 0U || arguments->objectValue.size() != 2U)
                return false;
            const auto* module = Field(*arguments, "module", JsonType::String);
            const auto* enabled = Field(*arguments, "enabled", JsonType::String);
            if (module == nullptr || enabled == nullptr ||
                (enabled->stringValue != "0" && enabled->stringValue != "1"))
                return false;
            constexpr std::array<std::string_view, 8U> kModules{
                "player-sync-observer", "remote-movement-runtime",
                "worldsync-hooks", "character-lifecycle", "engine-evidence",
                "native-diagnostics", "locomotion-flight", "game-thread-pumps"};
            if (std::find(kModules.begin(), kModules.end(),
                    std::string_view(module->stringValue)) == kModules.end())
                return false;
            output.command = command->stringValue;
            output.performanceIsolationModule = module->stringValue;
            output.performanceIsolationEnabled = enabled->stringValue == "1";
            return true;
        }




        if (command->stringValue == "WORLD_INTERACTION_STATS" ||
            command->stringValue == "WORLD_COMPOSITE_INPUT_STATS" ||
            command->stringValue == "WORLD_STATE_DISCOVERY_BASELINE" ||
            command->stringValue == "WORLD_STATE_DISCOVERY_AFTER" ||
            command->stringValue == "WORLD_STATE_DISCOVERY_STATUS" ||
            command->stringValue == "ALIEN_PVP_PROBE" ||
            command->stringValue == "ALIEN_PVP_RELEASE" ||
            command->stringValue == "RETURN_TO_FRONTEND") {
            if (!arguments->objectValue.empty()) return false;
            output.command = command->stringValue;
            return true;
        }

        if(command->stringValue=="ALIEN_PVP_VENTS") {
            if(arguments->objectValue.size()!=1U && arguments->objectValue.size()!=3U)return false;
            if(arguments->objectValue.size()==3U) {
                const auto* go=Field(*arguments,"go",JsonType::String);
                const auto* serial=Field(*arguments,"incarnation",JsonType::String);
                if(!go||!serial||go->stringValue.empty()||go->stringValue.size()>4U||serial->stringValue.empty()||serial->stringValue.size()>20U)return false;
                unsigned index{};std::uint64_t incarnation{};
                const auto a=std::from_chars(go->stringValue.data(),go->stringValue.data()+go->stringValue.size(),index);
                const auto b=std::from_chars(serial->stringValue.data(),serial->stringValue.data()+serial->stringValue.size(),incarnation);
                if(a.ec!=std::errc{}||a.ptr!=go->stringValue.data()+go->stringValue.size()||index>=4096U||b.ec!=std::errc{}||b.ptr!=serial->stringValue.data()+serial->stringValue.size()||!incarnation)return false;
                output.alienPvPVentGo=static_cast<int>(index);output.alienPvPVentIncarnation=incarnation;
            }
            const auto* page=Field(*arguments,"page",JsonType::String);
            if(!page || page->stringValue.empty() || page->stringValue.size()>2U)return false;
            std::uint32_t parsed{};const auto* first=page->stringValue.data();const auto* last=first+page->stringValue.size();
            const auto conversion=std::from_chars(first,last,parsed,10);
            if(conversion.ec!=std::errc{} || conversion.ptr!=last || parsed>=64U)return false;
            output.command=command->stringValue;output.alienPvPVentPage=parsed;return true;
        }
        if (command->stringValue == "ALIEN_PVP_BIND") {
            if (arguments->objectValue.size() != 1U) return false;
            const auto* peer = Field(*arguments, "ownerPeer", JsonType::String);
            if (!peer || peer->stringValue.empty() || peer->stringValue.size() > 20U) return false;
            std::uint64_t parsed{};
            const auto* first = peer->stringValue.data();
            const auto* last = first + peer->stringValue.size();
            const auto conversion = std::from_chars(first,last,parsed,10);
            if (conversion.ec != std::errc{} || conversion.ptr != last || parsed == 0U) return false;
            output.command = command->stringValue;
            output.alienPvPOwnerPeer = parsed;
            return true;
        }

        if (command->stringValue == "WORLD_MANIFEST_LOAD") {
            if (arguments->objectValue.size() != 1U) return false;
            const auto* manifestPath = Field(*arguments, "manifestPath", JsonType::String);
            if (manifestPath == nullptr || manifestPath->stringValue.empty() ||
                manifestPath->stringValue.size() > 1024U ||
                manifestPath->stringValue.find('\0') != std::string::npos ||
                manifestPath->stringValue.find('\r') != std::string::npos ||
                manifestPath->stringValue.find('\n') != std::string::npos) return false;
            output.command = command->stringValue;
            output.worldManifestLoad.manifestPath = manifestPath->stringValue;
            return true;
        }

        if (command->stringValue == "ITEM_SYNC_SPAWN") {
            if (arguments->objectValue.size() != 6U) return false;
            const auto* kind = Field(*arguments, "kind", JsonType::String);
            const auto* ownerKey = Field(*arguments, "ownerKey", JsonType::String);
            const auto* spawn = Field(*arguments, "spawn", JsonType::String);
            const auto* path = Field(*arguments, "path", JsonType::String);
            const auto* quantity = Field(*arguments, "quantity", JsonType::String);
            const auto* transform = Field(*arguments, "transform", JsonType::String);
            if (kind == nullptr || ownerKey == nullptr || spawn == nullptr || path == nullptr ||
                quantity == nullptr || transform == nullptr) return false;
            auto& spawnCommand = output.itemSyncSpawn;
            if (kind->stringValue == "owner") spawnCommand.drop = false;
            else if (kind->stringValue == "drop") spawnCommand.drop = true;
            else return false;
            const auto& keyText = ownerKey->stringValue;
            if (keyText.size() != 16U) return false;
            const auto parsedKey = std::from_chars(
                keyText.data(), keyText.data() + keyText.size(), spawnCommand.ownerKey, 16);
            if (parsedKey.ec != std::errc{} || parsedKey.ptr != keyText.data() + keyText.size() ||
                spawnCommand.ownerKey == 0U) return false;
            if (spawn->stringValue == "1") spawnCommand.spawn = true;
            else if (spawn->stringValue == "0") spawnCommand.spawn = false;
            else return false;
            spawnCommand.path = path->stringValue;
            if (spawnCommand.path.size() >= 96U ||
                (spawnCommand.spawn && spawnCommand.path.empty()) ||
                (!spawnCommand.spawn && !spawnCommand.path.empty())) return false;
            for (const char character : spawnCommand.path)
                if (static_cast<unsigned char>(character) < 0x20U ||
                    static_cast<unsigned char>(character) > 0x7EU) return false;
            const auto& quantityText = quantity->stringValue;
            const auto parsedQuantity = std::from_chars(quantityText.data(),
                quantityText.data() + quantityText.size(), spawnCommand.quantity, 10);
            if (parsedQuantity.ec != std::errc{} ||
                parsedQuantity.ptr != quantityText.data() + quantityText.size() ||
                spawnCommand.quantity < 0 || spawnCommand.quantity > 100000) return false;
            // 12 invariant floats separated by ',': 3x4 row-major transform.
            const auto& floats = transform->stringValue;
            const char* cursor = floats.data();
            const char* const end = floats.data() + floats.size();
            for (std::size_t index = 0U; index < 12U; ++index) {
                if (index != 0U) {
                    if (cursor == end || *cursor != ',') return false;
                    ++cursor;
                }
                float value{};
                const auto parsed = std::from_chars(cursor, end, value);
                if (parsed.ec != std::errc{} || !std::isfinite(value) ||
                    std::fabs(value) > 100000.0F) return false;
                spawnCommand.transform[index] = value;
                cursor = parsed.ptr;
            }
            if (cursor != end) return false;
            output.command = command->stringValue;
            return true;
        }

        if (command->stringValue == "MISSION_SYNC_CONFIG") {
            if (arguments->objectValue.size() != 2U) return false;
            const auto* path = Field(*arguments, "path", JsonType::String);
            const auto* role = Field(*arguments, "role", JsonType::String);
            if (path == nullptr || role == nullptr || path->stringValue.size() > 1024U ||
                path->stringValue.find('\0') != std::string::npos ||
                (role->stringValue != "host" && role->stringValue != "client" && role->stringValue != "off") ||
                (role->stringValue != "off" && path->stringValue.empty())) return false;
            output.command = command->stringValue;
            output.missionSync.path = path->stringValue; output.missionSync.role = role->stringValue;
            return true;
        }
        if (command->stringValue == "MISSION_SYNC_APPLY") {
            if (arguments->objectValue.size() != 4U) return false;
            const auto* entity = Field(*arguments, "entityId", JsonType::String);
            const auto* port = Field(*arguments, "signal", JsonType::String);
            const auto* selector = Field(*arguments, "selector", JsonType::String);
            const auto* sequence = Field(*arguments, "sequence", JsonType::String);
            if (entity == nullptr || entity->stringValue.size() != 32U || port == nullptr ||
                selector == nullptr || sequence == nullptr) return false;
            const auto parse = []<typename T>(std::string_view text, T& value, int base) {
                const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value, base);
                return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
            };
            auto& value = output.missionSync;
            if (!parse(std::string_view(entity->stringValue).substr(0, 16), value.entityHigh, 16) ||
                !parse(std::string_view(entity->stringValue).substr(16), value.entityLow, 16) ||
                (value.entityHigh == 0U && value.entityLow == 0U) ||
                !parse(port->stringValue, value.signal, 10) || value.signal == 0U ||
                !parse(sequence->stringValue, value.sequence, 10) || value.sequence == 0U ||
                (selector->stringValue != "1" && selector->stringValue != "2")) return false;
            value.selector = selector->stringValue == "1" ? 1U : 2U;
            output.command = command->stringValue;
            return true;
        }
        if (command->stringValue == "ITEM_SYNC_APPLY") {
            if (arguments->objectValue.size() != 1U) return false;
            const auto* entityId = Field(*arguments, "entityId", JsonType::String);
            if (entityId == nullptr || entityId->stringValue.size() != 32U) return false;
            const auto parseHalf = [](std::string_view value, std::uint64_t& outputValue) {
                const auto parsed = std::from_chars(
                    value.data(), value.data() + value.size(), outputValue, 16);
                return parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size();
            };
            std::uint64_t high = 0U;
            std::uint64_t low = 0U;
            if (!parseHalf(std::string_view(entityId->stringValue).substr(0U, 16U), high) ||
                !parseHalf(std::string_view(entityId->stringValue).substr(16U, 16U), low) ||
                (high == 0U && low == 0U)) return false;
            output.command = command->stringValue;
            output.itemSyncApply.entityHigh = high;
            output.itemSyncApply.entityLow = low;
            return true;
        }

        if (command->stringValue == "WORLD_NET_PROBE") {
            if (arguments->objectValue.size() != 1U) return false;
            const auto* entityId = Field(*arguments, "entityId", JsonType::String);
            if (entityId == nullptr || entityId->stringValue.size() != 32U) return false;
            const auto parseHalf = [](std::string_view value, std::uint64_t& outputValue) {
                const auto parsed = std::from_chars(
                    value.data(), value.data() + value.size(), outputValue, 16);
                return parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size();
            };
            std::uint64_t high = 0U;
            std::uint64_t low = 0U;
            if (!parseHalf(std::string_view(entityId->stringValue).substr(0U, 16U), high) ||
                !parseHalf(std::string_view(entityId->stringValue).substr(16U, 16U), low) ||
                (high == 0U && low == 0U)) return false;
            output.command = command->stringValue;
            output.worldNetProbe.entityHigh = high;
            output.worldNetProbe.entityLow = low;
            return true;
        }

        if (command->stringValue == "WORLD_REGISTRY_PROBE") {
            if (arguments->objectValue.size() != 1U) {
                return false;
            }
            const auto* manifestPath = Field(*arguments, "manifestPath", JsonType::String);
            if (manifestPath == nullptr || manifestPath->stringValue.empty() ||
                manifestPath->stringValue.size() > 1024U ||
                manifestPath->stringValue.find('\0') != std::string::npos ||
                manifestPath->stringValue.find('\r') != std::string::npos ||
                manifestPath->stringValue.find('\n') != std::string::npos) {
                return false;
            }
            output.command = command->stringValue;
            output.worldRegistryProbe.manifestPath = manifestPath->stringValue;
            return true;
        }

        if (command->stringValue == "PopupMessage") {
            if (arguments->objectValue.size() != 5U) {
                return false;
            }
            const auto* header = Field(*arguments, "headerText", JsonType::String);
            const auto* main = Field(*arguments, "mainText", JsonType::String);
            const auto* duration = Field(*arguments, "duration", JsonType::String);
            const auto* icon = Field(*arguments, "icon", JsonType::String);
            const auto* sound = Field(*arguments, "soundEvent", JsonType::String);
            if (header == nullptr || main == nullptr || duration == nullptr ||
                icon == nullptr || sound == nullptr ||
                !CommandText(header->stringValue, false) ||
                !CommandText(main->stringValue, true) ||
                (header->stringValue.empty() && main->stringValue.empty()) ||
                (icon->stringValue != "Alert" &&
                 icon->stringValue != "Audiolog" &&
                 icon->stringValue != "None") ||
                sound->stringValue != "OBJECTIVE_NEW") {
                return false;
            }
            float durationValue = 0.0F;
            const auto conversion = std::from_chars(
                duration->stringValue.data(),
                duration->stringValue.data() + duration->stringValue.size(),
                durationValue);
            if (conversion.ec != std::errc{} ||
                conversion.ptr !=
                    duration->stringValue.data() + duration->stringValue.size() ||
                !std::isfinite(durationValue) ||
                durationValue < 0.5F || durationValue > 120.0F) {
                return false;
            }
            output.command = command->stringValue;
            output.popupMessage.headerText = header->stringValue;
            output.popupMessage.mainText = main->stringValue;
            output.popupMessage.durationSeconds = durationValue;
            output.popupMessage.icon = icon->stringValue;
            output.popupMessage.soundEvent = 1U;
            return true;
        }

        if (command->stringValue == "SpawnCharacter") {
            // x/y/z plus an optional crew "character" (default E_RIPLEY).
            const auto* character = Field(*arguments, "character", JsonType::String);
            if (character == nullptr && arguments->objectValue.contains("character")) return false;
            if (arguments->objectValue.size() != (character != nullptr ? 4U : 3U)) {
                return false;
            }
            if (character != nullptr) {
                const auto* identity = player_characters::Find(character->stringValue);
                if (identity == nullptr) return false;
                output.spawnCharacter.characterHash = identity->nameHash;
                output.spawnCharacter.displayModelGuid = identity->displayModelGuid;
            }
            const auto* x = Field(*arguments, "x", JsonType::String);
            const auto* y = Field(*arguments, "y", JsonType::String);
            const auto* z = Field(*arguments, "z", JsonType::String);
            if (x == nullptr || y == nullptr || z == nullptr) {
                return false;
            }
            const auto parseCoordinate = [](
                const std::string& text,
                float& value) noexcept {
                const auto conversion = std::from_chars(
                    text.data(),
                    text.data() + text.size(),
                    value);
                return conversion.ec == std::errc{} &&
                    conversion.ptr == text.data() + text.size() &&
                    std::isfinite(value) &&
                    std::abs(value) <= 100000.0F;
            };
            if (!parseCoordinate(x->stringValue, output.spawnCharacter.x) ||
                !parseCoordinate(y->stringValue, output.spawnCharacter.y) ||
                !parseCoordinate(z->stringValue, output.spawnCharacter.z)) {
                return false;
            }
            output.command = command->stringValue;
            return true;
        }

        if (command->stringValue == "CHARACTER_DIRECT_MOVE") {
            const auto argumentCount = arguments->objectValue.size();
            if (argumentCount < 5U || argumentCount > 16U) {
                return false;
            }
            constexpr std::array<std::string_view, 16> allowedArguments{
                "characterAddress", "directionX", "directionZ", "intensity",
                "frameCount", "directionSpace", "replicationPositionX",
                "replicationPositionY", "replicationPositionZ", "replicationYaw",
                "moveType", "playerPosture", "playerTorch", "playerTorchDirection", "playerHiding", "playerHidingPresentation"};
            if (std::any_of(
                    arguments->objectValue.begin(),
                    arguments->objectValue.end(),
                    [&allowedArguments](const auto& field) {
                        return std::find(
                            allowedArguments.begin(),
                            allowedArguments.end(),
                            std::string_view(field.first)) == allowedArguments.end();
                    })) {
                return false;
            }
            const auto* characterAddress =
                Field(*arguments, "characterAddress", JsonType::String);
            const auto* directionX =
                Field(*arguments, "directionX", JsonType::String);
            const auto* directionZ =
                Field(*arguments, "directionZ", JsonType::String);
            const auto* intensity =
                Field(*arguments, "intensity", JsonType::String);
            const auto* frameCount =
                Field(*arguments, "frameCount", JsonType::String);
            const auto* directionSpace =
                Field(*arguments, "directionSpace", JsonType::String);
            const auto* replicationPositionX =
                Field(*arguments, "replicationPositionX", JsonType::String);
            const auto* replicationPositionY =
                Field(*arguments, "replicationPositionY", JsonType::String);
            const auto* replicationPositionZ =
                Field(*arguments, "replicationPositionZ", JsonType::String);
            const auto* replicationYaw =
                Field(*arguments, "replicationYaw", JsonType::String);
            const auto* moveType =
                Field(*arguments, "moveType", JsonType::String);
            if (characterAddress == nullptr || directionX == nullptr ||
                directionZ == nullptr || intensity == nullptr ||
                frameCount == nullptr) {
                return false;
            }
            if (argumentCount > 5U && directionSpace == nullptr) {
                return false;
            }
            if ((replicationPositionX == nullptr) != (replicationPositionY == nullptr) ||
                (replicationPositionX == nullptr) != (replicationPositionZ == nullptr)) {
                return false;
            }

            auto addressText = std::string_view(characterAddress->stringValue);
            if (addressText.starts_with("0x") || addressText.starts_with("0X")) {
                addressText.remove_prefix(2U);
            }
            std::uint32_t parsedAddress = 0U;
            const auto addressConversion = std::from_chars(
                addressText.data(),
                addressText.data() + addressText.size(),
                parsedAddress,
                16);
            if (addressText.empty() ||
                addressConversion.ec != std::errc{} ||
                addressConversion.ptr != addressText.data() + addressText.size() ||
                parsedAddress < 0x10000U ||
                (parsedAddress & 0x0FU) != 0U) {
                return false;
            }

            const auto parseFloat = [](
                const std::string& text,
                float& value,
                const float maximumMagnitude) noexcept {
                const auto conversion = std::from_chars(
                    text.data(),
                    text.data() + text.size(),
                    value);
                return conversion.ec == std::errc{} &&
                    conversion.ptr == text.data() + text.size() &&
                    std::isfinite(value) &&
                    std::abs(value) <= maximumMagnitude;
            };
            if (!parseFloat(
                    directionX->stringValue,
                    output.characterDirectMove.directionX,
                    kMaximumCharacterDirectMoveDirectionMagnitude) ||
                !parseFloat(
                    directionZ->stringValue,
                    output.characterDirectMove.directionZ,
                    kMaximumCharacterDirectMoveDirectionMagnitude) ||
                !parseFloat(
                    intensity->stringValue,
                    output.characterDirectMove.intensity,
                    4.0F) ||
                output.characterDirectMove.intensity < 1.0F ||
                output.characterDirectMove.directionX *
                        output.characterDirectMove.directionX +
                    output.characterDirectMove.directionZ *
                        output.characterDirectMove.directionZ >
                    kMaximumCharacterDirectMoveDirectionMagnitude *
                        kMaximumCharacterDirectMoveDirectionMagnitude) {
                return false;
            }

            std::uint32_t parsedFrameCount = 0U;
            const auto frameCountConversion = std::from_chars(
                frameCount->stringValue.data(),
                frameCount->stringValue.data() + frameCount->stringValue.size(),
                parsedFrameCount);
            if (frameCountConversion.ec != std::errc{} ||
                frameCountConversion.ptr !=
                    frameCount->stringValue.data() +
                        frameCount->stringValue.size() ||
                parsedFrameCount > 3600U ||
                (parsedFrameCount != 0U &&
                 output.characterDirectMove.directionX == 0.0F &&
                 output.characterDirectMove.directionZ == 0.0F)) {
                return false;
            }

            bool worldSpace = false;
            if (directionSpace != nullptr) {
                if (directionSpace->stringValue == "world") {
                    worldSpace = true;
                } else if (directionSpace->stringValue != "local") {
                    return false;
                }
            }

            const auto hasReplicationPosition = replicationPositionX != nullptr;
            const auto hasReplicationYaw = replicationYaw != nullptr;
            if (hasReplicationYaw &&
                (!worldSpace || !parseFloat(
                    replicationYaw->stringValue,
                    output.characterDirectMove.replicationYaw,
                    3.3F))) {
                return false;
            }
            if (hasReplicationPosition) {
                if (!hasReplicationYaw || !worldSpace ||
                    !parseFloat(
                        replicationPositionX->stringValue,
                        output.characterDirectMove.replicationPositionX,
                        100000.0F) ||
                    !parseFloat(
                        replicationPositionY->stringValue,
                        output.characterDirectMove.replicationPositionY,
                        100000.0F) ||
                    !parseFloat(
                        replicationPositionZ->stringValue,
                        output.characterDirectMove.replicationPositionZ,
                        100000.0F)) {
                    return false;
                }
            }

            if (hasReplicationPosition && moveType == nullptr) {
                return false;
            }
            if (moveType != nullptr) {
                std::uint32_t parsedMoveType = 0U;
                const auto moveTypeConversion = std::from_chars(
                    moveType->stringValue.data(),
                    moveType->stringValue.data() + moveType->stringValue.size(),
                    parsedMoveType);
                if (moveTypeConversion.ec != std::errc{} ||
                    moveTypeConversion.ptr !=
                        moveType->stringValue.data() + moveType->stringValue.size() ||
                    parsedMoveType > 3U) {
                    return false;
                }
                output.characterDirectMove.hasMoveType = true;
                output.characterDirectMove.moveType = parsedMoveType;
            }

            output.characterDirectMove.characterAddress = parsedAddress;
            if (const auto* torch = Field(*arguments, "playerTorch", JsonType::String)) {
                if (!hasReplicationPosition || !hasReplicationYaw || moveType == nullptr ||
                    (torch->stringValue != "0" && torch->stringValue != "1")) return false;
                output.characterDirectMove.hasPlayerTorch = true;
                output.characterDirectMove.playerTorchEnabled = torch->stringValue == "1";
            } else if (arguments->objectValue.contains("playerTorch")) return false;
            if (const auto* direction = Field(*arguments, "playerTorchDirection", JsonType::String)) {
                if (!output.characterDirectMove.hasPlayerTorch || !output.characterDirectMove.playerTorchEnabled ||
                    !player_torch::DecodeDirection(direction->stringValue, output.characterDirectMove.playerTorchDirection))
                    return false;
                output.characterDirectMove.hasPlayerTorchDirection = true;
            } else if (arguments->objectValue.contains("playerTorchDirection")) return false;
            if (const auto* hiding = Field(*arguments, "playerHiding", JsonType::String)) {
                if (!hasReplicationPosition || !hasReplicationYaw || moveType == nullptr ||
                    !player_hiding::Decode(hiding->stringValue, output.characterDirectMove.playerHidden,
                        output.characterDirectMove.playerHidingAnchor)) return false;
                output.characterDirectMove.hasPlayerHiding = true;
            } else if (arguments->objectValue.contains("playerHiding")) return false;
            if (const auto* presentation = Field(*arguments, "playerHidingPresentation", JsonType::String)) {
                if (!output.characterDirectMove.hasPlayerHiding ||
                    !player_hiding_presentation::Decode(presentation->stringValue,
                        output.characterDirectMove.playerHidingPresentation)) return false;
                output.characterDirectMove.hasPlayerHidingPresentation = true;
            } else if (arguments->objectValue.contains("playerHidingPresentation")) return false;
            if (const auto* posture = Field(*arguments, "playerPosture", JsonType::String)) {
                if (!hasReplicationPosition || !hasReplicationYaw || moveType == nullptr ||
                    !player_posture::Decode(posture->stringValue, output.characterDirectMove.playerPosture))
                    return false;
                output.characterDirectMove.hasPlayerPosture = true;
            } else if (arguments->objectValue.contains("playerPosture")) {
                return false;
            }
            output.characterDirectMove.frameCount = parsedFrameCount;
            output.characterDirectMove.hasReplicationPosition = hasReplicationPosition;
            output.characterDirectMove.hasReplicationYaw = hasReplicationYaw;
            output.characterDirectMove.worldSpace = worldSpace;
            output.command = command->stringValue;
            return true;
        }

        if (command->stringValue == "CHARACTER_AMMO_REFILL") {
            if (arguments->objectValue.size() != 1U) return false;
            const auto* address = Field(*arguments, "characterAddress", JsonType::String);
            if (address == nullptr) return false;
            auto text = std::string_view(address->stringValue);
            if (text.starts_with("0x") || text.starts_with("0X")) text.remove_prefix(2U);
            std::uint32_t parsed = 0U;
            const auto converted = std::from_chars(text.data(), text.data() + text.size(), parsed, 16);
            if (text.empty() || converted.ec != std::errc{} ||
                converted.ptr != text.data() + text.size() || parsed < 0x10000U ||
                parsed > 0xFFFF0000U || (parsed & 0x0FU) != 0U) return false;
            output.characterAmmoRefill.characterAddress = parsed;
            output.command = command->stringValue;
            return true;
        }

        if (command->stringValue == "CHARACTER_ACTION_TEST") {
            if (arguments->objectValue.size() != 2U) return false;
            const auto* characterAddress =
                Field(*arguments, "characterAddress", JsonType::String);
            const auto* action = Field(*arguments, "action", JsonType::String);
            if (characterAddress == nullptr || action == nullptr) return false;

            auto addressText = std::string_view(characterAddress->stringValue);
            if (addressText.starts_with("0x") || addressText.starts_with("0X")) {
                addressText.remove_prefix(2U);
            }
            std::uint32_t parsedAddress = 0U;
            const auto addressConversion = std::from_chars(
                addressText.data(), addressText.data() + addressText.size(),
                parsedAddress, 16);
            if (addressText.empty() || addressConversion.ec != std::errc{} ||
                addressConversion.ptr != addressText.data() + addressText.size() ||
                parsedAddress < 0x10000U || (parsedAddress & 0x0FU) != 0U) {
                return false;
            }

            CharacterActionKind actionKind{};
            if (action->stringValue == "aim-start") {
                actionKind = CharacterActionKind::AimStart;
            } else if (action->stringValue == "aim-stop") {
                actionKind = CharacterActionKind::AimStop;
            } else if (action->stringValue == "primary-start") {
                actionKind = CharacterActionKind::PrimaryStart;
            } else if (action->stringValue == "primary-stop") {
                actionKind = CharacterActionKind::PrimaryStop;
            } else {
                return false;
            }

            output.characterActionTest.characterAddress = parsedAddress;
            output.characterActionTest.action = actionKind;
            output.command = command->stringValue;
            return true;
        }

        if (command->stringValue == "REMOTE_EQUIPMENT_PROVISION") {
            const auto* activateHeld = Field(*arguments, "activateHeld", JsonType::String);
            const auto* releaseHeld = Field(*arguments, "releaseHeld", JsonType::String);
            if (arguments->objectValue.size() != ((activateHeld != nullptr || releaseHeld != nullptr) ? 8U : 7U) ||
                (activateHeld != nullptr && activateHeld->stringValue != "true") ||
                (releaseHeld != nullptr && releaseHeld->stringValue != "true") ||
                (activateHeld != nullptr && releaseHeld != nullptr)) {
                return false;
            }
            const auto* characterAddress =
                Field(*arguments, "characterAddress", JsonType::String);
            const auto* compositeName =
                Field(*arguments, "compositeName", JsonType::String);
            const auto* equipmentEntity =
                Field(*arguments, "equipmentEntityShortGuid", JsonType::String);
            const auto* equipmentOutput =
                Field(*arguments, "equipmentOutputShortGuid", JsonType::String);
            const auto* spawnScheduler =
                Field(*arguments, "spawnSchedulerParameterShortGuid", JsonType::String);
            const auto* spawnTargetInput =
                Field(*arguments, "spawnTargetInputShortGuid", JsonType::String);
            const auto* equipmentSlot =
                Field(*arguments, "equipmentSlot", JsonType::String);
            if (characterAddress == nullptr || compositeName == nullptr ||
                equipmentEntity == nullptr || equipmentOutput == nullptr ||
                spawnScheduler == nullptr || spawnTargetInput == nullptr ||
                equipmentSlot == nullptr ||
                compositeName->stringValue.empty() ||
                compositeName->stringValue.size() > 255U) {
                return false;
            }

            const auto parseHex = [](const std::string& text, std::uint32_t& value) {
                auto view = std::string_view(text);
                if (view.starts_with("0x") || view.starts_with("0X")) {
                    view.remove_prefix(2U);
                }
                if (view.empty()) return false;
                const auto conversion = std::from_chars(
                    view.data(), view.data() + view.size(), value, 16);
                return conversion.ec == std::errc{} &&
                    conversion.ptr == view.data() + view.size();
            };

            std::uint32_t parsedAddress = 0U;
            std::uint32_t parsedEquipmentEntity = 0U;
            std::uint32_t parsedEquipmentOutput = 0U;
            std::uint32_t parsedSpawnScheduler = 0U;
            std::uint32_t parsedSpawnTargetInput = 0U;
            std::uint32_t parsedEquipmentSlot = 0U;
            if (!parseHex(characterAddress->stringValue, parsedAddress) ||
                parsedAddress < 0x10000U ||
                (parsedAddress & 0x0FU) != 0U ||
                !parseHex(equipmentEntity->stringValue, parsedEquipmentEntity) ||
                parsedEquipmentEntity == 0U ||
                !parseHex(equipmentOutput->stringValue, parsedEquipmentOutput) ||
                parsedEquipmentOutput == 0U ||
                !parseHex(spawnScheduler->stringValue, parsedSpawnScheduler) ||
                parsedSpawnScheduler == 0U ||
                !parseHex(spawnTargetInput->stringValue, parsedSpawnTargetInput) ||
                parsedSpawnTargetInput == 0U) {
                return false;
            }
            const auto slotConversion = std::from_chars(
                equipmentSlot->stringValue.data(),
                equipmentSlot->stringValue.data() + equipmentSlot->stringValue.size(),
                parsedEquipmentSlot,
                10);
            if (equipmentSlot->stringValue.empty() ||
                slotConversion.ec != std::errc{} ||
                slotConversion.ptr != equipmentSlot->stringValue.data() + equipmentSlot->stringValue.size() ||
                parsedEquipmentSlot > 10U) {
                return false;
            }

            output.remoteEquipmentProvision.characterAddress = parsedAddress;
            output.remoteEquipmentProvision.compositeName =
                compositeName->stringValue;
            output.remoteEquipmentProvision.equipmentEntityShortGuid =
                parsedEquipmentEntity;
            output.remoteEquipmentProvision.equipmentOutputShortGuid =
                parsedEquipmentOutput;
            output.remoteEquipmentProvision.spawnSchedulerParameterShortGuid =
                parsedSpawnScheduler;
            output.remoteEquipmentProvision.spawnTargetInputShortGuid =
                parsedSpawnTargetInput;
            output.remoteEquipmentProvision.equipmentSlot =
                parsedEquipmentSlot;
            output.remoteEquipmentProvision.activateHeld = activateHeld != nullptr;
            output.remoteEquipmentProvision.releaseHeld = releaseHeld != nullptr;
            if ((activateHeld != nullptr || releaseHeld != nullptr) && (parsedEquipmentSlot != 9U || compositeName->stringValue !=
                "Required_Assets\\Archetypes\\Equipment\\Flare_Held")) return false;
            output.command = command->stringValue;
            return true;
        }

        if (command->stringValue == "CHARACTER_DIRECT_STEP") {
            if (arguments->objectValue.size() != 6U) {
                return false;
            }
            const auto* characterAddress =
                Field(*arguments, "characterAddress", JsonType::String);
            const auto* deltaX = Field(*arguments, "deltaX", JsonType::String);
            const auto* deltaY = Field(*arguments, "deltaY", JsonType::String);
            const auto* deltaZ = Field(*arguments, "deltaZ", JsonType::String);
            const auto* deltaYaw =
                Field(*arguments, "deltaYaw", JsonType::String);
            const auto* frameCount =
                Field(*arguments, "frameCount", JsonType::String);
            if (characterAddress == nullptr || deltaX == nullptr ||
                deltaY == nullptr || deltaZ == nullptr || deltaYaw == nullptr ||
                frameCount == nullptr) {
                return false;
            }

            auto addressText = std::string_view(characterAddress->stringValue);
            if (addressText.starts_with("0x") || addressText.starts_with("0X")) {
                addressText.remove_prefix(2U);
            }
            std::uint32_t parsedAddress = 0U;
            const auto addressConversion = std::from_chars(
                addressText.data(),
                addressText.data() + addressText.size(),
                parsedAddress,
                16);
            if (addressText.empty() ||
                addressConversion.ec != std::errc{} ||
                addressConversion.ptr != addressText.data() + addressText.size() ||
                parsedAddress < 0x10000U ||
                (parsedAddress & 0x0FU) != 0U) {
                return false;
            }

            const auto parseStep = [](
                const std::string& text,
                float& value,
                const float maximumMagnitude) noexcept {
                const auto conversion = std::from_chars(
                    text.data(),
                    text.data() + text.size(),
                    value);
                return conversion.ec == std::errc{} &&
                    conversion.ptr == text.data() + text.size() &&
                    std::isfinite(value) &&
                    std::abs(value) <= maximumMagnitude;
            };
            if (!parseStep(
                    deltaX->stringValue,
                    output.characterDirectStep.deltaX,
                    1.0F) ||
                !parseStep(
                    deltaY->stringValue,
                    output.characterDirectStep.deltaY,
                    1.0F) ||
                !parseStep(
                    deltaZ->stringValue,
                    output.characterDirectStep.deltaZ,
                    1.0F) ||
                !parseStep(
                    deltaYaw->stringValue,
                    output.characterDirectStep.deltaYaw,
                    1.0F)) {
                return false;
            }

            std::uint32_t parsedFrameCount = 0U;
            const auto frameCountConversion = std::from_chars(
                frameCount->stringValue.data(),
                frameCount->stringValue.data() + frameCount->stringValue.size(),
                parsedFrameCount);
            if (frameCountConversion.ec != std::errc{} ||
                frameCountConversion.ptr !=
                    frameCount->stringValue.data() + frameCount->stringValue.size() ||
                parsedFrameCount > 600U) {
                return false;
            }

            output.characterDirectStep.characterAddress = parsedAddress;
            output.characterDirectStep.frameCount = parsedFrameCount;
            output.command = command->stringValue;
            return true;
        }

        if (command->stringValue == "SpawnNetZCharacter") {
            if (arguments->objectValue.size() != 5U) {
                return false;
            }
            const auto* x = Field(*arguments, "x", JsonType::String);
            const auto* y = Field(*arguments, "y", JsonType::String);
            const auto* z = Field(*arguments, "z", JsonType::String);
            const auto* participantKey =
                Field(*arguments, "participantKey", JsonType::String);
            const auto* displayName =
                Field(*arguments, "displayName", JsonType::String);
            if (x == nullptr || y == nullptr || z == nullptr ||
                participantKey == nullptr || displayName == nullptr ||
                displayName->stringValue.empty() ||
                !CommandText(displayName->stringValue, false)) {
                return false;
            }
            const auto parseCoordinate = [](
                const std::string& text,
                float& value) noexcept {
                const auto conversion = std::from_chars(
                    text.data(),
                    text.data() + text.size(),
                    value);
                return conversion.ec == std::errc{} &&
                    conversion.ptr == text.data() + text.size() &&
                    std::isfinite(value) &&
                    std::abs(value) <= 100000.0F;
            };
            if (!parseCoordinate(
                    x->stringValue,
                    output.spawnNetZCharacter.x) ||
                !parseCoordinate(
                    y->stringValue,
                    output.spawnNetZCharacter.y) ||
                !parseCoordinate(
                    z->stringValue,
                    output.spawnNetZCharacter.z)) {
                return false;
            }
            std::uint32_t parsedParticipantKey = 0;
            const auto keyConversion = std::from_chars(
                participantKey->stringValue.data(),
                participantKey->stringValue.data() +
                    participantKey->stringValue.size(),
                parsedParticipantKey);
            if (keyConversion.ec != std::errc{} ||
                keyConversion.ptr !=
                    participantKey->stringValue.data() +
                        participantKey->stringValue.size() ||
                parsedParticipantKey == 0U ||
                parsedParticipantKey >
                    static_cast<std::uint32_t>((std::numeric_limits<int>::max)())) {
                return false;
            }
            output.spawnNetZCharacter.participantKey = parsedParticipantKey;
            output.spawnNetZCharacter.displayName = displayName->stringValue;
            output.command = command->stringValue;
            return true;
        }

        return false;
    } catch (...) {
        output = {};
        return false;
    }
}

std::vector<std::byte> EncodeCapabilityState(
    const RequestEnvelope& request,
    const std::string_view profileId,
    const std::string_view resolutionMode,
    const bool staticValidated,
    const bool runtimeValidated,
    const std::vector<CapabilityRecord>& capabilities,
    const std::vector<std::string>& blockedGates,
    const std::string_view nextAction) {
    std::ostringstream json;
    Prefix(json, request);
    json << ",\"profileId\":" << Escape(profileId)
         << ",\"resolutionMode\":" << Escape(resolutionMode)
         << ",\"staticValidated\":"
         << (staticValidated ? "true" : "false")
         << ",\"runtimeValidated\":"
         << (runtimeValidated ? "true" : "false")
         << ",\"capabilities\":[";
    for (std::size_t index = 0; index < capabilities.size(); ++index) {
        if (index != 0) {
            json << ',';
        }
        const auto& capability = capabilities[index];
        json << "{\"name\":" << Escape(ToString(capability.capability))
             << ",\"state\":" << Escape(ToString(capability.state))
             << ",\"reason\":" << Escape(capability.reason) << '}';
    }
    json << "],\"blockedGates\":";
    StringArray(json, blockedGates);
    json << ",\"nextAction\":" << Escape(nextAction) << '}';
    return Bytes(json.str());
}

std::vector<std::byte> EncodeCaptureState(
    const RequestEnvelope& request,
    const bool f0Captured,
    const bool request130Captured,
    const bool participantRegistryAvailable,
    const std::uint32_t participantCount,
    const bool contextPoolAvailable,
    const bool freeChannelAvailable,
    const bool gameThreadValidated,
    const std::uint32_t captureCatalogCount,
    const std::string_view lastOperation,
    const std::uint32_t gameThreadCandidateId,
    const std::uint32_t gameThreadSourceCount,
    const std::string_view engineThreadSource,
    const std::uint64_t firstEngineCallbackPerformanceCounter,
    const std::uint64_t lastEngineCallbackPerformanceCounter,
    const std::size_t pendingCommandCount,
    const bool mainThreadCharacterProcessingHookInstalled,
    const bool characterManagerUpdateHookInstalled,
    const bool characterReadinessProducerHookInstalled,
    const std::string_view remoteCharacterState,
    const std::uint32_t remoteCharacterSpawnKey,
    const std::uintptr_t remoteCharacterAddress,
    const std::uintptr_t remoteCharacterRecordAddress,
    const bool characterCreateHookInstalled,
    const std::uint64_t remoteCharacterCreationSerial,
    const std::uint32_t remoteCharacterRuntimeId) {
    std::ostringstream json;
    Prefix(json, request);
    json << ",\"spawnRequestF0Captured\":"
         << (f0Captured ? "true" : "false")
         << ",\"spawnRequest130Captured\":"
         << (request130Captured ? "true" : "false")
         << ",\"participantRegistryAvailable\":"
         << (participantRegistryAvailable ? "true" : "false")
         << ",\"participantCount\":" << participantCount
         << ",\"contextPoolAvailable\":"
         << (contextPoolAvailable ? "true" : "false")
         << ",\"freeChannelAvailable\":"
         << (freeChannelAvailable ? "true" : "false")
         << ",\"gameThreadValidated\":"
         << (gameThreadValidated ? "true" : "false")
         << ",\"gameThreadCandidateId\":"
         << gameThreadCandidateId
         << ",\"gameThreadSourceCount\":"
         << gameThreadSourceCount
         << ",\"engineThreadId\":" << gameThreadCandidateId
         << ",\"engineThreadSource\":" << Escape(engineThreadSource)
         << ",\"firstEngineCallbackPerformanceCounter\":"
         << firstEngineCallbackPerformanceCounter
         << ",\"lastEngineCallbackPerformanceCounter\":"
         << lastEngineCallbackPerformanceCounter
         << ",\"pendingCommandCount\":" << pendingCommandCount
         << ",\"mainThreadCharacterProcessingHookInstalled\":"
         << (mainThreadCharacterProcessingHookInstalled ? "true" : "false")
         << ",\"characterManagerUpdateHookInstalled\":"
         << (characterManagerUpdateHookInstalled ? "true" : "false")
         << ",\"characterReadinessProducerHookInstalled\":"
         << (characterReadinessProducerHookInstalled ? "true" : "false")
         << ",\"engineHooksInstalled\":"
         << ((mainThreadCharacterProcessingHookInstalled ||
              characterManagerUpdateHookInstalled ||
              characterReadinessProducerHookInstalled)
                 ? "true"
                 : "false")
         << ",\"captureCatalogCount\":" << captureCatalogCount
         << ",\"remoteCharacterState\":"
         << Escape(remoteCharacterState)
         << ",\"remoteCharacterSpawnKey\":"
         << remoteCharacterSpawnKey
         << ",\"remoteCharacterAddress\":"
         << static_cast<std::uint64_t>(remoteCharacterAddress)
         << ",\"remoteCharacterRecordAddress\":"
         << static_cast<std::uint64_t>(remoteCharacterRecordAddress)
         << ",\"characterCreateHookInstalled\":"
         << (characterCreateHookInstalled ? "true" : "false")
         << ",\"remoteCharacterCreationSerial\":" << remoteCharacterCreationSerial
         << ",\"remoteCharacterRuntimeId\":" << remoteCharacterRuntimeId
         << ",\"lastOperation\":" << Escape(lastOperation) << '}';
    return Bytes(json.str());
}

std::vector<std::byte> EncodeDummyOperationResult(
    const RequestEnvelope& request,
    const DummyOperationResult& result) {
    std::ostringstream json;
    Prefix(json, request);
    json << ",\"state\":" << Escape(ToString(result.state))
         << ",\"achievedLevel\":" << Escape(ToString(result.achieved))
         << ",\"statusCode\":" << Escape(result.statusCode)
         << ",\"message\":" << Escape(result.message)
         << ",\"rollbackPerformed\":"
         << (result.rollbackPerformed ? "true" : "false")
         << ",\"completedSteps\":";
    StringArray(json, result.completedSteps);
    json << ",\"blockedGates\":";
    StringArray(json, result.blockedGates);
    json << '}';
    return Bytes(json.str());
}

std::vector<std::byte> EncodeDummyMoveResult(
    const RequestEnvelope& request,
    const std::string_view statusCode,
    const std::string_view message,
    const std::string_view method,
    const bool applied) {
    std::ostringstream json;
    Prefix(json, request);
    json << ",\"statusCode\":" << Escape(statusCode)
         << ",\"message\":" << Escape(message)
         << ",\"method\":" << Escape(method)
         << ",\"applied\":" << (applied ? "true" : "false")
         << '}';
    return Bytes(json.str());
}

std::vector<std::byte> EncodeDummyBindingState(
    const RequestEnvelope& request,
    const DummyAchievement achieved,
    const std::string_view lastError) {
    const bool complete = achieved == DummyAchievement::C ||
        achieved == DummyAchievement::D;
    const bool character = achieved != DummyAchievement::None;
    std::ostringstream json;
    Prefix(json, request);
    json << ",\"participantBound\":"
         << (complete ? "true" : "false")
         << ",\"characterPresent\":"
         << (character ? "true" : "false")
         << ",\"characterRecordPresent\":"
         << (character ? "true" : "false")
         << ",\"channelActive\":"
         << (complete ? "true" : "false")
         << ",\"descriptorsPresent\":"
         << (complete ? "true" : "false")
         << ",\"replicationContextPresent\":"
         << (complete ? "true" : "false")
         << ",\"achievedLevel\":" << Escape(ToString(achieved))
         << ",\"lastError\":" << Escape(lastError) << '}';
    return Bytes(json.str());
}

std::vector<std::byte> EncodeDiagnosticSnapshotResult(
    const RequestEnvelope& request,
    const std::string_view runId,
    const std::string_view nativeLogPath,
    const std::string_view statusCode,
    const std::string_view message) {
    std::ostringstream json;
    Prefix(json, request);
    json << ",\"runId\":" << Escape(runId)
         << ",\"nativeLogPath\":" << Escape(nativeLogPath)
         << ",\"profilePath\":\"\""
         << ",\"captureDirectory\":\"\""
         << ",\"dumpPath\":\"\""
         << ",\"artifactPaths\":["
         << Escape(nativeLogPath) << ']'
         << ",\"statusCode\":" << Escape(statusCode)
         << ",\"message\":" << Escape(message) << '}';
    return Bytes(json.str());
}

std::vector<std::byte> EncodeExperimentalError(
    const RequestEnvelope& request,
    const std::string_view code,
    const std::string_view message,
    const std::vector<std::string>& blockedGates) {
    std::ostringstream json;
    Prefix(json, request);
    json << ",\"code\":" << Escape(code)
         << ",\"message\":" << Escape(message)
         << ",\"blockedGates\":";
    StringArray(json, blockedGates);
    json << '}';
    return Bytes(json.str());
}

std::vector<std::byte> EncodeLevelState(
    const RequestEnvelope& request,
    const std::string_view levelName,
    const bool transitioning,
    const bool gameThreadValidated) {
    std::ostringstream json;
    Prefix(json, request);
    json << ",\"levelName\":" << Escape(levelName)
         << ",\"transitioning\":"
         << (transitioning ? "true" : "false")
         << ",\"gameThreadValidated\":"
         << (gameThreadValidated ? "true" : "false")
         << '}';
    return Bytes(json.str());
}

std::vector<std::byte> EncodeGameCommandResult(
    const RequestEnvelope& request,
    const std::string_view command,
    const GameCommandExecutionResult& result) {
    std::ostringstream json;
    Prefix(json, request);
    const auto boundedMessage = BoundedGameCommandMessage(result.message,command=="ALIEN_PVP_VENTS"?16000U:1000U);
    json << ",\"command\":" << Escape(command)
         << ",\"statusCode\":" << Escape(result.statusCode)
         << ",\"message\":" << Escape(boundedMessage)
         << ",\"executed\":" << (result.executed ? "true" : "false")
         << '}';
    return Bytes(json.str());
}

} // namespace aimp::experimental
