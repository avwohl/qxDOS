/*
 * ContentView.swift - Main UI with config profiles and device settings
 */

import SwiftUI
import UniformTypeIdentifiers

struct ContentView: View {
    @StateObject private var viewModel = EmulatorViewModel()
    @State private var fontSize: CGFloat = 14
    @State private var showingSettings = false
    @State private var configAlertMode: ConfigAlertMode? = nil
    @State private var configAlertText = ""
    @State private var showingTouchEditor = false
    @State private var showingEmulatorHelp = false
    @State private var showingMenuHelp = false
    @State private var showingMenuAbout = false
    @Environment(\.verticalSizeClass) private var verticalSizeClass

    private enum ConfigAlertMode {
        case newConfig, saveAs
    }

    private var isMacCatalyst: Bool {
        #if targetEnvironment(macCatalyst)
        return true
        #else
        return false
        #endif
    }

    init() {
        let appearance = UINavigationBarAppearance()
        appearance.configureWithOpaqueBackground()
        UINavigationBar.appearance().scrollEdgeAppearance = appearance
    }

    var body: some View {
        Group {
            if viewModel.isRunning {
                runningView
            } else {
                NavigationView {
                    settingsView
                }
                .navigationViewStyle(.stack)
            }
        }
        .alert("Error", isPresented: $viewModel.showingError) {
            Button("OK") {}
        } message: {
            Text(viewModel.errorMessage)
        }
        .alert("Catalog Updated", isPresented: $viewModel.showingNotice) {
            Button("OK") {}
        } message: {
            Text(viewModel.noticeMessage)
        }
        .alert("Disk May Be Overwritten", isPresented: $viewModel.showingManifestWriteWarning) {
            Button("OK") {}
        } message: {
            Text("This disk was downloaded from the catalog and may be replaced when the catalog is updated. Any changes you save could be lost.\n\nTo keep changes permanently, use Save As on the load screen to copy to your own file.")
        }
        .fileImporter(isPresented: $viewModel.showingDiskPicker, allowedContentTypes: [.data]) { result in
            viewModel.handleDiskImport(result.map { [$0] })
        }
        .fileExporter(isPresented: $viewModel.showingDiskExporter, document: viewModel.exportDocument, contentType: .data, defaultFilename: viewModel.exportFilename) { result in
            viewModel.handleExportResult(result)
        }
        .onReceive(NotificationCenter.default.publisher(for: UIApplication.willResignActiveNotification)) { _ in
            viewModel.saveDisksOnBackground()
        }
        .onReceive(NotificationCenter.default.publisher(for: UIApplication.willTerminateNotification)) { _ in
            viewModel.saveDisksOnBackground()
            viewModel.stop()
        }
        .onReceive(NotificationCenter.default.publisher(for: .showAbout)) { _ in
            showingMenuAbout = true
        }
        .sheet(isPresented: $showingMenuAbout) {
            NavigationView {
                AboutView()
                    .toolbar {
                        ToolbarItem(placement: .confirmationAction) {
                            Button("Done") { showingMenuAbout = false }
                        }
                    }
            }
        }
        .onReceive(NotificationCenter.default.publisher(for: .showHelp)) { _ in
            showingMenuHelp = true
        }
        .sheet(isPresented: $showingMenuHelp) {
            HelpView()
        }
    }

    // MARK: - Running View

    var runningView: some View {
        ZStack {
            VStack(spacing: 0) {
                // Hide header on iPhone landscape to maximize vertical space for 4:3 display
                if verticalSizeClass != .compact {
                    HStack {
                        Text(viewModel.config.name)
                            .font(.headline)
                        Spacer()
                        if !viewModel.statusText.isEmpty {
                            Text(viewModel.statusText)
                                .font(.caption)
                                .foregroundColor(.secondary)
                        }
                        Button("Quit") { viewModel.stop() }
                            .foregroundColor(.red)
                            .disabled(viewModel.isStarting)
                    }
                    .padding(.horizontal)
                    .padding(.vertical, 6)
                }

                ZStack {
                    // DOSBox always renders as graphics — use TerminalWithToolbar
                    // which handles keyboard/mouse input, with the gfxImage overlay
                    TerminalWithToolbar(
                        cells: $viewModel.terminalCells,
                        cursorRow: $viewModel.cursorRow,
                        cursorCol: $viewModel.cursorCol,
                        shouldFocus: $viewModel.terminalShouldFocus,
                        onKeyInput: { viewModel.sendKey($0) },
                        onSetControlify: { viewModel.setControlify($0) },
                        onScancode: { a, s in viewModel.sendDirectScancode(ascii: a, scancode: s) },
                        onToggleFn: { viewModel.isFnActive.toggle() },
                        onToggleAlt: { viewModel.isAltActive.toggle() },
                        onMouseUpdate: { x, y, btn in viewModel.sendMouseUpdate(x: x, y: y, buttons: btn) },
                        onViewCreated: { tv in
                            viewModel.terminalView = tv
                            tv.onKeyboardStateChanged = { visible, docked in
                                viewModel.keyboardVisible = visible
                                viewModel.keyboardDocked = docked
                            }
                        },
                        onTouchEditor: { showingTouchEditor = true },
                        onToggleTouchControls: { viewModel.showTouchControls.toggle() },
                        onHelp: { showingEmulatorHelp = true },
                        onToggleKeyboard: {
                            if let tv = viewModel.terminalView {
                                if tv.isFirstResponder {
                                    tv.resignFirstResponder()
                                    viewModel.terminalShouldFocus = false
                                    viewModel.keyboardVisible = false
                                    viewModel.keyboardDocked = false
                                } else {
                                    viewModel.terminalShouldFocus = true
                                    tv.becomeFirstResponder()
                                    viewModel.keyboardVisible = true
                                }
                            }
                        },
                        onQuit: { viewModel.stop() },
                        showQuitButton: verticalSizeClass == .compact,
                        isControlifyActive: viewModel.isControlifyActive,
                        isFnActive: viewModel.isFnActive,
                        isAltActive: viewModel.isAltActive,
                        hasTouchLayout: viewModel.activeLayout != nil,
                        showTouchControls: viewModel.showTouchControls,
                        keyboardVisible: viewModel.keyboardVisible,
                        keyboardDocked: viewModel.keyboardDocked,
                        rows: viewModel.terminalRows,
                        cols: viewModel.terminalCols,
                        fontSize: fontSize,
                        gfxImage: viewModel.gfxImage
                    )
                    .sheet(isPresented: $showingEmulatorHelp) {
                        HelpView()
                    }
                    .sheet(isPresented: $showingTouchEditor) {
                        TouchControlEditorView(
                            layoutManager: viewModel.touchLayoutManager,
                            activeLayoutId: Binding(
                                get: { viewModel.config.touchLayoutId },
                                set: { newId in
                                    var cfg = viewModel.config
                                    cfg.touchLayoutId = newId
                                    if let id = newId {
                                        cfg.touchLayoutName = viewModel.touchLayoutManager.layout(for: id)?.name
                                    } else {
                                        cfg.touchLayoutName = nil
                                    }
                                    viewModel.configManager.updateConfig(cfg)
                                    viewModel.activeLayout = viewModel.touchLayoutManager.layout(for: newId)
                                }
                            )
                        )
                    }

                    // Touch controls overlay
                    if let layout = viewModel.activeLayout,
                       viewModel.showTouchControls,
                       !isMacCatalyst {
                        TouchControlsOverlay(
                            layout: layout,
                            onScancodePress: { viewModel.sendScancodePress($0) },
                            onScancodeRelease: { viewModel.sendScancodeRelease($0) },
                            onMouseDelta: { dx, dy in viewModel.sendMouseDelta(dx: dx, dy: dy) }
                        )
                    }
                }
            }
            .background(Color.black.ignoresSafeArea(.container, edges: .bottom))
            .ignoresSafeArea(.keyboard)

            // Loading overlay shown while disks are being loaded
            if viewModel.isStarting {
                Color.black.ignoresSafeArea()
                    .overlay {
                        VStack(spacing: 16) {
                            ProgressView()
                                .tint(.white)
                                .scaleEffect(1.5)
                            Text("Loading disks...")
                                .foregroundColor(.white)
                            Button("Cancel") { viewModel.cancelStart() }
                                .foregroundColor(.red)
                                .padding(.top, 8)
                        }
                    }
            }
        }
    }

    // MARK: - Settings View

    var settingsView: some View {
        Form {
            configProfileSection
            displaySection
            peripheralsSection
            touchControlsSection
            diskSection
            urlDownloadSection
            catalogSection
            bootSection
            preferencesSection
            aboutSection
        }
        .navigationTitle(viewModel.config.dosType.label)
        .toolbar {
            ToolbarItem(placement: .primaryAction) {
                HStack(spacing: 12) {
                    Button("Start") { viewModel.start() }
                        .disabled(viewModel.floppyAPath == nil && viewModel.hddCPath == nil && viewModel.isoPath == nil)
                    Button("Quit") { exit(0) }
                        .foregroundColor(.red)
                }
            }
        }
        .sheet(isPresented: $showingTouchEditor) {
            TouchControlEditorView(
                layoutManager: viewModel.touchLayoutManager,
                activeLayoutId: Binding(
                    get: { viewModel.config.touchLayoutId },
                    set: { newId in
                        var cfg = viewModel.config
                        cfg.touchLayoutId = newId
                        if let id = newId {
                            cfg.touchLayoutName = viewModel.touchLayoutManager.layout(for: id)?.name
                        } else {
                            cfg.touchLayoutName = nil
                        }
                        viewModel.configManager.updateConfig(cfg)
                        viewModel.activeLayout = viewModel.touchLayoutManager.layout(for: newId)
                    }
                )
            )
        }
        .alert(
            configAlertMode == .saveAs ? "Save As" : "New Configuration",
            isPresented: Binding(
                get: { configAlertMode != nil },
                set: { if !$0 { configAlertMode = nil } }
            )
        ) {
            TextField("Name", text: $configAlertText)
            Button(configAlertMode == .saveAs ? "Save" : "Create") {
                if !configAlertText.isEmpty {
                    if configAlertMode == .saveAs {
                        _ = viewModel.configManager.duplicateConfig(viewModel.config, name: configAlertText)
                    } else {
                        _ = viewModel.configManager.addConfig(name: configAlertText)
                    }
                    configAlertText = ""
                }
            }
            Button("Cancel", role: .cancel) { configAlertText = "" }
        }
    }

    // MARK: - Config Profile Section

    var configProfileSection: some View {
        Section("Configuration Profile") {
            Picker("Profile", selection: Binding(
                get: { viewModel.configManager.activeConfigId ?? UUID() },
                set: { id in
                    if let cfg = viewModel.configManager.configs.first(where: { $0.id == id }) {
                        viewModel.configManager.selectConfig(cfg)
                    }
                }
            )) {
                ForEach(viewModel.configManager.configs) { cfg in
                    Text(cfg.name).tag(cfg.id)
                }
            }

            HStack {
                Button("New") { configAlertText = ""; configAlertMode = .newConfig }
                Spacer()
                Button("Save As") { configAlertText = ""; configAlertMode = .saveAs }
                Spacer()
                Button("Delete", role: .destructive) {
                    if viewModel.configManager.configs.count > 1 {
                        viewModel.configManager.deleteConfig(viewModel.config)
                    }
                }
                .disabled(viewModel.configManager.configs.count <= 1)
            }

            HStack {
                Text("Name")
                Spacer()
                TextField("Config Name", text: Binding(
                    get: { viewModel.config.name },
                    set: { name in
                        var cfg = viewModel.config
                        cfg.name = name
                        viewModel.configManager.updateConfig(cfg)
                    }
                ))
                .multilineTextAlignment(.trailing)
                .frame(maxWidth: 200)
            }
        }
    }

    // MARK: - Machine Section

    var displaySection: some View {
        let caps = viewModel.config.backend.caps
        return Section("Machine") {
            Picker("Backend", selection: Binding(
                get: { viewModel.config.backend },
                set: { val in
                    var cfg = viewModel.config
                    cfg.backend = val
                    viewModel.configManager.updateConfig(cfg)
                }
            )) {
                ForEach(EmulatorBackend.allCases, id: \.self) { b in
                    Text(b.label).tag(b)
                }
            }

            Text(viewModel.config.backend.caption)
                .font(.caption).foregroundColor(.secondary)

            Picker("DOS", selection: Binding(
                get: { viewModel.config.dosType },
                set: { val in
                    var cfg = viewModel.config
                    cfg.dosType = val
                    viewModel.configManager.updateConfig(cfg)
                }
            )) {
                ForEach(DOSType.allCases, id: \.self) { type in
                    Text(type.label)
                        .tag(type)
                        .disabled(type == .dosboxDOS && !caps.supportsDOSBoxBuiltinShell)
                }
            }

            switch viewModel.config.dosType {
            case .dosboxDOS:
                Text("DOSBox built-in kernel and shell. ~30 utilities on Z: drive. No OS on disk needed.")
                    .font(.caption).foregroundColor(.secondary)
            case .freeDOS:
                Text("Boots FreeDOS from disk. Full kernel, COMMAND.COM, and 230+ utilities. Install from floppy or CD.")
                    .font(.caption).foregroundColor(.secondary)
            case .msDOS:
                Text("Boots MS-DOS from disk. Use MS-DOS 4.0 (MIT license) from catalog, or bring your own media.")
                    .font(.caption).foregroundColor(.secondary)
            }

            if let help = caps.disabledHelpForDOSBoxShell() {
                Text(help)
                    .font(.caption).foregroundColor(.secondary)
            }

            Picker("Type", selection: Binding(
                get: { viewModel.config.machineType },
                set: { val in
                    var cfg = viewModel.config
                    cfg.machineType = val
                    viewModel.configManager.updateConfig(cfg)
                }
            )) {
                Text("SVGA (S3)").tag(5).disabled(!caps.supportsSVGA)
                Text("VGA").tag(0)
                Text("EGA").tag(1)
                Text("CGA").tag(2)
                Text("Tandy").tag(3)
                Text("Hercules").tag(4)
            }

            if let help = caps.disabledHelpForSVGA() {
                Text(help)
                    .font(.caption).foregroundColor(.secondary)
            }

            Picker("RAM", selection: Binding(
                get: { viewModel.config.memoryMB },
                set: { val in
                    var cfg = viewModel.config
                    cfg.memoryMB = val
                    viewModel.configManager.updateConfig(cfg)
                }
            )) {
                Text("4 MB").tag(4)
                Text("8 MB").tag(8)
                Text("16 MB").tag(16)
                Text("32 MB").tag(32)
                Text("64 MB").tag(64)
            }
        }
    }

    // MARK: - Peripherals Section

    var peripheralsSection: some View {
        let caps = viewModel.config.backend.caps
        return Section("Peripherals") {
            Toggle("Mouse", isOn: Binding(
                get: { viewModel.config.mouseEnabled },
                set: { val in
                    var cfg = viewModel.config
                    cfg.mouseEnabled = val
                    viewModel.configManager.updateConfig(cfg)
                }
            ))

            Toggle("PC Speaker", isOn: Binding(
                get: { viewModel.config.speakerEnabled },
                set: { val in
                    var cfg = viewModel.config
                    cfg.speakerEnabled = val
                    viewModel.configManager.updateConfig(cfg)
                }
            ))

            Toggle("Sound Blaster 16", isOn: Binding(
                get: { viewModel.config.sbEnabled },
                set: { val in
                    var cfg = viewModel.config
                    cfg.sbEnabled = val
                    viewModel.configManager.updateConfig(cfg)
                }
            ))
            .disabled(!caps.supportsSB16)

            if let help = caps.disabledHelpForSB16() {
                Text(help)
                    .font(.caption).foregroundColor(.secondary)
            }

            Picker("CPU Type", selection: Binding(
                get: { viewModel.config.cpuTypeStr },
                set: { val in
                    var cfg = viewModel.config
                    cfg.cpuTypeStr = val
                    viewModel.configManager.updateConfig(cfg)
                }
            )) {
                Text("Auto (386 fast + 486 extras)").tag("auto")
                Text("386").tag("386")
                Text("386 Fast").tag("386_fast")
                Text("386 Prefetch").tag("386_prefetch")
                Text("486").tag("486").disabled(!caps.supports486AndPentium)
                Text("Pentium").tag("pentium").disabled(!caps.supports486AndPentium)
                Text("Pentium MMX").tag("pentium_mmx").disabled(!caps.supports486AndPentium)
            }

            if let help = caps.disabledHelpFor486Pentium() {
                Text(help)
                    .font(.caption).foregroundColor(.secondary)
            }

            Picker("CPU Speed", selection: Binding(
                get: { viewModel.config.speedMode },
                set: { val in
                    var cfg = viewModel.config
                    cfg.speedMode = val
                    viewModel.configManager.updateConfig(cfg)
                }
            )) {
                Text("Full Speed").tag(0)
                Text("IBM PC (4.77 MHz)").tag(1)
                Text("IBM AT (8 MHz)").tag(2)
                Text("386SX (16 MHz)").tag(3)
                Text("486DX2 (66 MHz)").tag(4).disabled(!caps.supports486AndPentium)
            }
        }
    }

    // MARK: - Touch Controls Section

    var touchControlsSection: some View {
        Section("Touch Controls") {
            HStack {
                Text("Layout")
                Spacer()
                Text(touchLayoutLabel)
                    .foregroundColor(.secondary)
            }

            Button("Choose Layout...") {
                showingTouchEditor = true
            }

            Text("Virtual D-Pads, buttons, and sticks overlaid on the display for playing games without a keyboard.")
                .font(.caption)
                .foregroundColor(.secondary)
        }
    }

    private var touchLayoutLabel: String {
        if let name = viewModel.config.touchLayoutName {
            return name
        }
        if let id = viewModel.config.touchLayoutId {
            return viewModel.touchLayoutManager.layout(for: id)?.name ?? "Unknown"
        }
        return "None"
    }

    // MARK: - Disk Section

    var diskSection: some View {
        Section("Disks") {
            diskRow(label: "Floppy A:", path: viewModel.floppyAPath, unit: 0)
            diskRow(label: "Floppy B:", path: viewModel.floppyBPath, unit: 1)
            diskRow(label: "Hard Disk C:", path: viewModel.hddCPath, unit: 0x80)
            diskRow(label: "Hard Disk D:", path: viewModel.hddDPath, unit: 0x81)
            diskRow(label: "CD-ROM:", path: viewModel.isoPath, unit: 0xE0)

            Menu("Create Blank Disk") {
                Button("360 KB Floppy") { viewModel.createBlankFloppy(sizeKB: 360) }
                Button("720 KB Floppy") { viewModel.createBlankFloppy(sizeKB: 720) }
                Button("1.44 MB Floppy") { viewModel.createBlankFloppy(sizeKB: 1440) }
                Divider()
                Button("10 MB HDD") { viewModel.createBlankHDD(sizeMB: 10) }
                Button("20 MB HDD") { viewModel.createBlankHDD(sizeMB: 20) }
                Button("32 MB HDD") { viewModel.createBlankHDD(sizeMB: 32) }
                Button("64 MB HDD") { viewModel.createBlankHDD(sizeMB: 64) }
            }
        }
    }

    private func catalogDisks(forUnit unit: Int) -> [DownloadableDisk] {
        viewModel.diskCatalog.filter { disk in
            switch unit {
            case 0, 1: return disk.type == .floppy
            case 0x80, 0x81: return disk.type == .hdd
            case 0xE0: return disk.type == .iso
            default: return false
            }
        }
    }

    func diskRow(label: String, path: URL?, unit: Int) -> some View {
        HStack {
            Text(label)
            Spacer()
            if let progress = viewModel.downloadProgressForDrive(unit) {
                ProgressView(value: progress)
                    .frame(maxWidth: 120)
            } else if let p = path {
                Text(p.lastPathComponent)
                    .font(.caption)
                    .foregroundColor(.secondary)
                    .lineLimit(1)
            }
            Menu {
                Button { viewModel.loadDisk(unit) } label: {
                    Label("Load from Files", systemImage: "folder")
                }
                let available = catalogDisks(forUnit: unit)
                if !available.isEmpty {
                    Menu("From Catalog") {
                        ForEach(available) { disk in
                            let state = viewModel.downloadStates[disk.filename] ?? .notDownloaded
                            Button {
                                viewModel.useCatalogDisk(disk, forDrive: unit)
                            } label: {
                                switch state {
                                case .downloaded:
                                    Label(disk.name, systemImage: "checkmark.circle.fill")
                                case .downloading:
                                    Label("\(disk.name) (downloading...)", systemImage: "arrow.down.circle.dotted")
                                case .error:
                                    Label("\(disk.name) (retry)", systemImage: "exclamationmark.circle")
                                case .notDownloaded:
                                    Label("\(disk.name) (\(disk.formattedSize))", systemImage: "arrow.down.circle")
                                }
                            }
                            .disabled({ if case .downloading = state { return true }; return false }())
                        }
                    }
                }
                if let p = path {
                    Divider()
                    Button { viewModel.saveDiskFile(p) } label: {
                        Label("Save As", systemImage: "square.and.arrow.up")
                    }
                    Button(role: .destructive) { viewModel.removeDisk(unit) } label: {
                        Label("Remove", systemImage: "trash")
                    }
                }
            } label: {
                Image(systemName: path != nil ? "ellipsis.circle" : "plus.circle")
                    .foregroundColor(.accentColor)
            }
        }
    }

    // MARK: - URL Download Section

    var urlDownloadSection: some View {
        Section("Download from URL") {
            HStack {
                TextField("https://...", text: $viewModel.urlInput)
                    .textInputAutocapitalization(.never)
                    .autocorrectionDisabled()
                    .keyboardType(.URL)
            }
            if viewModel.urlDownloading {
                ProgressView(value: viewModel.urlDownloadProgress)
            } else {
                HStack {
                    Button("A:") { viewModel.downloadFromURL(toDrive: 0) }
                        .disabled(viewModel.urlInput.isEmpty)
                    Spacer()
                    Button("C:") { viewModel.downloadFromURL(toDrive: 0x80) }
                        .disabled(viewModel.urlInput.isEmpty)
                    Spacer()
                    Button("D:") { viewModel.downloadFromURL(toDrive: 0x81) }
                        .disabled(viewModel.urlInput.isEmpty)
                    Spacer()
                    Button("CD-ROM") { viewModel.downloadFromURL(toDrive: 0xE0) }
                        .disabled(viewModel.urlInput.isEmpty)
                }
                .font(.caption)
            }
        }
    }

    // MARK: - Catalog Section

    var catalogSection: some View {
        Section("Disk Catalog") {
            if viewModel.catalogLoading {
                HStack { ProgressView(); Text("Loading catalog...").foregroundColor(.secondary) }
            } else if let err = viewModel.catalogError {
                HStack {
                    Text(err).foregroundColor(.red).font(.caption)
                    Spacer()
                    Button("Retry") { viewModel.fetchDiskCatalog() }
                }
            } else if viewModel.diskCatalog.isEmpty {
                Text("No disk images available")
                    .foregroundColor(.secondary)
            } else {
                ForEach(viewModel.diskCatalog) { disk in
                    catalogRow(disk)
                        .swipeActions(edge: .trailing) {
                            if case .downloaded = viewModel.downloadStates[disk.filename] {
                                Button("Delete", role: .destructive) {
                                    viewModel.deleteCatalogDisk(disk)
                                }
                            }
                        }
                }
            }
        }
    }

    func catalogRow(_ disk: DownloadableDisk) -> some View {
        VStack(alignment: .leading, spacing: 4) {
            HStack {
                Text(disk.name).font(.headline)
                Spacer()
                Text(disk.type.label)
                    .font(.caption2)
                    .padding(.horizontal, 6)
                    .padding(.vertical, 2)
                    .background(disk.type == .iso ? Color.purple.opacity(0.15) : disk.type == .hdd ? Color.orange.opacity(0.15) : Color.blue.opacity(0.15))
                    .cornerRadius(4)
                Text(disk.formattedSize).font(.caption).foregroundColor(.secondary)
            }
            Text(disk.description).font(.caption).foregroundColor(.secondary)

            let state = viewModel.downloadStates[disk.filename] ?? .notDownloaded
            switch state {
            case .notDownloaded:
                Button("Download") { viewModel.downloadDisk(disk) }
                    .font(.caption)
            case .downloading(let progress):
                ProgressView(value: progress)
            case .downloaded:
                HStack(spacing: 12) {
                    switch disk.type {
                    case .floppy:
                        Button("Use as A:") { viewModel.useCatalogDisk(disk, forDrive: 0) }
                        Button("Use as B:") { viewModel.useCatalogDisk(disk, forDrive: 1) }
                    case .hdd:
                        Button("Use as C:") { viewModel.useCatalogDisk(disk, forDrive: 0x80) }
                        Button("Use as D:") { viewModel.useCatalogDisk(disk, forDrive: 0x81) }
                    case .iso:
                        Button("Use as CD-ROM") { viewModel.useCatalogDisk(disk, forDrive: 0xE0) }
                    }
                    Spacer()
                    Button("Save As") {
                        viewModel.saveDiskFile(viewModel.disksDirectory.appendingPathComponent(disk.filename))
                    }
                }
                .font(.caption)
            case .error(let msg):
                HStack {
                    Text(msg).foregroundColor(.red).font(.caption)
                    Button("Retry") { viewModel.downloadDisk(disk) }.font(.caption)
                }
            }
        }
        .padding(.vertical, 2)
    }

    // MARK: - Boot Section

    var bootSection: some View {
        Section("Boot") {
            Picker("Boot Drive", selection: Binding(
                get: { viewModel.config.bootDrive },
                set: { val in
                    var cfg = viewModel.config
                    cfg.bootDrive = val
                    viewModel.configManager.updateConfig(cfg)
                }
            )) {
                Text("Floppy A:").tag(0)
                Text("Hard Disk C:").tag(0x80)
                Text("CD-ROM").tag(0xE0)
            }

            Button(action: { viewModel.start() }) {
                HStack {
                    Spacer()
                    Text("Start Emulator")
                        .font(.headline)
                    Spacer()
                }
            }
            .disabled(viewModel.floppyAPath == nil && viewModel.hddCPath == nil && viewModel.isoPath == nil)
        }
    }

    // MARK: - Preferences Section

    var preferencesSection: some View {
        Section("Preferences") {
            Toggle("Warn on Catalog Disk Writes", isOn: Binding(
                get: { viewModel.warnManifestWrites },
                set: { viewModel.warnManifestWrites = $0 }
            ))
            Text("Show a warning when the emulator writes to a disk downloaded from the catalog. Changes may be lost when the catalog is updated.")
                .font(.caption)
                .foregroundColor(.secondary)
        }
    }

    // MARK: - About Section

    @State private var showingHelp = false

    var aboutSection: some View {
        Section {
            Button("Help") {
                showingHelp = true
            }
            .sheet(isPresented: $showingHelp) {
                HelpView()
            }

            NavigationLink(destination: AboutView()) {
                Text("About qxDOS")
            }
        }
    }
}

// MARK: - About View

struct AboutView: View {
    private var appVersion: String {
        Bundle.main.infoDictionary?["CFBundleShortVersionString"] as? String ?? "0.1.0"
    }

    private var buildNumber: String {
        Bundle.main.infoDictionary?["CFBundleVersion"] as? String ?? "1"
    }

    var body: some View {
        Form {
            Section {
                VStack(spacing: 12) {
                    Image("AppLogo")
                        .resizable()
                        .aspectRatio(contentMode: .fit)
                        .frame(maxWidth: 280)
                        .accessibilityLabel("qxDOS logo")

                    Text("Version \(appVersion) (Build \(buildNumber))")
                        .font(.subheadline)
                        .foregroundColor(.secondary)

                    Text("A DOS emulator for your phone, tablet, and desktop. qxDOS provides the app shell and UI. PC emulation by DOSBox Staging. DOS operating systems by their respective projects.")
                        .font(.subheadline)
                        .foregroundColor(.secondary)
                        .multilineTextAlignment(.center)

                    Text("\u{00A9} 2025 Aaron Wohl")
                        .font(.caption)
                        .foregroundColor(.secondary)
                }
                .frame(maxWidth: .infinity)
                .padding(.vertical, 12)
            }

            Section("License") {
                Text("This emulator is licensed under the GNU General Public License v3.0. Source code is available on GitHub.")
                    .font(.caption)
                    .foregroundColor(.secondary)

                Text("This app includes GPL-licensed binaries from DOSBox Staging (GPL v2+), FreeDOS (GPL v2+), and mTCP (GPL v3). Complete source code is available from the links below, or upon request via GitHub Issues. This offer is valid for three years from each release.")
                    .font(.caption)
                    .foregroundColor(.secondary)
            }

            Section("Links") {
                Link(destination: URL(string: "https://github.com/avwohl/qxDOS")!) {
                    HStack {
                        Label("qxDOS Source Code", systemImage: "chevron.left.forwardslash.chevron.right")
                        Spacer()
                        Image(systemName: "arrow.up.right.square")
                            .foregroundColor(.secondary)
                    }
                }

                Link(destination: URL(string: "https://github.com/dosbox-staging/dosbox-staging")!) {
                    HStack {
                        Label("DOSBox Staging Source (GPL v2+)", systemImage: "cpu")
                        Spacer()
                        Image(systemName: "arrow.up.right.square")
                            .foregroundColor(.secondary)
                    }
                }

                Link(destination: URL(string: "https://github.com/FDOS")!) {
                    HStack {
                        Label("FreeDOS Source (GPL v2+)", systemImage: "desktopcomputer")
                        Spacer()
                        Image(systemName: "arrow.up.right.square")
                            .foregroundColor(.secondary)
                    }
                }

                Link(destination: URL(string: "https://www.brutman.com/mTCP/")!) {
                    HStack {
                        Label("mTCP Source (GPL v3)", systemImage: "network")
                        Spacer()
                        Image(systemName: "arrow.up.right.square")
                            .foregroundColor(.secondary)
                    }
                }

                Link(destination: URL(string: "https://github.com/avwohl/qxDOS/issues")!) {
                    HStack {
                        Label("Report an Issue", systemImage: "exclamationmark.bubble")
                        Spacer()
                        Image(systemName: "arrow.up.right.square")
                            .foregroundColor(.secondary)
                    }
                }
            }

            Section("Acknowledgments") {
                Text("DOSBox Staging is licensed under GPL v2+. Source: github.com/dosbox-staging/dosbox-staging")
                    .font(.caption)
                    .foregroundColor(.secondary)

                Text("FreeDOS is licensed under GPL v2+. Copyright 1995-2012 Pasquale J. Villani and The FreeDOS Project. Source: github.com/FDOS")
                    .font(.caption)
                    .foregroundColor(.secondary)

                Text("mTCP by Michael Brutman is licensed under GPL v3. Source: brutman.com/mTCP")
                    .font(.caption)
                    .foregroundColor(.secondary)

                Text("NE2000 packet driver by Crynwr Software. Source: crynwr.com/drivers")
                    .font(.caption)
                    .foregroundColor(.secondary)
            }
        }
        .navigationTitle("About")
        .navigationBarTitleDisplayMode(.inline)
    }
}
