package io.asternet.demo;

import io.asternet.AsterNet;

/** Shared formatting utilities used by both Fragments. */
final class AsterNetDemo {

    private AsterNetDemo() {}

    static String policyName(int policy) {
        switch (policy) {
            case AsterNet.Policy.HTTP_1_1_ONLY: return "HTTP/1.1 only";
            case AsterNet.Policy.HTTP_2_ONLY: return "HTTP/2 only";
            case AsterNet.Policy.HTTP_3_ONLY: return "HTTP/3 only";
            case AsterNet.Policy.PREFER_HTTP_3: return "Prefer HTTP/3";
            case AsterNet.Policy.PREFER_HTTP_2: return "Prefer HTTP/2";
            default: return "AUTO";
        }
    }

    static String formatResult(String name, String method, String displayUrl, int policy,
                               AsterNet.Response response, long wallMs) {
        StringBuilder output = new StringBuilder();
        output.append("Request\n");
        output.append(name).append('\n');
        output.append(method).append(' ').append(displayUrl).append('\n');
        output.append("Policy: ").append(policyName(policy)).append("\n\n");
        output.append("Result\n");
        output.append("Success: ").append(response.isSuccess()).append('\n');
        output.append("Error: ").append(response.error).append(" (").append(response.result).append(")\n");
        if (response.result == -8) {
            output.append("TLS note: Certificate verification or TLS negotiation failed.\n");
        }
        output.append("HTTP status: ").append(response.status).append('\n');
        output.append("Protocol: ").append(response.protocolName()).append('\n');
        output.append("Fallback used: ").append(response.degraded).append("\n");
        output.append("Response bytes: ").append(response.bodySize).append("\n\n");
        output.append("Timeline\n");
        output.append("DNS: ").append(response.dnsMs).append(" ms\n");
        output.append("Connect: ").append(response.connectMs).append(" ms\n");
        output.append("TLS: ").append(response.tlsMs).append(" ms\n");
        output.append("TTFB: ").append(response.ttfbMs).append(" ms\n");
        output.append("Core total: ").append(response.totalMs).append(" ms\n");
        output.append("Wall clock: ").append(wallMs).append(" ms\n\n");
        output.append("Body\n");
        if (response.body.isEmpty()) {
            output.append("<empty>");
        } else {
            output.append(response.body.length() > 400 ? response.body.substring(0, 400) + "\n..." : response.body);
        }
        return output.toString();
    }
}
