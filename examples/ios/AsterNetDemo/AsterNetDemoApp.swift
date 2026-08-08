import SwiftUI

@main
struct AsterNetDemoApp: App {
    @StateObject private var client = AsterNetClient()

    var body: some Scene {
        WindowGroup {
            ContentView()
                .environmentObject(client)
        }
    }
}
