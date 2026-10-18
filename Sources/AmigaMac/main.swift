// AmigaMac: AmigaOS rewritten as a native macOS application.
// This file is the macOS side: the window, the Shell console and the thread
// the Amiga system runs on. The OS itself is in the CAmiga module.
import AppKit
import CAmiga

// MARK: - Console bridge (C callbacks -> Swift)

final class Console {
    static let shared = Console()
    weak var view: ShellView?

    func write(_ s: String) {
        DispatchQueue.main.async { self.view?.appendOutput(s) }
    }

    /// A typed line goes to the Amiga side as an input event (never blocks).
    func submit(_ line: String) {
        let bytes = Array((line + "\n").data(using: .isoLatin1, allowLossyConversion: true) ?? Data())
        bytes.withUnsafeBufferPointer { p in
            p.baseAddress!.withMemoryRebound(to: CChar.self, capacity: bytes.count) {
                amiga_console_input($0, Int32(bytes.count))
            }
        }
    }
}

// Amiga text is ISO-8859-1
private let consoleWrite: @convention(c) (UnsafePointer<CChar>?, Int32) -> Void = { buf, len in
    guard let buf, len > 0 else { return }
    let data = Data(bytes: buf, count: Int(len))
    Console.shared.write(String(data: data, encoding: .isoLatin1) ?? "")
}

private let hostLog: @convention(c) (UnsafePointer<CChar>?) -> Void = { msg in
    guard let msg else { return }
    let s = String(cString: msg)
    NSLog("[AmigaMac] %@", s)
    Console.shared.write("\u{1}LOG\(s)\n")
}

private let hostShutdown: @convention(c) (Int32) -> Void = { _ in
    DispatchQueue.main.async { NSApp.terminate(nil) }
}

private let hostScreenOpened: @convention(c) (Int32, UnsafePointer<CChar>?) -> Void = { id, title in
    let t = title.map { String(cString: $0) } ?? ""
    DispatchQueue.main.async { ScreenWindowController.open[id] = ScreenWindowController(id: id, title: t) }
}

private let hostScreenClosed: @convention(c) (Int32) -> Void = { id in
    DispatchQueue.main.async {
        ScreenWindowController.open[id]?.close()
        ScreenWindowController.open[id] = nil
    }
}

private let hostBeep: @convention(c) () -> Void = { DispatchQueue.main.async { NSSound.beep() } }

// the Amiga clipboard (unit 0, text) is the Mac pasteboard
private let hostClipGet: @convention(c) (UnsafeMutablePointer<CChar>?, Int32) -> Int32 = { buf, max in
    guard let buf = buf else { return -1 }
    var text: String?
    if Thread.isMainThread { text = NSPasteboard.general.string(forType: .string) }
    else { DispatchQueue.main.sync { text = NSPasteboard.general.string(forType: .string) } }
    guard let t = text else { return -1 }
    let bytes = Array(t.utf8.prefix(Int(max) - 1))
    for (i, b) in bytes.enumerated() { buf[i] = CChar(bitPattern: b) }
    buf[bytes.count] = 0
    return Int32(bytes.count)
}
private let hostClipSet: @convention(c) (UnsafePointer<CChar>?, Int32) -> Void = { s, _ in
    guard let s = s else { return }
    let text = String(cString: s)
    DispatchQueue.main.async {
        NSPasteboard.general.clearContents()
        NSPasteboard.general.setString(text, forType: .string)
    }
}

// the Amiga pointer, as the cursor over the screen windows
private let hostPointer: @convention(c) (Int32, Int32, UnsafePointer<UInt32>?, Int32, Int32, Int32, Int32) -> Void = { _, kind, argb, w, h, hx, hy in
    var pixels: [UInt32] = []
    if kind == 1, let argb { pixels = Array(UnsafeBufferPointer(start: argb, count: Int(w * h))) }
    let p = AmigaPointer(kind: Int(kind), pixels: pixels, w: Int(w), h: Int(h), hx: Int(hx), hy: Int(hy))
    DispatchQueue.main.async { AmigaPointer.current = p; ScreenWindowController.pointerChanged() }
}

// the Workbench opens host files it has no Amiga tool for with their macOS application
private let hostOpenFile: @convention(c) (UnsafePointer<CChar>?) -> Void = { path in
    guard let path = path else { return }
    let url = URL(fileURLWithPath: String(cString: path))
    DispatchQueue.main.async { NSWorkspace.shared.open(url) }
}

// MARK: - The Shell view: type at the end, like an Amiga console

final class ShellView: NSTextView {
    private var inputStart = 0
    static let paper = NSColor(calibratedRed: 0.667, green: 0.667, blue: 0.667, alpha: 1)   // WB grey
    static let ink = NSColor.black
    static let logInk = NSColor(calibratedRed: 0.0, green: 0.2, blue: 0.53, alpha: 1)

    func setup() {
        backgroundColor = Self.paper
        insertionPointColor = Self.ink
        font = NSFont(name: "Monaco", size: 13) ?? .monospacedSystemFont(ofSize: 13, weight: .regular)
        textColor = Self.ink
        isRichText = false
        isAutomaticQuoteSubstitutionEnabled = false
        isAutomaticDashSubstitutionEnabled = false
        isAutomaticTextReplacementEnabled = false
        isContinuousSpellCheckingEnabled = false
        typingAttributes = [.font: font!, .foregroundColor: Self.ink]
    }

    func appendOutput(_ s: String) {
        guard let storage = textStorage else { return }
        var text = s
        var color = Self.ink
        if text.hasPrefix("\u{1}LOG") { text = String(text.dropFirst(4)); color = Self.logInk }
        let attrs: [NSAttributedString.Key: Any] = [.font: font!, .foregroundColor: color]
        // output goes before any partially typed input
        let pending = storage.attributedSubstring(from: NSRange(location: inputStart, length: storage.length - inputStart))
        storage.replaceCharacters(in: NSRange(location: inputStart, length: storage.length - inputStart),
                                  with: NSAttributedString(string: text, attributes: attrs))
        inputStart = storage.length
        storage.append(pending)
        setSelectedRange(NSRange(location: storage.length, length: 0))
        scrollToEndOfDocument(nil)
    }

    override func keyDown(with event: NSEvent) {
        if event.modifierFlags.contains(.control), event.charactersIgnoringModifiers == "c" {
            amiga_console_break()
            appendOutput("^C")
            return
        }
        super.keyDown(with: event)
    }

    override func shouldChangeText(in range: NSRange, replacementString: String?) -> Bool {
        range.location >= inputStart && super.shouldChangeText(in: range, replacementString: replacementString)
    }

    override func insertNewline(_ sender: Any?) {
        guard let storage = textStorage else { return }
        let line = (storage.string as NSString).substring(from: inputStart)
        storage.append(NSAttributedString(string: "\n", attributes: typingAttributes))
        inputStart = storage.length
        setSelectedRange(NSRange(location: storage.length, length: 0))
        Console.shared.submit(line)
    }

    override func setSelectedRange(_ r: NSRange, affinity: NSSelectionAffinity, stillSelecting: Bool) {
        super.setSelectedRange(r, affinity: affinity, stillSelecting: stillSelecting)
    }
}

// MARK: - System folder

func systemDirectory() -> URL {
    if let env = ProcessInfo.processInfo.environment["AMIGAMAC_SYS"] { return URL(fileURLWithPath: env) }
    let fm = FileManager.default
    let support = fm.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
        .appendingPathComponent("AmigaMac/System")
    try? fm.createDirectory(at: support, withIntermediateDirectories: true)
    if let template = Bundle.main.resourceURL?.appendingPathComponent("System") { installSystem(from: template, to: support) }
    return support
}

/// The system files this AmigaMac ships: installed when missing, replaced when the
/// bundle has a newer one. Everything else in the user's System is left alone.
func installSystem(from template: URL, to dest: URL) {
    let fm = FileManager.default
    guard let files = fm.enumerator(at: template, includingPropertiesForKeys: [.isDirectoryKey, .contentModificationDateKey]) else { return }
    for case let src as URL in files {
        let rel = String(src.path.dropFirst(template.path.count + 1))
        let dst = dest.appendingPathComponent(rel)
        let v = try? src.resourceValues(forKeys: [.isDirectoryKey, .contentModificationDateKey])
        if v?.isDirectory == true { try? fm.createDirectory(at: dst, withIntermediateDirectories: true); continue }
        let mine = (try? dst.resourceValues(forKeys: [.contentModificationDateKey]))?.contentModificationDate
        if let mine, let theirs = v?.contentModificationDate, mine >= theirs { continue }
        try? fm.removeItem(at: dst)
        try? fm.copyItem(at: src, to: dst)
    }
}

// MARK: - Application

final class AppDelegate: NSObject, NSApplicationDelegate {
    var window: NSWindow!

    func applicationDidFinishLaunching(_ note: Notification) {
        let frame = NSRect(x: 0, y: 0, width: 860, height: 520)
        window = NSWindow(contentRect: frame, styleMask: [.titled, .closable, .miniaturizable, .resizable],
                          backing: .buffered, defer: false)
        window.title = "AmigaShell — démarrage et journal"
        window.isReleasedWhenClosed = false
        window.center()
        let scroll = NSScrollView(frame: frame)
        scroll.hasVerticalScroller = true
        scroll.autoresizingMask = [.width, .height]
        let view = ShellView(frame: frame)
        view.autoresizingMask = [.width]
        view.textContainerInset = NSSize(width: 6, height: 6)
        view.setup()
        scroll.documentView = view
        scroll.backgroundColor = ShellView.paper
        window.contentView = scroll
        window.makeFirstResponder(view)
        Console.shared.view = view
        // as on the Amiga, the boot Shell has no window of its own: the Workbench
        // screen is what shows; the Fenêtre menu brings this one up when wanted
        startAmiga()
        DispatchQueue.main.asyncAfter(deadline: .now() + 3) {
            if ScreenWindowController.open.isEmpty { self.showShell(nil) }   /* nothing else to see */
        }
    }

    @objc func showShell(_ sender: Any?) {
        window.makeKeyAndOrderFront(nil)
        NSApp.activate(ignoringOtherApps: true)
    }

    @objc func newShell(_ sender: Any?) { Console.shared.submit("NEWSHELL") }

    func applicationShouldTerminateAfterLastWindowClosed(_ app: NSApplication) -> Bool { false }

    func startAmiga() {
        let sys = systemDirectory().path
        host.console_write = consoleWrite
        host.log = hostLog
        host.shutdown = hostShutdown
        host.screen_opened = hostScreenOpened
        host.screen_closed = hostScreenClosed
        host.beep = hostBeep
        host.open_file = hostOpenFile
        host.clipboard_get = hostClipGet
        host.clipboard_set = hostClipSet
        host.pointer = hostPointer
        os_boot(sys)
        Console.shared.write("AmigaMac — AmigaOS réécrit nativement pour macOS\n" +
                             "Tapez HELP pour la liste des commandes.\n\n")
        // demo / test hook: commands separated by ';;', run before the prompt
        var cmds: [String] = []
        if let auto = ProcessInfo.processInfo.environment["AMIGAMAC_AUTORUN"] {
            cmds = auto.components(separatedBy: ";;")
        } else if FileManager.default.fileExists(atPath: sys + "/S/Startup-Sequence") {
            cmds = ["EXECUTE S:Startup-Sequence"]          // as the Amiga boots: it does LoadWB
        } else {
            cmds = ["LOADWB"]
        }
        var cstrs = cmds.map { strdup($0) }
        cstrs.withUnsafeMutableBufferPointer { buf in
            buf.baseAddress!.withMemoryRebound(to: UnsafePointer<CChar>?.self, capacity: max(buf.count, 1)) {
                os_start($0, Int32(cmds.count), 1)
            }
        }
        cstrs.forEach { free($0) }
    }
}

// minimal menu so Cmd-Q, copy and paste work
func buildMenu() -> NSMenu {
    let main = NSMenu()
    let appItem = NSMenuItem()
    main.addItem(appItem)
    let appMenu = NSMenu()
    appMenu.addItem(withTitle: "Quitter AmigaMac", action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q")
    appItem.submenu = appMenu
    let editItem = NSMenuItem()
    main.addItem(editItem)
    let edit = NSMenu(title: "Édition")
    edit.addItem(withTitle: "Copier", action: #selector(NSText.copy(_:)), keyEquivalent: "c")
    edit.addItem(withTitle: "Coller", action: #selector(NSText.paste(_:)), keyEquivalent: "v")
    edit.addItem(withTitle: "Tout sélectionner", action: #selector(NSText.selectAll(_:)), keyEquivalent: "a")
    editItem.submenu = edit
    let winItem = NSMenuItem()
    main.addItem(winItem)
    let win = NSMenu(title: "Fenêtre")
    win.addItem(withTitle: "Nouveau Shell", action: #selector(AppDelegate.newShell(_:)), keyEquivalent: "n")
    win.addItem(withTitle: "Shell de démarrage et journal", action: #selector(AppDelegate.showShell(_:)), keyEquivalent: "l")
    win.addItem(NSMenuItem.separator())
    win.addItem(withTitle: "Réduire", action: #selector(NSWindow.performMiniaturize(_:)), keyEquivalent: "m")
    winItem.submenu = win
    app.windowsMenu = win
    return main
}

let app = NSApplication.shared
let delegate = AppDelegate()
app.delegate = delegate
app.setActivationPolicy(.regular)
app.mainMenu = buildMenu()
app.activate(ignoringOtherApps: true)
app.run()
