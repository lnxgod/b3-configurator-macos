import AppKit
import Foundation
import CryptoKit
import Darwin

struct B3Error: LocalizedError {
    let message: String
    init(_ message: String) { self.message = message }
    var errorDescription: String? { message }
}

let supportedSchema = "5819ff8235a6d18029d414ec778a7f5c"

enum NameInputLimit {
    // The device display was manually verified with "GameChangersAIh1".
    // Its storage accepts longer names, but the display does not.
    static let maximumBytes = 16

    static func prefix(_ text: String, fitting budget: Int) -> String {
        var result = "", bytes = 0
        for character in text {
            let size = String(character).utf8.count
            guard bytes + size <= budget else { break }
            result.append(character); bytes += size
        }
        return result
    }

    // Limit the edited portion so inserting at capacity does not erase the
    // existing suffix. Whole Characters keep emoji and combining marks intact.
    static func edit(_ candidate: String, previous: String) -> (text: String, removed: NSRange?) {
        guard candidate.utf8.count > maximumBytes else { return (candidate, nil) }
        let old = Array(previous), new = Array(candidate)
        var start = 0, end = 0
        func identical(_ a: Character, _ b: Character) -> Bool {
            String(a).utf8.elementsEqual(String(b).utf8)
        }
        while start < min(old.count, new.count), identical(old[start], new[start]) { start += 1 }
        while end < min(old.count - start, new.count - start),
              identical(old[old.count - end - 1], new[new.count - end - 1]) { end += 1 }
        let before = String(new.prefix(start)), after = String(new.suffix(end))
        let middle = String(new[start..<(new.count - end)])
        var retained = prefix(middle, fitting: maximumBytes - before.utf8.count - after.utf8.count)
        let oldMiddle = String(old[start..<(old.count - end)])
        // Adding a combining mark or emoji modifier must not erase a base
        // character already present when the larger cluster cannot fit.
        if retained.isEmpty, !oldMiddle.isEmpty, middle.utf8.starts(with: oldMiddle.utf8) {
            retained = oldMiddle
        }
        let result = before + retained + after
        return (result, NSRange(location: (before + retained).utf16.count,
                                length: middle.utf16.count - retained.utf16.count))
    }

    static func selection(_ range: NSRange, afterRemoving removed: NSRange?, length: Int) -> NSRange {
        func adjusted(_ offset: Int) -> Int {
            guard let removed = removed else { return min(offset, length) }
            if offset <= removed.location { return min(offset, length) }
            return min(max(removed.location, offset - removed.length), length)
        }
        let start = adjusted(range.location), end = adjusted(NSMaxRange(range))
        return NSRange(location: start, length: max(0, end - start))
    }
}

func capture(_ pattern: String, _ text: String) -> String? {
    guard let expression = try? NSRegularExpression(pattern: pattern, options: .anchorsMatchLines),
          let match = expression.firstMatch(in: text, range: NSRange(text.startIndex..., in: text)),
          let range = Range(match.range(at: 1), in: text) else { return nil }
    return String(text[range])
}

func sha256(_ data: Data) -> String {
    SHA256.hash(data: data).map { String(format: "%02x", $0) }.joined()
}

func privateDirectory(_ url: URL) throws {
    try FileManager.default.createDirectory(at: url, withIntermediateDirectories: true,
                                          attributes: [.posixPermissions: 0o700])
    let attributes = try FileManager.default.attributesOfItem(atPath: url.path)
    guard attributes[.type] as? FileAttributeType == .typeDirectory else {
        throw B3Error("The backup location is not a regular directory.")
    }
    try FileManager.default.setAttributes([.posixPermissions: 0o700], ofItemAtPath: url.path)
}

func privateWrite(_ data: Data, to url: URL, replace: Bool = false) throws {
    let flags = O_WRONLY | O_CREAT | O_NOFOLLOW | (replace ? O_TRUNC : O_EXCL)
    let descriptor = open(url.path, flags, mode_t(0o600))
    guard descriptor >= 0 else { throw B3Error("Could not create a private backup file: \(url.lastPathComponent).") }
    defer { close(descriptor) }
    guard fchmod(descriptor, mode_t(0o600)) == 0 else { throw B3Error("Could not protect the backup file.") }
    try data.withUnsafeBytes { bytes in
        var offset = 0
        while offset < bytes.count {
            let count = Darwin.write(descriptor, bytes.baseAddress!.advanced(by: offset), bytes.count - offset)
            if count < 0 && errno == EINTR { continue }
            guard count > 0 else { throw B3Error("Could not finish writing the backup file.") }
            offset += count
        }
    }
    guard fsync(descriptor) == 0 else { throw B3Error("Could not flush the backup file to disk.") }
}

struct CommandResult { let status: Int32; let output: String }
protocol CommandRunning {
    func run(_ helper: String, _ arguments: [String], timeout: TimeInterval) throws -> CommandResult
}

final class ProcessRunner: CommandRunning {
    let helpers: URL
    let directory: URL
    init(helpers: URL, directory: URL) { self.helpers = helpers; self.directory = directory }

    func run(_ helper: String, _ arguments: [String], timeout: TimeInterval = 40) throws -> CommandResult {
        let process = Process()
        process.executableURL = helpers.appendingPathComponent(helper)
        process.arguments = arguments
        process.currentDirectoryURL = directory
        let pipe = Pipe()
        process.standardOutput = pipe
        process.standardError = pipe
        let terminated = DispatchSemaphore(value: 0)
        process.terminationHandler = { _ in terminated.signal() }
        try process.run()
        // Drain concurrently so even an unusually verbose native failure cannot
        // fill the pipe and deadlock the operation. Never display raw USB data.
        let readComplete = DispatchSemaphore(value: 0)
        let outputLock = NSLock()
        var output = Data()
        DispatchQueue.global(qos: .utility).async {
            while true {
                let chunk = pipe.fileHandleForReading.availableData
                if chunk.isEmpty { break }
                outputLock.lock()
                if output.count < 262_144 { output.append(chunk.prefix(262_144 - output.count)) }
                outputLock.unlock()
            }
            readComplete.signal()
        }
        let timedOut = terminated.wait(timeout: .now() + timeout) == .timedOut
        if timedOut {
            process.terminate()
            if terminated.wait(timeout: .now() + 2) == .timedOut {
                kill(process.processIdentifier, SIGKILL)
                _ = terminated.wait(timeout: .now() + 5)
            }
        }
        _ = readComplete.wait(timeout: .now() + 2)
        outputLock.lock()
        let text = String(data: output, encoding: .utf8) ?? ""
        outputLock.unlock()
        if timedOut { throw B3Error("A USB operation timed out. No persistent write was retried.") }
        return CommandResult(status: process.terminationStatus, output: text)
    }
}

struct DeviceState {
    let registry: String
    let mode: Int
    static func parse(_ output: String) throws -> DeviceState {
        guard let registry = capture("^REGISTRY_ID=([0-9]+)$", output),
              let modeText = capture("^MODE=([0-9]+)$", output), let mode = Int(modeText),
              capture("^AHI protocol version: ([0-9]+\\.[0-9]+)$", output) == "0.4",
              capture("^Configuration signature type=1 value=([0-9a-f]{32})$", output) == supportedSchema,
              [1, 2].contains(mode) else {
            throw B3Error("The connected device does not match the supported B3 protocol and configuration layout.")
        }
        return DeviceState(registry: registry, mode: mode)
    }
}

final class Coordinator {
    let runner: CommandRunning
    let directory: URL
    let progress: (String) -> Void
    var pendingMode: (Int, String)?
    var pause: (TimeInterval) -> Void = { Thread.sleep(forTimeInterval: $0) }
    var journal: [String: Any] = ["schema": supportedSchema, "verified_after_reboot": false,
                                 "normal_mode_restored": false, "write_attempted": false]
    var nameJournal: [String: Any] = ["write_attempted": false, "verified_after_reboot": false,
                                     "debug_reset_verified": false]

    init(runner: CommandRunning, directory: URL, progress: @escaping (String) -> Void = { _ in }) {
        self.runner = runner; self.directory = directory; self.progress = progress
    }
    func command(_ helper: String, _ arguments: [String], timeout: TimeInterval = 40,
                 checked: Bool = true) throws -> CommandResult {
        let result = try runner.run(helper, arguments, timeout: timeout)
        if checked && result.status != 0 {
            throw B3Error("\(helper == "b3_image" ? "Configuration validation" : "USB operation") \(arguments.first ?? "") failed. Check that exactly one B3 is connected using a data cable. Any private backups have been preserved.")
        }
        return result
    }
    func save(_ value: [String: Any], _ filename: String) throws {
        // Replace by atomic rename so interruption cannot truncate a journal.
        let temporary = directory.appendingPathComponent(".journal-\(UUID().uuidString)")
        try privateWrite(try JSONSerialization.data(withJSONObject: value, options: [.prettyPrinted, .sortedKeys]), to: temporary)
        let target = directory.appendingPathComponent(filename)
        guard Darwin.rename(temporary.path, target.path) == 0 else { throw B3Error("Could not save the operation journal.") }
    }
    func state() throws -> DeviceState {
        try DeviceState.parse(command("probe", ["--info"]).output)
    }
    func waitFor(mode: Int? = nil, previous: String? = nil) throws -> DeviceState {
        let deadline = Date().addingTimeInterval(35)
        while Date() < deadline {
            if let current = try? state(), (mode == nil || current.mode == mode),
               (previous == nil || current.registry != previous) { return current }
            pause(0.75)
        }
        throw B3Error("The B3 did not reconnect in the expected mode. Keep its backup and reconnect the USB cable before recovery.")
    }
    func setMode(_ mode: Int) throws {
        guard [1, 2].contains(mode) else { throw B3Error("Unsupported operating mode.") }
        if let pending = pendingMode {
            _ = try waitFor(mode: pending.0, previous: pending.1)
            pendingMode = nil
        }
        let current = try waitFor()
        if current.mode == mode { return }
        pendingMode = (mode, current.registry)
        // A mode command can succeed even if USB disappears before its reply.
        // Wait for a fresh device instance instead of ever resending it.
        _ = try? command("probe", ["--mode", String(mode)])
        _ = try waitFor(mode: mode, previous: current.registry)
        pendingMode = nil
    }
    static func validatePin(_ pin: String) throws {
        guard pin.utf8.count == 4, pin.utf8.allSatisfy({ $0 >= 48 && $0 <= 57 }) else {
            throw B3Error("Enter exactly four digits for the PIN. Leading zeros are allowed.")
        }
    }
    static func patchPin(_ before: Data, pin: String) throws -> Data {
        try validatePin(pin)
        guard before.count == 50 else { throw B3Error("The PIN configuration must contain exactly 50 bytes.") }
        var after = before
        after.replaceSubrange(36..<40, with: pin.utf8)
        after[48] |= 0x08
        return after
    }
    static func restorePin(_ current: Data, backup: Data) throws -> Data {
        guard current.count == 50, backup.count == 50,
              let pin = String(data: backup[36..<40], encoding: .ascii) else { throw B3Error("Invalid PIN backup.") }
        try validatePin(pin)
        var result = current
        result.replaceSubrange(36..<40, with: backup[36..<40])
        result[48] = (current[48] & ~0x08) | (backup[48] & 0x08)
        return result
    }
    func readBlock() throws -> Data {
        let file = directory.appendingPathComponent("block-133.bin")
        try privateWrite(Data(), to: file, replace: true)
        _ = try command("probe", ["--block", "133"])
        let data = try Data(contentsOf: file)
        guard data.count == 50 else { throw B3Error("Unsupported PIN configuration size.") }
        return data
    }
    func applyPin(transform: (Data) throws -> Data) throws {
        _ = try state()
        var failure: Error?
        var backupSaved = false
        do {
            progress("Switching to PIN configuration mode…")
            try setMode(2)
            let before = try readBlock(), after = try transform(before)
            let beforeURL = directory.appendingPathComponent("before.bin")
            let afterURL = directory.appendingPathComponent("after.bin")
            try privateWrite(before, to: beforeURL)
            backupSaved = true
            try privateWrite(after, to: afterURL)
            journal["before_sha256"] = sha256(before)
            journal["after_sha256"] = sha256(after)
            try save(journal, "result.json")
            if before != after {
                journal["write_attempted"] = true
                try save(journal, "result.json")
                progress("Saving the PIN settings once…")
                let result = try command("probe", ["--write-pin-block", beforeURL.path, afterURL.path])
                guard result.output.contains("PIN_WRITE_VERIFIED=1") || result.output.contains("PIN_WRITE=UNCHANGED VERIFIED=1") else {
                    throw B3Error("The B3 did not verify the PIN write. No retry was performed.")
                }
            }
            progress("Rebooting and verifying the stored PIN settings…")
            try setMode(1)
            try setMode(2)
            guard try readBlock() == after else { throw B3Error("PIN settings did not match after reboot.") }
            journal["verified_after_reboot"] = true
        } catch { failure = error }
        do {
            try setMode(1)
            journal["normal_mode_restored"] = true
        } catch {
            failure = B3Error("\(failure?.localizedDescription ?? "PIN operation stopped.") Normal-mode recovery was not verified. Reconnect the B3 and use Return to normal mode.")
        }
        if let failure = failure { journal["error"] = failure.localizedDescription }
        if backupSaved { try save(journal, "result.json") }
        if let failure = failure { throw failure }
        progress("PIN settings verified after reboot. Normal operation restored.")
    }
    func setPin(_ pin: String) throws {
        try Self.validatePin(pin)
        try applyPin { try Self.patchPin($0, pin: pin) }
    }
    func restore(_ source: URL) throws {
        let raw = try Data(contentsOf: source.appendingPathComponent("before.bin"))
        let json = try JSONSerialization.jsonObject(with: Data(contentsOf: source.appendingPathComponent("result.json"))) as? [String: Any]
        guard json?["schema"] as? String == supportedSchema, json?["before_sha256"] as? String == sha256(raw) else {
            throw B3Error("Backup checksum or device configuration version does not match.")
        }
        _ = try Self.restorePin(raw, backup: raw)
        try applyPin { try Self.restorePin($0, backup: raw) }
    }
    func ensureDebug() throws {
        let result = try command("usb_descriptors", ["--debug"], checked: false)
        if result.status != 0 {
            guard capture("^DEBUG_PRESENT=(0)\\s*$", result.output) == "0" else {
                throw B3Error("Cannot identify the B3's internal USB connection. Check the cable and connect only one B3.")
            }
            _ = try command("debug_unlock", ["--unlock-default"])
            let deadline = Date().addingTimeInterval(8)
            var ready = false
            while Date() < deadline {
                if try command("usb_descriptors", ["--debug"], checked: false).status == 0 { ready = true; break }
                pause(0.25)
            }
            guard ready else { throw B3Error("The B3 configuration interface did not appear.") }
        }
        let reply = try command("usb_descriptors", ["--chip-id"]).output
        guard capture("^(03 00 11 20 03 00 00 00 49 20)\\s*$", reply) != nil else {
            throw B3Error("The B3 chip identifier does not match the supported hardware.")
        }
    }
    func reboot() throws {
        let previous = try state()
        _ = try? command("usb_descriptors", ["--reboot"])
        _ = try waitFor(mode: 1, previous: previous.registry)
    }
    func readImage(_ filename: String) throws -> URL {
        let url = directory.appendingPathComponent(filename)
        _ = try command("usb_descriptors", ["--read-config", url.path])
        _ = try command("b3_image", ["--read-name", url.path])
        return url
    }
    static func validateName(_ name: String) throws {
        guard !name.isEmpty, name.utf8.count <= NameInputLimit.maximumBytes,
              !name.unicodeScalars.contains(where: { $0.properties.generalCategory == .control }) else {
            throw B3Error("Use a name between 1 and \(NameInputLimit.maximumBytes) UTF-8 bytes, without control characters.")
        }
    }
    func setName(_ name: String) throws {
        try Self.validateName(name)
        _ = try state()
        try setMode(1)
        nameJournal["requested_name"] = name
        try save(nameJournal, "name-result.json")
        do {
            progress("Opening the B3 name configuration interface…")
            try ensureDebug()
            let before = try readImage("name-before.bin")
            let after = directory.appendingPathComponent("name-after.bin")
            let oldName = try command("b3_image", ["--read-name", before.path]).output
            nameJournal["before_name"] = oldName.hasSuffix("\n") ? String(oldName.dropLast()) : oldName
            _ = try command("b3_image", ["--patch-name", before.path, after.path, name])
            let beforeData = try Data(contentsOf: before), afterData = try Data(contentsOf: after)
            nameJournal["before_sha256"] = sha256(beforeData)
            nameJournal["after_sha256"] = sha256(afterData)
            try save(nameJournal, "name-result.json")
            if beforeData != afterData {
                nameJournal["write_attempted"] = true
                try save(nameJournal, "name-result.json")
                progress("Saving the Bluetooth name once…")
                let result = try command("usb_descriptors", ["--write-name-image", before.path, after.path], timeout: 50)
                guard capture("^NAME_WRITE_VERIFIED=(1)\\s*$", result.output) == "1" else {
                    throw B3Error("The B3 did not verify the name write. No retry was performed.")
                }
            }
            progress("Rebooting and verifying the stored Bluetooth name…")
            try reboot()
            var verificationFailure: Error?
            do {
                try ensureDebug()
                let readback = try readImage("name-after-reboot.bin")
                guard try Data(contentsOf: readback) == afterData else { throw B3Error("The name configuration did not match after reboot.") }
                nameJournal["verified_after_reboot"] = true
            } catch { verificationFailure = error }
            // This is only reached after a verified write/no-op and first reset.
            // Do not reset or retry after an uncertain persistent write.
            progress("Closing temporary configuration access…")
            try reboot()
            let result = try command("usb_descriptors", ["--debug"], checked: false)
            let closed = result.status != 0 && capture("^DEBUG_PRESENT=(0)\\s*$", result.output) == "0"
            nameJournal["debug_reset_verified"] = closed
            guard closed else { throw B3Error("Temporary configuration access did not close. Power-cycle the B3.") }
            if let failure = verificationFailure { throw failure }
            try save(nameJournal, "name-result.json")
            progress("Bluetooth name verified after reboot. Temporary access closed.")
        } catch {
            nameJournal["error"] = error.localizedDescription
            try? save(nameJournal, "name-result.json")
            if nameJournal["debug_reset_verified"] as? Bool != true {
                throw B3Error("\(error.localizedDescription) Temporary configuration access may remain active. No write was retried. Preserve the backup before recovery.")
            }
            throw error
        }
    }
}

final class OperationLock {
    let descriptor: Int32
    init(_ directory: URL) throws {
        descriptor = open(directory.appendingPathComponent(".operation.lock").path, O_CREAT | O_RDWR | O_NOFOLLOW, mode_t(0o600))
        guard descriptor >= 0 else { throw B3Error("Could not create the operation lock.") }
        guard flock(descriptor, LOCK_EX | LOCK_NB) == 0 else {
            close(descriptor)
            throw B3Error("Another B3 Configurator operation is already running.")
        }
    }
    deinit { flock(descriptor, LOCK_UN); close(descriptor) }
}

final class AppDelegate: NSObject, NSApplicationDelegate, NSWindowDelegate, NSTextFieldDelegate {
    var window: NSWindow!
    var busy = false
    var root: URL { FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent("Library/Application Support/B3 Configurator") }
    var backups: URL { root.appendingPathComponent("Backups") }
    let status = NSTextField(wrappingLabelWithString: "Connect one blafili B3 with a USB data cable.")
    let nameToggle = NSButton(checkboxWithTitle: "Change Bluetooth name", target: nil, action: nil)
    let nameField = NSTextField(string: "CHOMP")
    let nameHint = NSTextField(labelWithString: "")
    var acceptedName = "CHOMP"
    var limitingName = false
    let pinToggle = NSButton(checkboxWithTitle: "Set and enable PIN", target: nil, action: nil)
    let pinField = NSTextField(string: "1234")
    let confirmField = NSTextField(string: "1234")
    let log = NSTextView()
    let spinner = NSProgressIndicator()
    var actionButtons: [NSButton] = []

    func applicationDidFinishLaunching(_ notification: Notification) {
        NSApp.setActivationPolicy(.regular)
        setupMenu()
        window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 700, height: 630),
                          styleMask: [.titled, .closable, .miniaturizable], backing: .buffered, defer: false)
        window.title = "B3 Configurator"
        window.delegate = self
        window.center()
        let content = NSStackView()
        content.orientation = .vertical
        content.alignment = .leading
        content.spacing = 16
        content.edgeInsets = NSEdgeInsets(top: 26, left: 30, bottom: 26, right: 30)
        content.translatesAutoresizingMaskIntoConstraints = false
        window.contentView!.addSubview(content)
        NSLayoutConstraint.activate([
            content.topAnchor.constraint(equalTo: window.contentView!.topAnchor),
            content.bottomAnchor.constraint(equalTo: window.contentView!.bottomAnchor),
            content.leadingAnchor.constraint(equalTo: window.contentView!.leadingAnchor),
            content.trailingAnchor.constraint(equalTo: window.contentView!.trailingAnchor)
        ])
        let title = NSTextField(labelWithString: "Your B3. Your name.")
        title.font = .systemFont(ofSize: 27, weight: .semibold)
        content.addArrangedSubview(title)
        let subtitle = NSTextField(wrappingLabelWithString: "Configure the Bluetooth name and PIN directly over USB.")
        subtitle.textColor = .secondaryLabelColor
        content.addArrangedSubview(subtitle)
        status.font = .systemFont(ofSize: 13, weight: .medium)
        status.setAccessibilityIdentifier("connectionStatus")
        content.addArrangedSubview(status)
        nameToggle.state = .on
        nameToggle.target = self; nameToggle.action = #selector(updateControls)
        content.addArrangedSubview(nameToggle)
        nameField.placeholderString = "Bluetooth name"
        nameField.setAccessibilityLabel("Bluetooth name")
        nameField.setAccessibilityIdentifier("bluetoothName")
        nameField.font = .systemFont(ofSize: 17)
        nameField.delegate = self
        nameField.usesSingleLineMode = true
        nameField.maximumNumberOfLines = 1
        nameField.cell?.isScrollable = true
        nameField.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)
        content.addArrangedSubview(nameField)
        nameField.widthAnchor.constraint(equalTo: content.widthAnchor, constant: -60).isActive = true
        nameHint.font = .systemFont(ofSize: 11); nameHint.textColor = .secondaryLabelColor
        nameHint.setAccessibilityIdentifier("bluetoothNameLength")
        content.addArrangedSubview(nameHint)
        nameHint.widthAnchor.constraint(equalTo: nameField.widthAnchor).isActive = true
        updateNameHint()
        pinToggle.target = self; pinToggle.action = #selector(updateControls)
        content.addArrangedSubview(pinToggle)
        let pinRow = NSStackView(views: [pinField, confirmField])
        pinRow.orientation = .horizontal; pinRow.distribution = .fillEqually; pinRow.spacing = 12
        pinField.placeholderString = "Four-digit PIN"; pinField.setAccessibilityLabel("New PIN")
        confirmField.placeholderString = "Confirm PIN"; confirmField.setAccessibilityLabel("Confirm PIN")
        for field in [pinField, confirmField] {
            field.usesSingleLineMode = true
            field.maximumNumberOfLines = 1
            field.cell?.isScrollable = true
            field.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)
        }
        content.addArrangedSubview(pinRow)
        pinRow.widthAnchor.constraint(equalTo: nameField.widthAnchor).isActive = true
        let note = NSTextField(wrappingLabelWithString: "Keep USB connected until verification finishes. The B3 restarts and audio pauses. Name and PIN are saved separately; a later PIN failure does not undo a rename.")
        note.font = .systemFont(ofSize: 12); note.textColor = .secondaryLabelColor
        content.addArrangedSubview(note)
        note.widthAnchor.constraint(equalTo: nameField.widthAnchor).isActive = true
        let check = button("Check connection", #selector(checkConnection))
        let apply = button("Apply selected changes", #selector(applyChanges))
        apply.keyEquivalent = "\r"
        spinner.style = .spinning; spinner.controlSize = .small; spinner.isDisplayedWhenStopped = false
        let actions = NSStackView(views: [check, apply, spinner]); actions.spacing = 12
        content.addArrangedSubview(actions)
        log.isEditable = false; log.isSelectable = true; log.font = .monospacedSystemFont(ofSize: 11, weight: .regular)
        log.textColor = .labelColor; log.backgroundColor = .textBackgroundColor
        log.textContainerInset = NSSize(width: 10, height: 8)
        log.setAccessibilityLabel("Operation progress")
        let scroll = NSScrollView()
        scroll.documentView = log; scroll.hasVerticalScroller = true; scroll.borderType = .bezelBorder
        log.autoresizingMask = [.width]
        log.textContainer?.widthTracksTextView = true
        content.addArrangedSubview(scroll)
        scroll.widthAnchor.constraint(equalTo: nameField.widthAnchor).isActive = true
        scroll.heightAnchor.constraint(greaterThanOrEqualToConstant: 100).isActive = true
        let recovery = button("Return to normal mode", #selector(normalMode))
        let restore = button("Restore PIN…", #selector(restorePin))
        let show = button("Show backups", #selector(showBackups))
        let footer = NSStackView(views: [recovery, restore, show]); footer.spacing = 10
        content.addArrangedSubview(footer)
        actionButtons = [check, apply, recovery, restore]
        updateControls()
        append("Ready. No USB operation starts until you choose an action.")
        window.makeKeyAndOrderFront(nil)
        NSApp.activate(ignoringOtherApps: true)
    }
    func button(_ title: String, _ action: Selector) -> NSButton {
        let button = NSButton(title: title, target: self, action: action)
        button.bezelStyle = .rounded
        return button
    }
    func setupMenu() {
        let menu = NSMenu()
        let item = NSMenuItem(); menu.addItem(item)
        let applicationMenu = NSMenu()
        applicationMenu.addItem(withTitle: "Quit B3 Configurator", action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q")
        item.submenu = applicationMenu
        let editItem = NSMenuItem(); menu.addItem(editItem)
        let edit = NSMenu(title: "Edit")
        edit.addItem(withTitle: "Cut", action: #selector(NSText.cut(_:)), keyEquivalent: "x")
        edit.addItem(withTitle: "Copy", action: #selector(NSText.copy(_:)), keyEquivalent: "c")
        edit.addItem(withTitle: "Paste", action: #selector(NSText.paste(_:)), keyEquivalent: "v")
        edit.addItem(withTitle: "Select All", action: #selector(NSText.selectAll(_:)), keyEquivalent: "a")
        editItem.submenu = edit
        NSApp.mainMenu = menu
    }
    @objc func updateControls() {
        nameToggle.isEnabled = !busy; pinToggle.isEnabled = !busy
        nameField.isEnabled = !busy && nameToggle.state == .on
        pinField.isEnabled = !busy && pinToggle.state == .on
        confirmField.isEnabled = pinField.isEnabled
        actionButtons.forEach { $0.isEnabled = !busy }
    }
    func updateNameHint(limited: Bool = false, composing: Bool = false) {
        let bytes = nameField.stringValue.utf8.count
        let detail = composing ? "Finish typing to apply the limit." :
            (limited ? "Limit reached; extra text was not added." : "Up to \(NameInputLimit.maximumBytes) basic characters; emoji use more space.")
        nameHint.stringValue = "\(bytes) / \(NameInputLimit.maximumBytes) bytes · \(detail)"
    }
    func limitNameInput() {
        guard !limitingName else { return }
        let editor = nameField.currentEditor() as? NSTextView
        if editor?.hasMarkedText() == true {
            updateNameHint(composing: true)
            return
        }
        limitingName = true
        defer { limitingName = false }
        let candidate = editor?.string ?? nameField.stringValue
        let result = NameInputLimit.edit(candidate, previous: acceptedName)
        let selected = editor?.selectedRange()
        if result.removed != nil {
            nameField.stringValue = result.text
            if let editor = editor, let selected = selected {
                editor.string = result.text
                editor.setSelectedRange(NameInputLimit.selection(selected, afterRemoving: result.removed,
                                                                  length: result.text.utf16.count))
            }
        }
        acceptedName = result.text
        updateNameHint(limited: result.removed != nil)
    }
    func controlTextDidChange(_ notification: Notification) {
        if notification.object as? NSTextField === nameField { limitNameInput() }
    }
    func controlTextDidEndEditing(_ notification: Notification) {
        if notification.object as? NSTextField === nameField { limitNameInput() }
    }
    func append(_ text: String) {
        log.textStorage?.append(NSAttributedString(string: text + "\n", attributes: [
            .font: NSFont.monospacedSystemFont(ofSize: 11, weight: .regular),
            .foregroundColor: NSColor.labelColor
        ]))
        log.scrollToEndOfDocument(nil)
    }
    func present(_ error: Error) {
        let alert = NSAlert()
        alert.messageText = "Could not complete the operation"
        alert.informativeText = error.localizedDescription
        alert.alertStyle = .warning
        alert.beginSheetModal(for: window)
    }
    func perform(_ title: String, _ operation: @escaping (Coordinator) throws -> String) {
        guard !busy else { return }
        busy = true; updateControls(); spinner.startAnimation(nil)
        status.stringValue = title
        append(title)
        let root = self.root, backups = self.backups
        let helpers = Bundle.main.bundleURL.appendingPathComponent("Contents/Helpers")
        DispatchQueue.global(qos: .userInitiated).async {
            var session: URL?
            let outcome: Result<String, Error>
            do {
                try privateDirectory(root)
                let operationLock = try OperationLock(root)
                defer { withExtendedLifetime(operationLock) {} }
                try privateDirectory(backups)
                let date = ISO8601DateFormatter().string(from: Date()).replacingOccurrences(of: ":", with: "-")
                let directory = backups.appendingPathComponent("\(date)-\(UUID().uuidString.prefix(8))")
                try privateDirectory(directory); session = directory
                let coordinator = Coordinator(runner: ProcessRunner(helpers: helpers, directory: directory), directory: directory) { text in
                    DispatchQueue.main.async { self.status.stringValue = text; self.append(text) }
                }
                outcome = .success(try operation(coordinator))
            } catch { outcome = .failure(error) }
            DispatchQueue.main.async {
                self.busy = false; self.spinner.stopAnimation(nil); self.updateControls()
                switch outcome {
                case .success(let text): self.status.stringValue = text; self.append(text)
                case .failure(let error):
                    self.status.stringValue = "Stopped. Review the message before retrying."
                    self.append(error.localizedDescription); self.present(error)
                }
                if let session = session { self.append("Session: \(session.path)") }
            }
        }
    }
    @objc func checkConnection() {
        perform("Checking the USB connection…") { coordinator in
            let state = try coordinator.state()
            return "B3 connected • AHI 0.4 • \(state.mode == 1 ? "Normal mode" : "Configuration mode")"
        }
    }
    @objc func applyChanges() {
        guard window.makeFirstResponder(nil) else { return }
        let name = nameToggle.state == .on ? nameField.stringValue : nil
        let pin = pinToggle.state == .on ? pinField.stringValue : nil
        do {
            guard name != nil || pin != nil else { throw B3Error("Select a name change or a PIN change first.") }
            if let name = name { try Coordinator.validateName(name) }
            if let pin = pin {
                try Coordinator.validatePin(pin)
                guard pin == confirmField.stringValue else { throw B3Error("The two PIN entries do not match.") }
            }
        } catch { present(error); return }
        perform("Preparing the selected changes…") { coordinator in
            if let name = name { try coordinator.setName(name) }
            if let pin = pin {
                do { try coordinator.setPin(pin) }
                catch {
                    if name != nil { throw B3Error("The name was changed and verified, but the PIN operation failed. \(error.localizedDescription)") }
                    throw error
                }
            }
            return "Selected settings verified after reboot. B3 ready."
        }
    }
    @objc func normalMode() {
        perform("Returning the B3 to normal operation…") { coordinator in
            _ = try coordinator.state()
            try coordinator.setMode(1)
            return "Normal mode verified. If temporary name access remains active, power-cycle the B3."
        }
    }
    @objc func restorePin() {
        let panel = NSOpenPanel()
        panel.canChooseDirectories = true; panel.canChooseFiles = false; panel.allowsMultipleSelection = false
        panel.directoryURL = backups
        panel.message = "Choose a B3 session folder containing before.bin and result.json. Only the PIN settings will be restored."
        panel.prompt = "Restore PIN"
        panel.beginSheetModal(for: window) { response in
            guard response == .OK, let directory = panel.url else { return }
            self.perform("Restoring the saved PIN settings…") { coordinator in
                try coordinator.restore(directory)
                return "Saved PIN settings verified after reboot. B3 ready."
            }
        }
    }
    @objc func showBackups() {
        do { try privateDirectory(backups); NSWorkspace.shared.open(backups) }
        catch { present(error) }
    }
    func windowShouldClose(_ sender: NSWindow) -> Bool {
        if busy { append("Wait for verification to finish before closing the app."); return false }
        return true
    }
    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }
    func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
        if busy { append("Wait for verification to finish before quitting."); return .terminateCancel }
        return .terminateNow
    }
}

// All self-tests use a fake USB transport. They never enumerate or touch hardware.
final class FakeRunner: CommandRunning {
    let directory: URL
    var mode = 1, registry = 1, pinWrites = 0, nameWrites = 0, reboots = 0
    var block = Data(repeating: 0, count: 50)
    var image = Data([1, 2, 3, 4])
    var debug = false, failPinWrite = false, failNameWrite = false, corruptPinReadback = false
    var corruptNameReadback = false, failPostRebootChipCheck = false, pinReadCount = 0
    init(_ directory: URL) { self.directory = directory; block.replaceSubrange(36..<40, with: "0000".utf8) }
    func run(_ helper: String, _ arguments: [String], timeout: TimeInterval) throws -> CommandResult {
        let operation = arguments[0]
        if helper == "probe" {
            switch operation {
            case "--info": return CommandResult(status: 0, output: "REGISTRY_ID=\(registry)\nMODE=\(mode)\nAHI protocol version: 0.4\nConfiguration signature type=1 value=\(supportedSchema)\n")
            case "--mode": mode = Int(arguments[1])!; registry += 1
            case "--block":
                pinReadCount += 1
                var observed = block
                if corruptPinReadback && pinReadCount > 1 { observed[0] ^= 1 }
                try observed.write(to: directory.appendingPathComponent("block-133.bin"))
            case "--write-pin-block":
                pinWrites += 1
                if failPinWrite { return CommandResult(status: 4, output: "uncertain") }
                guard try Data(contentsOf: URL(fileURLWithPath: arguments[1])) == block else { throw B3Error("Test baseline mismatch") }
                block = try Data(contentsOf: URL(fileURLWithPath: arguments[2]))
                return CommandResult(status: 0, output: "PIN_WRITE_VERIFIED=1\n")
            default: throw B3Error("Unexpected fake PIN command")
            }
        } else if helper == "debug_unlock" { debug = true }
        else if helper == "b3_image" {
            if operation == "--read-name" { return CommandResult(status: 0, output: "CHOMP\n") }
            if operation == "--patch-name" { try Data([1, 2, 3, 5]).write(to: URL(fileURLWithPath: arguments[2])) }
            else { throw B3Error("Unexpected fake image command") }
        } else if helper == "usb_descriptors" {
            switch operation {
            case "--debug": return CommandResult(status: debug ? 0 : 1, output: debug ? "" : "DEBUG_PRESENT=0\n")
            case "--chip-id":
                if failPostRebootChipCheck && reboots > 0 { return CommandResult(status: 1, output: "failed") }
                return CommandResult(status: 0, output: "03 00 11 20 03 00 00 00 49 20\n")
            case "--read-config":
                var observed = image
                if corruptNameReadback && reboots > 0 { observed[0] ^= 1 }
                try observed.write(to: URL(fileURLWithPath: arguments[1]))
            case "--write-name-image":
                nameWrites += 1
                if failNameWrite { return CommandResult(status: 1, output: "uncertain") }
                image = try Data(contentsOf: URL(fileURLWithPath: arguments[2]))
                return CommandResult(status: 0, output: "NAME_WRITE_VERIFIED=1\n")
            case "--reboot": reboots += 1; registry += 1; mode = 1; debug = false
            default: throw B3Error("Unexpected fake name command")
            }
        } else { throw B3Error("Unexpected fake helper") }
        return CommandResult(status: 0, output: "")
    }
}

func selfTest() throws {
    var checks = 0
    func require(_ condition: Bool, _ message: String) throws {
        guard condition else { throw B3Error("Self-test failed: \(message)") }; checks += 1
    }
    func mustFail(_ label: String, _ action: () throws -> Void) throws {
        var failed = false
        do { try action() } catch { failed = true }
        try require(failed, label)
    }
    let temporary = FileManager.default.temporaryDirectory.appendingPathComponent("b3-native-tests-\(UUID().uuidString)")
    try privateDirectory(temporary)
    defer { try? FileManager.default.removeItem(at: temporary) }
    func testDevice(_ label: String) throws -> (Coordinator, FakeRunner) {
        let directory = temporary.appendingPathComponent(label); try privateDirectory(directory)
        let fake = FakeRunner(directory)
        let coordinator = Coordinator(runner: fake, directory: directory)
        coordinator.pause = { _ in }
        return (coordinator, fake)
    }
    let baseline = Data((0..<50).map(UInt8.init))
    let patched = try Coordinator.patchPin(baseline, pin: "0012")
    try require(patched[36..<40] == Data("0012".utf8), "leading zero PIN")
    try require(patched[48] == baseline[48] | 8, "enable bit")
    for index in 0..<50 where !(36..<40).contains(index) && index != 48 {
        try require(patched[index] == baseline[index], "unrelated PIN byte preserved")
    }
    for invalid in ["123", "12345", "１２３４", "12a4", "12\n4"] {
        try mustFail("invalid PIN rejected") { try Coordinator.validatePin(invalid) }
    }
    try Coordinator.validateName("GameChangersAIh1")
    try mustFail("seventeenth ASCII character rejected") { try Coordinator.validateName("GameChangersAIh12") }
    try mustFail("oversized UTF-8 name rejected") { try Coordinator.validateName(String(repeating: "é", count: 9)) }
    try mustFail("name control rejected") { try Coordinator.validateName("a\nb") }
    try mustFail("empty name rejected") { try Coordinator.validateName("") }
    let sixteen = String(repeating: "a", count: 16)
    let nameCases: [(String, String)] = [
        ("GameChangersAIh1", "GameChangersAIh1"),
        ("GameChangersAIh12", "GameChangersAIh1"),
        (sixteen, sixteen),
        (sixteen + "a", sixteen),
        (String(repeating: "é", count: 9), String(repeating: "é", count: 8)),
        (String(repeating: "🎵", count: 5), String(repeating: "🎵", count: 4)),
        (String(repeating: "a", count: 15) + "é", String(repeating: "a", count: 15)),
        (String(repeating: "a", count: 14) + "e\u{301}", String(repeating: "a", count: 14)),
        (String(repeating: "🇺🇸", count: 3), String(repeating: "🇺🇸", count: 2)),
        ("e" + String(repeating: "\u{301}", count: 30), ""),
        ("  New name  ", "  New name  "),
        ("", "")
    ]
    for (candidate, expected) in nameCases {
        let result = NameInputLimit.edit(candidate, previous: "CHOMP")
        try require(result.text.utf8.elementsEqual(expected.utf8), "live name limit preserves complete characters and spaces")
    }
    let middleInsert = String(repeating: "a", count: 8) + "X" + String(repeating: "a", count: 8)
    let limitedInsert = NameInputLimit.edit(middleInsert, previous: sixteen)
    try require(limitedInsert.text == sixteen, "inserting at capacity preserves the suffix")
    try require(NameInputLimit.selection(NSRange(location: 9, length: 0), afterRemoving: limitedInsert.removed,
                                        length: 16) == NSRange(location: 8, length: 0), "caret stays at rejected insertion")
    let replacement = String(repeating: "a", count: 6) + "🎵🎵" + String(repeating: "a", count: 6)
    let limitedReplacement = NameInputLimit.edit(replacement, previous: sixteen)
    try require(limitedReplacement.text == String(repeating: "a", count: 6) + "🎵" + String(repeating: "a", count: 6),
                "replacing a selection uses released bytes and preserves suffix")
    try require(NameInputLimit.selection(NSRange(location: 10, length: 0), afterRemoving: limitedReplacement.removed,
                                        length: 14) == NSRange(location: 8, length: 0), "emoji caret uses UTF-16 offsets")
    let fullName = String(repeating: "a", count: 15) + "e"
    let addedAccent = NameInputLimit.edit(fullName + "\u{301}", previous: fullName)
    try require(addedAccent.text == fullName, "combining mark overflow preserves the original base character")
    try require(NameInputLimit.selection(NSRange(location: 17, length: 0), afterRemoving: addedAccent.removed,
                                        length: 16) == NSRange(location: 16, length: 0), "rejected combining mark caret")
    let (invalidName, invalidNameFake) = try testDevice("name-too-long")
    try mustFail("oversized name blocked before USB") { try invalidName.setName("GameChangersAIh12") }
    try require(invalidNameFake.nameWrites == 0 && invalidNameFake.reboots == 0 && !invalidNameFake.debug,
                "oversized name never reaches a write or debug unlock")
    try mustFail("unknown schema rejected") { _ = try DeviceState.parse("REGISTRY_ID=1\nMODE=1\nAHI protocol version: 0.4\nConfiguration signature type=1 value=00000000000000000000000000000000\n") }
    let (pin, pinFake) = try testDevice("pin-success")
    try pin.setPin("0012")
    try require(pinFake.pinWrites == 1 && pinFake.mode == 1 && pin.journal["verified_after_reboot"] as? Bool == true, "PIN full lifecycle")
    let (noop, noopFake) = try testDevice("pin-noop")
    noopFake.block[48] |= 8
    try noop.setPin("0000")
    try require(noopFake.pinWrites == 0 && noopFake.mode == 1, "no-op avoids write")
    let (failedPin, failedPinFake) = try testDevice("pin-uncertain")
    failedPinFake.failPinWrite = true
    try mustFail("PIN write failure surfaces") { try failedPin.setPin("1234") }
    try require(failedPinFake.pinWrites == 1 && failedPinFake.mode == 1, "PIN no retry and mode cleanup")
    let (badPin, badPinFake) = try testDevice("pin-readback")
    badPinFake.corruptPinReadback = true
    try mustFail("PIN readback mismatch") { try badPin.setPin("1234") }
    try require(badPinFake.mode == 1 && badPin.journal["verified_after_reboot"] as? Bool == false, "mismatch not successful")
    let (restored, restoredFake) = try testDevice("pin-restore")
    restoredFake.block[12] = 77
    try restored.restore(pin.directory)
    try require(restoredFake.block[12] == 77 && restoredFake.block[36..<40] == Data("0000".utf8), "restore preserves current unrelated settings")
    let (name, nameFake) = try testDevice("name-success")
    try name.setName("New name")
    try require(nameFake.nameWrites == 1 && nameFake.reboots == 2 && !nameFake.debug, "name lifecycle and debug cleanup")
    let (badName, badNameFake) = try testDevice("name-uncertain")
    badNameFake.failNameWrite = true
    try mustFail("uncertain name write fails") { try badName.setName("New name") }
    try require(badNameFake.nameWrites == 1 && badNameFake.reboots == 0, "no blind retry or reset after uncertain name write")
    let (readback, readbackFake) = try testDevice("name-readback")
    readbackFake.corruptNameReadback = true
    try mustFail("name persistence mismatch fails") { try readback.setName("New name") }
    try require(!readbackFake.debug && readbackFake.reboots == 2, "name verification error still clears debug")
    let (chipFailure, chipFailureFake) = try testDevice("name-chip-check")
    chipFailureFake.failPostRebootChipCheck = true
    try mustFail("post-reboot chip check failure") { try chipFailure.setName("New name") }
    try require(!chipFailureFake.debug && chipFailureFake.reboots == 2, "post-reboot authentication failure still clears debug")
    let privateFile = temporary.appendingPathComponent("private.bin")
    try privateWrite(Data([1]), to: privateFile)
    try mustFail("private file never overwritten by default") { try privateWrite(Data([2]), to: privateFile) }
    let attributes = try FileManager.default.attributesOfItem(atPath: privateFile.path)
    try require((attributes[.posixPermissions] as? NSNumber)?.intValue == 0o600, "private backup permissions")
    let helperDirectory = Bundle.main.bundleURL.appendingPathComponent("Contents/Helpers")
    let native = ProcessRunner(helpers: helperDirectory, directory: temporary)
    for helper in ["b3_image", "debug_unlock"] {
        let result = try native.run(helper, ["--self-test"], timeout: 10)
        try require(result.status == 0 && result.output.lowercased().contains("self-test passed"), "real native helper subprocess and output capture")
    }
    let system = ProcessRunner(helpers: URL(fileURLWithPath: "/bin"), directory: temporary)
    let timeoutStart = Date()
    try mustFail("process timeout is bounded") { _ = try system.run("sleep", ["10"], timeout: 0.05) }
    try require(Date().timeIntervalSince(timeoutStart) < 8, "timed-out process terminated")
    print("Native coordinator self-test passed: \(checks) checks; no USB operations.")
}

umask(0o077)
if CommandLine.arguments.contains("--self-test") {
    do { try selfTest() } catch { fputs("\(error.localizedDescription)\n", stderr); exit(1) }
} else {
    let application = NSApplication.shared
    let delegate = AppDelegate()
    application.delegate = delegate
    application.run()
    withExtendedLifetime(delegate) {}
}
