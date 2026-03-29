/*
 * TouchControlEditorView.swift - Layout manager and editor
 *
 * Presents built-in presets and user-created layouts. Users can
 * select a preset, copy it to create a custom layout, and edit
 * control positions, sizes, and key bindings.
 */

import SwiftUI

// MARK: - Layout List (main editor entry point)

struct TouchControlEditorView: View {
    @ObservedObject var layoutManager: TouchLayoutManager
    @Binding var activeLayoutId: UUID?
    @Environment(\.dismiss) private var dismiss

    @State private var showingNewLayoutAlert = false
    @State private var showingRenameAlert = false
    @State private var newLayoutName = ""
    @State private var layoutToRename: TouchControlLayout?
    @State private var editingLayout: TouchControlLayout?

    private let builtInPresets = TouchControlLayout.builtInPresets()

    var body: some View {
        NavigationView {
            List {
                // Built-in presets
                Section("Built-in Presets") {
                    ForEach(builtInPresets) { preset in
                        HStack {
                            VStack(alignment: .leading) {
                                Text(preset.name).font(.body)
                                Text("\(preset.controls.count) controls")
                                    .font(.caption)
                                    .foregroundColor(.secondary)
                            }

                            Spacer()

                            if activeLayoutId == preset.id {
                                Image(systemName: "checkmark")
                                    .foregroundColor(.blue)
                            }

                            Button("Use") {
                                activeLayoutId = preset.id
                            }
                            .buttonStyle(.bordered)
                            .controlSize(.small)

                            Button("Edit") {
                                let copy = layoutManager.duplicateLayout(preset, name: "\(preset.name) (Custom)")
                                activeLayoutId = copy.id
                                editingLayout = copy
                            }
                            .buttonStyle(.bordered)
                            .controlSize(.small)
                        }
                    }
                }

                // User layouts
                Section("My Layouts") {
                    if layoutManager.layouts.isEmpty {
                        Text("No custom layouts yet")
                            .foregroundColor(.secondary)
                            .italic()
                    } else {
                        ForEach(layoutManager.layouts) { layout in
                            HStack {
                                VStack(alignment: .leading) {
                                    Text(layout.name).font(.body)
                                    Text("\(layout.controls.count) controls")
                                        .font(.caption)
                                        .foregroundColor(.secondary)
                                }

                                Spacer()

                                if activeLayoutId == layout.id {
                                    Image(systemName: "checkmark")
                                        .foregroundColor(.blue)
                                }

                                Button("Use") {
                                    activeLayoutId = layout.id
                                }
                                .buttonStyle(.bordered)
                                .controlSize(.small)

                                Button("Edit") {
                                    editingLayout = layout
                                }
                                .buttonStyle(.bordered)
                                .controlSize(.small)
                            }
                            .contextMenu {
                                Button("Rename") {
                                    layoutToRename = layout
                                    newLayoutName = layout.name
                                    showingRenameAlert = true
                                }
                                Button("Duplicate") {
                                    let dup = layoutManager.duplicateLayout(layout, name: "\(layout.name) Copy")
                                    activeLayoutId = dup.id
                                }
                                Divider()
                                Button("Delete", role: .destructive) {
                                    if activeLayoutId == layout.id {
                                        activeLayoutId = nil
                                    }
                                    layoutManager.deleteLayout(layout)
                                }
                            }
                        }
                        .onDelete { offsets in
                            for idx in offsets {
                                let layout = layoutManager.layouts[idx]
                                if activeLayoutId == layout.id {
                                    activeLayoutId = nil
                                }
                                layoutManager.deleteLayout(layout)
                            }
                        }
                    }

                    Button {
                        newLayoutName = "New Layout"
                        showingNewLayoutAlert = true
                    } label: {
                        Label("New Layout", systemImage: "plus")
                    }
                }

                // None option
                Section {
                    Button {
                        activeLayoutId = nil
                        dismiss()
                    } label: {
                        HStack {
                            Text("None - No Touch Controls")
                            Spacer()
                            if activeLayoutId == nil {
                                Image(systemName: "checkmark")
                                    .foregroundColor(.blue)
                            }
                        }
                    }
                }
            }
            .navigationTitle("Touch Controls")
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button("Done") { dismiss() }
                }
            }
            .alert("New Layout", isPresented: $showingNewLayoutAlert) {
                TextField("Layout Name", text: $newLayoutName)
                Button("Create") {
                    let layout = layoutManager.addLayout(name: newLayoutName)
                    activeLayoutId = layout.id
                    editingLayout = layout
                }
                Button("Cancel", role: .cancel) {}
            }
            .alert("Rename Layout", isPresented: $showingRenameAlert) {
                TextField("Name", text: $newLayoutName)
                Button("Save") {
                    if var layout = layoutToRename {
                        layout.name = newLayoutName
                        layoutManager.updateLayout(layout)
                    }
                }
                Button("Cancel", role: .cancel) {}
            }
            .sheet(item: $editingLayout) { layout in
                LayoutDetailEditor(
                    layout: layout,
                    layoutManager: layoutManager
                )
            }
        }
    }
}

// MARK: - Layout Detail Editor

struct LayoutDetailEditor: View {
    @State var layout: TouchControlLayout
    @ObservedObject var layoutManager: TouchLayoutManager
    @Environment(\.dismiss) private var dismiss

    @State private var showingAddControl = false

    var body: some View {
        NavigationView {
            Form {
                // Name
                Section("Layout") {
                    TextField("Name", text: $layout.name)
                }

                // Controls list
                Section("Controls") {
                    ForEach(layout.controls.indices, id: \.self) { idx in
                        NavigationLink {
                            ControlDetailEditor(control: $layout.controls[idx])
                        } label: {
                            controlSummary(layout.controls[idx])
                        }
                    }
                    .onDelete { offsets in
                        layout.controls.remove(atOffsets: offsets)
                    }

                    Button {
                        showingAddControl = true
                    } label: {
                        Label("Add Control", systemImage: "plus")
                    }
                }

                // Preview
                Section("Preview") {
                    ZStack {
                        RoundedRectangle(cornerRadius: 8)
                            .fill(Color.gray.opacity(0.3))
                            .aspectRatio(4.0/3.0, contentMode: .fit)

                        GeometryReader { geo in
                            ForEach(layout.controls) { control in
                                previewControl(control)
                                    .position(
                                        x: control.position.x * geo.size.width,
                                        y: control.position.y * geo.size.height
                                    )
                            }
                        }
                    }
                    .frame(height: 200)
                }
            }
            .navigationTitle(layout.name)
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button("Save") {
                        layoutManager.updateLayout(layout)
                        dismiss()
                    }
                }
                ToolbarItem(placement: .cancellationAction) {
                    Button("Cancel") { dismiss() }
                }
            }
            .sheet(isPresented: $showingAddControl) {
                AddControlSheet { newControl in
                    layout.controls.append(newControl)
                }
            }
        }
    }

    private func controlSummary(_ control: TouchControl) -> some View {
        VStack(alignment: .leading, spacing: 2) {
            HStack {
                Image(systemName: iconForType(control.type))
                Text(typeLabel(control.type))
                    .font(.body)
            }
            Text(bindingSummary(control))
                .font(.caption)
                .foregroundColor(.secondary)
            Text(String(format: "Position: %.0f%%, %.0f%%",
                        control.position.x * 100, control.position.y * 100))
                .font(.caption2)
                .foregroundColor(.secondary)
        }
    }

    @ViewBuilder
    private func previewControl(_ control: TouchControl) -> some View {
        let s = control.size * 0.4  // scale down for preview
        switch control.type {
        case .dpad:
            Circle()
                .stroke(Color.blue, lineWidth: 1.5)
                .frame(width: s, height: s)
                .overlay(Text("D").font(.caption2).foregroundColor(.blue))
        case .stick:
            Circle()
                .stroke(Color.green, lineWidth: 1.5)
                .frame(width: s, height: s)
                .overlay(Text("S").font(.caption2).foregroundColor(.green))
        case .button:
            let label = control.bindings[TouchControl.buttonAction]?.label ?? "?"
            Circle()
                .fill(Color.orange.opacity(0.3))
                .frame(width: s * 0.7, height: s * 0.7)
                .overlay(Text(label).font(.system(size: 8)).foregroundColor(.orange))
        case .buttonRow:
            let count = control.bindings.count
            RoundedRectangle(cornerRadius: 4)
                .stroke(Color.purple, lineWidth: 1)
                .frame(width: s * CGFloat(count) * 0.6, height: s * 0.5)
                .overlay(Text("\(count)btns").font(.system(size: 7)).foregroundColor(.purple))
        }
    }

    private func iconForType(_ type: TouchControlType) -> String {
        switch type {
        case .dpad: return "dpad"
        case .stick: return "circle.circle"
        case .button: return "circle.fill"
        case .buttonRow: return "rectangle.split.3x1"
        }
    }

    private func typeLabel(_ type: TouchControlType) -> String {
        switch type {
        case .dpad: return "D-Pad"
        case .stick: return "Look Stick"
        case .button: return "Button"
        case .buttonRow: return "Button Row"
        }
    }

    private func bindingSummary(_ control: TouchControl) -> String {
        switch control.type {
        case .dpad:
            return "Arrows: " + [TouchControl.dpadUp, TouchControl.dpadDown,
                                  TouchControl.dpadLeft, TouchControl.dpadRight]
                .compactMap { control.bindings[$0]?.label }
                .joined(separator: ", ")
        case .stick:
            let tap = control.bindings[TouchControl.stickTapAction]?.label ?? "none"
            return "Tap: \(tap)"
        case .button:
            return control.bindings[TouchControl.buttonAction]?.label ?? "unbound"
        case .buttonRow:
            return [TouchControl.buttonRow0, TouchControl.buttonRow1,
                    TouchControl.buttonRow2, TouchControl.buttonRow3,
                    TouchControl.buttonRow4]
                .compactMap { control.bindings[$0]?.label }
                .joined(separator: ", ")
        }
    }
}

// MARK: - Control Detail Editor

struct ControlDetailEditor: View {
    @Binding var control: TouchControl

    var body: some View {
        Form {
            Section("Type") {
                Picker("Control Type", selection: $control.type) {
                    ForEach(TouchControlType.allCases, id: \.self) { type in
                        Text(typeLabel(type)).tag(type)
                    }
                }
            }

            Section("Position & Size") {
                HStack {
                    Text("X")
                    Slider(value: $control.position.x, in: 0...1)
                    Text(String(format: "%.0f%%", control.position.x * 100))
                        .frame(width: 44)
                }
                HStack {
                    Text("Y")
                    Slider(value: $control.position.y, in: 0...1)
                    Text(String(format: "%.0f%%", control.position.y * 100))
                        .frame(width: 44)
                }
                HStack {
                    Text("Size")
                    Slider(value: $control.size, in: 30...200)
                    Text(String(format: "%.0f", control.size))
                        .frame(width: 44)
                }
                HStack {
                    Text("Opacity")
                    Slider(value: $control.opacity, in: 0.1...0.8)
                    Text(String(format: "%.0f%%", control.opacity * 100))
                        .frame(width: 44)
                }
            }

            // Key bindings section varies by type
            switch control.type {
            case .dpad:
                dpadBindings
            case .stick:
                stickBindings
            case .button:
                buttonBindings
            case .buttonRow:
                buttonRowBindings
            }
        }
        .navigationTitle(typeLabel(control.type))
    }

    private var dpadBindings: some View {
        Section("Direction Keys") {
            keyPicker("Up", bindingKey: TouchControl.dpadUp)
            keyPicker("Down", bindingKey: TouchControl.dpadDown)
            keyPicker("Left", bindingKey: TouchControl.dpadLeft)
            keyPicker("Right", bindingKey: TouchControl.dpadRight)
        }
    }

    private var stickBindings: some View {
        Section("Stick Settings") {
            keyPicker("Tap Action", bindingKey: TouchControl.stickTapAction)
        }
    }

    private var buttonBindings: some View {
        Section("Button Key") {
            keyPicker("Key", bindingKey: TouchControl.buttonAction)
        }
    }

    private var buttonRowBindings: some View {
        Section("Button Row Keys") {
            keyPicker("Button 1", bindingKey: TouchControl.buttonRow0)
            keyPicker("Button 2", bindingKey: TouchControl.buttonRow1)
            keyPicker("Button 3", bindingKey: TouchControl.buttonRow2)
            keyPicker("Button 4", bindingKey: TouchControl.buttonRow3)
            keyPicker("Button 5", bindingKey: TouchControl.buttonRow4)
        }
    }

    private func keyPicker(_ label: String, bindingKey: String) -> some View {
        let current = control.bindings[bindingKey]
        return Picker(label, selection: Binding(
            get: { current?.scancode ?? 0 },
            set: { newScancode in
                if let key = KeyBinding.allKeys.first(where: { $0.scancode == newScancode }) {
                    control.bindings[bindingKey] = key
                }
            }
        )) {
            Text("None").tag(UInt8(0))
            ForEach(KeyBinding.allKeys, id: \.scancode) { key in
                Text("\(key.label)").tag(key.scancode)
            }
        }
    }

    private func typeLabel(_ type: TouchControlType) -> String {
        switch type {
        case .dpad: return "D-Pad"
        case .stick: return "Look Stick"
        case .button: return "Button"
        case .buttonRow: return "Button Row"
        }
    }
}

// MARK: - Add Control Sheet

struct AddControlSheet: View {
    let onAdd: (TouchControl) -> Void
    @Environment(\.dismiss) private var dismiss

    @State private var selectedType: TouchControlType = .button

    var body: some View {
        NavigationView {
            Form {
                Section("Control Type") {
                    Picker("Type", selection: $selectedType) {
                        Text("D-Pad").tag(TouchControlType.dpad)
                        Text("Look Stick").tag(TouchControlType.stick)
                        Text("Button").tag(TouchControlType.button)
                        Text("Button Row").tag(TouchControlType.buttonRow)
                    }
                    .pickerStyle(.inline)
                }

                Section {
                    Text(typeDescription)
                        .font(.caption)
                        .foregroundColor(.secondary)
                }
            }
            .navigationTitle("Add Control")
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button("Add") {
                        onAdd(defaultControl(for: selectedType))
                        dismiss()
                    }
                }
                ToolbarItem(placement: .cancellationAction) {
                    Button("Cancel") { dismiss() }
                }
            }
        }
    }

    private var typeDescription: String {
        switch selectedType {
        case .dpad:
            return "8-directional movement pad. Drag in any direction to send arrow key presses. Supports diagonals."
        case .stick:
            return "Analog stick that sends relative mouse motion. Good for looking/aiming. Quick tap can trigger an action (e.g., fire)."
        case .button:
            return "Single action button. Press and hold to send a key press, release to send key release."
        case .buttonRow:
            return "Row of small buttons (e.g., weapon keys 1-5). Each button sends its own key."
        }
    }

    private func defaultControl(for type: TouchControlType) -> TouchControl {
        switch type {
        case .dpad:
            return TouchControl(
                type: .dpad,
                position: ControlPosition(x: 0.15, y: 0.5),
                size: 140,
                bindings: [
                    TouchControl.dpadUp:    .upArrow,
                    TouchControl.dpadDown:  .downArrow,
                    TouchControl.dpadLeft:  .leftArrow,
                    TouchControl.dpadRight: .rightArrow,
                ]
            )
        case .stick:
            return TouchControl(
                type: .stick,
                position: ControlPosition(x: 0.85, y: 0.5),
                size: 140,
                bindings: [
                    TouchControl.stickTapAction: .ctrl,
                ]
            )
        case .button:
            return TouchControl(
                type: .button,
                position: ControlPosition(x: 0.5, y: 0.85),
                size: 60,
                bindings: [TouchControl.buttonAction: .space]
            )
        case .buttonRow:
            return TouchControl(
                type: .buttonRow,
                position: ControlPosition(x: 0.5, y: 0.06),
                size: 36,
                bindings: [
                    TouchControl.buttonRow0: .key1,
                    TouchControl.buttonRow1: .key2,
                    TouchControl.buttonRow2: .key3,
                ]
            )
        }
    }
}
