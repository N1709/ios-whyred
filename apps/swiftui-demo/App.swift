import SwiftUI

@main
struct DemoApp: App {
    var body: some Scene {
        WindowGroup {
            ContentView()
        }
    }
}

struct ContentView: View {
    @State private var count = 0
    @State private var name = ""

    var body: some View {
        NavigationView {
            Form {
                Section("Counter") {
                    Stepper("Count: \(count)", value: $count)
                    Button("Reset") { count = 0 }
                }
                Section("Text") {
                    TextField("Your name", text: $name)
                    Text(name.isEmpty ? "Hello!" : "Hello, \(name)!")
                }
                Section("Device") {
                    Text(UIDevice.current.systemName + " " + UIDevice.current.systemVersion)
                    Text(UIDevice.current.model)
                }
            }
            .navigationTitle("SwiftUI Demo")
        }
    }
}
