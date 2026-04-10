/*
 * MachineConfig.swift - Named machine configuration profiles
 *
 * Updated for DOSBox backend.  Config options map to DOSBox settings:
 * machine type, cycles, memory, sound cards.
 */

import Foundation

// MARK: - Backend selection

/// Hardware-level emulator selection. The DOS layer (FreeDOS / MS-DOS /
/// DOSBox built-in shell) is chosen separately via DOSType. Once a session
/// has been started the backend is captured for that run; restart-in-place
/// is supported by emu88 but DOSBox still terminates the process on stop.
enum EmulatorBackend: Int, Codable, CaseIterable {
    case dosbox = 0
    case emu88  = 1

    var label: String {
        switch self {
        case .dosbox: return "DOSBox-staging"
        case .emu88:  return "emu88"
        }
    }

    var caption: String {
        switch self {
        case .dosbox:
            return "Full SVGA, Sound Blaster 16, dynamic recompiler. The default."
        case .emu88:
            return "Custom 8088/286/386 interpreter. Up to VGA, no SB16, supports clean restart."
        }
    }

    var caps: BackendCapabilities {
        switch self {
        case .dosbox:
            return BackendCapabilities(
                maxRamMB: 64,
                supportsSVGA: true,
                supportsSB16: true,
                supportsDOSBoxBuiltinShell: true,
                supportsCustomCycles: true,
                supports486AndPentium: true)
        case .emu88:
            return BackendCapabilities(
                maxRamMB: 64,
                supportsSVGA: false,
                supportsSB16: false,
                supportsDOSBoxBuiltinShell: false,
                supportsCustomCycles: false,
                supports486AndPentium: false)
        }
    }
}

/// Per-backend capability flags surfaced to the settings UI so the picker
/// can grey out options that don't apply with an explanatory caption.
struct BackendCapabilities {
    let maxRamMB: Int
    let supportsSVGA: Bool
    let supportsSB16: Bool
    let supportsDOSBoxBuiltinShell: Bool
    let supportsCustomCycles: Bool
    let supports486AndPentium: Bool

    /// Help text shown beneath each disabled control. Returns nil for
    /// controls that are enabled under the current backend.
    func disabledHelpForSVGA() -> String? {
        supportsSVGA ? nil : "emu88 supports up to VGA — pick VGA or lower."
    }
    func disabledHelpForSB16() -> String? {
        supportsSB16 ? nil : "emu88 has no Sound Blaster — only PC speaker."
    }
    func disabledHelpForDOSBoxShell() -> String? {
        supportsDOSBoxBuiltinShell ? nil : "emu88 has no built-in shell — boot FreeDOS or MS-DOS from disk."
    }
    func disabledHelpForCustomCycles() -> String? {
        supportsCustomCycles ? nil : "emu88 doesn't support custom cycle counts."
    }
    func disabledHelpFor486Pentium() -> String? {
        supports486AndPentium ? nil : "emu88 supports 8088/286/386 only."
    }
}

enum DOSType: Int, Codable, CaseIterable {
    case dosboxDOS = 0   // DOSBox built-in kernel + shell + Z: utilities
    case freeDOS = 1     // Boot FreeDOS from disk image
    case msDOS = 2       // Boot MS-DOS from disk image

    var label: String {
        switch self {
        case .dosboxDOS: return "DOSBox DOS"
        case .freeDOS: return "FreeDOS"
        case .msDOS: return "MS-DOS"
        }
    }
}

struct MachineConfig: Codable, Identifiable, Equatable {
    var id: UUID = UUID()
    var name: String = "Default"

    // Hardware-level emulator selection
    var backend: EmulatorBackend = .dosbox

    // DOS type — which kernel/shell runs above the hardware layer
    var dosType: DOSType = .freeDOS

    // Machine type (maps to DOSBox "machine" setting)
    // 0=VGA, 1=EGA, 2=CGA, 3=Tandy, 4=Hercules, 5=SVGA(S3)
    var machineType: Int = 5  // SVGA by default for max compatibility

    // CPU speed / cycles
    // 0=max, 1=3000(XT), 2=8000(AT), 3=20000(386), 4=50000(486), 5=custom
    var speedMode: Int = 0

    // Custom cycle count (when speedMode == 5)
    var customCycles: Int = 10000

    // CPU type (DOSBox cputype setting)
    // "auto", "386", "386_fast", "386_prefetch", "486", "pentium", "pentium_mmx"
    var cpuTypeStr: String = "auto"

    // RAM in MB
    var memoryMB: Int = 16

    // Sound
    var speakerEnabled: Bool = true
    var sbEnabled: Bool = true      // Sound Blaster 16

    // Input
    var mouseEnabled: Bool = true

    // Touch controls
    var touchLayoutId: UUID? = nil
    var touchLayoutName: String? = nil

    // Disks
    var floppyAFilename: String?
    var floppyBFilename: String?
    var hddCFilename: String?
    var hddDFilename: String?
    var bootDrive: Int = 0  // 0=A, 0x80=C, 0xE0=CD-ROM

    // Display labels
    var machineLabel: String {
        switch machineType {
        case 0: return "VGA"
        case 1: return "EGA"
        case 2: return "CGA"
        case 3: return "Tandy"
        case 4: return "Hercules"
        case 5: return "SVGA (S3)"
        default: return "Unknown"
        }
    }

    var speedLabel: String {
        switch speedMode {
        case 0: return "Full Speed"
        case 1: return "IBM PC (4.77 MHz)"
        case 2: return "IBM AT (8 MHz)"
        case 3: return "386SX (16 MHz)"
        case 4: return "486DX2 (66 MHz)"
        case 5: return "Custom (\(customCycles) cycles)"
        default: return "Unknown"
        }
    }

    // Legacy compatibility — map old cpuType/displayAdapter to new fields
    // These properties allow existing saved configs to still load
    var cpuType: Int {
        get { return 2 }  // always 386+ with DOSBox
        set { }
    }

    var displayAdapter: Int {
        get { return machineType }
        set { machineType = newValue }
    }

    var soundCard: Int {
        get { return sbEnabled ? 2 : 0 }
        set { sbEnabled = newValue > 0 }
    }

    var cdromEnabled: Bool {
        get { return true }  // DOSBox always supports CD-ROM
        set { }
    }

    // Coding keys for backward compatibility
    enum CodingKeys: String, CodingKey {
        case id, name, backend, dosType, machineType, speedMode, customCycles, cpuTypeStr, memoryMB
        case speakerEnabled, sbEnabled, mouseEnabled
        case touchLayoutId, touchLayoutName
        case floppyAFilename, floppyBFilename, hddCFilename, hddDFilename
        case bootDrive
        // Legacy keys we can decode
        case cpuType, displayAdapter, soundCard, cdromEnabled
    }

    init() {}

    init(name: String) {
        self.name = name
    }

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        id = try c.decodeIfPresent(UUID.self, forKey: .id) ?? UUID()
        name = try c.decodeIfPresent(String.self, forKey: .name) ?? "Default"
        backend = try c.decodeIfPresent(EmulatorBackend.self, forKey: .backend) ?? .dosbox
        dosType = try c.decodeIfPresent(DOSType.self, forKey: .dosType) ?? .freeDOS

        // Try new keys first, fall back to legacy
        if let mt = try c.decodeIfPresent(Int.self, forKey: .machineType) {
            machineType = mt
        } else if let da = try c.decodeIfPresent(Int.self, forKey: .displayAdapter) {
            machineType = da
        }

        speedMode = try c.decodeIfPresent(Int.self, forKey: .speedMode) ?? 0
        customCycles = try c.decodeIfPresent(Int.self, forKey: .customCycles) ?? 10000
        cpuTypeStr = try c.decodeIfPresent(String.self, forKey: .cpuTypeStr) ?? "auto"
        memoryMB = try c.decodeIfPresent(Int.self, forKey: .memoryMB) ?? 16
        speakerEnabled = try c.decodeIfPresent(Bool.self, forKey: .speakerEnabled) ?? true
        mouseEnabled = try c.decodeIfPresent(Bool.self, forKey: .mouseEnabled) ?? true

        if let sb = try c.decodeIfPresent(Bool.self, forKey: .sbEnabled) {
            sbEnabled = sb
        } else if let sc = try c.decodeIfPresent(Int.self, forKey: .soundCard) {
            sbEnabled = sc > 0
        }

        touchLayoutId = try c.decodeIfPresent(UUID.self, forKey: .touchLayoutId)
        touchLayoutName = try c.decodeIfPresent(String.self, forKey: .touchLayoutName)

        floppyAFilename = try c.decodeIfPresent(String.self, forKey: .floppyAFilename)
        floppyBFilename = try c.decodeIfPresent(String.self, forKey: .floppyBFilename)
        hddCFilename = try c.decodeIfPresent(String.self, forKey: .hddCFilename)
        hddDFilename = try c.decodeIfPresent(String.self, forKey: .hddDFilename)
        bootDrive = try c.decodeIfPresent(Int.self, forKey: .bootDrive) ?? 0
    }

    func encode(to encoder: Encoder) throws {
        var c = encoder.container(keyedBy: CodingKeys.self)
        try c.encode(id, forKey: .id)
        try c.encode(name, forKey: .name)
        try c.encode(backend, forKey: .backend)
        try c.encode(dosType, forKey: .dosType)
        try c.encode(machineType, forKey: .machineType)
        try c.encode(speedMode, forKey: .speedMode)
        try c.encode(customCycles, forKey: .customCycles)
        try c.encode(cpuTypeStr, forKey: .cpuTypeStr)
        try c.encode(memoryMB, forKey: .memoryMB)
        try c.encode(speakerEnabled, forKey: .speakerEnabled)
        try c.encode(sbEnabled, forKey: .sbEnabled)
        try c.encode(mouseEnabled, forKey: .mouseEnabled)
        try c.encodeIfPresent(touchLayoutId, forKey: .touchLayoutId)
        try c.encodeIfPresent(touchLayoutName, forKey: .touchLayoutName)
        try c.encode(floppyAFilename, forKey: .floppyAFilename)
        try c.encode(floppyBFilename, forKey: .floppyBFilename)
        try c.encode(hddCFilename, forKey: .hddCFilename)
        try c.encode(hddDFilename, forKey: .hddDFilename)
        try c.encode(bootDrive, forKey: .bootDrive)
    }
}

// MARK: - Config Storage

class ConfigManager: ObservableObject {
    @Published var configs: [MachineConfig] = []
    @Published var activeConfigId: UUID?

    private let configsKey = "machineConfigs"
    private let activeKey = "activeConfigId"

    var activeConfig: MachineConfig? {
        get { configs.first { $0.id == activeConfigId } }
        set {
            if let cfg = newValue, let idx = configs.firstIndex(where: { $0.id == cfg.id }) {
                configs[idx] = cfg
                save()
            }
        }
    }

    init() {
        load()
        if configs.isEmpty {
            let def = MachineConfig(name: "Default PC")
            configs.append(def)
            activeConfigId = def.id
            save()
        }
        if activeConfigId == nil {
            activeConfigId = configs.first?.id
        }
    }

    func save() {
        if let data = try? JSONEncoder().encode(configs) {
            UserDefaults.standard.set(data, forKey: configsKey)
        }
        if let id = activeConfigId {
            UserDefaults.standard.set(id.uuidString, forKey: activeKey)
        }
    }

    func load() {
        if let data = UserDefaults.standard.data(forKey: configsKey),
           let loaded = try? JSONDecoder().decode([MachineConfig].self, from: data) {
            configs = loaded
        }
        if let str = UserDefaults.standard.string(forKey: activeKey) {
            activeConfigId = UUID(uuidString: str)
        }
    }

    func addConfig(name: String) -> MachineConfig {
        var cfg = activeConfig ?? MachineConfig()
        cfg.id = UUID()
        cfg.name = name
        configs.append(cfg)
        activeConfigId = cfg.id
        save()
        return cfg
    }

    func duplicateConfig(_ config: MachineConfig, name: String) -> MachineConfig {
        var dup = config
        dup.id = UUID()
        dup.name = name
        configs.append(dup)
        save()
        return dup
    }

    func deleteConfig(_ config: MachineConfig) {
        configs.removeAll { $0.id == config.id }
        if activeConfigId == config.id {
            activeConfigId = configs.first?.id
        }
        save()
    }

    func selectConfig(_ config: MachineConfig) {
        activeConfigId = config.id
        save()
    }

    func updateConfig(_ config: MachineConfig) {
        if let idx = configs.firstIndex(where: { $0.id == config.id }) {
            configs[idx] = config
            save()
        }
    }

    func exportConfig(_ config: MachineConfig) -> Data? {
        return try? JSONEncoder().encode(config)
    }

    func importConfig(from data: Data) -> MachineConfig? {
        guard var cfg = try? JSONDecoder().decode(MachineConfig.self, from: data) else { return nil }
        cfg.id = UUID()
        configs.append(cfg)
        save()
        return cfg
    }
}
