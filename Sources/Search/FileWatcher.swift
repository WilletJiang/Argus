import CoreServices
import Foundation

final class FileWatcher {
    private var stream: FSEventStreamRef?
    private let onChange: (String, FSEventStreamEventFlags) -> Void

    init(paths: [String], queue: DispatchQueue, onChange: @escaping (String, FSEventStreamEventFlags) -> Void) {
        self.onChange = onChange
        var context = FSEventStreamContext(version: 0, info: Unmanaged.passUnretained(self).toOpaque(), retain: nil, release: nil, copyDescription: nil)
        stream = FSEventStreamCreate(nil, { _, info, count, rawPaths, flags, _ in
            guard let info else { return }
            let watcher = Unmanaged<FileWatcher>.fromOpaque(info).takeUnretainedValue()
            let paths = unsafeBitCast(rawPaths, to: NSArray.self)
            for index in 0..<count { if let path = paths[index] as? String { watcher.onChange(path, flags[index]) } }
        }, &context, paths as CFArray, FSEventStreamEventId(kFSEventStreamEventIdSinceNow), 0.6,
        FSEventStreamCreateFlags(kFSEventStreamCreateFlagUseCFTypes | kFSEventStreamCreateFlagFileEvents | kFSEventStreamCreateFlagWatchRoot))
        if let stream { FSEventStreamSetDispatchQueue(stream, queue); FSEventStreamStart(stream) }
    }

    deinit { if let stream { FSEventStreamStop(stream); FSEventStreamInvalidate(stream); FSEventStreamRelease(stream) } }
}
