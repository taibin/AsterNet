import Foundation

// MARK: - 类型别名（映射 C enum → Swift）

enum AsterNetResult: Int32 {
    case ok = 0
    case errAbiVersion = -1
    case errInvalidArg = -2
    case errNotInit = -3
    case errOutOfMemory = -4
    case errTimeout = -5
    case errDns = -6
    case errConnect = -7
    case errTls = -8
    case errProtocol = -9
    case errCanceled = -10
    case errDegraded = -11
    case errNetworkChanged = -12
    case errUnsupported = -13
    case errBufferTooSmall = -14
    case errInternal = -100
}

enum AsterNetProtocol: Int32 {
    case unknown = 0
    case http1_1 = 1
    case http2 = 2
    case http3 = 3

    var label: String {
        switch self {
        case .http1_1: "HTTP/1.1"
        case .http2:   "HTTP/2"
        case .http3:   "HTTP/3"
        default:       "UNKNOWN"
        }
    }
}

enum AsterNetPolicy: Int32, CaseIterable, Identifiable {
    case auto = 0
    case http1_1_only = 1
    case http2_only = 2
    case http3_only = 3
    case preferHttp3 = 4
    case preferHttp2 = 5

    var id: Int32 { rawValue }
    var label: String {
        switch self {
        case .auto:          "AUTO"
        case .http1_1_only:  "HTTP/1.1 only"
        case .http2_only:    "HTTP/2 only"
        case .http3_only:    "HTTP/3 only"
        case .preferHttp3:   "Prefer HTTP/3"
        case .preferHttp2:   "Prefer HTTP/2"
        }
    }
}

// MARK: - 响应模型

struct AsterNetResponse {
    let result: AsterNetResult
    let httpStatus: Int
    let proto: AsterNetProtocol
    let degraded: Bool
    let bodySize: Int
    let dnsMs: Int64
    let connectMs: Int64
    let tlsMs: Int64
    let ttfbMs: Int64
    let totalMs: Int64
    let body: String
}

// MARK: - Client（线程安全）

@MainActor
final class AsterNetClient: ObservableObject {
    private var handle: OpaquePointer?
    private let lock = NSLock()
    private let responseBufferSize = 4 * 1024 * 1024

    var isReady: Bool { handle != nil }

    init() {
        var cfg = asternet_client_config_t(
            struct_size: MemoryLayout<asternet_client_config_t>.size,
            abi_version: UInt32(ASTERNET_ABI_VERSION),
            default_timeout_ms: 12000,
            max_response_body_bytes: Int32(responseBufferSize),
            enable_http3: 1,
            allow_insecure_tls_for_testing: 0,
            ca_cert_pem: nil
        )
        var error: asternet_result_t = ASTERNET_OK
        handle = asternet_client_create(&cfg, &error)
        if handle == nil {
            print("[AsterNet] Failed to create client: \(String(cString: asternet_result_string(error)))")
        }
    }

    deinit {
        if let h = handle {
            asternet_client_destroy(h)
        }
    }

    /// 同步请求（在后台线程调用）
    func request(host: String, port: UInt16, method: String, path: String,
                 policy: AsterNetPolicy, headers: [String: String] = [:],
                 body: Data = Data(), timeoutMs: Int32 = 12000,
                 idempotent: Bool = true) -> AsterNetResponse {
        guard let client = handle else {
            return AsterNetResponse(result: .errNotInit, httpStatus: 0, proto: .unknown,
                                     degraded: false, bodySize: 0, dnsMs: -1, connectMs: -1,
                                     tlsMs: -1, ttfbMs: -1, totalMs: -1, body: "")
        }

        var nativeHeaders = [asternet_header_t]()
        var headerStorage = [String]()
        for (name, value) in headers {
            headerStorage.append(name)
            headerStorage.append(value)
        }
        for i in stride(from: 0, to: headerStorage.count, by: 2) {
            nativeHeaders.append(asternet_header_t(
                name: (headerStorage[i] as NSString).utf8String,
                value: (headerStorage[i + 1] as NSString).utf8String
            ))
        }

        let rawBody = [UInt8](body)
        var req = asternet_request_t(
            host: (host as NSString).utf8String,
            port: port,
            method: (method as NSString).utf8String,
            path: (path as NSString).utf8String,
            headers: nativeHeaders.isEmpty ? nil : &nativeHeaders,
            header_count: nativeHeaders.count,
            body: rawBody.isEmpty ? nil : UnsafePointer(rawBody),
            body_len: rawBody.count,
            protocol_policy: asternet_protocol_policy_t(rawValue: policy.rawValue),
            timeout_ms: timeoutMs,
            idempotent: idempotent ? 1 : 0
        )

        var respBuf = [UInt8](repeating: 0, count: responseBufferSize)
        var info = asternet_response_info_t()

        let result = lock.withLock {
            asternet_client_request_sync(client, &req, &respBuf, responseBufferSize, &info)
        }

        let bodyStr: String
        if result == ASTERNET_OK && info.body_copied > 0 {
            bodyStr = String(bytes: respBuf[0..<Int(info.body_copied)], encoding: .utf8) ?? ""
        } else {
            bodyStr = ""
        }

        return AsterNetResponse(
            result: AsterNetResult(rawValue: result) ?? .errInternal,
            httpStatus: Int(info.http_status),
            proto: AsterNetProtocol(rawValue: info.protocol.rawValue) ?? .unknown,
            degraded: info.degraded != 0,
            bodySize: Int(info.body_size),
            dnsMs: info.dns_ms,
            connectMs: info.connect_ms,
            tlsMs: info.tls_ms,
            ttfbMs: info.ttfb_ms,
            totalMs: info.total_ms,
            body: bodyStr
        )
    }

    static func version() -> String {
        String(cString: asternet_version())
    }
}
