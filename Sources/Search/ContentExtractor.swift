import AppKit
import PDFKit

enum ContentExtractor {
    private static let textExtensions: Set<String> = ["txt", "md", "markdown", "csv", "tsv", "json", "yaml", "yml", "toml", "ini", "log", "tex", "bib", "swift", "c", "h", "cpp", "hpp", "py", "js", "ts", "html", "css", "sh", "xml", "sql", "rs", "go"]
    static let byteLimit = 32768

    static func supports(_ url: URL, size: Int) -> Bool {
        let ext = url.pathExtension.lowercased()
        return size > 0 && size <= (ext == "pdf" ? 10_000_000 : 2_000_000) &&
            (textExtensions.contains(ext) || ["pdf", "rtf", "docx", "doc", "odt"].contains(ext))
    }

    // The bounded independent cache supplies snippets; MetadataSearch covers system-indexed tails and other formats.
    static func extract(_ url: URL) -> String {
        let ext = url.pathExtension.lowercased()
        if ext == "pdf", let document = PDFDocument(url: url) {
            var parts: [String] = [], length = 0
            for page in 0..<min(document.pageCount, 24) {
                if let value = document.page(at: page)?.string { parts.append(value); length += value.utf8.count }
                if length >= byteLimit { break }
            }
            return bounded(parts.joined(separator: "\n"))
        }
        if ["rtf", "docx", "doc", "odt"].contains(ext) {
            return bounded((try? NSAttributedString(url: url, options: [:], documentAttributes: nil))?.string ?? "")
        }
        guard let handle = try? FileHandle(forReadingFrom: url) else { return "" }
        defer { try? handle.close() }
        guard let data = try? handle.read(upToCount: byteLimit) else { return "" }
        if data.starts(with: [0xff, 0xfe]) || data.starts(with: [0xfe, 0xff]) {
            let value = String(data: data, encoding: .utf16) ?? String(data: data.dropLast(2), encoding: .utf16) ?? ""
            return bounded(value)
        }
        if data.contains(0) { return "" }
        for trim in 0..<min(4, data.count) {
            if let value = String(data: data.dropLast(trim), encoding: .utf8) { return bounded(value) }
        }
        return bounded(String(decoding: data, as: UTF8.self))
    }

    private static func bounded(_ value: String) -> String {
        let bytes = Array(value.utf8.prefix(byteLimit))
        var end = bytes.count
        while end > 0, String(bytes: bytes[..<end], encoding: .utf8) == nil { end -= 1 }
        return String(decoding: bytes[..<end], as: UTF8.self)
    }
}
