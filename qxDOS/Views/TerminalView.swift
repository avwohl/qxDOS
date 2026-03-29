/*
 * TerminalView.swift - CGA terminal display with mouse support
 */

import SwiftUI
import UIKit

struct TerminalCell: Equatable {
    var character: Character = " "
    var foreground: UInt8 = 7
    var background: UInt8 = 0
}

struct TerminalView: UIViewRepresentable {
    @Binding var cells: [[TerminalCell]]
    @Binding var cursorRow: Int
    @Binding var cursorCol: Int
    @Binding var shouldFocus: Bool
    var onKeyInput: ((Character) -> Void)?
    var onScancode: ((UInt8, UInt8) -> Void)?  // ascii, scancode
    var onMouseUpdate: ((Int, Int, Int) -> Void)?  // x, y, buttons
    var onViewCreated: ((TerminalUIView) -> Void)?

    let rows: Int
    let cols: Int
    let fontSize: CGFloat

    func makeUIView(context: Context) -> TerminalUIView {
        let view = TerminalUIView(rows: rows, cols: cols, fontSize: fontSize)
        view.onKeyInput = onKeyInput
        view.onScancode = onScancode
        view.onMouseUpdate = onMouseUpdate
        onViewCreated?(view)
        return view
    }

    func updateUIView(_ uiView: TerminalUIView, context: Context) {
        uiView.updateFontSize(fontSize)
        uiView.onScancode = onScancode
        uiView.onMouseUpdate = onMouseUpdate
        if shouldFocus && !uiView.isFirstResponder {
            DispatchQueue.main.async { uiView.becomeFirstResponder() }
        }
    }
}

// MARK: - Terminal with Control Toolbar

struct TerminalWithToolbar: View {
    @Binding var cells: [[TerminalCell]]
    @Binding var cursorRow: Int
    @Binding var cursorCol: Int
    @Binding var shouldFocus: Bool
    var onKeyInput: ((Character) -> Void)?
    var onSetControlify: ((Int) -> Void)?
    var onScancode: ((UInt8, UInt8) -> Void)?
    var onToggleFn: (() -> Void)?
    var onToggleAlt: (() -> Void)?
    var onMouseUpdate: ((Int, Int, Int) -> Void)?
    var onViewCreated: ((TerminalUIView) -> Void)?
    var onTouchEditor: (() -> Void)?
    var onToggleTouchControls: (() -> Void)?
    var onHelp: (() -> Void)?
    var onToggleKeyboard: (() -> Void)?
    var onQuit: (() -> Void)?
    var showQuitButton: Bool = false
    var isControlifyActive: Bool = false
    var isFnActive: Bool = false
    var isAltActive: Bool = false
    var hasTouchLayout: Bool = false
    var showTouchControls: Bool = false
    var keyboardVisible: Bool = false
    var keyboardDocked: Bool = false

    let rows: Int
    let cols: Int
    let fontSize: CGFloat
    var gfxImage: UIImage? = nil

    @State private var stripOnRight = false
    @State private var maxDisplayHeight: CGFloat = 400

    /// Put the strip on the side opposite the camera/Dynamic Island.
    private func updateStripSide() {
        #if targetEnvironment(macCatalyst)
        return
        #else
        let dev = UIDevice.current.orientation
        if dev == .landscapeLeft {
            stripOnRight = true; return
        }
        if dev == .landscapeRight {
            stripOnRight = false; return
        }
        if let scene = UIApplication.shared.connectedScenes
            .compactMap({ $0 as? UIWindowScene }).first {
            stripOnRight = scene.interfaceOrientation == .landscapeRight
        }
        #endif
    }

    /// Read actual safe area height from UIKit — bypasses SwiftUI layout issues.
    private func updateMaxDisplayHeight() {
        #if targetEnvironment(macCatalyst)
        maxDisplayHeight = .infinity
        #else
        guard let scene = UIApplication.shared.connectedScenes
            .compactMap({ $0 as? UIWindowScene }).first,
            let window = scene.windows.first else { return }
        let insets = window.safeAreaInsets
        let screenH = UIScreen.main.bounds.height
        maxDisplayHeight = screenH - insets.top - insets.bottom
        #endif
    }

    private var controlStrip: some View {
        VStack(spacing: 3) {
            // Keyboard show/hide
            ToolbarIconButton(icon: "keyboard", isActive: keyboardVisible) {
                onToggleKeyboard?()
            }

            // Modifier toggles
            ToolbarButton(title: "Ctrl", isActive: isControlifyActive) {
                onSetControlify?(isControlifyActive ? 0 : 1)
            }
            ToolbarButton(title: "Alt", isActive: isAltActive) {
                onToggleAlt?()
            }
            ToolbarButton(title: "Fn", isActive: isFnActive) {
                onToggleFn?()
            }

            // Hide extra keys when docked keyboard takes screen space
            if !keyboardDocked {
                Divider().frame(width: 24)

                ToolbarButton(title: "Esc", isActive: false) {
                    onSetControlify?(0)
                    onKeyInput?(Character(UnicodeScalar(27)))
                }
                ToolbarButton(title: "↵", isActive: false) {
                    onSetControlify?(0)
                    onKeyInput?(Character("\r"))
                }
                ToolbarButton(title: "Tab", isActive: false) {
                    onSetControlify?(0)
                    onKeyInput?(Character(UnicodeScalar(9)))
                }

                Divider().frame(width: 24)

                ToolbarButton(title: "Del", isActive: false) {
                    onScancode?(0, 0x53)
                }
                ToolbarButton(title: "Ins", isActive: false) {
                    onScancode?(0, 0x52)
                }
                ToolbarButton(title: "PgU", isActive: false) {
                    onScancode?(0, 0x49)
                }
                ToolbarButton(title: "PgD", isActive: false) {
                    onScancode?(0, 0x51)
                }

                if hasTouchLayout {
                    Divider().frame(width: 24)

                    ToolbarIconButton(icon: "gamecontroller", isActive: false) {
                        onTouchEditor?()
                    }
                    ToolbarIconButton(icon: showTouchControls ? "eye.fill" : "eye.slash",
                                      isActive: showTouchControls) {
                        onToggleTouchControls?()
                    }
                }

            }

            Divider().frame(width: 24)

            ToolbarIconButton(icon: "questionmark.circle", isActive: false) {
                onHelp?()
            }

            if showQuitButton {
                ToolbarIconButton(icon: "xmark.circle", isActive: false) {
                    onQuit?()
                }
            }

            Spacer()
        }
        .padding(.top, 6)
        .padding(.bottom, 4)
        .padding(.horizontal, 3)
        .background(Color(UIColor.systemGray5))
    }

    private var displayArea: some View {
        ZStack {
            TerminalView(
                cells: $cells,
                cursorRow: $cursorRow,
                cursorCol: $cursorCol,
                shouldFocus: $shouldFocus,
                onKeyInput: onKeyInput,
                onScancode: onScancode,
                onMouseUpdate: onMouseUpdate,
                onViewCreated: onViewCreated,
                rows: rows,
                cols: cols,
                fontSize: fontSize
            )
            .opacity(gfxImage == nil ? 1 : 0)

            if let img = gfxImage {
                Image(uiImage: img)
                    .interpolation(.none)
                    .resizable()
                    .aspectRatio(CGSize(width: 4, height: 3), contentMode: .fit)
                    .frame(maxHeight: maxDisplayHeight)
                    .allowsHitTesting(false)
            }
        }
        .frame(maxHeight: maxDisplayHeight)
        .clipped()
    }

    var body: some View {
        HStack(spacing: 0) {
            if stripOnRight {
                displayArea
                controlStrip
            } else {
                controlStrip
                displayArea
            }
        }
        .onAppear {
            #if !targetEnvironment(macCatalyst)
            UIDevice.current.beginGeneratingDeviceOrientationNotifications()
            #endif
            updateStripSide()
            updateMaxDisplayHeight()
        }
        .onReceive(NotificationCenter.default.publisher(
            for: UIDevice.orientationDidChangeNotification
        )) { _ in
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.15) {
                updateStripSide()
                updateMaxDisplayHeight()
            }
        }
    }
}

struct ToolbarIconButton: View {
    let icon: String
    let isActive: Bool
    let action: () -> Void

    var body: some View {
        Button(action: action) {
            Image(systemName: icon)
                .font(.system(size: 11, weight: .medium))
                .frame(width: 30, height: 22)
                .background(isActive ? Color.blue : Color(UIColor.systemGray4))
                .foregroundColor(isActive ? .white : .primary)
                .cornerRadius(5)
        }
        .buttonStyle(.plain)
    }
}

struct ToolbarButton: View {
    let title: String
    let isActive: Bool
    let action: () -> Void

    var body: some View {
        Button(action: action) {
            Text(title)
                .font(.system(size: 11, weight: .medium))
                .frame(width: 30)
                .padding(.vertical, 4)
                .background(isActive ? Color.blue : Color(UIColor.systemGray4))
                .foregroundColor(isActive ? .white : .primary)
                .cornerRadius(5)
        }
        .buttonStyle(.plain)
    }
}

// MARK: - Terminal UI View

class TerminalUIView: UIView, UIKeyInput {
    var onKeyInput: ((Character) -> Void)?
    var onScancode: ((UInt8, UInt8) -> Void)?
    var onMouseUpdate: ((Int, Int, Int) -> Void)?
    var onKeyboardStateChanged: ((Bool, Bool) -> Void)?  // (visible, docked)

    private let rows: Int
    private let cols: Int
    private var cells: [[TerminalCell]] = []
    private var cursorRow: Int = 0
    private var cursorCol: Int = 0
    private var charWidth: CGFloat = 0
    private var charHeight: CGFloat = 0
    private var font: UIFont
    private var currentFontSize: CGFloat

    // Mouse tracking
    private var mouseButtons: Int = 0

    // Haptic feedback for keyboard
    private let keyFeedback = UIImpactFeedbackGenerator(style: .light)

    private let cgaColors: [UIColor] = [
        UIColor(red: 0/255, green: 0/255, blue: 0/255, alpha: 1),
        UIColor(red: 0/255, green: 0/255, blue: 170/255, alpha: 1),
        UIColor(red: 0/255, green: 170/255, blue: 0/255, alpha: 1),
        UIColor(red: 0/255, green: 170/255, blue: 170/255, alpha: 1),
        UIColor(red: 170/255, green: 0/255, blue: 0/255, alpha: 1),
        UIColor(red: 170/255, green: 0/255, blue: 170/255, alpha: 1),
        UIColor(red: 170/255, green: 85/255, blue: 0/255, alpha: 1),
        UIColor(red: 170/255, green: 170/255, blue: 170/255, alpha: 1),
        UIColor(red: 85/255, green: 85/255, blue: 85/255, alpha: 1),
        UIColor(red: 85/255, green: 85/255, blue: 255/255, alpha: 1),
        UIColor(red: 85/255, green: 255/255, blue: 85/255, alpha: 1),
        UIColor(red: 85/255, green: 255/255, blue: 255/255, alpha: 1),
        UIColor(red: 255/255, green: 85/255, blue: 85/255, alpha: 1),
        UIColor(red: 255/255, green: 85/255, blue: 255/255, alpha: 1),
        UIColor(red: 255/255, green: 255/255, blue: 85/255, alpha: 1),
        UIColor(red: 255/255, green: 255/255, blue: 255/255, alpha: 1)
    ]

    init(rows: Int, cols: Int, fontSize: CGFloat = 20) {
        self.rows = rows
        self.cols = cols
        self.currentFontSize = fontSize
        self.font = UIFont.monospacedSystemFont(ofSize: fontSize, weight: .regular)
        super.init(frame: .zero)
        updateCharDimensions()
        cells = Array(repeating: Array(repeating: TerminalCell(), count: cols), count: rows)
        backgroundColor = .black
        contentMode = .redraw
        autoresizingMask = [.flexibleWidth, .flexibleHeight]
        isMultipleTouchEnabled = false

        let tap = UITapGestureRecognizer(target: self, action: #selector(handleTap(_:)))
        addGestureRecognizer(tap)

        let longPress = UILongPressGestureRecognizer(target: self, action: #selector(handleLongPress(_:)))
        addGestureRecognizer(longPress)

        let pan = UIPanGestureRecognizer(target: self, action: #selector(handlePan(_:)))
        addGestureRecognizer(pan)

        #if !targetEnvironment(macCatalyst)
        NotificationCenter.default.addObserver(
            forName: UIResponder.keyboardWillChangeFrameNotification, object: nil, queue: .main
        ) { [weak self] notification in
            guard let frame = notification.userInfo?[UIResponder.keyboardFrameEndUserInfoKey] as? CGRect,
                  frame.width > 0 else { return }
            let screen = UIScreen.main.bounds
            let isHidden = frame.origin.y >= screen.height - 1
            let isDocked = !isHidden &&
                           frame.width >= screen.width * 0.9 &&
                           frame.maxY >= screen.height - 1
            self?.onKeyboardStateChanged?(!isHidden, isDocked)
        }
        #endif
    }

    required init?(coder: NSCoder) { fatalError() }

    override func layoutSubviews() {
        super.layoutSubviews()
        setNeedsDisplay()
    }

    private func updateCharDimensions() {
        let size = ("M" as NSString).size(withAttributes: [.font: font])
        charWidth = size.width
        charHeight = size.height
    }

    func updateFontSize(_ newSize: CGFloat) {
        guard newSize != currentFontSize else { return }
        currentFontSize = newSize
        font = UIFont.monospacedSystemFont(ofSize: newSize, weight: .regular)
        updateCharDimensions()
        setNeedsDisplay()
    }

    func updateCells(_ newCells: [[TerminalCell]], cursorRow: Int, cursorCol: Int) {
        self.cells = newCells
        self.cursorRow = cursorRow
        self.cursorCol = cursorCol
        setNeedsDisplay()
    }

    func updateCursor(row: Int, col: Int) {
        self.cursorRow = row
        self.cursorCol = col
        setNeedsDisplay()
    }

    // MARK: - Coordinate conversion

    private func terminalLayout() -> (offsetX: CGFloat, offsetY: CGFloat, scale: CGFloat) {
        let tw = CGFloat(cols) * charWidth
        let th = CGFloat(rows) * charHeight
        let sx = bounds.width / tw
        let sy = bounds.height / th
        let s = min(sx, sy)
        let ox = (bounds.width - tw * s) / 2 + 2
        let oy = (bounds.height - th * s) / 2
        return (ox, oy, s)
    }

    /// Convert a view point to virtual DOS mouse coordinates (0-639, 0-199)
    private func viewPointToMouse(_ point: CGPoint) -> (x: Int, y: Int) {
        let layout = terminalLayout()
        // Normalize to 0..1 within the terminal area
        let nx = (point.x - layout.offsetX) / (CGFloat(cols) * charWidth * layout.scale)
        let ny = (point.y - layout.offsetY) / (CGFloat(rows) * charHeight * layout.scale)
        let mx = Int(max(0, min(639, nx * 640)))
        let my = Int(max(0, min(199, ny * 200)))
        return (mx, my)
    }

    // MARK: - Touch / Mouse

    @objc private func handleTap(_ gesture: UITapGestureRecognizer) {
        becomeFirstResponder()
        let pt = gesture.location(in: self)
        let (mx, my) = viewPointToMouse(pt)
        // Tap = left click + release
        onMouseUpdate?(mx, my, 1)
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.05) { [weak self] in
            self?.onMouseUpdate?(mx, my, 0)
        }
    }

    @objc private func handleLongPress(_ gesture: UILongPressGestureRecognizer) {
        guard gesture.state == .began else { return }
        becomeFirstResponder()

        // Long press = right click
        let pt = gesture.location(in: self)
        let (mx, my) = viewPointToMouse(pt)
        onMouseUpdate?(mx, my, 2)
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.1) { [weak self] in
            self?.onMouseUpdate?(mx, my, 0)
        }
    }

    @objc private func handlePan(_ gesture: UIPanGestureRecognizer) {
        let pt = gesture.location(in: self)
        let (mx, my) = viewPointToMouse(pt)
        switch gesture.state {
        case .began:
            mouseButtons = 1  // Left button down while dragging
            onMouseUpdate?(mx, my, mouseButtons)
        case .changed:
            onMouseUpdate?(mx, my, mouseButtons)
        case .ended, .cancelled:
            mouseButtons = 0
            onMouseUpdate?(mx, my, 0)
        default:
            break
        }
    }

    // MARK: - Drawing

    override func draw(_ rect: CGRect) {
        guard let context = UIGraphicsGetCurrentContext() else { return }
        UIColor.black.setFill()
        context.fill(bounds)

        let layout = terminalLayout()
        context.saveGState()
        context.translateBy(x: layout.offsetX, y: layout.offsetY)
        context.scaleBy(x: layout.scale, y: layout.scale)

        for row in 0..<min(rows, cells.count) {
            for col in 0..<min(cols, cells[row].count) {
                let cell = cells[row][col]
                let x = CGFloat(col) * charWidth
                let y = CGFloat(row) * charHeight
                if cell.background != 0 {
                    cgaColors[Int(cell.background) & 0x0F].setFill()
                    context.fill(CGRect(x: x, y: y, width: charWidth, height: charHeight))
                }
                let attrs: [NSAttributedString.Key: Any] = [
                    .font: font,
                    .foregroundColor: cgaColors[Int(cell.foreground) & 0x0F]
                ]
                (String(cell.character) as NSString).draw(at: CGPoint(x: x, y: y), withAttributes: attrs)
            }
        }

        let cx = CGFloat(cursorCol) * charWidth
        let cy = CGFloat(cursorRow) * charHeight
        UIColor.green.withAlphaComponent(0.7).setFill()
        context.fill(CGRect(x: cx, y: cy, width: charWidth, height: charHeight))
        if cursorRow < cells.count && cursorCol < cells[cursorRow].count {
            let attrs: [NSAttributedString.Key: Any] = [.font: font, .foregroundColor: UIColor.black]
            (String(cells[cursorRow][cursorCol].character) as NSString).draw(at: CGPoint(x: cx, y: cy), withAttributes: attrs)
        }

        context.restoreGState()
    }

    // MARK: - UIKeyInput

    override var canBecomeFirstResponder: Bool { true }
    var hasText: Bool { true }

    func insertText(_ text: String) {
        keyFeedback.impactOccurred()
        for char in text { onKeyInput?(char) }
    }

    func deleteBackward() {
        keyFeedback.impactOccurred()
        onKeyInput?(Character(UnicodeScalar(8)))
    }

    override var keyCommands: [UIKeyCommand]? {
        var cmds = [
            UIKeyCommand(input: "\r", modifierFlags: [], action: #selector(enterKey)),
            UIKeyCommand(input: UIKeyCommand.inputEscape, modifierFlags: [], action: #selector(escKey)),
            UIKeyCommand(input: UIKeyCommand.inputUpArrow, modifierFlags: [], action: #selector(upKey)),
            UIKeyCommand(input: UIKeyCommand.inputDownArrow, modifierFlags: [], action: #selector(downKey)),
            UIKeyCommand(input: UIKeyCommand.inputLeftArrow, modifierFlags: [], action: #selector(leftKey)),
            UIKeyCommand(input: UIKeyCommand.inputRightArrow, modifierFlags: [], action: #selector(rightKey)),
            UIKeyCommand(input: "c", modifierFlags: .command, action: #selector(copyText)),
            UIKeyCommand(input: "v", modifierFlags: .command, action: #selector(pasteText))
        ]
        for ch in "abcdefghijklmnopqrstuvwxyz" {
            cmds.append(UIKeyCommand(input: String(ch), modifierFlags: .control, action: #selector(ctrlKey(_:))))
        }
        return cmds
    }

    @objc private func copyText() {
        var text = ""
        for row in cells {
            var line = row.map { String($0.character) }.joined()
            while line.hasSuffix(" ") { line.removeLast() }
            text += line + "\n"
        }
        while text.hasSuffix("\n\n") { text.removeLast() }
        UIPasteboard.general.string = text
    }

    @objc private func pasteText() {
        guard let text = UIPasteboard.general.string else { return }
        for ch in text { onKeyInput?(ch == "\n" ? Character("\r") : ch) }
    }

    @objc private func enterKey() { onKeyInput?(Character("\r")) }
    @objc private func escKey() { onKeyInput?(Character(UnicodeScalar(27))) }
    @objc private func upKey() { onKeyInput?(Character(UnicodeScalar(0xF700)!)) }
    @objc private func downKey() { onKeyInput?(Character(UnicodeScalar(0xF701)!)) }
    @objc private func leftKey() { onKeyInput?(Character(UnicodeScalar(0xF702)!)) }
    @objc private func rightKey() { onKeyInput?(Character(UnicodeScalar(0xF703)!)) }

    @objc private func ctrlKey(_ cmd: UIKeyCommand) {
        guard let ch = cmd.input?.first, let a = ch.asciiValue else { return }
        onKeyInput?(Character(UnicodeScalar(a - 96)))
    }

    override func pressesBegan(_ presses: Set<UIPress>, with event: UIPressesEvent?) {
        var handled = false
        for press in presses {
            guard let key = press.key else { continue }

            // Function keys and navigation keys via HID keyCode
            if let scan = Self.hidToDosScancode(key.keyCode) {
                onScancode?(0, scan)
                handled = true
                continue
            }

            // Skip Cmd combos (system shortcuts, handled by keyCommands)
            if key.modifierFlags.contains(.command) { continue }

            // Ctrl+letter from hardware keyboard: send ASCII 1-26
            if key.modifierFlags.contains(.control) {
                for ch in key.charactersIgnoringModifiers {
                    if let a = ch.asciiValue {
                        if a >= 0x61 && a <= 0x7A { // a-z
                            onKeyInput?(Character(UnicodeScalar(a - 0x60)))
                            handled = true
                        } else if a >= 0x41 && a <= 0x5A { // A-Z
                            onKeyInput?(Character(UnicodeScalar(a - 0x40)))
                            handled = true
                        }
                    }
                }
                continue
            }

            // Alt/Option+key: send scancode with ascii=0
            if key.modifierFlags.contains(.alternate) {
                for ch in key.charactersIgnoringModifiers {
                    if let a = ch.asciiValue, let scan = Self.charToDosScancode(a) {
                        onScancode?(0, scan)
                        handled = true
                    }
                }
                continue
            }

            for ch in key.characters { onKeyInput?(ch); handled = true }
        }
        if !handled { super.pressesBegan(presses, with: event) }
    }

    /// Map USB HID key codes to IBM PC scancodes for keys not in the text input path
    private static func hidToDosScancode(_ usage: UIKeyboardHIDUsage) -> UInt8? {
        switch usage {
        case .keyboardDeleteOrBackspace: return 0x0E
        case .keyboardEscape:        return 0x01
        case .keyboardTab:           return 0x0F
        case .keyboardF1:            return 0x3B
        case .keyboardF2:            return 0x3C
        case .keyboardF3:            return 0x3D
        case .keyboardF4:            return 0x3E
        case .keyboardF5:            return 0x3F
        case .keyboardF6:            return 0x40
        case .keyboardF7:            return 0x41
        case .keyboardF8:            return 0x42
        case .keyboardF9:            return 0x43
        case .keyboardF10:           return 0x44
        case .keyboardF11:           return 0x57
        case .keyboardF12:           return 0x58
        case .keyboardHome:          return 0x47
        case .keyboardEnd:           return 0x4F
        case .keyboardPageUp:        return 0x49
        case .keyboardPageDown:      return 0x51
        case .keyboardInsert:        return 0x52
        case .keyboardDeleteForward: return 0x53
        default:                     return nil
        }
    }

    /// Map ASCII to IBM PC scancode (for Alt+key on hardware keyboards)
    private static func charToDosScancode(_ ascii: UInt8) -> UInt8? {
        switch ascii {
        case 0x61...0x7A: // a-z
            let t: [UInt8] = [0x1E,0x30,0x2E,0x20,0x12,0x21,0x22,0x23,0x17,0x24,
                              0x25,0x26,0x32,0x31,0x18,0x19,0x10,0x13,0x1F,0x14,
                              0x16,0x2F,0x11,0x2D,0x15,0x2C]
            return t[Int(ascii - 0x61)]
        case 0x41...0x5A: return charToDosScancode(ascii + 0x20)
        case 0x31...0x39: return UInt8(ascii - 0x31 + 0x02)
        case 0x30: return 0x0B
        default:   return nil
        }
    }

    override func canPerformAction(_ action: Selector, withSender sender: Any?) -> Bool {
        action == #selector(copyText) || action == #selector(copy(_:)) || super.canPerformAction(action, withSender: sender)
    }

    @objc override func copy(_ sender: Any?) { copyText() }
}
