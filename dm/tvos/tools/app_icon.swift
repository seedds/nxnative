// Reuse the supplied icon artwork without cropping its title or characters.
import AppKit
import Foundation

let args = CommandLine.arguments
guard args.count == 6, let image = NSImage(contentsOfFile: args[1]),
      let width = Int(args[3]), let height = Int(args[4]) else { fatalError("icon arguments") }
let size = NSSize(width: width, height: height)
let rep = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: width, pixelsHigh: height,
    bitsPerSample: 8, samplesPerPixel: 4, hasAlpha: true, isPlanar: false,
    colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0)!
NSGraphicsContext.saveGraphicsState()
NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: rep)
let bounds = NSRect(origin: .zero, size: size)
NSColor.clear.setFill(); bounds.fill()
if args[5] != "front" {
    NSColor(calibratedRed: 0.035, green: 0.08, blue: 0.16, alpha: 1).setFill()
    bounds.fill()
}
if args[5] != "back" {
    let side = CGFloat(height)
    NSGraphicsContext.current?.imageInterpolation = .high
    image.draw(in: NSRect(x: (CGFloat(width)-side)/2, y: 0, width: side, height: side),
               from: .zero, operation: .sourceOver, fraction: 1)
}
NSGraphicsContext.restoreGraphicsState()
try rep.representation(using: .png, properties: [:])!.write(to: URL(fileURLWithPath: args[2]))
