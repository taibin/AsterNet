/* AsterNet Android JNI bridge for the Network Lab demo. */
#include <jni.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "asternet/asternet.h"

namespace {

class UtfChars {
public:
    UtfChars(JNIEnv *env, jstring value) : env_(env), value_(value) {
        if (value_ != nullptr) chars_ = env_->GetStringUTFChars(value_, nullptr);
    }
    ~UtfChars() {
        if (chars_ != nullptr) env_->ReleaseStringUTFChars(value_, chars_);
    }
    const char *get() const { return chars_; }

private:
    JNIEnv *env_;
    jstring value_;
    const char *chars_ = nullptr;
};

std::string json_escape(const uint8_t *data, size_t size) {
    std::string escaped;
    escaped.reserve(size + 32);
    static constexpr char kHex[] = "0123456789abcdef";
    auto utf8_length = [data, size](size_t offset) -> size_t {
        const uint8_t first = data[offset];
        if (first >= 0xc2 && first <= 0xdf) {
            return offset + 1 < size && (data[offset + 1] & 0xc0) == 0x80 ? 2 : 0;
        }
        if (first >= 0xe0 && first <= 0xef && offset + 2 < size) {
            const uint8_t second = data[offset + 1];
            const bool second_ok = first == 0xe0 ? second >= 0xa0 && second <= 0xbf
                                  : first == 0xed ? second >= 0x80 && second <= 0x9f
                                                  : (second & 0xc0) == 0x80;
            return second_ok && (data[offset + 2] & 0xc0) == 0x80 ? 3 : 0;
        }
        if (first >= 0xf0 && first <= 0xf4 && offset + 3 < size) {
            const uint8_t second = data[offset + 1];
            const bool second_ok = first == 0xf0 ? second >= 0x90 && second <= 0xbf
                                  : first == 0xf4 ? second >= 0x80 && second <= 0x8f
                                                  : (second & 0xc0) == 0x80;
            return second_ok && (data[offset + 2] & 0xc0) == 0x80
                       && (data[offset + 3] & 0xc0) == 0x80 ? 4 : 0;
        }
        return first < 0x80 ? 1 : 0;
    };
    for (size_t i = 0; i < size; ++i) {
        const unsigned char ch = data[i];
        switch (ch) {
        case '"': escaped += "\\\""; break;
        case '\\': escaped += "\\\\"; break;
        case '\b': escaped += "\\b"; break;
        case '\f': escaped += "\\f"; break;
        case '\n': escaped += "\\n"; break;
        case '\r': escaped += "\\r"; break;
        case '\t': escaped += "\\t"; break;
        default:
            if (ch < 0x20) {
                escaped += "\\u00";
                escaped += kHex[ch >> 4];
                escaped += kHex[ch & 0x0f];
            } else if (ch >= 0x80) {
                const size_t length = utf8_length(i);
                if (length > 0) {
                    escaped.append(reinterpret_cast<const char *>(data + i), length);
                    i += length - 1;
                } else {
                    escaped += "\\u00";
                    escaped += kHex[ch >> 4];
                    escaped += kHex[ch & 0x0f];
                }
            } else {
                escaped += static_cast<char>(ch);
            }
        }
    }
    return escaped;
}

}  // namespace

extern "C" {

JNIEXPORT jstring JNICALL
Java_io_asternet_AsterNet_nativeVersion(JNIEnv *env, jclass /*clazz*/) {
    return env->NewStringUTF(asternet_version());
}

JNIEXPORT jint JNICALL
Java_io_asternet_AsterNet_nativeAbiVersion(JNIEnv * /*env*/, jclass /*clazz*/) {
    return ASTERNET_ABI_VERSION;
}

JNIEXPORT jlong JNICALL
Java_io_asternet_AsterNet_nativeCreateClient(JNIEnv *env, jclass /*clazz*/, jint abi_version,
                                               jboolean enable_h3, jboolean allow_insecure,
                                               jstring jca_cert_pem, jboolean allow_private_networks) {
    UtfChars ca_cert_pem(env, jca_cert_pem);
    asternet_client_config_t config{};
    config.struct_size = sizeof(config);
    config.abi_version = static_cast<uint32_t>(abi_version);
    config.default_timeout_ms = 12000;
    config.max_response_body_bytes = 4 * 1024 * 1024;  // 4 MB
    config.enable_http3 = enable_h3 ? 1 : 0;
    config.allow_insecure_tls_for_testing = allow_insecure ? 1 : 0;
    config.ca_cert_pem = ca_cert_pem.get();
    config.allow_private_networks = allow_private_networks ? 1 : 0;
    asternet_result_t error = ASTERNET_OK;
    asternet_client_t *client = asternet_client_create(&config, &error);
    return client == nullptr ? 0 : reinterpret_cast<jlong>(client);
}

JNIEXPORT void JNICALL
Java_io_asternet_AsterNet_nativeDestroyClient(JNIEnv * /*env*/, jclass /*clazz*/, jlong handle) {
    asternet_client_destroy(reinterpret_cast<asternet_client_t *>(handle));
}

JNIEXPORT jint JNICALL
Java_io_asternet_AsterNet_nativePrefetch(JNIEnv *env, jclass /*clazz*/, jlong handle,
                                          jstring jhost) {
    UtfChars host(env, jhost);
    if (host.get() == nullptr) {
        return ASTERNET_ERR_INVALID_ARGUMENT;
    }
    return asternet_client_prefetch(reinterpret_cast<asternet_client_t *>(handle), host.get());
}

JNIEXPORT void JNICALL
Java_io_asternet_AsterNet_nativeOnNetworkChange(JNIEnv * /*env*/, jclass /*clazz*/, jlong handle,
                                                jint network) {
    asternet_client_on_network_change(reinterpret_cast<asternet_client_t *>(handle),
                                      static_cast<asternet_network_t>(network));
}

JNIEXPORT jstring JNICALL
Java_io_asternet_AsterNet_nativeRequest(JNIEnv *env, jclass /*clazz*/, jlong handle,
                                         jstring jhost, jint port, jstring jmethod, jstring jpath,
                                         jint policy, jstring jheaders, jbyteArray jbody,
                                         jint timeout_ms, jboolean idempotent,
                                         jboolean allow_insecure) {
    UtfChars host(env, jhost);
    UtfChars method(env, jmethod);
    UtfChars path(env, jpath);
    UtfChars headers(env, jheaders);
    if (host.get() == nullptr || method.get() == nullptr || path.get() == nullptr
        || port <= 0 || port > UINT16_MAX) {
        return env->NewStringUTF("{\"result\":-2,\"error\":\"INVALID_ARGUMENT\"}");
    }

    try {
    std::vector<uint8_t> body;
    if (jbody != nullptr) {
        const jsize length = env->GetArrayLength(jbody);
        if (length > 0) {
            body.resize(static_cast<size_t>(length));
            env->GetByteArrayRegion(jbody, 0, length, reinterpret_cast<jbyte *>(body.data()));
        }
    }

    std::vector<std::pair<std::string, std::string>> parsed_headers;
    std::vector<asternet_header_t> native_headers;
    if (headers.get() != nullptr && headers.get()[0] != '\0') {
        std::string raw(headers.get());
        size_t start = 0;
        while (start <= raw.size()) {
            const size_t end = raw.find('\n', start);
            std::string line = raw.substr(start, end == std::string::npos ? std::string::npos : end - start);
            const size_t colon = line.find(':');
            if (colon != std::string::npos && colon > 0) {
                std::string name = line.substr(0, colon);
                size_t value_start = colon + 1;
                while (value_start < line.size() && line[value_start] == ' ') ++value_start;
                parsed_headers.emplace_back(std::move(name), line.substr(value_start));
            }
            if (end == std::string::npos) break;
            start = end + 1;
        }
    }

    std::vector<std::string> header_storage;
    header_storage.reserve(parsed_headers.size() * 2);
    native_headers.reserve(parsed_headers.size());
    for (auto &header : parsed_headers) {
        header_storage.push_back(std::move(header.first));
        header_storage.push_back(std::move(header.second));
        native_headers.push_back({header_storage[header_storage.size() - 2].c_str(),
                                  header_storage.back().c_str()});
    }

    asternet_request_t request{};
    request.host = host.get();
    request.port = static_cast<uint16_t>(port);
    request.method = method.get();
    request.path = path.get();
    request.headers = native_headers.data();
    request.header_count = native_headers.size();
    request.body = body.data();
    request.body_len = body.size();
    request.protocol_policy = static_cast<asternet_protocol_policy_t>(policy);
    request.timeout_ms = timeout_ms;
    request.idempotent = idempotent ? 1 : 0;
    request.allow_insecure_tls_for_testing = allow_insecure ? 1 : 0;

    std::vector<uint8_t> response_body(4 * 1024 * 1024);  // 4 MB 缓冲区
    asternet_response_info_t response{};
    asternet_result_t result = asternet_client_request_sync(
        reinterpret_cast<asternet_client_t *>(handle), &request, response_body.data(),
        response_body.size(), &response);

    std::string json = "{\"result\":" + std::to_string(result)
        + ",\"error\":\"" + asternet_result_string(result)
        + "\",\"status\":" + std::to_string(response.http_status)
        + ",\"protocol\":" + std::to_string(response.protocol)
        + ",\"degraded\":" + std::to_string(response.degraded)
        + ",\"body_size\":" + std::to_string(response.body_size)
        + ",\"dns_ms\":" + std::to_string(response.dns_ms)
        + ",\"connect_ms\":" + std::to_string(response.connect_ms)
        + ",\"tls_ms\":" + std::to_string(response.tls_ms)
        + ",\"ttfb_ms\":" + std::to_string(response.ttfb_ms)
        + ",\"total_ms\":" + std::to_string(response.total_ms)
        + ",\"body\":\"";
    if (result == ASTERNET_OK) json += json_escape(response_body.data(), response.body_copied);
    json += "\"}";
    return env->NewStringUTF(json.c_str());
    } catch (const std::bad_alloc &) {
        return env->NewStringUTF("{\"result\":-4,\"error\":\"OUT_OF_MEMORY\"}");
    } catch (...) {
        return env->NewStringUTF("{\"result\":-100,\"error\":\"INTERNAL\"}");
    }
}

}  // extern "C"
