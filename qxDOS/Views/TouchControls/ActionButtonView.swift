/*
 * ActionButtonView.swift - Single action button and button row
 *
 * Circular hold-buttons that send scancode press on touch begin and
 * release on touch end. ButtonRowView renders a horizontal row of
 * small buttons (e.g., weapon keys 1-5).
 */

import SwiftUI

// MARK: - Single Action Button

struct ActionButtonView: View {
    let control: TouchControl
    let onPress: (UInt8) -> Void
    let onRelease: (UInt8) -> Void

    @State private var isPressed = false

    private var binding: KeyBinding? {
        control.bindings[TouchControl.buttonAction]
    }

    var body: some View {
        let size = control.size
        ZStack {
            Circle()
                .fill(Color.white.opacity(isPressed ? control.opacity + 0.15 : control.opacity))
                .frame(width: size, height: size)

            Text(binding?.label ?? "?")
                .font(.system(size: min(size * 0.35, 18), weight: .semibold))
                .foregroundColor(.white)
        }
        .contentShape(Circle())
        .gesture(
            DragGesture(minimumDistance: 0)
                .onChanged { _ in
                    if !isPressed, let sc = binding?.scancode {
                        isPressed = true
                        onPress(sc)
                    }
                }
                .onEnded { _ in
                    if isPressed, let sc = binding?.scancode {
                        isPressed = false
                        onRelease(sc)
                    }
                }
        )
    }
}

// MARK: - Button Row

struct ButtonRowView: View {
    let control: TouchControl
    let onPress: (UInt8) -> Void
    let onRelease: (UInt8) -> Void

    private var sortedBindings: [(key: String, binding: KeyBinding)] {
        let keys = [TouchControl.buttonRow0, TouchControl.buttonRow1,
                    TouchControl.buttonRow2, TouchControl.buttonRow3,
                    TouchControl.buttonRow4]
        return keys.compactMap { key in
            guard let b = control.bindings[key] else { return nil }
            return (key, b)
        }
    }

    var body: some View {
        let btnSize = control.size
        let spacing: CGFloat = 4

        HStack(spacing: spacing) {
            ForEach(sortedBindings, id: \.key) { item in
                RowButton(
                    label: item.binding.label,
                    scancode: item.binding.scancode,
                    size: btnSize,
                    opacity: control.opacity,
                    onPress: onPress,
                    onRelease: onRelease
                )
            }
        }
    }
}

private struct RowButton: View {
    let label: String
    let scancode: UInt8
    let size: CGFloat
    let opacity: CGFloat
    let onPress: (UInt8) -> Void
    let onRelease: (UInt8) -> Void

    @State private var isPressed = false

    var body: some View {
        ZStack {
            RoundedRectangle(cornerRadius: size * 0.2)
                .fill(Color.white.opacity(isPressed ? opacity + 0.15 : opacity))
                .frame(width: size, height: size)

            Text(label)
                .font(.system(size: min(size * 0.45, 16), weight: .medium))
                .foregroundColor(.white)
        }
        .contentShape(Rectangle())
        .gesture(
            DragGesture(minimumDistance: 0)
                .onChanged { _ in
                    if !isPressed {
                        isPressed = true
                        onPress(scancode)
                    }
                }
                .onEnded { _ in
                    if isPressed {
                        isPressed = false
                        onRelease(scancode)
                    }
                }
        )
    }
}
