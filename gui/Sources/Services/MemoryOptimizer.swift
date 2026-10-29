import Foundation
import AppKit
import Darwin

public final class MemoryOptimizer {
    public static let shared = MemoryOptimizer()

    private var optimizationDebounceWorkItem: DispatchWorkItem?
    private let lock = NSLock()

    private init() {}

    /// Trigger full memory reclamation across all caches and return free pages to macOS.
    @MainActor
    public func optimizeForBackground(viewModel: AppViewModel? = nil) {
        // 1. Purge image & artwork caches
        ArtworkStore.shared.purgeMemoryCaches()
        GameCoverService.shared.purgeMemoryCaches()
        CompatibilityService.shared.purgeTransientCaches()

        // 2. Clear ephemeral URL caches
        URLCache.shared.removeAllCachedResponses()

        // 3. Trim live logs if not actively building
        if let vm = viewModel, !vm.isBuilding {
            vm.trimHistoricalLogs(keepCount: 200)
        }

        // 4. Signal Darwin allocator to return free memory pages to macOS kernel
        malloc_zone_pressure_relief(nil, 0)
    }

    /// Schedule an optimization pass after a brief debounce period when idle
    public func scheduleIdleOptimization(viewModel: AppViewModel? = nil, delay: TimeInterval = 4.0) {
        lock.lock()
        optimizationDebounceWorkItem?.cancel()
        let item = DispatchWorkItem { [weak self] in
            Task { @MainActor [weak self] in
                guard let self = self else { return }
                self.optimizeForBackground(viewModel: viewModel)
            }
        }
        optimizationDebounceWorkItem = item
        lock.unlock()

        DispatchQueue.main.asyncAfter(deadline: .now() + delay, execute: item)
    }

    /// Cancel any pending scheduled optimization
    public func cancelScheduledOptimization() {
        lock.lock()
        optimizationDebounceWorkItem?.cancel()
        optimizationDebounceWorkItem = nil
        lock.unlock()
    }
}
