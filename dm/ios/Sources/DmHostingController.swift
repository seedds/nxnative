import SwiftUI
import UIKit

// Full-screen SwiftUI presentations (the launcher) follow the game's orientation policy: every orientation, so the app's list decides, unless Rotation locks it (front_ios.m dm_rotation_vc_mask).
final class DmHostingController<Content: View>: UIHostingController<Content> {
    override var supportedInterfaceOrientations: UIInterfaceOrientationMask { dm_rotation_vc_mask() }
}
