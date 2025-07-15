#pragma once

#include "net/serde/serde.hpp"

namespace proto {
struct PacketType {
    enum : uint8_t {
        Success = 0,
        Error,
        StartStreaming,
        ChangeResolution,
        ChangeFramerate,
        ChangeBitrate,
    };
};

struct Success {
    constexpr static auto pt = PacketType::Success;
};

struct Error {
    constexpr static auto pt = PacketType::Error;
};

struct StartStreaming {
    constexpr static auto pt = PacketType::StartStreaming;
    SerdeFieldsBegin;
    uint16_t SerdeField(port);
    SerdeFieldsEnd;
};

struct ChangeResolution {
    constexpr static auto pt = PacketType::ChangeResolution;
    SerdeFieldsBegin;
    uint32_t SerdeField(width);
    uint32_t SerdeField(height);
    SerdeFieldsEnd;
};

struct ChangeFramerate {
    constexpr static auto pt = PacketType::ChangeFramerate;
    SerdeFieldsBegin;
    uint32_t SerdeField(framerate);
    SerdeFieldsEnd;
};

struct ChangeBitrate {
    constexpr static auto pt = PacketType::ChangeBitrate;
    SerdeFieldsBegin;
    uint32_t SerdeField(bitrate); // in kbps
    SerdeFieldsEnd;
};
} // namespace proto
