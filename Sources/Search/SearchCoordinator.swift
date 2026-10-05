import AppKit

final class SearchCoordinator {
    let index: FileIndex
    let providers: SystemProviders
    var onResults: (([SearchResult], Double) -> Void)?
    private var generation = 0
    private var local: [SearchResult] = []
    private var supplemental: [SearchResult] = []
    private var contentMatches: [SearchResult] = []
    private let metadata = MetadataSearch()
    private var query = ""
    private var elapsed = 0.0
    private var pending: DispatchWorkItem?

    init(index: FileIndex, providers: SystemProviders) {
        self.index = index
        self.providers = providers
    }

    func search(_ text: String, scope: SearchScope) {
        generation += 1
        let token = generation
        query = text.trimmingCharacters(in: .whitespacesAndNewlines)
        local = []; supplemental = []; contentMatches = []; elapsed = 0
        pending?.cancel()
        providers.cancelSearch()
        metadata.cancel()
        if query.isEmpty {
            let recent = Preferences.shared.recentTargets.compactMap { path -> SearchResult? in
                guard FileManager.default.fileExists(atPath: path) else { return nil }
                let url = URL(fileURLWithPath: path)
                var directory: ObjCBool = false
                FileManager.default.fileExists(atPath: path, isDirectory: &directory)
                let kind: SearchKind = url.pathExtension == "app" ? .application : directory.boolValue ? .folder : .file
                guard scope.includes(kind) else { return nil }
                return SearchResult(id: "file:" + path, title: url.deletingPathExtension().lastPathComponent, subtitle: "Recently opened · " + path.replacingOccurrences(of: FileManager.default.homeDirectoryForCurrentUser.path, with: "~"), kind: kind, target: path, score: 100)
            }
            onResults?(Array(recent.prefix(7)), 0)
            if scope == .actions { providers.search("", scope: scope) { [weak self] in guard self?.generation == token else { return }; self?.onResults?($0, 0) } }
            return
        }
        let current = query
        onResults?([], -1)
        index.search(current, scope: scope) { [weak self] values, milliseconds in
            guard let self, self.generation == token else { return }
            self.local = values
            self.elapsed = milliseconds
            self.publish()
        }
        let work = DispatchWorkItem { [weak self] in
            guard let self else { return }
            self.providers.search(current, scope: scope) { [weak self] values in
                guard let self, self.generation == token else { return }
                self.supplemental = values
                self.publish()
            }
            if Preferences.shared.contentIndexing, [.all, .files].contains(scope), current.count >= 2 {
                self.metadata.search(current) { [weak self] values in
                    guard let self, self.generation == token else { return }
                    self.contentMatches = values.filter { !self.index.excluded(URL(fileURLWithPath: $0.target)) && FileManager.default.fileExists(atPath: $0.target) }
                    self.publish()
                }
            }
        }
        pending = work
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.035, execute: work)
    }

    private func publish() {
        let values = ResultRanking.merge(local: local, supplemental: supplemental, content: contentMatches)
        onResults?(Array(values.prefix(48)), elapsed)
    }
}
