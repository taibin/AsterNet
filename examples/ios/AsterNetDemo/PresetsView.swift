import SwiftUI

// MARK: - 预设场景

struct PresetScenario: Identifiable {
    let id = UUID()
    let name: String
    let description: String
    let url: String
    let method: String
    let policy: AsterNetPolicy
    let headers: [String: String]
    let body: String
    let idempotent: Bool
}

struct PresetsView: View {
    @EnvironmentObject var client: AsterNetClient

    private let scenarios: [PresetScenario] = [
        .init(name: "Automatic GET",
              description: "HTTPS GET. Tries HTTP/3, then HTTP/2, then HTTP/1.1.",
              url: "https://www.cloudflare.com/", method: "GET", policy: .auto,
              headers: [:], body: "", idempotent: true),
        .init(name: "HTTP/1.1 GET",
              description: "Forces HTTP/1.1 over TLS.",
              url: "https://www.cloudflare.com/", method: "GET", policy: .http1_1_only,
              headers: [:], body: "", idempotent: true),
        .init(name: "HTTP/2 GET",
              description: "Forces HTTP/2 over TLS with ALPN.",
              url: "https://www.cloudflare.com/", method: "GET", policy: .http2_only,
              headers: [:], body: "", idempotent: true),
        .init(name: "HTTP/3 GET",
              description: "Forces HTTP/3 over QUIC (needs UDP/443).",
              url: "https://www.cloudflare.com/", method: "GET", policy: .http3_only,
              headers: [:], body: "", idempotent: true),
        .init(name: "HTTP/2 POST",
              description: "Sends a JSON POST (non-idempotent, no fallback).",
              url: "https://nghttp2.org/httpbin/post", method: "POST", policy: .http2_only,
              headers: ["content-type": "application/json"],
              body: #"{"source":"AsterNet iOS Demo"}"#, idempotent: false),
    ]

    @State private var selectedIndex = 0
    @State private var isLoading = false
    @State private var resultText = ""

    var body: some View {
        NavigationStack {
            ScrollView {
                VStack(alignment: .leading, spacing: 16) {
                    // 场景选择器
                    Picker("Scenario", selection: $selectedIndex) {
                        ForEach(Array(scenarios.enumerated()), id: \.offset) { i, s in
                            Text(s.name).tag(i)
                        }
                    }
                    .pickerStyle(.menu)
                    .padding(.horizontal)

                    // 场景详情
                    let scenario = scenarios[selectedIndex]
                    VStack(alignment: .leading, spacing: 4) {
                        Text(scenario.description)
                            .font(.subheadline)
                            .foregroundColor(.secondary)
                        Text("\(scenario.method) \(scenario.url)")
                            .font(.caption)
                            .monospaced()
                        Text("Policy: \(scenario.policy.label)")
                            .font(.caption)
                    }
                    .padding()
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .background(Color(.systemGray6))
                    .cornerRadius(12)
                    .padding(.horizontal)

                    // 发送按钮
                    Button(action: { send(scenario: scenario) }) {
                        HStack {
                            if isLoading { ProgressView().tint(.white) }
                            Text(isLoading ? "Sending..." : "Send Request")
                        }
                        .frame(maxWidth: .infinity)
                        .padding(.vertical, 14)
                    }
                    .buttonStyle(.borderedProminent)
                    .disabled(isLoading)
                    .padding(.horizontal)

                    // 响应
                    if !resultText.isEmpty {
                        VStack(alignment: .leading, spacing: 4) {
                            Text("Response")
                                .font(.headline)
                                .foregroundColor(.indigo)
                            ScrollView(.horizontal) {
                                Text(resultText)
                                    .font(.caption)
                                    .monospaced()
                                    .padding(8)
                                    .background(Color(.systemGray6))
                                    .cornerRadius(8)
                            }
                        }
                        .padding(.horizontal)
                    }

                    Text("AsterNet \(AsterNetClient.version())")
                        .font(.caption2)
                        .foregroundColor(.secondary)
                        .frame(maxWidth: .infinity)
                        .padding(.top, 20)
                }
                .padding(.vertical)
            }
            .navigationTitle("AsterNet Presets")
        }
    }

    private func send(scenario: PresetScenario) {
        guard let url = URL(string: scenario.url),
              let host = url.host else { return }

        isLoading = true
        resultText = "Running \(scenario.name)...\n"

        let port = UInt16(url.port ?? 443)
        let path = url.path.isEmpty ? "/" : url.path + (url.query.map { "?\($0)" } ?? "")

        Task.detached {
            let start = Date()
            let resp = await client.request(
                host: host, port: port, method: scenario.method, path: path,
                policy: scenario.policy, headers: scenario.headers,
                body: scenario.body.data(using: .utf8) ?? Data(),
                timeoutMs: 12000, idempotent: scenario.idempotent
            )
            let wallMs = Int(Date().timeIntervalSince(start) * 1000)

            let output = formatResult(name: scenario.name, method: scenario.method,
                                       url: scenario.url, policy: scenario.policy,
                                       response: resp, wallMs: wallMs)

            await MainActor.run {
                resultText = output
                isLoading = false
            }
        }
    }

    private func formatResult(name: String, method: String, url: String,
                               policy: AsterNetPolicy, response: AsterNetResponse,
                               wallMs: Int) -> String {
        var s = ""
        s += "Request\n\(name)\n\(method) \(url)\nPolicy: \(policy.label)\n\n"
        s += "Result\n"
        s += "Success: \(response.result == .ok)\n"
        s += "Error: \(response.result) (\(response.result.rawValue))\n"
        s += "HTTP status: \(response.httpStatus)\n"
        s += "Protocol: \(response.proto.label)\n"
        s += "Fallback: \(response.degraded)\n"
        s += "Body size: \(response.bodySize) bytes\n\n"
        s += "Timeline\n"
        s += "DNS: \(response.dnsMs) ms\n"
        s += "Connect: \(response.connectMs) ms\n"
        s += "TLS: \(response.tlsMs) ms\n"
        s += "TTFB: \(response.ttfbMs) ms\n"
        s += "Core: \(response.totalMs) ms\n"
        s += "Wall: \(wallMs) ms\n\n"
        s += "Body\n"
        s += response.body.isEmpty ? "<empty>" : String(response.body.prefix(400))
        return s
    }
}
