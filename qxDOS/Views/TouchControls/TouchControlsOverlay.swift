/*
 * TouchControlsOverlay.swift - Composites all touch controls from a layout
 *
 * Positioned as a ZStack overlay on top of the DOSBox display.
 * Each control is placed using normalized (0..1) coordinates.
 */

import SwiftUI

struct TouchControlsOverlay: View {
    let layout: TouchControlLayout
    let onScancodePress: (UInt8) -> Void
    let onScancodeRelease: (UInt8) -> Void
    let onMouseDelta: (Int, Int) -> Void

    var body: some View {
        GeometryReader { geo in
            ZStack {
                ForEach(layout.controls) { control in
                    controlView(for: control)
                        .position(
                            x: control.position.x * geo.size.width,
                            y: control.position.y * geo.size.height
                        )
                }
            }
        }
        .allowsHitTesting(true)
    }

    @ViewBuilder
    private func controlView(for control: TouchControl) -> some View {
        switch control.type {
        case .dpad:
            DPadView(
                control: control,
                onPress: onScancodePress,
                onRelease: onScancodeRelease
            )
        case .stick:
            StickView(
                control: control,
                onMouseDelta: onMouseDelta,
                onPress: onScancodePress,
                onRelease: onScancodeRelease
            )
        case .button:
            ActionButtonView(
                control: control,
                onPress: onScancodePress,
                onRelease: onScancodeRelease
            )
        case .buttonRow:
            ButtonRowView(
                control: control,
                onPress: onScancodePress,
                onRelease: onScancodeRelease
            )
        }
    }
}
