/*
 * TouchControlLayout.swift - Data model for configurable virtual touch controls
 *
 * Defines layout structures, key bindings, and built-in presets for
 * playing DOS games on iPad without a physical keyboard.
 */

import Foundation

// MARK: - Data Model

struct TouchControlLayout: Codable, Identifiable, Equatable {
    var id: UUID
    var name: String
    var controls: [TouchControl]

    /// Whether this is a built-in preset (not editable)
    var isBuiltIn: Bool = false
}

struct TouchControl: Codable, Identifiable, Equatable {
    var id: UUID = UUID()
    var type: TouchControlType
    var position: ControlPosition
    var size: CGFloat = 120
    var opacity: CGFloat = 0.3
    var bindings: [String: KeyBinding]
}

enum TouchControlType: String, Codable, CaseIterable {
    case dpad       // 8-directional movement pad
    case stick      // Analog stick for mouse look
    case button     // Single action button
    case buttonRow  // Row of 2-5 small buttons
}

struct ControlPosition: Codable, Equatable {
    var x: CGFloat  // 0..1 fraction of screen width
    var y: CGFloat  // 0..1 fraction of screen height
}

struct KeyBinding: Codable, Equatable {
    var scancode: UInt8
    var ascii: UInt8 = 0
    var label: String

    // Common key bindings
    static let upArrow    = KeyBinding(scancode: 0x48, label: "Up")
    static let downArrow  = KeyBinding(scancode: 0x50, label: "Down")
    static let leftArrow  = KeyBinding(scancode: 0x4B, label: "Left")
    static let rightArrow = KeyBinding(scancode: 0x4D, label: "Right")
    static let ctrl       = KeyBinding(scancode: 0x1D, label: "Ctrl")
    static let alt        = KeyBinding(scancode: 0x38, label: "Alt")
    static let shift      = KeyBinding(scancode: 0x2A, label: "Shift")
    static let space      = KeyBinding(scancode: 0x39, label: "Space")
    static let enter      = KeyBinding(scancode: 0x1C, label: "Enter")
    static let escape     = KeyBinding(scancode: 0x01, label: "Esc")
    static let tab        = KeyBinding(scancode: 0x0F, label: "Tab")
    static let key1       = KeyBinding(scancode: 0x02, label: "1")
    static let key2       = KeyBinding(scancode: 0x03, label: "2")
    static let key3       = KeyBinding(scancode: 0x04, label: "3")
    static let key4       = KeyBinding(scancode: 0x05, label: "4")
    static let key5       = KeyBinding(scancode: 0x06, label: "5")
    static let keyA       = KeyBinding(scancode: 0x1E, label: "A")
    static let keyZ       = KeyBinding(scancode: 0x2C, label: "Z")
    static let keyY       = KeyBinding(scancode: 0x15, label: "Y")
    static let keyN       = KeyBinding(scancode: 0x31, label: "N")

    /// All available keys for the binding picker
    static let allKeys: [KeyBinding] = [
        // Arrows
        .upArrow, .downArrow, .leftArrow, .rightArrow,
        // Modifiers
        .ctrl, .alt, .shift,
        // Common
        .space, .enter, .escape, .tab,
        // Numbers
        .key1, .key2, .key3, .key4, .key5,
        KeyBinding(scancode: 0x07, label: "6"),
        KeyBinding(scancode: 0x08, label: "7"),
        KeyBinding(scancode: 0x09, label: "8"),
        KeyBinding(scancode: 0x0A, label: "9"),
        KeyBinding(scancode: 0x0B, label: "0"),
        // Letters
        .keyA,
        KeyBinding(scancode: 0x30, label: "B"),
        KeyBinding(scancode: 0x2E, label: "C"),
        KeyBinding(scancode: 0x20, label: "D"),
        KeyBinding(scancode: 0x12, label: "E"),
        KeyBinding(scancode: 0x21, label: "F"),
        KeyBinding(scancode: 0x22, label: "G"),
        KeyBinding(scancode: 0x23, label: "H"),
        KeyBinding(scancode: 0x17, label: "I"),
        KeyBinding(scancode: 0x24, label: "J"),
        KeyBinding(scancode: 0x25, label: "K"),
        KeyBinding(scancode: 0x26, label: "L"),
        KeyBinding(scancode: 0x32, label: "M"),
        .keyN,
        KeyBinding(scancode: 0x18, label: "O"),
        KeyBinding(scancode: 0x19, label: "P"),
        KeyBinding(scancode: 0x10, label: "Q"),
        KeyBinding(scancode: 0x13, label: "R"),
        KeyBinding(scancode: 0x1F, label: "S"),
        KeyBinding(scancode: 0x14, label: "T"),
        KeyBinding(scancode: 0x16, label: "U"),
        KeyBinding(scancode: 0x2F, label: "V"),
        KeyBinding(scancode: 0x11, label: "W"),
        KeyBinding(scancode: 0x2D, label: "X"),
        .keyY,
        .keyZ,
        // Function keys
        KeyBinding(scancode: 0x3B, label: "F1"),
        KeyBinding(scancode: 0x3C, label: "F2"),
        KeyBinding(scancode: 0x3D, label: "F3"),
        KeyBinding(scancode: 0x3E, label: "F4"),
        KeyBinding(scancode: 0x3F, label: "F5"),
        KeyBinding(scancode: 0x40, label: "F6"),
        KeyBinding(scancode: 0x41, label: "F7"),
        KeyBinding(scancode: 0x42, label: "F8"),
        KeyBinding(scancode: 0x43, label: "F9"),
        KeyBinding(scancode: 0x44, label: "F10"),
        KeyBinding(scancode: 0x57, label: "F11"),
        KeyBinding(scancode: 0x58, label: "F12"),
        // Misc
        KeyBinding(scancode: 0x0E, label: "Bksp"),
        KeyBinding(scancode: 0x53, label: "Del"),
        KeyBinding(scancode: 0x52, label: "Ins"),
        KeyBinding(scancode: 0x49, label: "PgUp"),
        KeyBinding(scancode: 0x51, label: "PgDn"),
        KeyBinding(scancode: 0x47, label: "Home"),
        KeyBinding(scancode: 0x4F, label: "End"),
    ]
}

// MARK: - D-Pad binding keys

extension TouchControl {
    static let dpadUp        = "up"
    static let dpadDown      = "down"
    static let dpadLeft      = "left"
    static let dpadRight     = "right"
    // Stick
    static let stickTapAction = "tap"
    static let stickSensitivity = "sensitivity"
    // Button
    static let buttonAction  = "action"
    // Button row
    static let buttonRow0    = "btn0"
    static let buttonRow1    = "btn1"
    static let buttonRow2    = "btn2"
    static let buttonRow3    = "btn3"
    static let buttonRow4    = "btn4"
}

// MARK: - Built-in Presets

extension TouchControlLayout {
    // Deterministic UUIDs for built-in presets so MachineConfig can reference them
    private static let doomId   = UUID(uuidString: "00000000-0000-0000-0000-000000000001")!
    private static let dukeId   = UUID(uuidString: "00000000-0000-0000-0000-000000000002")!
    private static let fpsId    = UUID(uuidString: "00000000-0000-0000-0000-000000000003")!
    private static let arrowsId = UUID(uuidString: "00000000-0000-0000-0000-000000000004")!

    static let builtInIds: Set<UUID> = [doomId, dukeId, fpsId, arrowsId]

    static func builtInPresets() -> [TouchControlLayout] {
        [doomPreset(), dukePreset(), fpsPreset(), arrowKeysPreset()]
    }

    // MARK: DOOM

    static func doomPreset() -> TouchControlLayout {
        TouchControlLayout(
            id: doomId,
            name: "DOOM",
            controls: [
                // Left D-Pad for movement
                TouchControl(
                    type: .dpad,
                    position: ControlPosition(x: 0.12, y: 0.5),
                    size: 140,
                    opacity: 0.25,
                    bindings: [
                        TouchControl.dpadUp:    .upArrow,
                        TouchControl.dpadDown:  .downArrow,
                        TouchControl.dpadLeft:  .leftArrow,
                        TouchControl.dpadRight: .rightArrow,
                    ]
                ),
                // Right stick for mouse look / strafing
                TouchControl(
                    type: .stick,
                    position: ControlPosition(x: 0.88, y: 0.45),
                    size: 140,
                    opacity: 0.25,
                    bindings: [
                        TouchControl.stickTapAction: .ctrl,  // tap to fire
                    ]
                ),
                // Action buttons row (Fire, Use, Strafe)
                TouchControl(
                    type: .buttonRow,
                    position: ControlPosition(x: 0.78, y: 0.88),
                    size: 44,
                    opacity: 0.3,
                    bindings: [
                        TouchControl.buttonRow0: KeyBinding(scancode: 0x38, label: "Strf"),
                        TouchControl.buttonRow1: KeyBinding(scancode: 0x39, label: "Use"),
                        TouchControl.buttonRow2: KeyBinding(scancode: 0x1D, label: "Fire"),
                    ]
                ),
                // Weapon keys 1-5
                TouchControl(
                    type: .buttonRow,
                    position: ControlPosition(x: 0.70, y: 0.06),
                    size: 36,
                    opacity: 0.2,
                    bindings: [
                        TouchControl.buttonRow0: .key1,
                        TouchControl.buttonRow1: .key2,
                        TouchControl.buttonRow2: .key3,
                        TouchControl.buttonRow3: .key4,
                        TouchControl.buttonRow4: .key5,
                    ]
                ),
                // Esc button
                TouchControl(
                    type: .button,
                    position: ControlPosition(x: 0.05, y: 0.06),
                    size: 40,
                    opacity: 0.2,
                    bindings: [TouchControl.buttonAction: .escape]
                ),
                // Tab (map) button
                TouchControl(
                    type: .button,
                    position: ControlPosition(x: 0.95, y: 0.06),
                    size: 40,
                    opacity: 0.2,
                    bindings: [TouchControl.buttonAction: .tab]
                ),
                // Enter button
                TouchControl(
                    type: .button,
                    position: ControlPosition(x: 0.50, y: 0.88),
                    size: 40,
                    opacity: 0.2,
                    bindings: [TouchControl.buttonAction: .enter]
                ),
            ],
            isBuiltIn: true
        )
    }

    // MARK: Duke Nukem 3D

    static func dukePreset() -> TouchControlLayout {
        TouchControlLayout(
            id: dukeId,
            name: "Duke Nukem 3D",
            controls: [
                // Left D-Pad
                TouchControl(
                    type: .dpad,
                    position: ControlPosition(x: 0.12, y: 0.5),
                    size: 140,
                    opacity: 0.25,
                    bindings: [
                        TouchControl.dpadUp:    .upArrow,
                        TouchControl.dpadDown:  .downArrow,
                        TouchControl.dpadLeft:  .leftArrow,
                        TouchControl.dpadRight: .rightArrow,
                    ]
                ),
                // Right stick for mouselook
                TouchControl(
                    type: .stick,
                    position: ControlPosition(x: 0.88, y: 0.45),
                    size: 140,
                    opacity: 0.25,
                    bindings: [
                        TouchControl.stickTapAction: .ctrl,
                    ]
                ),
                // Action buttons row (Crouch, Jump, Open, Fire)
                TouchControl(
                    type: .buttonRow,
                    position: ControlPosition(x: 0.72, y: 0.88),
                    size: 44,
                    opacity: 0.3,
                    bindings: [
                        TouchControl.buttonRow0: KeyBinding(scancode: 0x2C, label: "Crch"),
                        TouchControl.buttonRow1: KeyBinding(scancode: 0x1E, label: "Jump"),
                        TouchControl.buttonRow2: KeyBinding(scancode: 0x39, label: "Open"),
                        TouchControl.buttonRow3: KeyBinding(scancode: 0x1D, label: "Fire"),
                    ]
                ),
                // Weapon keys
                TouchControl(
                    type: .buttonRow,
                    position: ControlPosition(x: 0.70, y: 0.06),
                    size: 36,
                    opacity: 0.2,
                    bindings: [
                        TouchControl.buttonRow0: .key1,
                        TouchControl.buttonRow1: .key2,
                        TouchControl.buttonRow2: .key3,
                        TouchControl.buttonRow3: .key4,
                        TouchControl.buttonRow4: .key5,
                    ]
                ),
                // Esc
                TouchControl(
                    type: .button,
                    position: ControlPosition(x: 0.05, y: 0.06),
                    size: 40,
                    opacity: 0.2,
                    bindings: [TouchControl.buttonAction: .escape]
                ),
                // Tab (map)
                TouchControl(
                    type: .button,
                    position: ControlPosition(x: 0.95, y: 0.06),
                    size: 40,
                    opacity: 0.2,
                    bindings: [TouchControl.buttonAction: .tab]
                ),
                // Enter
                TouchControl(
                    type: .button,
                    position: ControlPosition(x: 0.50, y: 0.88),
                    size: 40,
                    opacity: 0.2,
                    bindings: [TouchControl.buttonAction: .enter]
                ),
            ],
            isBuiltIn: true
        )
    }

    // MARK: General FPS

    static func fpsPreset() -> TouchControlLayout {
        var layout = doomPreset()
        layout.id = fpsId
        layout.name = "General FPS"
        // Same controls, relabel buttons generically
        if layout.controls.count > 2 {
            layout.controls[2].bindings[TouchControl.buttonAction] =
                KeyBinding(scancode: 0x1D, label: "Btn1")  // Ctrl
            layout.controls[3].bindings[TouchControl.buttonAction] =
                KeyBinding(scancode: 0x39, label: "Btn2")  // Space
            layout.controls[4].bindings[TouchControl.buttonAction] =
                KeyBinding(scancode: 0x38, label: "Btn3")  // Alt
        }
        return layout
    }

    // MARK: Arrow Keys Only

    static func arrowKeysPreset() -> TouchControlLayout {
        TouchControlLayout(
            id: arrowsId,
            name: "Arrow Keys Only",
            controls: [
                // D-Pad
                TouchControl(
                    type: .dpad,
                    position: ControlPosition(x: 0.12, y: 0.5),
                    size: 140,
                    opacity: 0.25,
                    bindings: [
                        TouchControl.dpadUp:    .upArrow,
                        TouchControl.dpadDown:  .downArrow,
                        TouchControl.dpadLeft:  .leftArrow,
                        TouchControl.dpadRight: .rightArrow,
                    ]
                ),
                // Enter
                TouchControl(
                    type: .button,
                    position: ControlPosition(x: 0.45, y: 0.88),
                    size: 56,
                    opacity: 0.3,
                    bindings: [TouchControl.buttonAction: .enter]
                ),
                // Space
                TouchControl(
                    type: .button,
                    position: ControlPosition(x: 0.58, y: 0.88),
                    size: 56,
                    opacity: 0.3,
                    bindings: [TouchControl.buttonAction: .space]
                ),
                // Y
                TouchControl(
                    type: .button,
                    position: ControlPosition(x: 0.71, y: 0.88),
                    size: 48,
                    opacity: 0.25,
                    bindings: [TouchControl.buttonAction: .keyY]
                ),
                // N
                TouchControl(
                    type: .button,
                    position: ControlPosition(x: 0.82, y: 0.88),
                    size: 48,
                    opacity: 0.25,
                    bindings: [TouchControl.buttonAction: .keyN]
                ),
                // Esc
                TouchControl(
                    type: .button,
                    position: ControlPosition(x: 0.05, y: 0.06),
                    size: 40,
                    opacity: 0.2,
                    bindings: [TouchControl.buttonAction: .escape]
                ),
            ],
            isBuiltIn: true
        )
    }
}

// MARK: - Layout Manager

class TouchLayoutManager: ObservableObject {
    @Published var layouts: [TouchControlLayout] = []

    private let storageKey = "touchLayouts"

    init() {
        load()
    }

    func save() {
        if let data = try? JSONEncoder().encode(layouts) {
            UserDefaults.standard.set(data, forKey: storageKey)
        }
    }

    func load() {
        if let data = UserDefaults.standard.data(forKey: storageKey),
           let loaded = try? JSONDecoder().decode([TouchControlLayout].self, from: data) {
            layouts = loaded
        }
    }

    func addLayout(name: String) -> TouchControlLayout {
        var layout = TouchControlLayout(id: UUID(), name: name, controls: [])
        layout.isBuiltIn = false
        layouts.append(layout)
        save()
        return layout
    }

    func duplicateLayout(_ source: TouchControlLayout, name: String) -> TouchControlLayout {
        var dup = source
        dup.id = UUID()
        dup.name = name
        dup.isBuiltIn = false
        layouts.append(dup)
        save()
        return dup
    }

    func deleteLayout(_ layout: TouchControlLayout) {
        layouts.removeAll { $0.id == layout.id }
        save()
    }

    func updateLayout(_ layout: TouchControlLayout) {
        if let idx = layouts.firstIndex(where: { $0.id == layout.id }) {
            layouts[idx] = layout
            save()
        }
    }

    /// Find a layout by ID, checking user layouts then built-in presets
    func layout(for id: UUID?) -> TouchControlLayout? {
        guard let id = id else { return nil }
        if let userLayout = layouts.first(where: { $0.id == id }) {
            return userLayout
        }
        return TouchControlLayout.builtInPresets().first(where: { $0.id == id })
    }
}
