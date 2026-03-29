import SwiftUI

@main
struct qxDOSApp: App {
    var body: some Scene {
        WindowGroup {
            ContentView()
        }
        .commands {
            CommandGroup(replacing: .appInfo) {
                Button("About qxDOS") {
                    NotificationCenter.default.post(name: .showAbout, object: nil)
                }
            }
            CommandGroup(replacing: .help) {
                Button("qxDOS Help") {
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
