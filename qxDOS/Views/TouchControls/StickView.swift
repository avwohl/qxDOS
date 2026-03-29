/*
 * StickView.swift - Analog stick for mouse look
 *
 * Circular stick that converts drag deltas to relative mouse motion.
 * Quick tap (< 200ms, < 10pt movement) triggers the configured tap action
 * (typically fire). Based on ios_hexen's LookPadView.
 */

import SwiftUI

struct StickView: View {
    let control: TouchControl
    let onMouseDelta: (Int, Int) -> Void  // dx, dy relative
    let onPress: (UInt8) -> Void
    let onRelease: (UInt8) -> Void

    @State private var thumbOffset: CGSize = .zero
    @State private var isDragging = false
    @State private var lastDragPos: CGPoint = .zero
    @State private var dragStartTime: Date = .distantPast
    @State private var dragStartPos: CGPoint = .zero
    @State private var totalMoved: CGFloat = 0

    private let sensitivity: CGFloat = 2.0
    private let tapTimeout: TimeInterval = 0.2
    private let tapMaxMove: CGFloat = 10

    var body: some View {
        let size = control.size
        ZStack {
            // Outer ring
            Circle()
                .stroke(Color.white.opacity(control.opacity), lineWidth: 2)
                .frame(width: size, height: size)

            // Inner area
            Circle()
                .fill(Color.white.opacity(control.opacity * 0.3))
                .frame(width: size, height: size)

            // Thumb
            Circle()
                .fill(Color.white.opacity(isDragging ? 0.5 : 0.3))
                .frame(width: size * 0.35, height: size * 0.35)
                .offset(clampedOffset(maxRadius: size * 0.32))
        }
        .contentShape(Circle().size(CGSize(width: size, height: size)))
        .gesture(
            DragGesture(minimumDistance: 0)
                .onChanged { value in
                    if !isDragging {
                        isDragging = true
                        lastDragPos = value.location
                        dragStartTime = Date()
                        dragStartPos = value.location
                        totalMoved = 0
                        return
                    }

                    let dx = value.location.x - lastDragPos.x
                    let dy = value.location.y - lastDragPos.y
                    lastDragPos = value.location
                    totalMoved += sqrt(dx * dx + dy * dy)

                    thumbOffset = value.translation

                    // Send relative mouse motion
                    let mdx = Int(dx * sensitivity)
                    let mdy = Int(dy * sensitivity)
                    if mdx != 0 || mdy != 0 {
                        onMouseDelta(mdx, mdy)
                    }
                }
                .onEnded { value in
                    isDragging = false
                    thumbOffset = .zero

                    // Quick tap detection
                    let elapsed = Date().timeIntervalSince(dragStartTime)
                    if elapsed < tapTimeout && totalMoved < tapMaxMove {
                        if let tapBinding = control.bindings[TouchControl.stickTapAction] {
                            onPress(tapBinding.scancode)
                            DispatchQueue.main.asyncAfter(deadline: .now() + 0.05) {
                                onRelease(tapBinding.scancode)
                            }
                        }
                    }
                }
        )
    }

    private func clampedOffset(maxRadius: CGFloat) -> CGSize {
        let dist = sqrt(thumbOffset.width * thumbOffset.width + thumbOffset.height * thumbOffset.height)
        if dist < 1 { return .zero }
        let clamped = min(dist, maxRadius)
        let scale = clamped / dist
        return CGSize(width: thumbOffset.width * scale, height: thumbOffset.height * scale)
    }
}
