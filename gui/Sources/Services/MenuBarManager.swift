import Foundation
import AppKit
import SwiftUI

@MainActor
public final class MenuBarManager: NSObject, ObservableObject, NSWindowDelegate, NSMenuDelegate {
    public static let shared = MenuBarManager()

    nonisolated static let stateLock = NSLock()
    nonisolated(unsafe) private static var _isMinimizedRaw: Bool = false
    public nonisolated static var isMinimized: Bool {
        stateLock.lock()
        defer { stateLock.unlock() }
        return _isMinimizedRaw
    }

    private var statusItem: NSStatusItem?
    private weak var mainWindow: NSWindow?
    private weak var viewModel: AppViewModel?
    private var windowObservationTimer: Timer?

    @Published public private(set) var isMinimizedToMenuBar: Bool = false

    private override init() {
        super.init()
    }

    public func configure(with viewModel: AppViewModel) {
        self.viewModel = viewModel
        setupStatusItem()
        observeWindows()
        setupNotificationObservers()
    }

    private func setupStatusItem() {
        guard statusItem == nil else { return }

        let item = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)
        if let button = item.button {
            button.image = createMenuBarIcon()
            button.imagePosition = .imageOnly
            button.toolTip = "NxNative Launcher"
        }

        let menu = NSMenu()
        menu.delegate = self
        item.menu = menu
        self.statusItem = item
    }

    private func createMenuBarIcon() -> NSImage {
        let size = NSSize(width: 18, height: 18)
        let candidates = [
            Bundle.main.url(forResource: "AppIcon-Mono", withExtension: "png"),
            Bundle.main.url(forResource: "AppIcon-Mono", withExtension: "icns")
        ].compactMap { $0 }

        for url in candidates {
            if let img = NSImage(contentsOf: url) {
                img.size = size
                img.isTemplate = true
                return img
            }
        }

        if let symbolImage = NSImage(systemSymbolName: "gamecontroller.fill", accessibilityDescription: "NxNative") {
            symbolImage.size = size
            symbolImage.isTemplate = true
            return symbolImage
        }

        let fallback = NSImage(size: size)
        fallback.isTemplate = true
        return fallback
    }

    private func observeWindows() {
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.3) { [weak self] in
            guard let self = self else { return }
            if let win = NSApplication.shared.windows.first(where: { $0.canBecomeMain && $0.isVisible }) {
                self.registerMainWindow(win)
            }
        }
    }

    private func setupNotificationObservers() {
        NotificationCenter.default.addObserver(
            forName: NSWindow.didBecomeKeyNotification,
            object: nil,
            queue: .main
        ) { [weak self] notif in
            Task { @MainActor [weak self] in
                guard let self = self, let win = notif.object as? NSWindow else { return }
                if win.canBecomeMain && !(win is NSPanel) {
                    self.registerMainWindow(win)
                }
            }
        }

        NotificationCenter.default.addObserver(
            forName: NSWindow.willMiniaturizeNotification,
            object: nil,
            queue: .main
        ) { [weak self] notif in
            Task { @MainActor [weak self] in
                guard let self = self, let win = notif.object as? NSWindow else { return }
                if win === self.mainWindow || self.mainWindow == nil {
                    self.minimizeToMenuBar()
                }
            }
        }

        NotificationCenter.default.addObserver(
            forName: NSApplication.didResignActiveNotification,
            object: nil,
            queue: .main
        ) { [weak self] _ in
            Task { @MainActor [weak self] in
                guard let self = self else { return }
                if self.isMinimizedToMenuBar || !(self.viewModel?.isBuilding ?? false) {
                    MemoryOptimizer.shared.scheduleIdleOptimization(viewModel: self.viewModel, delay: 5.0)
                }
            }
        }
    }

    public func registerMainWindow(_ window: NSWindow) {
        guard self.mainWindow !== window else { return }
        self.mainWindow = window
        window.delegate = self
        if let miniBtn = window.standardWindowButton(.miniaturizeButton) {
            miniBtn.target = self
            miniBtn.action = #selector(handleMiniaturizeButtonClick)
        }
    }

    @objc private func handleMiniaturizeButtonClick() {
        minimizeToMenuBar()
    }

    // MARK: - NSWindowDelegate (Intercept native minimize button)
    public func windowWillMiniaturize(_ notification: Notification) {
        minimizeToMenuBar()
    }

    // MARK: - Menu Bar Actions
    public func minimizeToMenuBar() {
        guard !isMinimizedToMenuBar else { return }
        isMinimizedToMenuBar = true
        Self.stateLock.lock()
        Self._isMinimizedRaw = true
        Self.stateLock.unlock()

        if let win = mainWindow ?? NSApplication.shared.windows.first(where: { $0.canBecomeMain }) {
            self.mainWindow = win
            win.orderOut(nil)
        }

        // Switch to .accessory policy so the application disappears from the macOS Dock completely
        NSApplication.shared.setActivationPolicy(.accessory)

        // Aggressively reclaim idle memory while suspended in Menu Bar
        MemoryOptimizer.shared.optimizeForBackground(viewModel: viewModel)
    }

    public func restoreMainWindow() {
        isMinimizedToMenuBar = false
        Self.stateLock.lock()
        Self._isMinimizedRaw = false
        Self.stateLock.unlock()

        // Restore .regular policy so the app returns to the macOS Dock
        NSApplication.shared.setActivationPolicy(.regular)
        viewModel?.updateDockIcon()

        let win = mainWindow ?? NSApplication.shared.windows.first(where: { $0.canBecomeMain })
        win?.makeKeyAndOrderFront(nil)
        NSApplication.shared.activate(ignoringOtherApps: true)
    }

    public func toggleMainWindow() {
        if isMinimizedToMenuBar || !(mainWindow?.isVisible ?? false) {
            restoreMainWindow()
        } else {
            minimizeToMenuBar()
        }
    }

    // MARK: - NSMenuDelegate (Build dynamic menu items)
    public func menuNeedsUpdate(_ menu: NSMenu) {
        menu.removeAllItems()

        // 1. Status Header
        let statusTitle: String
        let statusImageName: String
        if let vm = viewModel, vm.isBuilding {
            let pct = Int(vm.progressFraction * 100)
            let gameTitle = vm.appName.isEmpty ? (vm.selectedGame?.name ?? "Game") : vm.appName
            statusTitle = L10n.t("menubar.status_building", gameTitle, pct)
            statusImageName = "gearshape.arrow.triangle.2.circlepath"
        } else if let vm = viewModel, let activeGame = vm.runningGameTitle {
            statusTitle = L10n.t("menubar.status_playing", activeGame)
            statusImageName = "play.circle.fill"
        } else {
            statusTitle = L10n.t("menubar.status_idle")
            statusImageName = "leaf.fill"
        }

        let statusItem = NSMenuItem(title: statusTitle, action: nil, keyEquivalent: "")
        statusItem.image = NSImage(systemSymbolName: statusImageName, accessibilityDescription: nil)
        statusItem.isEnabled = false
        menu.addItem(statusItem)

        menu.addItem(NSMenuItem.separator())

        // 2. Open / Show Launcher
        let openTitle = L10n.t("menubar.open")
        let openItem = NSMenuItem(title: openTitle, action: #selector(handleOpenLauncher), keyEquivalent: "o")
        openItem.target = self
        openItem.image = NSImage(systemSymbolName: "arrow.up.forward.app.fill", accessibilityDescription: nil)
        menu.addItem(openItem)

        // 3. Minimize to Menu Bar (if currently visible)
        if !isMinimizedToMenuBar && (mainWindow?.isVisible ?? false) {
            let minTitle = L10n.t("menubar.minimize")
            let minItem = NSMenuItem(title: minTitle, action: #selector(handleMinimizeToMenuBar), keyEquivalent: "m")
            minItem.target = self
            minItem.image = NSImage(systemSymbolName: "menubar.arrow.up.rectangle", accessibilityDescription: nil)
            menu.addItem(minItem)
        }

        menu.addItem(NSMenuItem.separator())

        // 4. Quick Play Submenu
        let quickPlayMenu = NSMenu()
        let macGames = viewModel?.macLibraryGames ?? []
        if macGames.isEmpty {
            let emptyItem = NSMenuItem(title: L10n.t("menubar.no_games"), action: nil, keyEquivalent: "")
            emptyItem.isEnabled = false
            quickPlayMenu.addItem(emptyItem)
        } else {
            for (idx, game) in macGames.prefix(8).enumerated() {
                let gameItem = NSMenuItem(
                    title: game.name,
                    action: #selector(handleQuickPlay(_:)),
                    keyEquivalent: idx < 9 ? "\(idx + 1)" : ""
                )
                gameItem.target = self
                gameItem.representedObject = game
                gameItem.image = NSImage(systemSymbolName: "gamecontroller", accessibilityDescription: nil)
                quickPlayMenu.addItem(gameItem)
            }
        }

        let quickPlayParent = NSMenuItem(title: L10n.t("menubar.quick_play"), action: nil, keyEquivalent: "")
        quickPlayParent.submenu = quickPlayMenu
        quickPlayParent.image = NSImage(systemSymbolName: "play.fill", accessibilityDescription: nil)
        menu.addItem(quickPlayParent)

        menu.addItem(NSMenuItem.separator())

        // 5. Free Standby RAM
        let freeMemoryItem = NSMenuItem(title: L10n.t("menubar.free_memory"), action: #selector(handleFreeMemory), keyEquivalent: "")
        freeMemoryItem.target = self
        freeMemoryItem.image = NSImage(systemSymbolName: "memorychip", accessibilityDescription: nil)
        menu.addItem(freeMemoryItem)

        menu.addItem(NSMenuItem.separator())

        // 6. Quit
        let quitItem = NSMenuItem(title: L10n.t("menubar.quit"), action: #selector(handleQuit), keyEquivalent: "q")
        quitItem.target = self
        quitItem.image = NSImage(systemSymbolName: "power", accessibilityDescription: nil)
        menu.addItem(quitItem)
    }

    @objc private func handleOpenLauncher() {
        restoreMainWindow()
    }

    @objc private func handleMinimizeToMenuBar() {
        minimizeToMenuBar()
    }

    @objc private func handleQuickPlay(_ sender: NSMenuItem) {
        guard let game = sender.representedObject as? PortedGame, let vm = viewModel else { return }
        vm.launchGame(game)
    }

    @objc private func handleFreeMemory() {
        MemoryOptimizer.shared.optimizeForBackground(viewModel: viewModel)
    }

    @objc private func handleQuit() {
        NSApplication.shared.terminate(nil)
    }
}
