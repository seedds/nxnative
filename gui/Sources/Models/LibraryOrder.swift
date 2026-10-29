import Foundation

/// User-defined order of the library shelves, stored as app paths. Games not in the saved list
/// (new installs) keep their scan order after the known ones.
public enum LibraryOrder {
    private static let key = "NXNative_LibraryOrder"

    private static func id(_ game: PortedGame) -> String { game.url.standardized.path }

    public static func save(_ games: [PortedGame]) {
        UserDefaults.standard.set(games.map(id), forKey: key)
    }

    public static func apply(to games: [PortedGame]) -> [PortedGame] {
        guard let saved = UserDefaults.standard.stringArray(forKey: key), !saved.isEmpty else { return games }
        let rank = Dictionary(saved.enumerated().map { ($1, $0) }, uniquingKeysWith: { first, _ in first })
        return games.enumerated().sorted { a, b in
            let ra = rank[id(a.element)] ?? Int.max
            let rb = rank[id(b.element)] ?? Int.max
            return ra != rb ? ra < rb : a.offset < b.offset
        }.map { $0.element }
    }
}
