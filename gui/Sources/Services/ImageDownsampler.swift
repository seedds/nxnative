import Foundation
import AppKit
import ImageIO

public enum ImageDownsampler {
    /// Downsample an image from a file URL to a target maximum pixel dimension.
    /// This decodes only the required resolution into memory, avoiding large uncompressed bitmaps.
    public static func downsample(url: URL, maxPixelSize: CGFloat) -> NSImage? {
        let imageSourceOptions = [kCGImageSourceShouldCache: false] as CFDictionary
        guard let imageSource = CGImageSourceCreateWithURL(url as CFURL, imageSourceOptions) else {
            return nil
        }
        return createThumbnail(from: imageSource, maxPixelSize: maxPixelSize)
    }

    /// Downsample an image from in-memory Data to a target maximum pixel dimension.
    public static func downsample(data: Data, maxPixelSize: CGFloat) -> NSImage? {
        let imageSourceOptions = [kCGImageSourceShouldCache: false] as CFDictionary
        guard let imageSource = CGImageSourceCreateWithData(data as CFData, imageSourceOptions) else {
            return nil
        }
        return createThumbnail(from: imageSource, maxPixelSize: maxPixelSize)
    }

    private static func createThumbnail(from imageSource: CGImageSource, maxPixelSize: CGFloat) -> NSImage? {
        let downsampleOptions: [CFString: Any] = [
            kCGImageSourceCreateThumbnailFromImageAlways: true,
            kCGImageSourceShouldCacheImmediately: true,
            kCGImageSourceCreateThumbnailWithTransform: true,
            kCGImageSourceThumbnailMaxPixelSize: max(maxPixelSize, 32)
        ]
        guard let thumbnail = CGImageSourceCreateThumbnailAtIndex(imageSource, 0, downsampleOptions as CFDictionary) else {
            return nil
        }
        let size = NSSize(width: CGFloat(thumbnail.width), height: CGFloat(thumbnail.height))
        return NSImage(cgImage: thumbnail, size: size)
    }
}
