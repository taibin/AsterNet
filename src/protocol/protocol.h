/*
 * AsterNet 网络核心 —— 业务协议适配（占位）
 *
 * IM 协议：现有 WebSocket + protobuf 迁入。迁移期保持 WebSocket over QUIC（协议不变）；
 *          远期可演进为基于 QUIC Datagram 的自定义长连接协议（更低延迟）。
 * 网关协议（可选）：若后端有统一网关，引入类 MTOP 协议层（统一签名/加密/压缩/错误码）。
 * 阶段 3（IM）/ 视后端（网关）实现。
 */
#ifndef ASTERNET_PROTOCOL_H
#define ASTERNET_PROTOCOL_H

namespace asternet {
namespace protocol {

// 占位：阶段 3 定义 ImProtocol / GatewayProtocol / Codec 接口

}  // namespace protocol
}  // namespace asternet

#endif  // ASTERNET_PROTOCOL_H
