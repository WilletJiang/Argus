import AppKit
import Contacts

final class SystemProviders {
    private struct IndexedItem {
        var result: SearchResult
        private let title: String
        private let text: String
        private let aliases: String

        init(result: SearchResult, text: String) {
            self.result = result
            title = SearchText.normalized(result.title)
            self.text = SearchText.normalized(text)
            aliases = SearchText.aliases(result.title + " " + text)
        }

        func score(_ query: String) -> Double {
            let score = max(Self.match(query, in: title), Self.match(query, in: text))
            if score > 0 { return score }
            return aliases.contains(query) ? 500 : 0
        }

        private static func match(_ query: String, in text: String) -> Double {
            if text == query { return 1000 }
            if text.hasPrefix(query) { return 850 }
            if text.contains(query) { return 650 }
            return query.split(whereSeparator: \.isWhitespace).allSatisfy { text.contains($0) } ? 550 : 0
        }
    }

    private let queue: OperationQueue = {
        let queue = OperationQueue()
        queue.name = "Argus.system-search"
        queue.qualityOfService = .userInitiated
        queue.maxConcurrentOperationCount = 1
        return queue
    }()
    private let lock = NSLock()
    private var generation = 0
    private var contactStatus = "Contacts: enable access in Settings"
    private var shortcutStatus = "Shortcuts: loading"
    private var contacts: [IndexedItem] = []
    private var shortcuts: [IndexedItem] = []
    private var contactsLoading = false
    private var contactRefreshPending = false
    private var contactRefreshCompletions: [() -> Void] = []
    private var shortcutsLoading = false
    private var shortcutsUpdated = Date.distantPast
    private var contactsObserver: NSObjectProtocol?
    private var mailQuery: NSMetadataQuery?
    private var mailObservers: [NSObjectProtocol] = []
    private let dictionary = DictionaryProvider()
    private var definitionClient: ScanClient?

    var status: String {
        lock.lock()
        defer { lock.unlock() }
        return "\(contactStatus) · \(shortcutStatus) · Mail: indexed Apple Mail messages only"
    }

    init() {}

    deinit {
        if let observer = contactsObserver { NotificationCenter.default.removeObserver(observer) }
        mailObservers.forEach(NotificationCenter.default.removeObserver)
        let query = mailQuery
        queue.addOperation { query?.stop() }
    }

    func prepare() {
        queue.addOperation { [weak self] in
            guard let self else { return }
            if self.contactsObserver == nil {
                self.contactsObserver = NotificationCenter.default.addObserver(
                    forName: .CNContactStoreDidChange, object: nil, queue: self.queue
                ) { [weak self] _ in self?.refreshContacts() }
            }
            self.refreshContacts()
            self.refreshShortcuts()
        }
    }

    func requestContactsAccess(completion: @escaping (String) -> Void) {
        guard Bundle.main.object(forInfoDictionaryKey: "NSContactsUsageDescription") != nil else {
            DispatchQueue.main.async { completion("The app is missing a contacts permission description. Rebuild the app.") }
            return
        }
        CNContactStore().requestAccess(for: .contacts) { [weak self] granted, error in
            guard let self else { return }
            self.queue.addOperation {
                self.refreshContacts {
                    let message = granted ? "Contacts access enabled. \(self.contacts.count) contacts loaded." :
                        (error?.localizedDescription ?? "Contacts access denied. Allow Argus in System Settings → Privacy & Security → Contacts.")
                    DispatchQueue.main.async { completion(message) }
                }
            }
        }
    }

    func search(_ query: String, scope: SearchScope, completion: @escaping ([SearchResult]) -> Void) {
        lock.lock()
        generation += 1
        let version = generation
        let oldDefinition = definitionClient
        lock.unlock()
        oldDefinition?.cancel()
        queue.addOperation { [weak self] in
            guard let self, self.isCurrent(version) else { return }
            self.stopMailQuery()
            let text = query.trimmingCharacters(in: .whitespacesAndNewlines)
            let normalized = SearchText.normalized(text)
            if Date().timeIntervalSince(self.shortcutsUpdated) > 120 { self.refreshShortcuts() }
            if !self.contactsAuthorized { self.contacts.removeAll() }

            if scope == .all, let web = self.webResults(text) {
                self.deliver(web, version: version, completion: completion)
                return
            }

            let candidates = self.systemActions + self.shortcuts + self.contacts
            var results = candidates.compactMap { item -> SearchResult? in
                guard scope.includes(item.result.kind) else { return nil }
                var result = item.result
                if text.isEmpty {
                    guard result.kind == .action else { return nil }
                    result.score = 100
                } else {
                    result.score = item.score(normalized)
                    guard result.score > 0 else { return nil }
                }
                return result
            }
            results.sort { $0.score == $1.score ? $0.title < $1.title : $0.score > $1.score }
            results = Array(results.prefix(text.isEmpty ? 8 : 40))
            self.deliver(results, version: version, completion: completion)
            if scope == .all, !text.isEmpty, let definition = self.dictionary.result(for: text, lookup: { self.lookupDefinition($0, version: version) }), self.isCurrent(version) {
                results.append(definition)
                results.sort { $0.score > $1.score }
                self.deliver(results, version: version, completion: completion)
            }
            if scope.includes(.mail), text.count >= 2 {
                self.startMailQuery(text, base: results, version: version, completion: completion)
            }
        }
    }

    func cancelSearch() {
        lock.lock()
        generation += 1
        let client = definitionClient
        lock.unlock()
        client?.cancel()
        queue.addOperation { [weak self] in self?.stopMailQuery() }
    }

    private var contactsAuthorized: Bool {
        let authorization = CNContactStore.authorizationStatus(for: .contacts)
        return authorization == .authorized
    }

    private func isCurrent(_ version: Int) -> Bool {
        lock.lock()
        defer { lock.unlock() }
        return generation == version
    }

    private func deliver(_ results: [SearchResult], version: Int, completion: @escaping ([SearchResult]) -> Void) {
        guard isCurrent(version) else { return }
        DispatchQueue.main.async { [weak self] in
            guard self?.isCurrent(version) == true else { return }
            completion(results)
        }
    }

    private func refreshContacts(completion: (() -> Void)? = nil) {
        if let completion { contactRefreshCompletions.append(completion) }
        guard contactsAuthorized else {
            contacts.removeAll()
            contactRefreshPending = false
            let authorization = CNContactStore.authorizationStatus(for: .contacts)
            lock.lock()
            contactStatus = authorization == .denied || authorization == .restricted ?
                "Contacts: allow access in System Settings → Privacy & Security → Contacts" : "Contacts: enable access in Argus Settings"
            lock.unlock()
            finishContactRefresh()
            return
        }
        guard !contactsLoading else { contactRefreshPending = true; return }
        contactsLoading = true
        DispatchQueue.global(qos: .utility).async { [weak self] in
            guard let self else { return }
            let keys: [CNKeyDescriptor] = [CNContactFormatter.descriptorForRequiredKeys(for: .fullName),
                CNContactIdentifierKey as NSString, CNContactOrganizationNameKey as NSString,
                CNContactNicknameKey as NSString, CNContactEmailAddressesKey as NSString,
                CNContactPhoneNumbersKey as NSString, CNContactPhoneticGivenNameKey as NSString,
                CNContactPhoneticFamilyNameKey as NSString]
            var items: [IndexedItem] = []
            var failure: String?
            do {
                try CNContactStore().enumerateContacts(with: CNContactFetchRequest(keysToFetch: keys)) { contact, _ in
                    let emails = contact.emailAddresses.map { String($0.value) }
                    let phones = contact.phoneNumbers.map { $0.value.stringValue }
                    let name = CNContactFormatter.string(from: contact, style: .fullName) ?? contact.organizationName
                    let title = name.isEmpty ? (emails.first ?? phones.first ?? "Unnamed contact") : name
                    let destination: String
                    let subtitle: String
                    if let email = emails.first {
                        destination = "mailto:" + Self.encodedAddress(email)
                        subtitle = "Compose email · \(email)"
                    } else if let phone = phones.first {
                        destination = "tel:" + Self.encodedAddress(phone)
                        subtitle = "Call · \(phone)"
                    } else {
                        destination = "/System/Applications/Contacts.app"
                        subtitle = contact.organizationName.isEmpty ? "Open Contacts" : contact.organizationName + " · Open Contacts"
                    }
                    let result = SearchResult(id: "contact:\(contact.identifier)", title: title,
                                              subtitle: subtitle, kind: .contact, target: destination, score: 0)
                    let words = [title, contact.nickname, contact.organizationName, contact.phoneticGivenName,
                                 contact.phoneticFamilyName] + emails + phones
                    items.append(IndexedItem(result: result, text: words.joined(separator: " ")))
                }
            } catch { failure = error.localizedDescription }
            let collected = items
            let errorMessage = failure
            self.queue.addOperation {
                self.contactsLoading = false
                guard self.contactsAuthorized else { self.refreshContacts(); return }
                self.contacts = collected
                self.lock.lock()
                self.contactStatus = errorMessage.map { "Contacts: \($0)" } ?? "Contacts: \(self.contacts.count) loaded"
                self.lock.unlock()
                if self.contactRefreshPending {
                    self.contactRefreshPending = false
                    self.refreshContacts()
                } else { self.finishContactRefresh() }
            }
        }
    }

    private func finishContactRefresh() {
        let callbacks = contactRefreshCompletions
        contactRefreshCompletions.removeAll()
        callbacks.forEach { $0() }
    }

    private static func encodedAddress(_ value: String) -> String {
        value.addingPercentEncoding(withAllowedCharacters: CharacterSet(charactersIn: "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789@.+-_")) ?? ""
    }

    private func refreshShortcuts() {
        guard !shortcutsLoading else { return }
        shortcutsLoading = true
        shortcutsUpdated = Date()
        DispatchQueue.global(qos: .utility).async { [weak self] in
            guard let self else { return }
            let process = Process()
            let pipe = Pipe()
            process.executableURL = URL(fileURLWithPath: "/usr/bin/shortcuts")
            process.arguments = ["list", "--show-identifiers"]
            process.standardOutput = pipe
            process.standardError = FileHandle.nullDevice
            var names: [String] = []
            var succeeded = false
            do {
                try process.run()
                let timeout = DispatchWorkItem { if process.isRunning { process.terminate() } }
                DispatchQueue.global(qos: .utility).asyncAfter(deadline: .now() + 8, execute: timeout)
                var data = Data()
                while let chunk = try pipe.fileHandleForReading.read(upToCount: 65536), !chunk.isEmpty {
                    data.append(chunk.prefix(max(0, 1_048_576 - data.count)))
                }
                process.waitUntilExit()
                timeout.cancel()
                succeeded = process.terminationStatus == 0
                if succeeded, let output = String(data: data, encoding: .utf8) {
                    names = Array(output.split(whereSeparator: \.isNewline).map(String.init).prefix(2000))
                }
            } catch { succeeded = false }
            let found = names.map { line in
                let suffix = line.suffix(38)
                let identifier = suffix.hasPrefix("(") && suffix.hasSuffix(")") ?
                    UUID(uuidString: String(suffix.dropFirst().dropLast()))?.uuidString : nil
                let name = identifier == nil ? line : String(line.dropLast(38)).trimmingCharacters(in: .whitespaces)
                let target = identifier ?? name
                return IndexedItem(result: SearchResult(id: "shortcut:\(target)", title: name,
                    subtitle: "Run shortcut", kind: .shortcut, target: target, score: 0), text: name + " 快捷指令 shortcut")
            }
            let didSucceed = succeeded
            self.queue.addOperation {
                self.shortcutsLoading = false
                self.shortcuts = found
                self.lock.lock()
                self.shortcutStatus = didSucceed ? "Shortcuts: \(found.count)" : "Shortcuts: unavailable; retrying later"
                self.lock.unlock()
            }
        }
    }

    private func lookupDefinition(_ word: String, version: Int) -> DictionaryProvider.Lookup {
        let client = ScanClient()
        let done = DispatchSemaphore(value: 0)
        var value: String?
        lock.lock()
        guard generation == version else { lock.unlock(); return .cancelled }
        definitionClient = client
        client.perform(ScanRequest(mode: .dictionary, path: word)) { result in value = result.text; done.signal() }
        lock.unlock()
        done.wait()
        lock.lock()
        if definitionClient === client { definitionClient = nil }
        lock.unlock()
        client.close()
        return isCurrent(version) ? .completed(value) : .cancelled
    }

    private func webResults(_ query: String) -> [SearchResult]? {
        if let url = URL(string: query), ["https", "http"].contains(url.scheme?.lowercased() ?? ""),
           let host = url.host, !host.isEmpty {
            return [SearchResult(id: "web:\(url.absoluteString)", title: "Open website: \(host)", subtitle: url.absoluteString,
                kind: .web, target: url.absoluteString, score: 1200)]
        }
        let prefixes = ["搜索网页 ", "网页 ", "web ", "google ", "ddg ", "duckduckgo ", "bing "]
        guard let prefix = prefixes.first(where: { SearchText.normalized(query).hasPrefix($0) }) else { return nil }
        let text = String(query.dropFirst(prefix.count)).trimmingCharacters(in: .whitespaces)
        guard !text.isEmpty else { return [] }
        let engines = [("Google", "https://www.google.com/search"), ("DuckDuckGo", "https://duckduckgo.com/"), ("Bing", "https://www.bing.com/search")].filter { name, _ in
            if prefix == "google " { return name == "Google" }
            if prefix == "ddg " || prefix == "duckduckgo " { return name == "DuckDuckGo" }
            if prefix == "bing " { return name == "Bing" }
            return true
        }
        return engines.compactMap { name, address in
            guard var url = URLComponents(string: address) else { return nil }
            url.queryItems = [URLQueryItem(name: "q", value: text)]
            let encodedQuery = url.percentEncodedQuery?.replacingOccurrences(of: "+", with: "%2B")
            url.percentEncodedQuery = encodedQuery
            guard let target = url.url?.absoluteString else { return nil }
            return SearchResult(id: "web:\(name):\(text)", title: "Search \(name): \(text)",
                subtitle: "Press Return to search in your browser", kind: .web, target: target, score: 1100)
        }
    }

    private func startMailQuery(_ text: String, base: [SearchResult], version: Int, completion: @escaping ([SearchResult]) -> Void) {
        let query = NSMetadataQuery()
        query.operationQueue = queue
        query.notificationBatchingInterval = 0.15
        query.predicate = NSPredicate(format: "kMDItemContentType == %@ AND (kMDItemTitle CONTAINS[cd] %@ OR kMDItemAuthors CONTAINS[cd] %@ OR kMDItemAuthorEmailAddresses CONTAINS[cd] %@ OR kMDItemRecipients CONTAINS[cd] %@ OR kMDItemRecipientEmailAddresses CONTAINS[cd] %@ OR kMDItemTextContent CONTAINS[cd] %@)",
            "com.apple.mail.emlx", text, text, text, text, text, text)
        query.sortDescriptors = [NSSortDescriptor(key: "kMDItemContentCreationDate", ascending: false)]
        mailQuery = query
        for notification in [NSNotification.Name.NSMetadataQueryGatheringProgress, .NSMetadataQueryDidFinishGathering] {
            let observer = NotificationCenter.default.addObserver(forName: notification, object: query, queue: queue) { [weak self] note in
                guard let self, self.isCurrent(version), self.mailQuery === query else { return }
                self.publishMail(query, text: text, base: base, version: version, completion: completion)
                if note.name == .NSMetadataQueryDidFinishGathering { self.stopMailQuery() }
            }
            mailObservers.append(observer)
        }
        if !query.start() { stopMailQuery(); return }
        DispatchQueue.global(qos: .utility).asyncAfter(deadline: .now() + 3) { [weak self] in
            self?.queue.addOperation { [weak self] in
                guard let self, self.isCurrent(version), self.mailQuery === query else { return }
                self.publishMail(query, text: text, base: base, version: version, completion: completion)
                self.stopMailQuery()
            }
        }
    }

    private func publishMail(_ query: NSMetadataQuery, text: String, base: [SearchResult], version: Int, completion: @escaping ([SearchResult]) -> Void) {
        query.disableUpdates()
        defer { query.enableUpdates() }
        var results = base
        for index in 0..<min(query.resultCount, 30) {
            guard let item = query.result(at: index) as? NSMetadataItem,
                  let path = item.value(forAttribute: NSMetadataItemPathKey) as? String else { continue }
            let title = (item.value(forAttribute: NSMetadataItemTitleKey) as? String) ?? "Mail"
            let authors = (item.value(forAttribute: NSMetadataItemAuthorsKey) as? [String]) ?? []
            let date = item.value(forAttribute: NSMetadataItemContentCreationDateKey) as? Date
            let subtitle = ([authors.joined(separator: ", "), date.map { $0.formatted(date: .abbreviated, time: .omitted) } ?? ""])
                .filter { !$0.isEmpty }.joined(separator: " · ")
            results.append(SearchResult(id: "mail:\(path)", title: title, subtitle: subtitle.isEmpty ? "Apple Mail · System index" : subtitle,
                kind: .mail, target: path, score: max(430, SearchText.score(text, in: title)),
                snippet: "Matches an indexed mail subject, sender, recipient, or body. Press Return to open in Mail."))
        }
        deliver(results.sorted { $0.score > $1.score }, version: version, completion: completion)
    }

    private func stopMailQuery() {
        mailObservers.forEach(NotificationCenter.default.removeObserver)
        mailObservers.removeAll()
        mailQuery?.stop()
        mailQuery = nil
    }

    private lazy var systemActions = makeSystemActions()

    private func makeSystemActions() -> [IndexedItem] {
        let actions: [(String, String, String, String, String)] = [
            ("System Settings", "Open System Settings", "/System/Applications/System Settings.app", "系统设置 设置 settings preferences", "gearshape"),
            ("Screenshot & Recording", "Open the system screenshot tool", "/System/Applications/Utilities/Screenshot.app", "截图与录屏 截图 截屏 录屏 screenshot capture recording", "camera"),
            ("Toggle Dark Mode", "Switch between light and dark mode", "argus-action:appearance", "切换深色外观 深色 浅色 外观 dark light appearance mode", "moon.fill"),
            ("Volume Up", "Increase volume by 10%", "argus-action:volume-up", "调高音量 增加音量 volume up louder", "speaker.wave.3.fill"),
            ("Volume Down", "Decrease volume by 10%", "argus-action:volume-down", "调低音量 降低音量 volume down quieter", "speaker.wave.1.fill"),
            ("Toggle Mute", "Toggle system audio mute", "argus-action:mute", "切换静音 静音 mute unmute sound", "speaker.slash.fill"),
            ("Sleep Display", "Put the display to sleep", "argus-action:display-sleep", "关闭显示器 屏幕睡眠 screen display sleep", "display"),
            ("Sleep", "Put this Mac to sleep", "argus-action:sleep", "睡眠 休眠 sleep suspend", "moon.zzz.fill"),
            ("Wi-Fi Settings", "Open Wi-Fi Settings", "x-apple.systempreferences:com.apple.wifi-settings-extension", "Wi-Fi 设置 无线 网络 wifi wireless", "wifi"),
            ("Bluetooth Settings", "Open Bluetooth Settings", "x-apple.systempreferences:com.apple.BluetoothSettings", "蓝牙设置 蓝牙 bluetooth", "antenna.radiowaves.left.and.right"),
            ("Display Settings", "Open Display Settings", "x-apple.systempreferences:com.apple.Displays-Settings.extension", "显示器设置 显示器 分辨率 亮度 display resolution brightness", "display"),
            ("Sound Settings", "Open Sound Settings", "x-apple.systempreferences:com.apple.Sound-Settings.extension", "声音设置 声音 音频 sound audio", "speaker.wave.2"),
            ("Keyboard Settings", "Open Keyboard Settings", "x-apple.systempreferences:com.apple.Keyboard-Settings.extension", "键盘设置 键盘 keyboard", "keyboard"),
            ("Privacy & Security", "Open Privacy & Security Settings", "x-apple.systempreferences:com.apple.settings.PrivacySecurity.extension", "隐私与安全性 隐私 安全 权限 privacy security permissions", "hand.raised"),
            ("Open Downloads", "Open Downloads in Finder", FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent("Downloads").path, "打开下载文件夹 下载 downloads", "arrow.down.circle"),
            ("Open Documents", "Open Documents in Finder", FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent("Documents").path, "打开文稿文件夹 文稿 documents", "doc"),
            ("Open Desktop", "Open Desktop in Finder", FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent("Desktop").path, "打开桌面文件夹 桌面 desktop", "desktopcomputer")
        ]
        return actions.map { title, subtitle, target, aliases, symbol in
            IndexedItem(result: SearchResult(id: "action:\(target)", title: title, subtitle: subtitle,
                kind: .action, target: target, score: 0, symbol: symbol), text: title + " " + aliases)
        }
    }
}
