// The Mac app icon from the game's square icon (pack_app.py make_icns): the art inside macOS's rounded-square shape on Apple's 1024 grid
// (an 824-point body with a 100-point margin, corner radius 185.4, a soft drop shadow), written as the PNGs of an .iconset for iconutil.
// A square bitmap with no margin, as sips made it, sits edge to edge in the Dock and macOS 26 puts it on a grey plate.
// --ribbon TEXT draws a teal corner ribbon across the top right (a Mac-only development build's app: DEV).
// Usage: swift app_icon.swift <icon.jpg> <out.iconset> [--ribbon TEXT]
import AppKit
import CoreImage

let args = CommandLine.arguments
guard args.count >= 3, let art = NSImage(contentsOfFile: args[1]) else {
    FileHandle.standardError.write("usage: app_icon.swift <icon image> <out.iconset> [--ribbon TEXT]\n".data(using: .utf8)!); exit(2)
}
let iconset = URL(fileURLWithPath: args[2])
let ribbon = args.count >= 5 && args[3] == "--ribbon" ? args[4] : nil
try FileManager.default.createDirectory(at: iconset, withIntermediateDirectories: true)

func render(_ pixels: Int) -> Data {
    let rep = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: pixels, pixelsHigh: pixels, bitsPerSample: 8, samplesPerPixel: 4, hasAlpha: true,
                               isPlanar: false, colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0)!
    NSGraphicsContext.saveGraphicsState()
    NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: rep)
    let ctx = NSGraphicsContext.current!.cgContext
    let k = CGFloat(pixels) / 1024
    ctx.scaleBy(x: k, y: k)
    let body = CGRect(x: 100, y: 100, width: 824, height: 824)
    let shape = CGPath(roundedRect: body, cornerWidth: 185.4, cornerHeight: 185.4, transform: nil)

    ctx.saveGState()
    ctx.setShadow(offset: CGSize(width: 0, height: -10), blur: 28, color: NSColor.black.withAlphaComponent(0.32).cgColor)
    ctx.addPath(shape); ctx.setFillColor(NSColor.black.cgColor); ctx.fillPath()
    ctx.restoreGState()

    ctx.saveGState()
    ctx.addPath(shape); ctx.clip()
    // The whole art, inset on a blurred and darkened copy of itself: a game icon is cover art whose logo runs to its edges, which the rounded corners would cut
    let side = min(art.size.width, art.size.height)   // a non-square icon is cropped to its centre
    let from = NSRect(x: (art.size.width - side) / 2, y: (art.size.height - side) / 2, width: side, height: side)
    if let cg = art.cgImage(forProposedRect: nil, context: nil, hints: nil) {
        let blurred = CIImage(cgImage: cg).clampedToExtent().applyingGaussianBlur(sigma: Double(cg.width) / 14).cropped(to: CGRect(x: 0, y: 0, width: cg.width, height: cg.height))
        if let bg = CIContext().createCGImage(blurred, from: blurred.extent) { ctx.draw(bg, in: body.insetBy(dx: -40, dy: -40)) }
    }
    ctx.setFillColor(NSColor.black.withAlphaComponent(0.25).cgColor); ctx.fill(body)
    let inner = body.insetBy(dx: 70, dy: 70), plate = CGPath(roundedRect: inner, cornerWidth: 70, cornerHeight: 70, transform: nil)
    ctx.saveGState()
    ctx.setShadow(offset: CGSize(width: 0, height: -6), blur: 18, color: NSColor.black.withAlphaComponent(0.45).cgColor)
    ctx.addPath(plate); ctx.fillPath()
    ctx.restoreGState()
    ctx.saveGState(); ctx.addPath(plate); ctx.clip()
    art.draw(in: inner, from: from, operation: .copy, fraction: 1, respectFlipped: false, hints: [.interpolation: NSImageInterpolation.high.rawValue])
    ctx.restoreGState()
    if let text = ribbon {
        ctx.saveGState()
        ctx.translateBy(x: body.maxX - 118, y: body.maxY - 118)
        ctx.rotate(by: -.pi / 4)
        let band = CGRect(x: -300, y: -50, width: 600, height: 100)
        ctx.setShadow(offset: CGSize(width: 0, height: -4), blur: 12, color: NSColor.black.withAlphaComponent(0.35).cgColor)
        ctx.setFillColor(NSColor(srgbRed: 0.24, green: 0.74, blue: 0.80, alpha: 1).cgColor)
        ctx.fill(band)
        ctx.setShadow(offset: .zero, blur: 0, color: nil)
        let style = NSMutableParagraphStyle(); style.alignment = .center
        let label = NSAttributedString(string: text, attributes: [.font: NSFont.systemFont(ofSize: 62, weight: .heavy), .foregroundColor: NSColor.white,
                                                                  .kern: 10, .paragraphStyle: style])
        let h = label.size().height
        label.draw(in: NSRect(x: band.minX, y: -h / 2, width: band.width, height: h))
        ctx.restoreGState()
    }
    ctx.restoreGState()

    ctx.addPath(shape); ctx.setStrokeColor(NSColor.black.withAlphaComponent(0.18).cgColor); ctx.setLineWidth(2); ctx.strokePath()
    NSGraphicsContext.restoreGraphicsState()
    return rep.representation(using: .png, properties: [:])!
}

for n in [16, 32, 128, 256, 512] {
    try render(n).write(to: iconset.appendingPathComponent("icon_\(n)x\(n).png"))
    try render(n * 2).write(to: iconset.appendingPathComponent("icon_\(n)x\(n)@2x.png"))
}
