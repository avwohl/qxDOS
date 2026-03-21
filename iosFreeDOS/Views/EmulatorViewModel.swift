/*
 * EmulatorViewModel.swift - View model for DOS emulator
 */

import SwiftUI
import Combine
import UniformTypeIdentifiers
import CryptoKit

// MARK: - Catalog Image Types

enum CatalogImageType: String, Codable {
    case floppy
    case hdd
    case iso

    var label: String {
        switch self {
        case .floppy: return "Floppy"
        case .hdd: return "Hard Disk"
        case .iso: return "CD-ROM ISO"
        }
    }
}

// MARK: - Downloadable Disk

struct DownloadableDisk: Identifiable, Codable {
    let filename: String
    let name: String
    let description: String
    let url: String
    let sizeBytes: Int64
    let license: String
    let sha256: String?
    let defaultDrive: Int?
    let type: CatalogImageType
    var id: String { filename }

    var formattedSize: String {
        sizeBytes >= 1024 * 1024
            ? String(format: "%.1f MB", Double(sizeBytes) / (1024 * 1024))
            : "\(sizeBytes / 1024) KB"
    }
}

enum DownloadState: Equatable {
    case notDownloaded
    case downloading(progress: Double)
    case downloaded
    case error(String)
}

// MARK: - View Model

class EmulatorViewModel: NSObject, ObservableObject, DOSEmulatorDelegate {

    // Terminal
    @Published var terminalCells: [[TerminalCell]] = []
    @Published var cursorRow: Int = 0
    @Published var cursorCol: Int = 0
    @Published var terminalShouldFocus: Bool = false
    @Published var gfxImage: UIImage? = nil  // VGA mode 13h framebuffer
    let terminalRows = 25
    let terminalCols = 80
    weak var terminalView: TerminalUIView?

    // Emulator state
    @Published var isRunning: Bool = false
    @Published var isStarting: Bool = false
    @Published var statusText: String = ""
    @Published var isControlifyActive: Bool = false
    @Published var isFnActive: Bool = false
    @Published var isAltActive: Bool = false
    private var startCancelled: Bool = false

    // Configuration
    @Published var configManager = ConfigManager()

    // Disk paths
    @Published var floppyAPath: URL? = nil
    @Published var floppyBPath: URL? = nil
    @Published var hddCPath: URL? = nil
    @Published var hddDPath: URL? = nil
    @Published var isoPath: URL? = nil
    // bootDrive is stored per-config (config.bootDrive)

    // File picker
    @Published var showingDiskPicker: Bool = false
    @Published var showingDiskExporter: Bool = false
    @Published var showingError: Bool = false
    @Published var errorMessage: String = ""
    @Published var showingManifestWriteWarning: Bool = false

    // Manifest disk tracking - which drive slots hold catalog images
    private var manifestDrives: Set<Int> = []
    private var manifestWriteWarningShown = false
    private var manifestPollTimer: Timer?

    var warnManifestWrites: Bool {
        get {
            if UserDefaults.standard.object(forKey: "warnManifestWrites") == nil { return true }
            return UserDefaults.standard.bool(forKey: "warnManifestWrites")
        }
        set { UserDefaults.standard.set(newValue, forKey: "warnManifestWrites") }
    }

    // Catalog
    @Published var diskCatalog: [DownloadableDisk] = []
    @Published var downloadStates: [String: DownloadState] = [:]
    @Published var catalogLoading: Bool = false
    @Published var catalogError: String? = nil

    // URL download
    @Published var urlInput: String = ""
    @Published var urlDownloading: Bool = false
    @Published var urlDownloadProgress: Double = 0

    var currentDiskUnit: Int = 0
    var exportDocument: DiskImageDocument? = nil
    var exportFilename: String = "disk.img"

    private var emulator: DOSEmulator?
    private var diskSaveTimer: Timer?
    private var configCancellable: AnyCancellable?
    private var pendingAttachments: [String: Int] = [:]
    private var bookmarksResolved = false

    // Dedicated URLSession with no caching for disk downloads (avoids redirect caching issues)
    private lazy var downloadSession: URLSession = {
        let config = URLSessionConfiguration.ephemeral
        config.requestCachePolicy = .reloadIgnoringLocalAndRemoteCacheData
        config.urlCache = nil
        return URLSession(configuration: config)
    }()

    private let catalogURL = "https://github.com/avwohl/iosFreeDOS/releases/latest/download/disks.xml"
    private let releaseBaseURL = "https://github.com/avwohl/iosFreeDOS/releases/latest/download"

    var disksDirectory: URL {
        let dir = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("Disks")
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        return dir
    }

    // Convenience accessor for current config
    var config: MachineConfig {
        get { configManager.activeConfig ?? MachineConfig() }
        set { configManager.updateConfig(newValue) }
    }

    override init() {
        super.init()
        configCancellable = configManager.objectWillChange
            .sink { [weak self] _ in
                self?.objectWillChange.send()
            }
        clearTerminal()
        _ = loadCachedCatalog()
        // Resolve disk bookmarks off main thread — bookmark resolution can
        // stall for seconds per bookmark on real devices, freezing the UI.
        DispatchQueue.global(qos: .userInitiated).async { [weak self] in
            self?.resolveBookmarksInBackground()
        }
        fetchDiskCatalog()

    }

    func clearTerminal() {
        terminalCells = Array(
            repeating: Array(repeating: TerminalCell(), count: terminalCols),
            count: terminalRows
        )
        cursorRow = 0; cursorCol = 0
    }

    // MARK: - Emulator Lifecycle

    func start() {
        guard !isRunning else { return }
        guard floppyAPath != nil || hddCPath != nil || isoPath != nil else {
            errorMessage = "No disk image loaded."
            showingError = true
            return
        }

        // Capture state on main thread before dispatching to background
        let cfg = config
        let paths = (floppyA: floppyAPath, floppyB: floppyBPath,
                     hddC: hddCPath, hddD: hddDPath, iso: isoPath)
        let manifests = manifestDrives

        isStarting = true
        isRunning = true
        startCancelled = false
        clearTerminal()
        statusText = "Loading disks..."

        DispatchQueue.global(qos: .userInitiated).async { [weak self] in
            guard let self = self else { return }

            let emu = DOSEmulator()

            // Apply machine config
            emu.setMachineType(DOSMachineType(rawValue: cfg.machineType) ?? .svga)
            emu.setMemoryMB(Int32(cfg.memoryMB))
            emu.setMouseEnabled(cfg.mouseEnabled)
            emu.setSpeakerEnabled(cfg.speakerEnabled)
            emu.setSoundBlasterEnabled(cfg.sbEnabled)
            emu.setSpeed(DOSSpeedMode(rawValue: cfg.speedMode) ?? .max)

            // Load disks (heavy I/O, done off main thread)
            var loadError: String? = nil

            if let url = paths.floppyA, !self.startCancelled {
                _ = url.startAccessingSecurityScopedResource()
                if !emu.loadDisk(0, fromPath: url.path) { loadError = "Failed to load floppy A:" }
                url.stopAccessingSecurityScopedResource()
            }
            if let url = paths.floppyB, !self.startCancelled, loadError == nil {
                _ = url.startAccessingSecurityScopedResource()
                _ = emu.loadDisk(1, fromPath: url.path)
                url.stopAccessingSecurityScopedResource()
            }
            if let url = paths.hddC, !self.startCancelled, loadError == nil {
                _ = url.startAccessingSecurityScopedResource()
                if !emu.loadDisk(0x80, fromPath: url.path) { loadError = "Failed to load hard disk C:" }
                url.stopAccessingSecurityScopedResource()
            }
            if let url = paths.hddD, !self.startCancelled, loadError == nil {
                _ = url.startAccessingSecurityScopedResource()
                _ = emu.loadDisk(0x81, fromPath: url.path)
                url.stopAccessingSecurityScopedResource()
            }
            if let url = paths.iso, !self.startCancelled, loadError == nil {
                _ = url.startAccessingSecurityScopedResource()
                if emu.loadISO(url.path) < 0 { loadError = "Failed to load CD-ROM ISO" }
                url.stopAccessingSecurityScopedResource()
            }

            DispatchQueue.main.async {
                if self.startCancelled {
                    self.isStarting = false
                    self.isRunning = false
                    self.statusText = ""
                    return
                }
                if let error = loadError {
                    self.errorMessage = error
                    self.showingError = true
                    self.isStarting = false
                    self.isRunning = false
                    self.statusText = ""
                    return
                }

                self.emulator = emu
                emu.delegate = self

                for drive in manifests {
                    emu.setDiskIsManifest(Int32(drive), isManifest: true)
                }

                self.terminalShouldFocus = true
                self.manifestWriteWarningShown = false
                self.isStarting = false
                self.statusText = ""

                emu.start(withBootDrive: Int32(cfg.bootDrive))

                self.diskSaveTimer = Timer.scheduledTimer(withTimeInterval: 30, repeats: true) { [weak self] _ in
                    self?.saveAllDisks()
                }
                self.manifestPollTimer = Timer.scheduledTimer(withTimeInterval: 2, repeats: true) { [weak self] _ in
                    self?.checkManifestWriteWarning()
                }
            }
        }
    }

    func cancelStart() {
        startCancelled = true
        isStarting = false
        isRunning = false
        statusText = ""
    }

    func stop() {
        if isStarting { cancelStart(); return }
        guard isRunning else { return }
        diskSaveTimer?.invalidate(); diskSaveTimer = nil
        manifestPollTimer?.invalidate(); manifestPollTimer = nil
        saveAllDisks()
        emulator?.stop()
        isRunning = false
    }

    func reset() { stop(); emulator = nil }

    // MARK: - Input

    func sendKey(_ char: Character) {
        guard let emu = emulator else { return }
        let scalar = char.unicodeScalars.first?.value ?? 0

        // Arrow keys (private-use Unicode values from UIKeyCommand)
        switch scalar {
        case 0xF700: emu.sendScancode(0, scancode: 0x48); return
        case 0xF701: emu.sendScancode(0, scancode: 0x50); return
        case 0xF702: emu.sendScancode(0, scancode: 0x4B); return
        case 0xF703: emu.sendScancode(0, scancode: 0x4D); return
        default: break
        }

        // Fn mode: digits → function keys
        if isFnActive {
            if let fScan = Self.fnScancode(for: scalar) {
                isFnActive = false
                emu.sendScancode(0, scancode: fScan)
                return
            }
            isFnActive = false  // non-digit cancels Fn
        }

        // Alt mode: key → Alt+key (ascii=0, same scancode)
        if isAltActive {
            if let scan = Self.charToScancode(scalar) {
                isAltActive = false
                emu.sendScancode(0, scancode: scan)
                return
            }
            isAltActive = false
        }

        emu.sendCharacter(char.unicodeScalars.first.map { unichar($0.value) } ?? 0)

        // Controlify handled by DOSBox natively (Ctrl key combos work)
    }

    func sendDirectScancode(ascii: UInt8, scancode: UInt8) {
        emulator?.sendScancode(ascii, scancode: scancode)
    }

    func sendMouseUpdate(x: Int, y: Int, buttons: Int) {
        emulator?.updateMouseX(Int32(x), y: Int32(y), buttons: Int32(buttons))
    }

    func setControlify(_ mode: Int) {
        // Controlify is handled natively by DOSBox
        isControlifyActive = (mode != 0)
    }

    // MARK: - Scancode Tables

    /// Map digit keys to function key scancodes (Fn modifier)
    private static func fnScancode(for scalar: UInt32) -> UInt8? {
        switch scalar {
        case 0x31: return 0x3B  // 1→F1
        case 0x32: return 0x3C  // 2→F2
        case 0x33: return 0x3D  // 3→F3
        case 0x34: return 0x3E  // 4→F4
        case 0x35: return 0x3F  // 5→F5
        case 0x36: return 0x40  // 6→F6
        case 0x37: return 0x41  // 7→F7
        case 0x38: return 0x42  // 8→F8
        case 0x39: return 0x43  // 9→F9
        case 0x30: return 0x44  // 0→F10
        case 0x2D: return 0x57  // -→F11
        case 0x3D: return 0x58  // =→F12
        default:   return nil
        }
    }

    /// Map ASCII character to IBM PC keyboard scancode
    private static func charToScancode(_ scalar: UInt32) -> UInt8? {
        switch scalar {
        case 0x61...0x7A: // a-z
            let t: [UInt8] = [0x1E,0x30,0x2E,0x20,0x12,0x21,0x22,0x23,0x17,0x24,
                              0x25,0x26,0x32,0x31,0x18,0x19,0x10,0x13,0x1F,0x14,
                              0x16,0x2F,0x11,0x2D,0x15,0x2C]
            return t[Int(scalar - 0x61)]
        case 0x41...0x5A: return charToScancode(scalar + 0x20) // A-Z
        case 0x31...0x39: return UInt8(scalar - 0x31 + 0x02)   // 1-9
        case 0x30: return 0x0B  // 0
        case 0x2D: return 0x0C  // -
        case 0x3D: return 0x0D  // =
        case 0x09: return 0x0F  // Tab
        case 0x0D: return 0x1C  // Enter
        case 0x20: return 0x39  // Space
        default:   return nil
        }
    }

    func setSpeed(_ mode: DOSSpeedMode) {
        var cfg = config
        cfg.speedMode = mode.rawValue
        configManager.updateConfig(cfg)
        emulator?.setSpeed(mode)
    }

    // MARK: - Disk Management

    func loadDisk(_ unit: Int) {
        currentDiskUnit = unit
        showingDiskPicker = true
    }

    func handleDiskImport(_ result: Result<[URL], Error>) {
        guard case .success(let urls) = result, let url = urls.first else { return }
        let bookmark = try? url.bookmarkData(options: .minimalBookmark, includingResourceValuesForKeys: nil, relativeTo: nil)
        setDiskPath(currentDiskUnit, url: url, bookmark: bookmark)
    }

    func setDiskPath(_ unit: Int, url: URL, bookmark: Data? = nil) {
        switch unit {
        case 0:
            floppyAPath = url
            if let b = bookmark { UserDefaults.standard.set(b, forKey: "floppyABookmark") }
            setConfigBootDrive(0)
        case 1:
            floppyBPath = url
            if let b = bookmark { UserDefaults.standard.set(b, forKey: "floppyBBookmark") }
        case 0x80:
            hddCPath = url
            if let b = bookmark { UserDefaults.standard.set(b, forKey: "hddCBookmark") }
            if floppyAPath == nil { setConfigBootDrive(0x80) }
        case 0x81:
            hddDPath = url
            if let b = bookmark { UserDefaults.standard.set(b, forKey: "hddDBookmark") }
        case 0xE0:
            isoPath = url
            if let b = bookmark { UserDefaults.standard.set(b, forKey: "isoBookmark") }
            if floppyAPath == nil && hddCPath == nil { setConfigBootDrive(0xE0) }
        default: break
        }
        statusText = "Loaded \(url.lastPathComponent)"
    }

    private func setConfigBootDrive(_ drive: Int) {
        var cfg = config
        cfg.bootDrive = drive
        configManager.updateConfig(cfg)
    }

    func saveDisk(_ unit: Int) {
        guard let emu = emulator, let data = emu.getDiskData(Int32(unit)) else { return }
        currentDiskUnit = unit
        exportDocument = DiskImageDocument(data: data)
        exportFilename = "disk.img"
        showingDiskExporter = true
    }

    /// Export a disk file from the settings screen (no emulator needed)
    func saveDiskFile(_ url: URL) {
        guard let data = try? Data(contentsOf: url) else { return }
        exportDocument = DiskImageDocument(data: data)
        exportFilename = url.lastPathComponent
        showingDiskExporter = true
    }

    func handleExportResult(_ result: Result<URL, Error>) { exportDocument = nil }

    func createBlankFloppy(sizeKB: Int) {
        let data = Data(repeating: 0, count: sizeKB * 1024)
        let url = disksDirectory.appendingPathComponent("floppy_\(sizeKB)KB.img")
        do {
            try data.write(to: url)
            floppyAPath = url; setConfigBootDrive(0)
            UserDefaults.standard.removeObject(forKey: "floppyABookmark")
            statusText = "Created \(sizeKB)KB floppy"
        } catch {
            errorMessage = error.localizedDescription; showingError = true
        }
    }

    func createBlankHDD(sizeMB: Int) {
        let url = disksDirectory.appendingPathComponent("hdd_\(sizeMB)MB.img")
        if FileManager.default.createFile(atPath: url.path, contents: nil) {
            FileHandle(forWritingAtPath: url.path)?.truncateFile(atOffset: UInt64(sizeMB) * 1024 * 1024)
            hddCPath = url
            if floppyAPath == nil { setConfigBootDrive(0x80) }
            UserDefaults.standard.removeObject(forKey: "hddCBookmark")
            statusText = "Created \(sizeMB)MB hard disk"
        } else {
            errorMessage = "Failed to create hard disk"; showingError = true
        }
    }

    func saveAllDisks() {
        guard let emu = emulator else { return }
        for (drive, path) in [(0, floppyAPath), (1, floppyBPath), (0x80, hddCPath), (0x81, hddDPath)] {
            guard let url = path, url.path.contains("/Documents/"),
                  let data = emu.getDiskData(Int32(drive)) else { continue }
            try? data.write(to: url)
        }
    }

    func saveDisksOnBackground() { saveAllDisks() }

    func removeDisk(_ unit: Int) {
        switch unit {
        case 0: floppyAPath = nil; UserDefaults.standard.removeObject(forKey: "floppyABookmark")
        case 1: floppyBPath = nil; UserDefaults.standard.removeObject(forKey: "floppyBBookmark")
        case 0x80: hddCPath = nil; UserDefaults.standard.removeObject(forKey: "hddCBookmark")
        case 0x81: hddDPath = nil; UserDefaults.standard.removeObject(forKey: "hddDBookmark")
        case 0xE0: isoPath = nil; UserDefaults.standard.removeObject(forKey: "isoBookmark")
        default: break
        }
        manifestDrives.remove(unit)
    }

    /// Resolve security-scoped bookmarks on a background thread, then apply
    /// results on main.  Bookmark resolution can take seconds per entry on
    /// real devices (especially after reboot or for iCloud files).
    private func resolveBookmarksInBackground() {
        let keys = ["floppyABookmark", "floppyBBookmark", "hddCBookmark", "hddDBookmark", "isoBookmark"]
        var resolved: [(String, URL)] = []
        for key in keys {
            if let data = UserDefaults.standard.data(forKey: key) {
                var stale = false
                if let url = try? URL(resolvingBookmarkData: data, bookmarkDataIsStale: &stale) {
                    resolved.append((key, url))
                }
            }
        }
        DispatchQueue.main.async { [weak self] in
            guard let self = self else { return }
            for (key, url) in resolved {
                switch key {
                case "floppyABookmark": self.floppyAPath = url
                case "floppyBBookmark": self.floppyBPath = url
                case "hddCBookmark":    self.hddCPath = url
                case "hddDBookmark":    self.hddDPath = url
                case "isoBookmark":     self.isoPath = url
                default: break
                }
            }
            self.bookmarksResolved = true
            self.autoAttachDefaultDisks()
        }
    }

    // MARK: - Disk Catalog

    func fetchDiskCatalog() {
        guard let url = URL(string: catalogURL) else { return }
        catalogLoading = true; catalogError = nil
        URLSession.shared.dataTask(with: url) { [weak self] data, response, error in
            DispatchQueue.main.async {
                guard let self = self else { return }
                self.catalogLoading = false
                if error != nil {
                    _ = self.loadCachedCatalog() ? () : (self.catalogError = error!.localizedDescription)
                    return
                }
                if let http = response as? HTTPURLResponse, http.statusCode != 200 {
                    _ = self.loadCachedCatalog() ? () : (self.catalogError = "HTTP \(http.statusCode) from \(url.absoluteString)")
                    return
                }
                guard let data = data else { self.catalogError = "No data"; return }
                try? data.write(to: self.disksDirectory.appendingPathComponent("disks_catalog.xml"))
                self.parseCatalogXML(data)
                self.refreshDownloadStates()
                self.autoAttachDefaultDisks()
            }
        }.resume()
    }

    private func loadCachedCatalog() -> Bool {
        guard let data = try? Data(contentsOf: disksDirectory.appendingPathComponent("disks_catalog.xml")) else { return false }
        parseCatalogXML(data); refreshDownloadStates()
        autoAttachDefaultDisks()
        return !diskCatalog.isEmpty
    }

    private func parseCatalogXML(_ data: Data) {
        let parser = DiskCatalogXMLParser(data: data, baseURL: releaseBaseURL)
        diskCatalog = parser.parse()
        if let newVersion = parser.catalogVersion.map(String.init) {
            checkCatalogVersionAndInvalidate(newVersion: newVersion)
        }
    }

    func refreshDownloadStates() {
        for disk in diskCatalog {
            let path = disksDirectory.appendingPathComponent(disk.filename)
            downloadStates[disk.filename] = FileManager.default.fileExists(atPath: path.path) ? .downloaded : (downloadStates[disk.filename] ?? .notDownloaded)
        }
    }

    /// On first launch (no disks attached), auto-download and attach default disks from catalog.
    /// Only disks with a <defaultDrive> tag are auto-downloaded.
    private func autoAttachDefaultDisks() {
        guard bookmarksResolved else { return }
        let hasAnyDisk = floppyAPath != nil || floppyBPath != nil || hddCPath != nil || hddDPath != nil || isoPath != nil
        guard !hasAnyDisk else { return }

        var usedDrives = Set<Int>()
        for disk in diskCatalog {
            guard let drive = disk.defaultDrive else { continue }
            guard !usedDrives.contains(drive) else { continue }
            usedDrives.insert(drive)
            attachOrDownloadCatalogDisk(disk, forDrive: drive)
        }
    }

    // MARK: - Catalog Versioning

    private func checkCatalogVersionAndInvalidate(newVersion: String) {
        let storedVersion = UserDefaults.standard.string(forKey: "catalogVersion") ?? ""
        if storedVersion.isEmpty {
            UserDefaults.standard.set(newVersion, forKey: "catalogVersion")
        } else if storedVersion != newVersion {
            deleteAllDownloadedDisks()
            UserDefaults.standard.set(newVersion, forKey: "catalogVersion")
            statusText = "Catalog updated - disks need redownload"
            errorMessage = "Disk catalog has been updated (v\(storedVersion) -> v\(newVersion)). Downloaded disks have been cleared and need to be redownloaded."
            showingError = true
        }
    }

    private func deleteAllDownloadedDisks() {
        let fm = FileManager.default
        if let contents = try? fm.contentsOfDirectory(at: disksDirectory, includingPropertiesForKeys: nil) {
            for url in contents {
                let ext = url.pathExtension.lowercased()
                if ext == "img" || ext == "iso" || ext == "bin" {
                    try? fm.removeItem(at: url)
                    downloadStates[url.lastPathComponent] = .notDownloaded
                }
            }
        }
        // Clear drive paths that pointed to deleted catalog disks
        let disksDir = disksDirectory.path
        let pairs: [(Int, KeyPath<EmulatorViewModel, URL?>)] =
            [(0, \.floppyAPath), (1, \.floppyBPath), (0x80, \.hddCPath), (0x81, \.hddDPath), (0xE0, \.isoPath)]
        for (unit, kp) in pairs {
            if let path = self[keyPath: kp], path.path.hasPrefix(disksDir) {
                removeDisk(unit)
            }
        }
        // Clear any manifest drive assignments that pointed to catalog disks
        manifestDrives.removeAll()
    }

    // MARK: - Manifest Write Warning

    private func checkManifestWriteWarning() {
        guard warnManifestWrites, !manifestWriteWarningShown else { return }
        if emulator?.pollManifestWriteWarning() == true {
            manifestWriteWarningShown = true
            showingManifestWriteWarning = true
        }
    }

    /// Returns download progress (0–1) for a pending catalog disk attachment
    /// targeting the given drive, or nil if no download is active for that drive.
    func downloadProgressForDrive(_ drive: Int) -> Double? {
        for (filename, pendingDrive) in pendingAttachments {
            if pendingDrive == drive, case .downloading(let progress) = downloadStates[filename] {
                return progress
            }
        }
        return nil
    }

    func downloadDisk(_ disk: DownloadableDisk) {
        downloadDiskWithRetry(disk, attemptsRemaining: 3)
    }

    private func downloadDiskWithRetry(_ disk: DownloadableDisk, attemptsRemaining: Int) {
        guard let url = URL(string: disk.url) else {
            downloadStates[disk.filename] = .error("Invalid URL: \(disk.url)")
            return
        }
        downloadStates[disk.filename] = .downloading(progress: 0)

        let task = downloadSession.downloadTask(with: url) { [weak self] temp, resp, err in
            // Stage the temp file immediately — URLSession deletes it after
            // this completion handler returns, so it won't survive an async
            // dispatch to the main queue.
            var stagedURL: URL? = nil
            if let temp = temp {
                let staging = FileManager.default.temporaryDirectory
                    .appendingPathComponent(UUID().uuidString + "_" + disk.filename)
                do {
                    try FileManager.default.moveItem(at: temp, to: staging)
                    stagedURL = staging
                } catch {
                    // Will report error on main queue
                }
            }

            DispatchQueue.main.async {
                guard let self = self else { return }

                if let http = resp as? HTTPURLResponse, http.statusCode < 200 || http.statusCode >= 300 {
                    if attemptsRemaining > 1 {
                        DispatchQueue.main.asyncAfter(deadline: .now() + 1.0) {
                            self.downloadDiskWithRetry(disk, attemptsRemaining: attemptsRemaining - 1)
                        }
                    } else {
                        self.downloadStates[disk.filename] = .error("HTTP \(http.statusCode) from \(disk.url)")
                        self.pendingAttachments.removeValue(forKey: disk.filename)
                    }
                    return
                }

                if let err = err {
                    if attemptsRemaining > 1 {
                        DispatchQueue.main.asyncAfter(deadline: .now() + 1.0) {
                            self.downloadDiskWithRetry(disk, attemptsRemaining: attemptsRemaining - 1)
                        }
                    } else {
                        self.downloadStates[disk.filename] = .error("\(err.localizedDescription) (\(disk.url))")
                        self.pendingAttachments.removeValue(forKey: disk.filename)
                    }
                    return
                }

                guard let staged = stagedURL else {
                    if attemptsRemaining > 1 {
                        DispatchQueue.main.asyncAfter(deadline: .now() + 1.0) {
                            self.downloadDiskWithRetry(disk, attemptsRemaining: attemptsRemaining - 1)
                        }
                    } else {
                        self.downloadStates[disk.filename] = .error("Download failed: \(disk.url)")
                        self.pendingAttachments.removeValue(forKey: disk.filename)
                    }
                    return
                }

                if let expected = disk.sha256, !expected.isEmpty,
                   let d = try? Data(contentsOf: staged) {
                    let hash = SHA256.hash(data: d).map { String(format: "%02x", $0) }.joined()
                    if hash != expected.lowercased() {
                        try? FileManager.default.removeItem(at: staged)
                        self.downloadStates[disk.filename] = .error("SHA256 mismatch")
                        self.pendingAttachments.removeValue(forKey: disk.filename)
                        return
                    }
                }

                let dest = self.disksDirectory.appendingPathComponent(disk.filename)
                try? FileManager.default.removeItem(at: dest)
                do {
                    try FileManager.default.moveItem(at: staged, to: dest)
                    self.downloadStates[disk.filename] = .downloaded
                    self.statusText = "Downloaded \(disk.name)"
                    // Auto-attach if there's a pending attachment for this disk
                    if let drive = self.pendingAttachments.removeValue(forKey: disk.filename) {
                        self.attachDiskPath(dest, forDrive: drive, diskName: disk.name)
                        self.manifestDrives.insert(drive)
                    }
                } catch {
                    try? FileManager.default.removeItem(at: staged)
                    self.downloadStates[disk.filename] = .error(error.localizedDescription)
                    self.pendingAttachments.removeValue(forKey: disk.filename)
                }
            }
        }
        let obs = task.progress.observe(\.fractionCompleted) { [weak self] p, _ in
            DispatchQueue.main.async { self?.downloadStates[disk.filename] = .downloading(progress: p.fractionCompleted) }
        }
        objc_setAssociatedObject(task, "obs", obs, .OBJC_ASSOCIATION_RETAIN)
        task.resume()
    }

    /// Explicit user selection from catalog — use cached copy if available,
    /// otherwise download.
    func useCatalogDisk(_ disk: DownloadableDisk, forDrive drive: Int) {
        let path = disksDirectory.appendingPathComponent(disk.filename)
        if FileManager.default.fileExists(atPath: path.path) {
            attachDiskPath(path, forDrive: drive, diskName: disk.name)
            manifestDrives.insert(drive)
        } else {
            pendingAttachments[disk.filename] = drive
            statusText = "Downloading \(disk.name)..."
            downloadDisk(disk)
        }
    }

    /// Use cached disk if available, otherwise download.  Used for
    /// auto-attach on startup to avoid unnecessary network traffic.
    private func attachOrDownloadCatalogDisk(_ disk: DownloadableDisk, forDrive drive: Int) {
        let path = disksDirectory.appendingPathComponent(disk.filename)
        if FileManager.default.fileExists(atPath: path.path) {
            attachDiskPath(path, forDrive: drive, diskName: disk.name)
            manifestDrives.insert(drive)
        } else {
            pendingAttachments[disk.filename] = drive
            statusText = "Downloading \(disk.name)..."
            downloadDisk(disk)
        }
    }

    /// Delete a downloaded catalog disk image from local storage.
    func deleteCatalogDisk(_ disk: DownloadableDisk) {
        let path = disksDirectory.appendingPathComponent(disk.filename)
        try? FileManager.default.removeItem(at: path)
        downloadStates[disk.filename] = .notDownloaded
        // Detach from any drive that references this file
        let pairs: [(Int, URL?)] = [(0, floppyAPath), (1, floppyBPath),
                                     (0x80, hddCPath), (0x81, hddDPath), (0xE0, isoPath)]
        for (unit, diskPath) in pairs {
            if diskPath?.lastPathComponent == disk.filename {
                removeDisk(unit)
            }
        }
    }

    private func attachDiskPath(_ path: URL, forDrive drive: Int, diskName: String) {
        switch drive {
        case 0: floppyAPath = path; setConfigBootDrive(0)
        case 1: floppyBPath = path
        case 0x80: hddCPath = path; if floppyAPath == nil { setConfigBootDrive(0x80) }
        case 0x81: hddDPath = path
        case 0xE0: isoPath = path; if floppyAPath == nil && hddCPath == nil { setConfigBootDrive(0xE0) }
        default: break
        }
        statusText = "Loaded \(diskName)"
    }

    func downloadFromURL(toDrive drive: Int) {
        let trimmed = urlInput.trimmingCharacters(in: .whitespacesAndNewlines)
        guard let url = URL(string: trimmed), url.scheme == "http" || url.scheme == "https" else {
            errorMessage = "Enter a valid URL"; showingError = true; return
        }
        urlDownloading = true; urlDownloadProgress = 0
        let task = downloadSession.downloadTask(with: url) { [weak self] temp, resp, err in
            DispatchQueue.main.async {
                guard let self = self else { return }
                self.urlDownloading = false
                if let err = err { self.errorMessage = "\(err.localizedDescription)\n\(trimmed)"; self.showingError = true; return }
                if let http = resp as? HTTPURLResponse, http.statusCode != 200 { self.errorMessage = "HTTP \(http.statusCode) from \(trimmed)"; self.showingError = true; return }
                guard let temp = temp else { return }
                let filename = url.lastPathComponent.isEmpty ? "download.img" : url.lastPathComponent
                let dest = self.disksDirectory.appendingPathComponent(filename)
                try? FileManager.default.removeItem(at: dest)
                do {
                    try FileManager.default.moveItem(at: temp, to: dest)
                    switch drive {
                    case 0: self.floppyAPath = dest; self.setConfigBootDrive(0)
                    case 1: self.floppyBPath = dest
                    case 0x80: self.hddCPath = dest; if self.floppyAPath == nil { self.setConfigBootDrive(0x80) }
                    case 0x81: self.hddDPath = dest
                    case 0xE0: self.isoPath = dest; if self.floppyAPath == nil && self.hddCPath == nil { self.setConfigBootDrive(0xE0) }
                    default: break
                    }
                    self.statusText = "Downloaded \(filename)"
                    self.urlInput = ""
                } catch { self.errorMessage = error.localizedDescription; self.showingError = true }
            }
        }
        let obs = task.progress.observe(\.fractionCompleted) { [weak self] p, _ in
            DispatchQueue.main.async { self?.urlDownloadProgress = p.fractionCompleted }
        }
        objc_setAssociatedObject(task, "obs", obs, .OBJC_ASSOCIATION_RETAIN)
        task.resume()
    }

    // MARK: - DOSEmulatorDelegate

    /// DOSBox rendered a new frame (RGBA pixels)
    func emulatorFrameReady(_ pixels: Data, width: Int32, height: Int32) {
        let w = Int(width), h = Int(height)
        let bytes = [UInt8](pixels)
        let colorSpace = CGColorSpaceCreateDeviceRGB()
        var pixelData = bytes
        if let ctx = CGContext(data: &pixelData, width: w, height: h, bitsPerComponent: 8,
                               bytesPerRow: w * 4, space: colorSpace,
                               bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue),
           let cgImage = ctx.makeImage() {
            gfxImage = UIImage(cgImage: cgImage)
        }
    }

    func emulatorDidRequestInput() {
        if !terminalShouldFocus { terminalShouldFocus = true }
    }

    func emulatorDidExit() {
        diskSaveTimer?.invalidate(); diskSaveTimer = nil
        manifestPollTimer?.invalidate(); manifestPollTimer = nil
        saveAllDisks()
        isRunning = false
        gfxImage = nil
    }
}

// MARK: - XML Parser

class DiskCatalogXMLParser: NSObject, XMLParserDelegate {
    private let data: Data
    private let baseURL: String
    private var disks: [DownloadableDisk] = []
    var catalogVersion: Int?
    private var elem = "", inDisk = false
    private var cFilename = "", cName = "", cDesc = "", cLicense = "", cType = ""
    private var cSize: Int64 = 0
    private var cSHA: String?
    private var cDrive: Int?

    init(data: Data, baseURL: String) { self.data = data; self.baseURL = baseURL }

    func parse() -> [DownloadableDisk] {
        let p = XMLParser(data: data); p.delegate = self; p.parse()
        return disks
    }

    func parser(_ p: XMLParser, didStartElement e: String, namespaceURI: String?, qualifiedName: String?, attributes: [String: String] = [:]) {
        elem = e
        if e == "disks" || e == "catalog" { catalogVersion = attributes["version"].flatMap(Int.init) }
        else if e == "disk" { inDisk = true; cFilename = ""; cName = ""; cDesc = ""; cSize = 0; cLicense = ""; cSHA = nil; cDrive = nil; cType = "" }
    }

    func parser(_ p: XMLParser, foundCharacters s: String) {
        guard inDisk else { return }
        let t = s.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !t.isEmpty else { return }
        switch elem {
        case "filename": cFilename += t
        case "name": cName += t
        case "description": cDesc += t
        case "size": cSize = Int64(t) ?? 0
        case "license": cLicense += t
        case "sha256": cSHA = (cSHA ?? "") + t
        case "defaultDrive": cDrive = Int(t)
        case "type": cType += t
        default: break
        }
    }

    func parser(_ p: XMLParser, didEndElement e: String, namespaceURI: String?, qualifiedName: String?) {
        if e == "disk" && inDisk {
            let imageType = CatalogImageType(rawValue: cType) ?? inferType(filename: cFilename, drive: cDrive)
            disks.append(DownloadableDisk(filename: cFilename, name: cName, description: cDesc, url: "\(baseURL)/\(cFilename)", sizeBytes: cSize, license: cLicense, sha256: cSHA, defaultDrive: cDrive, type: imageType))
            inDisk = false
        }
        elem = ""
    }

    /// Infer type from filename extension or drive number for v1 catalogs without <type>
    private func inferType(filename: String, drive: Int?) -> CatalogImageType {
        let ext = (filename as NSString).pathExtension.lowercased()
        if ext == "iso" || ext == "cue" || ext == "bin" { return .iso }
        if let d = drive, d == 0xE0 { return .iso }
        if let d = drive, d >= 0x80 { return .hdd }
        return .floppy
    }
}

// MARK: - File Document

struct DiskImageDocument: FileDocument {
    static var readableContentTypes: [UTType] { [.data] }
    var data: Data
    init(data: Data) { self.data = data }
    init(configuration: ReadConfiguration) throws { data = configuration.file.regularFileContents ?? Data() }
    func fileWrapper(configuration: WriteConfiguration) throws -> FileWrapper { FileWrapper(regularFileWithContents: data) }
}
