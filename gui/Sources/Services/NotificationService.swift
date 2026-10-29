import Foundation
import UserNotifications
import AppKit

public final class NotificationService {
    public static let shared = NotificationService()
    
    private init() {}
    
    public func requestAuthorization() {
        guard Bundle.main.bundleIdentifier != nil else { return }
        UNUserNotificationCenter.current().requestAuthorization(options: [.alert, .sound, .badge]) { granted, error in
            if let error = error {
                print("[NotificationService] Authorization error: \(error.localizedDescription)")
            }
        }
    }
    
    public func sendPortSuccessNotification(gameName: String, outputPath: String?) {
        guard Bundle.main.bundleIdentifier != nil else { return }
        let content = UNMutableNotificationContent()
        content.title = "Game Porting Complete!"
        content.subtitle = gameName
        if let path = outputPath {
            content.body = "Successfully built and ready to play at: \(URL(fileURLWithPath: path).lastPathComponent)"
        } else {
            content.body = "Successfully built and ready to play in your Games library."
        }
        content.sound = .default
        
        let request = UNNotificationRequest(
            identifier: UUID().uuidString,
            content: content,
            trigger: nil // deliver immediately
        )
        
        UNUserNotificationCenter.current().add(request) { error in
            if let error = error {
                print("[NotificationService] Failed to schedule notification: \(error)")
            }
        }
    }
    
    public func sendPortErrorNotification(gameName: String, error: String) {
        guard Bundle.main.bundleIdentifier != nil else { return }
        let content = UNMutableNotificationContent()
        content.title = "Game Porting Failed"
        content.subtitle = gameName
        content.body = error.count > 120 ? String(error.prefix(117)) + "..." : error
        content.sound = .defaultCritical
        
        let request = UNNotificationRequest(
            identifier: UUID().uuidString,
            content: content,
            trigger: nil
        )
        
        UNUserNotificationCenter.current().add(request) { err in
            if let err = err {
                print("[NotificationService] Failed to schedule error notification: \(err)")
            }
        }
    }
}
