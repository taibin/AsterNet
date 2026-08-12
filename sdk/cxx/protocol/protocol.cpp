#include "protocol/protocol.h"

#include <cstring>
#include <limits>

#include "asternet/asternet.h"

namespace asternet {
namespace protocol {

LengthPrefixedCodec::LengthPrefixedCodec(size_t max_message_bytes)
    : max_message_bytes_(max_message_bytes == 0 ? 1 : max_message_bytes) {}

LengthPrefixedCodec::~LengthPrefixedCodec() = default;

int LengthPrefixedCodec::encode(const std::string &plain, std::vector<uint8_t> &wire) const {
    if (plain.size() > max_message_bytes_ || plain.size() > std::numeric_limits<uint32_t>::max()) {
        return ASTERNET_ERR_BUFFER_TOO_SMALL;
    }
    const uint32_t size = static_cast<uint32_t>(plain.size());
    wire.resize(sizeof(size) + plain.size());
    wire[0] = static_cast<uint8_t>((size >> 24) & 0xff);
    wire[1] = static_cast<uint8_t>((size >> 16) & 0xff);
    wire[2] = static_cast<uint8_t>((size >> 8) & 0xff);
    wire[3] = static_cast<uint8_t>(size & 0xff);
    if (!plain.empty()) std::memcpy(wire.data() + sizeof(size), plain.data(), plain.size());
    return ASTERNET_OK;
}

int LengthPrefixedCodec::decode(const uint8_t *wire, size_t wire_size, std::string &plain) const {
    if (wire == nullptr || wire_size < sizeof(uint32_t)) return ASTERNET_ERR_PROTOCOL;
    const uint32_t size = (static_cast<uint32_t>(wire[0]) << 24)
        | (static_cast<uint32_t>(wire[1]) << 16) | (static_cast<uint32_t>(wire[2]) << 8)
        | static_cast<uint32_t>(wire[3]);
    if (size > max_message_bytes_ || wire_size != sizeof(uint32_t) + size) {
        return size > max_message_bytes_ ? ASTERNET_ERR_BUFFER_TOO_SMALL : ASTERNET_ERR_PROTOCOL;
    }
    plain.assign(reinterpret_cast<const char *>(wire + sizeof(uint32_t)), size);
    return ASTERNET_OK;
}

int DefaultGatewayProtocol::adapt(const GatewayRequest &input, engine::Request &request) const {
    if (input.path.empty() || input.path.front() != '/') return ASTERNET_ERR_INVALID_ARGUMENT;
    if (input.path.find('\r') != std::string::npos || input.path.find('\n') != std::string::npos) {
        return ASTERNET_ERR_INVALID_ARGUMENT;
    }
    for (const engine::Header &header : input.headers) {
        if (header.name.empty() || header.name.find_first_of("\r\n") != std::string::npos
            || header.value.find_first_of("\r\n") != std::string::npos) {
            return ASTERNET_ERR_INVALID_ARGUMENT;
        }
        std::string name = header.name;
        for (char &character : name) {
            character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
        }
        if (name == "host" || name == "content-length" || name == "transfer-encoding"
            || name == "connection") {
            return ASTERNET_ERR_INVALID_ARGUMENT;
        }
    }
    request.path = input.path;
    request.body = input.body;
    request.headers = input.headers;
    request.idempotent = input.idempotent;
    return ASTERNET_OK;
}

}  // namespace protocol
}  // namespace asternet
