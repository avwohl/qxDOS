/*
 * DPadView.swift - 8-directional virtual D-Pad
 *
 * Drag-based movement pad inspired by ios_hexen. Supports 8 directions
 * (up, down, left, right, and diagonals). Only sends press/release
 * events when the active direction set changes.
 */

import SwiftUI

struct DPadView: View {
    let control: TouchControl
    let onPress: (UInt8) -> Void
    let onRelease: (UInt8) -> Void

    @State private var activeKeys: Set<UInt8> = []
    @State private var dragOffset: CGSize = .zero
    @State private var isDragging = false

    private let deadzone: CGFloat = 20

    var body: some View {
        let size = control.size
        ZStack {
            // Background circle
            Circle()
                .fill(Color.white.opacity(control.opacity * 0.5))
                .frame(width: size, height: size)

            // Direction indicators
            VStack(spacing: size * 0.25) {
                Image(systemName: "chevron.up")
                    .foregroundColor(directionColor("up"))
                Spacer().frame(height: 0)
                Image(systemName: "chevron.down")
                    .foregroundColor(directionColor("down"))
            }
            .frame(height: size * 0.6)

            HStack(spacing: size * 0.25) {
                Image(systemName: "chevron.left")
                    .foregroundColor(directionColor("left"))
                Spacer().frame(width: 0)
                Image(systemName: "chevron.right")
                    .foregroundColor(directionColor("right"))
            }
            .frame(width: size * 0.6)

            // Thumb indicator when dragging
            if isDragging {
                Circle()
                    .fill(Color.white.opacity(0.4))
                    .frame(width: size * 0.35, height: size * 0.35)
                    .offset(thumbOffset(maxRadius: size * 0.3))
            }
        }
        .font(.system(size: size * 0.15, weight: .bold))
        .contentShape(Circle().size(CGSize(width: size, height: size)))
        .gesture(
            DragGesture(minimumDistance: 0)
                .onChanged { value in
                    isDragging = true
                    dragOffset = value.translation
                    updateDirections()
                }
                .onEnded { _ in
                    isDragging = false
                    dragOffset = .zero
                    releaseAll()
                }
        )
    }

    private func thumbOffset(maxRadius: CGFloat) -> CGSize {
        let dist = sqrt(dragOffset.width * dragOffset.width + dragOffset.height * dragOffset.height)
        if dist < 1 { return .zero }
        let clamped = min(dist, maxRadius)
        let scale = clamped / dist
        return CGSize(width: dragOffset.width * scale, height: dragOffset.height * scale)
    }

    private func directionColor(_ dir: String) -> Color {
        guard let binding = control.bindings[dir] else { return .white.opacity(0.5) }
        return activeKeys.contains(binding.scancode) ? .white : .white.opacity(0.5)
    }

    private func updateDirections() {
        let dx = dragOffset.width
        let dy = dragOffset.height
        let dist = sqrt(dx * dx + dy * dy)

        var newKeys: Set<UInt8> = []

        if dist > deadzone {
            // Use angle to determine 8-way direction
            let angle = atan2(dy, dx)  // radians, 0 = right

            // Map angle to directions
            // Right: -pi/8 to pi/8
            // Down-right: pi/8 to 3pi/8
            // Down: 3pi/8 to 5pi/8
            // etc.

            if let up = control.bindings[TouchControl.dpadUp]?.scancode {
                // Up: angle between -5pi/8 and -3pi/8 (pure up)
                // Also up in diagonals: -7pi/8 to -pi/8
                if angle < -3 * .pi / 8 && angle > -7 * .pi / 8 {
                    newKeys.insert(up)
                }
            }
            if let down = control.bindings[TouchControl.dpadDown]?.scancode {
                if angle > 3 * .pi / 8 && angle < 7 * .pi / 8 {
                    newKeys.insert(down)
                }
            }
            if let left = control.bindings[TouchControl.dpadLeft]?.scancode {
                if abs(angle) > 5 * .pi / 8 {
                    newKeys.insert(left)
                }
            }
            if let right = control.bindings[TouchControl.dpadRight]?.scancode {
                if abs(angle) < 3 * .pi / 8 {
                    newKeys.insert(right)
                }
            }
        }

        // Send press/release only for changes
        let released = activeKeys.subtracting(newKeys)
        let pressed = newKeys.subtracting(activeKeys)
        for key in released { onRelease(key) }
        for key in pressed { onPress(key) }
        activeKeys = newKeys
    }

    private func releaseAll() {
        for key in activeKeys { onRelease(key) }
        activeKeys = []
    }
}
