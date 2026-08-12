/*
 * AsterNet 网络核心 —— 业务协议适配（占位）
 *
 * 业务协议层仅提供安全的字节编解码与网关请求适配，不假设当前同步 HTTP 引擎已经
 * 支持 WebSocket、QUIC Datagram 或长期 stream。可靠 IM 信令必须使用可靠 stream。
 */
#ifndef ASTERNET_PROTOCOL_H
#define ASTERNET_PROTOCOL_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "engine/engine.h"

namespace asternet {
namespace protocol {

class Codec {
public:
    virtual ~Codec() = default;
    virtual int encode(const std::string &plain, std::vector<uint8_t> &wire) const = 0;
    virtual int decode(const uint8_t *wire, size_t wire_size, std::string &plain) const = 0;
};

class LengthPrefixedCodec final : public Codec {
public:
    explicit LengthPrefixedCodec(size_t max_message_bytes = 1024 * 1024);
    ~LengthPrefixedCodec() override;

    int encode(const std::string &plain, std::vector<uint8_t> &wire) const override;
    int decode(const uint8_t *wire, size_t wire_size, std::string &plain) const override;

private:
    size_t max_message_bytes_;
};

struct GatewayRequest {
    std::string path;
    std::string body;
    std::vector<engine::Header> headers;
    bool idempotent = false;
};

class GatewayProtocol {
public:
    virtual ~GatewayProtocol() = default;
    virtual int adapt(const GatewayRequest &input, engine::Request &request) const = 0;
};

class DefaultGatewayProtocol final : public GatewayProtocol {
public:
    int adapt(const GatewayRequest &input, engine::Request &request) const override;
};

}  // namespace protocol
}  // namespace asternet

#endif  // ASTERNET_PROTOCOL_H
