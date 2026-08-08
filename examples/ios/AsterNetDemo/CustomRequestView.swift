import SwiftUI

struct CustomRequestView: View {
    @EnvironmentObject var client: AsterNetClient

    @State private var host = "www.cloudflare.com"
    @State private var port = "443"
    @State private var path = "/"
    @State private var selectedMethod = "GET"
    @State private var selectedPolicy: AsterNetPolicy = .auto
    @State private var headers = ""
    @State private var bodyText = ""
    @State private var timeout = "12000"

    @State private var isLoading = false
    @State private var resultText = ""

    private let methods = ["GET", "POST", "PUT", "DELETE"]

    var body: some View {
        NavigationStack {
            ScrollView {
                VStack(alignment: .leading, spacing: 16) {
                    // Target
                    GroupBox(label: Label("Target", systemImage: "globe")) {
                        VStack(spacing: 8) {
                            TextField("Host / Domain", text: $host)
                                .textFieldStyle(.roundedBorder)
                                .autocapitalization(.none)
                            HStack {
                                TextField("Port", text: $port)
                                    .textFieldStyle(.roundedBorder)
                                    .frame(width: 100)
                                    .keyboardType(.numberPad)
                                TextField("Path", text: $path)
                                    .textFieldStyle(.roundedBorder)
                                    .autocapitalization(.none)
                            }
                        }
                    }

                    // Method + Protocol
                    GroupBox(label: Label("Method & Protocol", systemImage: "arrow.left.arrow.right")) {
                        VStack(alignment: .leading, spacing: 8) {
                            Text("Method").font(.subheadline).foregroundColor(.secondary)
                            Picker("Method", selection: $selectedMethod) {
                                ForEach(methods, id: \.self) { Text($0).tag($0) }
                            }
                            .pickerStyle(.segmented)

                            Text("Protocol").font(.subheadline).foregroundColor(.secondary)
                            Picker("Protocol", selection: $selectedPolicy) {
                                ForEach(AsterNetPolicy.allCases) {
                                    Text($0.label).tag($0)
                                }
                            }
                            .pickerStyle(.menu)
                        }
                    }

                    // Headers
                    GroupBox(label: Label("Headers", systemImage: "text.alignleft")) {
                        TextField("Name: Value, one per line", text: $headers, axis: .vertical)
                            .textFieldStyle(.roundedBorder)
                            .lineLimit(4...8)
                            .autocapitalization(.none)
                    }

                    // Body
                    if selectedMethod == "POST" || selectedMethod == "PUT" {
                        GroupBox(label: Label("Body", systemImage: "doc.text")) {
                            TextField("Request body...", text: $bodyText, axis: .vertical)
                                .textFieldStyle(.roundedBorder)
                                .lineLimit(4...10)
                        }
                    }

                    // Settings
                    GroupBox(label: Label("Settings", systemImage: "gearshape")) {
                        HStack {
                            Text("Timeout:")
                            TextField("ms", text: $timeout)
                                .textFieldStyle(.roundedBorder)
                                .frame(width: 100)
                                .keyboardType(.numberPad)
                            Text("ms")
                            Spacer()
                        }
                    }

                    // Send
                    Button(action: send) {
                        HStack {
                            if isLoading { ProgressView().tint(.white) }
                            Text(isLoading ? "Sending..." : "Send Request")
                        }
                        .frame(maxWidth: .infinity)
                        .padding(.vertical, 14)
                    }
                    .buttonStyle(.borderedProminent)
                    .disabled(isLoading || host.isEmpty)

                    // Result
                    if !resultText.isEmpty {
                        GroupBox(label: Label("Response", systemImage: "arrow.down.doc")) {
                            ScrollView(.horizontal) {
                                Text(resultText)
                                    .font(.caption)
                                    .monospaced()
                                    .padding(8)
                                    .background(Color(.systemGray6))
                                    .cornerRadius(8)
                            }
                        }
                    }
                }
                .padding()
            }
            .navigationTitle("Custom Request")
        }
    }

    private func send() {
        guard !host.isEmpty else { return }
        let hostStr = host.trimmingCharacters(in: .whitespaces)
        let pathStr = path.trimmingCharacters(in: .whitespaces)
        let portNum = UInt16(port.trimmingCharacters(in: .whitespaces)) ?? 443
        let timeoutMs = Int32(timeout.trimmingCharacters(in: .whitespaces)) ?? 12000

        // 解析 headers
        var headerDict = [String: String]()
        for line in headers.split(separator: "\n") {
            let parts = line.split(separator: ":", maxSplits: 1)
            if parts.count == 2 {
                headerDict[parts[0].trimmingCharacters(in: .whitespaces)] =
                    parts[1].trimmingCharacters(in: .whitespaces)
            }
        }

        let bodyData = bodyText.data(using: .utf8) ?? Data()
        let displayUrl = "https://\(hostStr)\(portNum != 443 ? ":\(portNum)" : "")\(pathStr)"

        isLoading = true
        resultText = "Running \(selectedMethod) \(displayUrl)...\n"

        Task.detached {
            let start = Date()
            let resp = await client.request(
                host: hostStr, port: portNum, method: selectedMethod, path: pathStr,
                policy: selectedPolicy, headers: headerDict, body: bodyData,
                timeoutMs: timeoutMs,
                idempotent: selectedMethod == "GET" || selectedMethod == "HEAD"
            )
            let wallMs = Int(Date().timeIntervalSince(start) * 1000)

            var s = ""
            s += "Request\nCustom \(selectedMethod) \(displayUrl)\nPolicy: \(selectedPolicy.label)\n\n"
            s += "Result\nSuccess: \(resp.result == .ok)\nError: \(resp.result) (\(resp.result.rawValue))\n"
            s += "HTTP: \(resp.httpStatus)  Proto: \(resp.proto.label)\n"
            s += "Body: \(resp.bodySize) B  Core: \(resp.totalMs) ms  Wall: \(wallMs) ms\n\n"
            s += resp.body.isEmpty ? "<empty>" : String(resp.body.prefix(500))

            await MainActor.run {
                resultText = s
                isLoading = false
            }
        }
    }
}
