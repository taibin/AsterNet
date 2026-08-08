import SwiftUI

struct ContentView: View {
    @EnvironmentObject var client: AsterNetClient

    var body: some View {
        TabView {
            PresetsView()
                .tabItem { Label("Presets", systemImage: "list.bullet.rectangle") }

            CustomRequestView()
                .tabItem { Label("Custom", systemImage: "slider.horizontal.3") }
        }
        .tint(.indigo)
    }
}
