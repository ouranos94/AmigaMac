// One macOS window per Amiga screen: the composed image (screen + windows +
// menus) at 50 Hz, and mouse/keyboard turned into Amiga input events.
import AppKit
import CAmiga

final class ScreenWindowController: NSObject, NSWindowDelegate {
    static var open: [Int32: ScreenWindowController] = [:]
    let id: Int32
    let window: NSWindow
    let view: ScreenView

    init(id: Int32, title: String) {
        self.id = id
        var w: Int32 = 640, h: Int32 = 256, hires: Int32 = 1, lace: Int32 = 0
        amiga_display_size(id, &w, &h, &hires, &lace)
        // pixel aspect: hires non-interlaced pixels are twice as tall as wide;
        // hires 3 = the square-pixel AmigaMac modes
        let px: CGFloat = [3.0, 1.5, 0.75, 1.0][Int(max(0, min(3, hires)))]
        let py: CGFloat = hires == 3 ? 1.0 : (lace != 0 ? 1.5 : 3.0)
        var size = NSSize(width: CGFloat(w) * px, height: CGFloat(h) * py)
        if let vis = NSScreen.main?.visibleFrame {              // fit the Mac screen
            let k = min(1, (vis.width * 0.95) / size.width, (vis.height * 0.92 - 28) / size.height)
            size = NSSize(width: (size.width * k).rounded(), height: (size.height * k).rounded())
        }
        view = ScreenView(frame: NSRect(origin: .zero, size: size), id: id, width: Int(w), height: Int(h))
        window = NSWindow(contentRect: NSRect(origin: .zero, size: size),
                          styleMask: [.titled, .closable, .miniaturizable, .resizable], backing: .buffered, defer: false)
        super.init()
        window.title = title.isEmpty ? "Amiga" : title
        window.contentView = view
        window.contentAspectRatio = size
        window.delegate = self
        window.isReleasedWhenClosed = false
        window.acceptsMouseMovedEvents = true
        window.center()
        window.makeKeyAndOrderFront(nil)
        window.makeFirstResponder(view)
    }

    func windowShouldClose(_ sender: NSWindow) -> Bool {
        // the close box asks the Amiga programs (CLOSEWINDOW), it does not kill the screen
        var ev = InputEv(type: Int32(IEV_CLOSE_SCREEN), screen: id, x: 0, y: 0, code: 0, qual: 0, ch: 0)
        amiga_input_event(&ev)
        return false
    }

    func close() {
        view.stop()
        window.orderOut(nil)
    }
}

final class ScreenView: NSView {
    let id: Int32
    let w: Int, h: Int
    private var pixels: [UInt32]
    private var timer: Timer?
    private var tick: Timer?
    private var lastSerial: UInt32 = .max
    private var qualifiers: Int32 = 0

    init(frame: NSRect, id: Int32, width: Int, height: Int) {
        self.id = id
        self.w = width
        self.h = height
        pixels = [UInt32](repeating: 0, count: width * height)
        super.init(frame: frame)
        wantsLayer = true
        layer?.magnificationFilter = .nearest
        layer?.contentsGravity = .resize
        timer = Timer.scheduledTimer(withTimeInterval: 0.02, repeats: true) { [weak self] _ in self?.refresh() }
        tick = Timer.scheduledTimer(withTimeInterval: 0.1, repeats: true) { _ in
            var ev = InputEv(type: Int32(IEV_TICK), screen: id, x: 0, y: 0, code: 0, qual: 0, ch: 0)
            amiga_input_event(&ev)
        }
        addTrackingArea(NSTrackingArea(rect: .zero, options: [.mouseMoved, .activeInKeyWindow, .inVisibleRect], owner: self))
    }
    required init?(coder: NSCoder) { fatalError() }

    override var isFlipped: Bool { true }
    override var acceptsFirstResponder: Bool { true }
    override func acceptsFirstMouse(for event: NSEvent?) -> Bool { true }

    func stop() { timer?.invalidate(); tick?.invalidate() }

    func refresh() {
        let serial = amiga_display_serial()
        if serial == lastSerial { return }
        lastSerial = serial
        let ok = pixels.withUnsafeMutableBufferPointer { amiga_display_render(id, $0.baseAddress, Int32(w), Int32(h)) }
        if ok == 0 { return }
        let data = pixels.withUnsafeBufferPointer { Data(buffer: $0) }
        guard let provider = CGDataProvider(data: data as CFData),
              let image = CGImage(width: w, height: h, bitsPerComponent: 8, bitsPerPixel: 32, bytesPerRow: w * 4,
                                  space: CGColorSpaceCreateDeviceRGB(),
                                  bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.noneSkipFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue),
                                  provider: provider, decode: nil, shouldInterpolate: false, intent: .defaultIntent)
        else { return }
        layer?.contents = image
    }

    // MARK: mouse

    private func amigaPoint(_ e: NSEvent) -> (Int32, Int32) {
        let p = convert(e.locationInWindow, from: nil)
        let x = Int32(max(0, min(CGFloat(w - 1), p.x * CGFloat(w) / bounds.width)))
        let y = Int32(max(0, min(CGFloat(h - 1), p.y * CGFloat(h) / bounds.height)))
        return (x, y)
    }
    private func send(_ type: UInt32, _ e: NSEvent, code: Int32 = 0) {
        let (x, y) = amigaPoint(e)
        var ev = InputEv(type: Int32(type), screen: id, x: x, y: y, code: code, qual: qualifiers, ch: 0)
        amiga_input_event(&ev)
    }
    override func mouseMoved(with e: NSEvent) { send(UInt32(IEV_MOUSE), e) }
    override func mouseDragged(with e: NSEvent) { send(UInt32(IEV_MOUSE), e) }
    override func rightMouseDragged(with e: NSEvent) { send(UInt32(IEV_MOUSE), e) }
    override func mouseDown(with e: NSEvent) { send(UInt32(IEV_MOUSE), e); send(UInt32(IEV_BUTTON), e, code: 1) }
    override func mouseUp(with e: NSEvent) { send(UInt32(IEV_BUTTON), e, code: -1) }
    override func rightMouseDown(with e: NSEvent) { send(UInt32(IEV_MOUSE), e); send(UInt32(IEV_BUTTON), e, code: 2) }
    override func rightMouseUp(with e: NSEvent) { send(UInt32(IEV_BUTTON), e, code: -2) }

    // MARK: keyboard

    // macOS virtual key codes (ANSI positions) -> Amiga raw key codes
    static let rawKeys: [UInt16: Int32] = [
        50: 0x00, 18: 0x01, 19: 0x02, 20: 0x03, 21: 0x04, 23: 0x05, 22: 0x06, 26: 0x07, 28: 0x08, 25: 0x09, 29: 0x0A,
        27: 0x0B, 24: 0x0C, 42: 0x0D, 12: 0x10, 13: 0x11, 14: 0x12, 15: 0x13, 17: 0x14, 16: 0x15, 32: 0x16, 34: 0x17,
        31: 0x18, 35: 0x19, 33: 0x1A, 30: 0x1B, 0: 0x20, 1: 0x21, 2: 0x22, 3: 0x23, 5: 0x24, 4: 0x25, 38: 0x26,
        40: 0x27, 37: 0x28, 41: 0x29, 39: 0x2A, 6: 0x31, 7: 0x32, 8: 0x33, 9: 0x34, 11: 0x35, 45: 0x36, 46: 0x37,
        43: 0x38, 47: 0x39, 44: 0x3A, 49: 0x40, 51: 0x41, 48: 0x42, 76: 0x43, 36: 0x44, 53: 0x45, 117: 0x46,
        126: 0x4C, 125: 0x4D, 124: 0x4E, 123: 0x4F, 122: 0x50, 120: 0x51, 99: 0x52, 118: 0x53, 96: 0x54, 97: 0x55,
        98: 0x56, 100: 0x57, 101: 0x58, 109: 0x59, 114: 0x5F,
    ]

    private func key(_ e: NSEvent, up: Bool) {
        guard let raw = Self.rawKeys[e.keyCode] else { return }
        var ch: Int32 = 0
        if !up, let s = e.characters, let d = s.data(using: .isoLatin1), d.count == 1 {
            ch = Int32(d[0])
            if raw == 0x41 { ch = 8 } else if raw == 0x44 || raw == 0x43 { ch = 13 } else if raw == 0x46 { ch = 127 }
            else if raw >= 0x4C { ch = 0 }                // arrows, function keys: raw only
        }
        var ev = InputEv(type: Int32(IEV_KEY), screen: id, x: 0, y: 0, code: raw | (up ? 0x80 : 0), qual: qualifiers, ch: ch)
        amiga_input_event(&ev)
    }
    override func keyDown(with e: NSEvent) { key(e, up: false) }
    override func keyUp(with e: NSEvent) { key(e, up: true) }
    override func flagsChanged(with e: NSEvent) {
        let f = e.modifierFlags
        var q: Int32 = 0
        if f.contains(.shift) { q |= 0x01 }
        if f.contains(.capsLock) { q |= 0x04 }
        if f.contains(.control) { q |= 0x08 }
        if f.contains(.option) { q |= 0x10 }
        if f.contains(.command) { q |= 0x80 }               // right Amiga
        qualifiers = q
    }
}
