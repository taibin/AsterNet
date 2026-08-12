package io.asternet;

import org.json.JSONObject;

public final class AsterNet {
    public static final int ABI_VERSION = 0x00010000;

    public static final class Policy {
        public static final int AUTO = 0;
        public static final int HTTP_1_1_ONLY = 1;
        public static final int HTTP_2_ONLY = 2;
        public static final int HTTP_3_ONLY = 3;
        public static final int PREFER_HTTP_3 = 4;
        public static final int PREFER_HTTP_2 = 5;

        private Policy() {}
    }

    public static final class Response {
        public final int result;
        public final String error;
        public final int status;
        public final int protocol;
        public final boolean degraded;
        public final long bodySize;
        public final long dnsMs;
        public final long connectMs;
        public final long tlsMs;
        public final long ttfbMs;
        public final long totalMs;
        public final String body;

        Response(JSONObject json) {
            result = json.optInt("result", -100);
            error = json.optString("error", "INTERNAL");
            status = json.optInt("status", 0);
            protocol = json.optInt("protocol", 0);
            degraded = json.optInt("degraded", 0) != 0;
            bodySize = json.optLong("body_size", 0L);
            dnsMs = json.optLong("dns_ms", -1L);
            connectMs = json.optLong("connect_ms", -1L);
            tlsMs = json.optLong("tls_ms", -1L);
            ttfbMs = json.optLong("ttfb_ms", -1L);
            totalMs = json.optLong("total_ms", -1L);
            body = json.optString("body", "");
        }

        public boolean isSuccess() { return result == 0; }

        public String protocolName() {
            switch (protocol) {
                case 1: return "HTTP/1.1";
                case 2: return "HTTP/2";
                case 3: return "HTTP/3";
                default: return "UNKNOWN";
            }
        }
    }

    public static final class Client implements AutoCloseable {
        private long handle;

        private Client(long handle) {
            this.handle = handle;
        }

        public synchronized Response request(String host, int port, String method, String path,
                                              int policy, String headers, byte[] body, int timeoutMs,
                                              boolean idempotent) {
            return request(host, port, method, path, policy, headers, body, timeoutMs, idempotent, false);
        }

        public synchronized Response request(String host, int port, String method, String path,
                                              int policy, String headers, byte[] body, int timeoutMs,
                                              boolean idempotent, boolean allowInsecure) {
            return requestNative(handle, host, port, method, path, policy, headers, body,
                timeoutMs, idempotent, allowInsecure);
        }

        @Override
        public synchronized void close() {
            if (handle != 0L) {
                nativeDestroyClient(handle);
                handle = 0L;
            }
        }
    }

    static {
        // 当前为静态链接模式：三方符号已嵌入 libasternet-jni.so，仅加载此库即可。
        // 切换为动态链接时，按顺序加载：crypto → ssl → nghttp2 → xquic → asternet-jni
        System.loadLibrary("asternet-jni");
    }

    private AsterNet() {}

    public static native String nativeVersion();
    private static native long nativeCreateClient(int abiVersion, boolean enableH3,
                                                    boolean allowInsecure, String caCertPem,
                                                    boolean allowPrivateNetworks);
    private static native void nativeDestroyClient(long handle);
    private static native String nativeRequest(long handle, String host, int port, String method,
                                               String path, int policy, String headers, byte[] body,
                                               int timeoutMs, boolean idempotent,
                                               boolean allowInsecure);

    public static Client createClient(boolean enableH3, String caCertPem) {
        return createClient(enableH3, false, caCertPem);
    }

    public static Client createClient(boolean enableH3, boolean allowInsecure, String caCertPem) {
        return createClient(enableH3, allowInsecure, caCertPem, false);
    }

    public static Client createClient(boolean enableH3, boolean allowInsecure, String caCertPem,
                                      boolean allowPrivateNetworks) {
        return new Client(nativeCreateClient(ABI_VERSION, enableH3, allowInsecure, caCertPem,
            allowPrivateNetworks));
    }

    private static Response requestNative(long handle, String host, int port, String method,
                                          String path, int policy, String headers, byte[] body,
                                          int timeoutMs, boolean idempotent,
                                          boolean allowInsecure) {
        try {
            return new Response(new JSONObject(nativeRequest(handle, host, port, method, path,
                policy, headers, body, timeoutMs, idempotent, allowInsecure)));
        } catch (Exception error) {
            try {
                return new Response(new JSONObject("{\"result\":-100,\"error\":\""
                    + error.getClass().getSimpleName() + "\"}"));
            } catch (Exception ignored) {
                throw new AssertionError(ignored);
            }
        }
    }
}
