import SwiftUI

// MARK: - Help Index Model

struct HelpIndex: Codable {
    let version: Int
    let base_url: String
    let topics: [HelpTopic]
}

struct HelpTopic: Codable, Identifiable {
    let id: String
    let title: String
    let description: String
    let filename: String?
    let url: String?

    var isExternalLink: Bool { url != nil }
}

// MARK: - Help View

struct HelpView: View {
    @StateObject private var viewModel = HelpViewModel()
    @State private var selectedTopic: HelpTopic?
    @Environment(\.presentationMode) private var presentationMode

    var body: some View {
        NavigationView {
            Group {
                switch viewModel.indexState {
                case .loading:
                    ProgressView("Loading help topics...")
                        .frame(maxWidth: .infinity, maxHeight: .infinity)

                case .loaded(let index):
                    List(index.topics) { topic in
                        Button(action: {
                            if topic.isExternalLink, let urlString = topic.url,
                               let url = URL(string: urlString) {
                                UIApplication.shared.open(url)
                            } else {
                                selectedTopic = topic
                            }
                        }) {
                            HStack {
                                VStack(alignment: .leading, spacing: 4) {
                                    Text(topic.title)
                                        .font(.headline)
                                        .foregroundColor(.primary)
                                    Text(topic.description)
                                        .font(.subheadline)
                                        .foregroundColor(.secondary)
                                }
                                if topic.isExternalLink {
                                    Spacer()
                                    Image(systemName: "arrow.up.right.square")
                                        .foregroundColor(.secondary)
                                }
                            }
                            .padding(.vertical, 4)
                        }
                    }
                    .listStyle(.insetGrouped)

                case .error(let message):
                    VStack(spacing: 16) {
                        Image(systemName: "exclamationmark.triangle")
                            .font(.largeTitle)
                            .foregroundColor(.orange)
                        Text("Failed to load help")
                            .font(.headline)
                        Text(message)
                            .font(.subheadline)
                            .foregroundColor(.secondary)
                            .multilineTextAlignment(.center)
                        Button("Retry") {
                            viewModel.fetchIndex()
                        }
                    }
                    .padding()
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
                }
            }
            .navigationTitle("Help")
            #if os(iOS)
            .navigationBarTitleDisplayMode(.inline)
            #endif
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button("Done") {
                        presentationMode.wrappedValue.dismiss()
                    }
                }
            }
            .sheet(item: $selectedTopic) { topic in
                HelpTopicView(viewModel: viewModel, topic: topic)
            }
        }
        .onAppear {
            if case .loading = viewModel.indexState {
                viewModel.fetchIndex()
            }
        }
    }
}

// MARK: - Help Topic View

struct HelpTopicView: View {
    @ObservedObject var viewModel: HelpViewModel
    let topic: HelpTopic
    @Environment(\.presentationMode) private var presentationMode

    var body: some View {
        NavigationView {
            Group {
                switch viewModel.contentState(for: topic.id) {
                case .loading:
                    ProgressView("Loading...")
                        .frame(maxWidth: .infinity, maxHeight: .infinity)

                case .loaded(let content):
                    ScrollView {
                        HelpMarkdownView(content: content)
                            .padding()
                    }

                case .error(let message):
                    VStack(spacing: 16) {
                        Image(systemName: "exclamationmark.triangle")
                            .font(.largeTitle)
                            .foregroundColor(.orange)
                        Text("Failed to load content")
                            .font(.headline)
                        Text(message)
                            .font(.subheadline)
                            .foregroundColor(.secondary)
                        Button("Retry") {
                            viewModel.fetchContent(for: topic)
                        }
                    }
                    .padding()
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
                }
            }
            .navigationTitle(topic.title)
            #if os(iOS)
            .navigationBarTitleDisplayMode(.inline)
            #endif
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button("Done") {
                        presentationMode.wrappedValue.dismiss()
                    }
                }
            }
        }
        .onAppear {
            viewModel.fetchContent(for: topic)
        }
    }
}

// MARK: - Simple Markdown View

struct HelpMarkdownView: View {
    let content: String

    var body: some View {
        if #available(iOS 15.0, macOS 12.0, *) {
            Text(attributedContent)
                .frame(maxWidth: .infinity, alignment: .leading)
        } else {
            Text(content)
                .frame(maxWidth: .infinity, alignment: .leading)
        }
    }

    @available(iOS 15.0, macOS 12.0, *)
    private var attributedContent: AttributedString {
        do {
            return try AttributedString(markdown: content, options: .init(interpretedSyntax: .inlineOnlyPreservingWhitespace))
        } catch {
            return AttributedString(content)
        }
    }
}

// MARK: - Help View Model

class HelpViewModel: ObservableObject {
    enum LoadState<T> {
        case loading
        case loaded(T)
        case error(String)
    }

    @Published var indexState: LoadState<HelpIndex> = .loading
    @Published private var contentCache: [String: LoadState<String>] = [:]

    private static let indexURL = "https://github.com/avwohl/qxDOS/releases/latest/download/help_index.json"
    private var baseURL: String = "https://github.com/avwohl/qxDOS/releases/latest/download/"

    private var cacheDirectory: URL {
        FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("help", isDirectory: true)
    }

    init() {
        try? FileManager.default.createDirectory(at: cacheDirectory, withIntermediateDirectories: true)
    }

    func contentState(for topicId: String) -> LoadState<String> {
        return contentCache[topicId] ?? .loading
    }

    // MARK: - Bundled fallback

    /// Load index or content from the app bundle (release_assets/ copied into Resources/)
    private func bundledIndex() -> HelpIndex? {
        guard let url = Bundle.main.url(forResource: "help_index", withExtension: "json"),
              let data = try? Data(contentsOf: url),
              let index = try? JSONDecoder().decode(HelpIndex.self, from: data) else {
            return nil
        }
        return index
    }

    private func bundledContent(for filename: String) -> String? {
        let name = (filename as NSString).deletingPathExtension
        let ext = (filename as NSString).pathExtension
        guard let url = Bundle.main.url(forResource: name, withExtension: ext),
              let content = try? String(contentsOf: url, encoding: .utf8) else {
            return nil
        }
        return content
    }

    /// Try cache, then bundle; returns nil if neither has data
    private func offlineIndex(cachedURL: URL) -> HelpIndex? {
        if let data = try? Data(contentsOf: cachedURL),
           let index = try? JSONDecoder().decode(HelpIndex.self, from: data) {
            return index
        }
        return bundledIndex()
    }

    private func offlineContent(for topic: HelpTopic, cachedURL: URL) -> String? {
        if let content = try? String(contentsOf: cachedURL, encoding: .utf8) {
            return content
        }
        guard let filename = topic.filename else { return nil }
        return bundledContent(for: filename)
    }

    // MARK: - Fetch

    func fetchIndex() {
        indexState = .loading

        guard let url = URL(string: Self.indexURL) else {
            if let index = bundledIndex() {
                baseURL = index.base_url
                indexState = .loaded(index)
            } else {
                indexState = .error("Invalid URL")
            }
            return
        }

        let cachedIndexURL = cacheDirectory.appendingPathComponent("help_index.json")

        URLSession.shared.dataTask(with: url) { [weak self] data, response, error in
            DispatchQueue.main.async {
                if error != nil {
                    if let index = self?.offlineIndex(cachedURL: cachedIndexURL) {
                        self?.baseURL = index.base_url
                        self?.indexState = .loaded(index)
                    } else {
                        self?.indexState = .error("No internet connection and no cached help available.")
                    }
                    return
                }

                if let httpResponse = response as? HTTPURLResponse,
                   httpResponse.statusCode != 200 {
                    if let index = self?.offlineIndex(cachedURL: cachedIndexURL) {
                        self?.baseURL = index.base_url
                        self?.indexState = .loaded(index)
                    } else {
                        self?.indexState = .error("Server returned status \(httpResponse.statusCode)")
                    }
                    return
                }

                guard let data = data, !data.isEmpty else {
                    if let index = self?.offlineIndex(cachedURL: cachedIndexURL) {
                        self?.baseURL = index.base_url
                        self?.indexState = .loaded(index)
                    } else {
                        self?.indexState = .error("No data received from server")
                    }
                    return
                }

                do {
                    let remoteIndex = try JSONDecoder().decode(HelpIndex.self, from: data)
                    // Use whichever index has the higher version
                    let bundled = self?.bundledIndex()
                    let index: HelpIndex
                    if let bundled = bundled, bundled.version > remoteIndex.version {
                        index = bundled
                    } else {
                        index = remoteIndex
                        try? data.write(to: cachedIndexURL)
                    }
                    self?.baseURL = index.base_url
                    self?.indexState = .loaded(index)
                } catch {
                    // Network parse failed — try offline
                    if let index = self?.offlineIndex(cachedURL: cachedIndexURL) {
                        self?.baseURL = index.base_url
                        self?.indexState = .loaded(index)
                    } else {
                        let preview = String(data: data.prefix(100), encoding: .utf8) ?? "(binary)"
                        self?.indexState = .error("Parse error: \(error.localizedDescription)\nResponse: \(preview)...")
                    }
                }
            }
        }.resume()
    }

    func fetchContent(for topic: HelpTopic) {
        if case .loaded = contentCache[topic.id] {
            return
        }

        guard let filename = topic.filename else { return }

        contentCache[topic.id] = .loading

        let urlString = baseURL + filename
        guard let url = URL(string: urlString) else {
            // Try offline
            if let content = bundledContent(for: filename) {
                contentCache[topic.id] = .loaded(content)
            } else {
                contentCache[topic.id] = .error("Invalid URL")
            }
            return
        }

        let cachedFileURL = cacheDirectory.appendingPathComponent(filename)

        URLSession.shared.dataTask(with: url) { [weak self] data, response, error in
            DispatchQueue.main.async {
                if error != nil {
                    if let content = self?.offlineContent(for: topic, cachedURL: cachedFileURL) {
                        self?.contentCache[topic.id] = .loaded(content)
                    } else {
                        self?.contentCache[topic.id] = .error("No internet connection and no cached content available.")
                    }
                    return
                }

                if let httpResponse = response as? HTTPURLResponse,
                   httpResponse.statusCode != 200 {
                    if let content = self?.offlineContent(for: topic, cachedURL: cachedFileURL) {
                        self?.contentCache[topic.id] = .loaded(content)
                    } else {
                        self?.contentCache[topic.id] = .error("HTTP \(httpResponse.statusCode)")
                    }
                    return
                }

                guard let data = data, !data.isEmpty,
                      let content = String(data: data, encoding: .utf8) else {
                    if let content = self?.offlineContent(for: topic, cachedURL: cachedFileURL) {
                        self?.contentCache[topic.id] = .loaded(content)
                    } else {
                        self?.contentCache[topic.id] = .error("Failed to decode content")
                    }
                    return
                }

                self?.contentCache[topic.id] = .loaded(content)
                try? content.write(to: cachedFileURL, atomically: true, encoding: .utf8)
            }
        }.resume()
    }
}
