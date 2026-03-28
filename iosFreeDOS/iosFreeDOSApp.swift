import SwiftUI

@main
struct iosFreeDOSApp: App {
    var body: some Scene {
        WindowGroup {
            ContentView()
        }
        .commands {
            CommandGroup(replacing: .appInfo) {
                Button("About FreeDOS") {
                    NotificationCenter.default.post(name: .showAbout, object: nil)
                }
            }
            CommandGroup(replacing: .help) {
                Button("FreeDOS Help") {
                    NotificationCenter.default.post(name: .showHelp, object: nil)
                }
            }
        }
    }
}

extension Notification.Name {
    static let showAbout = Notification.Name("showAbout")
    static let showHelp = Notification.Name("showHelp")
}
