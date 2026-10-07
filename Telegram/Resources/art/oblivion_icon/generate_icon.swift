// Oblivion app icon generator (the PREVIOUS icon).
//
// Since round 5 the default icon is the "eclipse" one, installed by
// design-mockups/src/install_icon.py (next to the repository). This file
// draws the former "black hole and paper plane" icon, which is kept as
// oblivion_previous_1024.png and offered in the icon chooser. Running
// "install" from here would put the former icon back as the default.
//
// Renders the "black hole swallowing the paper plane" icon procedurally
// and writes every icon asset used by the build.
//
// Usage (from the repository root):
//   swiftc -O Telegram/Resources/art/oblivion_icon/generate_icon.swift \
//     -o /tmp/oblivion_icon && /tmp/oblivion_icon preview /tmp/icon_preview
//   /tmp/oblivion_icon install .
//
// "preview" writes a few large renders and a contact sheet into the folder,
// "install" overwrites the icon assets inside the repository:
//   Telegram/Telegram/Images.xcassets/Icon.appiconset/icon*.png  (macOS)
//   Telegram/Telegram/Images.xcassets/Icon.iconset/icon_*.png    (macOS)
//   Telegram/Resources/art/icon*.png, icon_round512@2x.png,
//   logo_256.png, logo_256_no_margin.png, icon256.ico
//   Telegram/Resources/art/oblivion_icon/squircle_mask.png (runtime mask)
//
// On macOS Telegram.rcc depends only on the .qrc files, so after "install"
// touch Telegram/Resources/qrc/telegram/*.qrc to get the new logo_256*.png
// and icon_round512@2x.png into the next build.
//
// The original Telegram icon is kept in
// Telegram/Resources/art/oblivion_icon/telegram_original_1024.png.

import CoreGraphics
import Foundation
import ImageIO
import SwiftUI
import UniformTypeIdentifiers

// MARK: - Math and color helpers

struct RGB {
	var r: Float
	var g: Float
	var b: Float

	static func hex(_ v: UInt32) -> RGB {
		RGB(
			r: Float((v >> 16) & 0xFF) / 255,
			g: Float((v >> 8) & 0xFF) / 255,
			b: Float(v & 0xFF) / 255)
	}
	static let white = RGB(r: 1, g: 1, b: 1)
	static let black = RGB(r: 0, g: 0, b: 0)

	func mixed(_ o: RGB, _ t: Float) -> RGB {
		RGB(r: r + (o.r - r) * t, g: g + (o.g - g) * t, b: b + (o.b - b) * t)
	}
	func scaled(_ k: Float) -> RGB {
		RGB(r: r * k, g: g * k, b: b * k)
	}
}

@inline(__always) func clamp01(_ x: Float) -> Float {
	max(0, min(1, x))
}

@inline(__always) func smoothstep(_ e0: Float, _ e1: Float, _ x: Float) -> Float {
	let t = clamp01((x - e0) / (e1 - e0))
	return t * t * (3 - 2 * t)
}

@inline(__always) func gauss(_ x: Float, _ sigma: Float) -> Float {
	let t = x / sigma
	return expf(-t * t)
}

// Palette of the accretion disk, cyclic in [0, 1).
let violet = RGB.hex(0x7C4DFF)
let magenta = RGB.hex(0xE040FB)
let cyan = RGB.hex(0x00E5FF)

func palette(_ phase: Float) -> RGB {
	var t = phase - floorf(phase)
	let stops: [(Float, RGB)] = [
		(0.00, violet),
		(0.30, magenta),
		(0.52, violet),
		(0.78, cyan),
		(1.00, violet),
	]
	for i in 1..<stops.count where t <= stops[i].0 {
		let (p0, c0) = stops[i - 1]
		let (p1, c1) = stops[i]
		t = (t - p0) / (p1 - p0)
		return c0.mixed(c1, t * t * (3 - 2 * t))
	}
	return violet
}

// MARK: - Buffers

// Opaque RGB working buffer (the icon body before clipping).
final class Canvas {
	let w: Int
	let h: Int
	var px: [Float]

	init(_ w: Int, _ h: Int, fill: RGB = .black) {
		self.w = w
		self.h = h
		px = [Float](repeating: 0, count: w * h * 3)
		for i in 0..<(w * h) {
			px[i * 3] = fill.r
			px[i * 3 + 1] = fill.g
			px[i * 3 + 2] = fill.b
		}
	}

	// Calls body(x, y) for every pixel center, body returns
	// (color, alpha, mode) where mode 0 = normal over, 1 = screen.
	func paint(_ body: (Float, Float) -> (RGB, Float, Int)?) {
		px.withUnsafeMutableBufferPointer { p in
			for y in 0..<h {
				for x in 0..<w {
					guard let (c, a, mode) = body(Float(x) + 0.5, Float(y) + 0.5),
						a > 0 else {
						continue
					}
					let k = (y * w + x) * 3
					if mode == 0 {
						let ia = 1 - a
						p[k] = c.r * a + p[k] * ia
						p[k + 1] = c.g * a + p[k + 1] * ia
						p[k + 2] = c.b * a + p[k + 2] * ia
					} else {
						p[k] = 1 - (1 - p[k]) * (1 - clamp01(c.r * a))
						p[k + 1] = 1 - (1 - p[k + 1]) * (1 - clamp01(c.g * a))
						p[k + 2] = 1 - (1 - p[k + 2]) * (1 - clamp01(c.b * a))
					}
				}
			}
		}
	}
}

// Premultiplied RGBA output buffer.
final class Image {
	let w: Int
	let h: Int
	var px: [Float]

	init(_ w: Int, _ h: Int) {
		self.w = w
		self.h = h
		px = [Float](repeating: 0, count: w * h * 4)
	}

	// self = (color, alpha) over self.
	func over(_ i: Int, _ c: RGB, _ a: Float) {
		let k = i * 4
		let ia = 1 - a
		px[k] = c.r * a + px[k] * ia
		px[k + 1] = c.g * a + px[k + 1] * ia
		px[k + 2] = c.b * a + px[k + 2] * ia
		px[k + 3] = a + px[k + 3] * ia
	}

	func cgImage() -> CGImage {
		var bytes = [UInt8](repeating: 0, count: w * h * 4)
		for i in 0..<(w * h * 4) {
			bytes[i] = UInt8(clamp01(px[i]) * 255 + 0.5)
		}
		let provider = CGDataProvider(data: Data(bytes) as CFData)!
		return CGImage(
			width: w,
			height: h,
			bitsPerComponent: 8,
			bitsPerPixel: 32,
			bytesPerRow: w * 4,
			space: CGColorSpace(name: CGColorSpace.sRGB)!,
			bitmapInfo: CGBitmapInfo(
				rawValue: CGImageAlphaInfo.premultipliedLast.rawValue),
			provider: provider,
			decode: nil,
			shouldInterpolate: true,
			intent: .defaultIntent)!
	}
}

// Rasterizes a path given in top-left based coordinates into coverage.
func rasterize(_ path: CGPath, _ w: Int, _ h: Int) -> [Float] {
	var bytes = [UInt8](repeating: 0, count: w * h)
	bytes.withUnsafeMutableBytes { raw in
		let ctx = CGContext(
			data: raw.baseAddress,
			width: w,
			height: h,
			bitsPerComponent: 8,
			bytesPerRow: w,
			space: CGColorSpaceCreateDeviceGray(),
			bitmapInfo: CGImageAlphaInfo.none.rawValue)!
		ctx.translateBy(x: 0, y: CGFloat(h))
		ctx.scaleBy(x: 1, y: -1)
		ctx.setShouldAntialias(true)
		ctx.setAllowsAntialiasing(true)
		ctx.setFillColor(gray: 1, alpha: 1)
		ctx.addPath(path)
		ctx.fillPath()
	}
	return bytes.map { Float($0) / 255 }
}

// Three-pass box blur approximating a gaussian with the given sigma.
func blur(_ src: [Float], _ w: Int, _ h: Int, sigma: Float) -> [Float] {
	if sigma < 0.5 {
		return src
	}
	let radius = max(1, Int((sigma * sqrtf(3)).rounded()))
	var a = src
	var b = [Float](repeating: 0, count: src.count)
	func pass(_ from: [Float], _ to: inout [Float], horizontal: Bool) {
		let lines = horizontal ? h : w
		let length = horizontal ? w : h
		let norm = 1 / Float(radius * 2 + 1)
		for line in 0..<lines {
			@inline(__always) func index(_ i: Int) -> Int {
				let c = max(0, min(length - 1, i))
				return horizontal ? (line * w + c) : (c * w + line)
			}
			var sum: Float = 0
			for i in -radius...radius {
				sum += from[index(i)]
			}
			for i in 0..<length {
				to[index(i)] = sum * norm
				sum += from[index(i + radius + 1)] - from[index(i - radius)]
			}
		}
	}
	for _ in 0..<3 {
		pass(a, &b, horizontal: true)
		pass(b, &a, horizontal: false)
	}
	return a
}

// MARK: - Shapes

// Telegram paper plane, from Telegram/Resources/icons/plane_white.svg.
let planeSvg = """
M226.328419,494.722069 C372.088573,431.216685 469.284839,389.350049 \
517.917216,369.122161 C656.772535,311.36743 685.625481,301.334815 \
704.431427,301.003532 C708.567621,300.93067 717.815839,301.955743 \
723.806446,306.816707 C728.864797,310.92121 730.256552,316.46581 \
730.922551,320.357329 C731.588551,324.248848 732.417879,333.113828 \
731.758626,340.040666 C724.234007,419.102486 691.675104,610.964674 \
675.110982,699.515267 C668.10208,736.984342 654.301336,749.547532 \
640.940618,750.777006 C611.904684,753.448938 589.856115,731.588035 \
561.733393,713.153237 C517.726886,684.306416 492.866009,666.349181 \
450.150074,638.200013 C400.78442,605.66878 432.786119,587.789048 \
460.919462,558.568563 C468.282091,550.921423 596.21508,434.556479 \
598.691227,424.000355 C599.00091,422.680135 599.288312,417.758981 \
596.36474,415.160431 C593.441168,412.561881 589.126229,413.450484 \
586.012448,414.157198 C581.598758,415.158943 511.297793,461.625274 \
375.109553,553.556189 C355.154858,567.258623 337.080515,573.934908 \
320.886524,573.585046 C303.033948,573.199351 268.692754,563.490928 \
243.163606,555.192408 C211.851067,545.013936 186.964484,539.632504 \
189.131547,522.346309 C190.260287,513.342589 202.659244,504.134509 \
226.328419,494.722069 Z
"""

func parseSvgPath(_ d: String) -> CGPath {
	let path = CGMutablePath()
	var tokens: [String] = []
	var current = ""
	for ch in d {
		if ch == "M" || ch == "C" || ch == "Z" || ch == "L" {
			if !current.isEmpty { tokens.append(current); current = "" }
			tokens.append(String(ch))
		} else if ch == " " || ch == "," || ch == "\n" {
			if !current.isEmpty { tokens.append(current); current = "" }
		} else {
			current.append(ch)
		}
	}
	if !current.isEmpty { tokens.append(current) }
	var i = 0
	var command = "M"
	func number() -> CGFloat {
		defer { i += 1 }
		return CGFloat(Double(tokens[i])!)
	}
	while i < tokens.count {
		let t = tokens[i]
		if t == "M" || t == "C" || t == "Z" || t == "L" {
			command = t
			i += 1
			if t == "Z" { path.closeSubpath() }
			continue
		}
		switch command {
		case "M":
			path.move(to: CGPoint(x: number(), y: number()))
			command = "L"
		case "L":
			path.addLine(to: CGPoint(x: number(), y: number()))
		case "C":
			let c1 = CGPoint(x: number(), y: number())
			let c2 = CGPoint(x: number(), y: number())
			let to = CGPoint(x: number(), y: number())
			path.addCurve(to: to, control1: c1, control2: c2)
		default:
			i += 1
		}
	}
	return path
}

let planePath = parseSvgPath(planeSvg)
// Plane geometry in svg units.
let planeBounds = planePath.boundingBoxOfPath
let planeTip = CGPoint(x: 731.0, y: 320.0)
let planeTail = CGPoint(x: 189.0, y: 522.0)

func squircle(_ rect: CGRect) -> CGPath {
	// Apple's macOS app icon shape: 824 body with 185.4 continuous corners.
	let radius = rect.width * (185.4 / 824.0)
	return RoundedRectangle(cornerRadius: radius, style: .continuous)
		.path(in: rect)
		.cgPath
}

// MARK: - Design

enum Shape {
	case squircle
	case circle
}

enum Detail {
	case full   // 64 px and larger.
	case small  // 24..48 px: bolder ring, bigger plane, no stars.
	case tiny   // 16 px: ring and a plane blob only.
}

struct Variant {
	var size: Int          // Output size in pixels.
	var shape: Shape
	var margin: CGFloat    // Transparent margin, fraction of the canvas.
	var shadow: Bool
	var detail: Detail
}

// Tunable look of one detail level. Lengths are fractions of the body size.
struct Look {
	var holeOffset: Float      // Hole center shift to the top-right.
	var coreR: Float           // Event horizon radius.
	var photonGap: Float       // Photon ring distance from the horizon.
	var photonW: Float         // Photon ring thickness.
	var photonWhite: Float     // How much the photon ring is whitened.
	var bandGap: Float         // Main disk band distance from the horizon.
	var bandW: Float           // Main disk band thickness.
	var bandLight: Float
	var haloScale: Float       // Outer glow falloff.
	var haloLight: Float
	var arms: Float            // Spiral arms strength, 0..1.
	var swirl: Float           // Color swirl with the radius.
	var planeWidth: Float
	var planeEntry: Float      // Degrees, direction from the hole to the plane.
	var planeTilt: Float       // Degrees, heading deviation from the hole.
	var planeTipGap: Float     // Tip distance from the horizon (negative = inside).
	var planeFadeIn: Float     // Fade band around the horizon.
	var planeFadeOut: Float
	var stars: Bool
	var trails: Bool
	var nebula: Float
	var rim: Bool

	static let full = Look(
		holeOffset: 0.05,
		coreR: 0.155,
		photonGap: 0.010,
		photonW: 0.0075,
		photonWhite: 0.55,
		bandGap: 0.038,
		bandW: 0.045,
		bandLight: 0.95,
		haloScale: 0.19,
		haloLight: 0.85,
		arms: 0.85,
		swirl: 1.1,
		planeWidth: 0.30,
		planeEntry: 138,
		planeTilt: 9,
		planeTipGap: -0.035,
		planeFadeIn: 0.0,
		planeFadeOut: 0.035,
		stars: true,
		trails: true,
		nebula: 1,
		rim: true)

	static let small = Look(
		holeOffset: 0.06,
		coreR: 0.17,
		photonGap: 0.018,
		photonW: 0.024,
		photonWhite: 0.45,
		bandGap: 0.055,
		bandW: 0.06,
		bandLight: 1.15,
		haloScale: 0.12,
		haloLight: 0.7,
		arms: 0.3,
		swirl: 0.5,
		planeWidth: 0.38,
		planeEntry: 138,
		planeTilt: 6,
		planeTipGap: -0.035,
		planeFadeIn: 0.0,
		planeFadeOut: 0.03,
		stars: false,
		trails: false,
		nebula: 0.8,
		rim: true)

	static let tiny = Look(
		holeOffset: 0.07,
		coreR: 0.16,
		photonGap: 0.03,
		photonW: 0.045,
		photonWhite: 0.3,
		bandGap: 0.07,
		bandW: 0.07,
		bandLight: 1.25,
		haloScale: 0.10,
		haloLight: 0.7,
		arms: 0,
		swirl: 0.3,
		planeWidth: 0.40,
		planeEntry: 136,
		planeTilt: 4,
		planeTipGap: -0.02,
		planeFadeIn: 0.0,
		planeFadeOut: 0.02,
		stars: false,
		trails: false,
		nebula: 0.6,
		rim: false)
}

// Renders the icon at the given resolution (downsampled afterwards).
func render(_ v: Variant, resolution: Int) -> CGImage {
	let n = resolution
	let s = Float(n) / 1024
	let fn = Float(n)
	let look: Look = (v.detail == .full) ? .full : (v.detail == .small) ? .small : .tiny

	// Body geometry.
	let marginPx = CGFloat(n) * v.margin
	let bodyRect = CGRect(
		x: marginPx,
		y: marginPx,
		width: CGFloat(n) - marginPx * 2,
		height: CGFloat(n) - marginPx * 2)
	let B = Float(bodyRect.width)
	let bodyPath: CGPath = (v.shape == .squircle)
		? squircle(bodyRect)
		: CGPath(ellipseIn: bodyRect, transform: nil)
	let bodyMask = rasterize(bodyPath, n, n)
	let cx = fn / 2
	let cy = fn / 2

	// Black hole geometry.
	let hx = cx + look.holeOffset * B
	let hy = cy - look.holeOffset * B
	let coreR = look.coreR * B
	let photonR = coreR + look.photonGap * B
	let photonW = look.photonW * B
	let bandR = coreR + look.bandGap * B
	let bandW = look.bandW * B
	let haloScale = look.haloScale * B

	// Plane placement: the tip points at the hole, slightly tilted.
	let entry = Double(look.planeEntry) * .pi / 180
	let tipDistance = Double(coreR + look.planeTipGap * B)
	let tipTarget = CGPoint(
		x: Double(hx) + cos(entry) * tipDistance,
		y: Double(hy) + sin(entry) * tipDistance)
	let svgHeading = atan2(
		Double(planeTip.y - planeTail.y),
		Double(planeTip.x - planeTail.x))
	let heading = entry + .pi + Double(look.planeTilt) * .pi / 180
	let planeScale = CGFloat(look.planeWidth * B) / planeBounds.width
	var planeTransform = CGAffineTransform.identity
		.translatedBy(x: tipTarget.x, y: tipTarget.y)
		.rotated(by: heading - svgHeading)
		.scaledBy(x: planeScale, y: planeScale)
		.translatedBy(x: -planeTip.x, y: -planeTip.y)
	let planeShape = planePath.copy(using: &planeTransform)!
	let planeMask = rasterize(planeShape, n, n)
	let tipPoint = planeTip.applying(planeTransform)
	let tailPoint = planeTail.applying(planeTransform)
	let dirX = Float(tipPoint.x - tailPoint.x)
	let dirY = Float(tipPoint.y - tailPoint.y)
	let dirLen = sqrtf(dirX * dirX + dirY * dirY)
	let ux = dirX / dirLen
	let uy = dirY / dirLen
	let entryAngle = Float(entry)

	let canvas = Canvas(n, n)

	// 1. Deep space background.
	let top = RGB.hex(0x1E1240)
	let bottom = RGB.hex(0x0B0A1A)
	canvas.paint { x, y in
		let u = (x - Float(bodyRect.minX)) / B
		let w = (y - Float(bodyRect.minY)) / B
		let t = clamp01(w * 0.75 + u * 0.25)
		var c = top.mixed(bottom, smoothstep(0, 1, t))
		let dx = (x - cx) / B
		let dy = (y - cy) / B
		let d = sqrtf(dx * dx + dy * dy)
		c = c.scaled(1 - 0.3 * smoothstep(0.35, 0.72, d))
		return (c, 1, 0)
	}

	// 2. Nebula haze around the hole and in the corners.
	let nebulaViolet = RGB.hex(0x3A1C9A)
	let nebulaMagenta = RGB.hex(0x8A1AA8)
	let nebulaCyan = RGB.hex(0x0B5C88)
	canvas.paint { x, y in
		func blob(_ bx: Float, _ by: Float, _ sigma: Float) -> Float {
			let dx = x - bx
			let dy = y - by
			return gauss(sqrtf(dx * dx + dy * dy), sigma * B)
		}
		let k = look.nebula
		let a = 0.55 * k * blob(hx, hy, 0.40)
		let b = 0.22 * k * blob(cx + 0.36 * B, cy - 0.38 * B, 0.24)
		let c = 0.22 * k * blob(cx + 0.40 * B, cy + 0.42 * B, 0.22)
		let color = RGB(
			r: nebulaViolet.r * a + nebulaMagenta.r * b + nebulaCyan.r * c,
			g: nebulaViolet.g * a + nebulaMagenta.g * b + nebulaCyan.g * c,
			b: nebulaViolet.b * a + nebulaMagenta.b * b + nebulaCyan.b * c)
		return (color, 1, 1)
	}

	// 3. Stars.
	if look.stars {
		struct Star {
			var u: Float
			var v: Float
			var r: Float
			var a: Float
			var sparkle: Bool
			var tint: RGB
		}
		let lavender = RGB.hex(0xD9CCFF)
		let ice = RGB.hex(0xCCF6FF)
		let stars: [Star] = [
			Star(u: -0.34, v: -0.35, r: 0.0060, a: 0.90, sparkle: true, tint: .white),
			Star(u: -0.17, v: -0.41, r: 0.0030, a: 0.55, sparkle: false, tint: lavender),
			Star(u: -0.41, v: -0.13, r: 0.0026, a: 0.45, sparkle: false, tint: .white),
			Star(u: -0.24, v: -0.20, r: 0.0022, a: 0.35, sparkle: false, tint: ice),
			Star(u: 0.02, v: -0.44, r: 0.0024, a: 0.45, sparkle: false, tint: .white),
			Star(u: 0.23, v: -0.43, r: 0.0030, a: 0.50, sparkle: false, tint: ice),
			Star(u: 0.42, v: -0.31, r: 0.0026, a: 0.50, sparkle: false, tint: .white),
			Star(u: 0.44, v: -0.06, r: 0.0022, a: 0.40, sparkle: false, tint: lavender),
			Star(u: 0.35, v: 0.27, r: 0.0050, a: 0.80, sparkle: true, tint: ice),
			Star(u: 0.43, v: 0.42, r: 0.0024, a: 0.40, sparkle: false, tint: .white),
			Star(u: 0.20, v: 0.43, r: 0.0026, a: 0.45, sparkle: false, tint: lavender),
			Star(u: 0.00, v: 0.40, r: 0.0020, a: 0.35, sparkle: false, tint: .white),
			Star(u: -0.44, v: 0.10, r: 0.0022, a: 0.35, sparkle: false, tint: ice),
			Star(u: -0.12, v: 0.44, r: 0.0026, a: 0.40, sparkle: false, tint: .white),
			Star(u: 0.10, v: -0.30, r: 0.0018, a: 0.30, sparkle: false, tint: .white),
		]
		// Keep the corner stars inside a round body.
		let inset: Float = (v.shape == .circle) ? 0.84 : 1
		for star in stars {
			let sx = cx + star.u * B * inset
			let sy = cy + star.v * B * inset
			let r = max(star.r * B, 0.7 * s)
			let reach = star.sparkle ? r * 9 : r * 3
			canvas.paint { x, y in
				let dx = x - sx
				let dy = y - sy
				if abs(dx) > reach || abs(dy) > reach {
					return nil
				}
				let d = sqrtf(dx * dx + dy * dy)
				var a = star.a * (1 - smoothstep(r * 0.5, r * 1.2, d))
				a += star.a * 0.35 * gauss(d, r * 2.2)
				if star.sparkle {
					let thin = r * 0.3
					a += star.a * 0.85 * gauss(dy, thin) * gauss(dx, r * 3.8)
					a += star.a * 0.85 * gauss(dx, thin) * gauss(dy, r * 3.8)
				}
				return (star.tint, clamp01(a), 1)
			}
		}
	}

	// 4. Accretion disk glow with spiral arms (behind the plane).
	let doppler = { (theta: Float) -> Float in
		0.62 + 0.6 * (0.5 - 0.5 * cosf(theta - entryAngle))
	}
	canvas.paint { x, y in
		let dx = x - hx
		let dy = y - hy
		let r = sqrtf(dx * dx + dy * dy)
		if r < coreR * 0.8 {
			return nil
		}
		let theta = atan2f(dy, dx)
		let phase = theta / (2 * .pi) + 0.62 + look.swirl * (r - photonR) / B
		let color = palette(phase)
		let outside = max(0, r - coreR)
		// Two logarithmic spiral arms winding into the hole.
		let spiral = 0.5 + 0.5 * cosf(2 * theta + 5.5 * logf(max(r, 1) / coreR))
		let arm = (1 - look.arms) + look.arms * spiral * spiral
		let halo = look.haloLight
			* expf(-outside / haloScale)
			* smoothstep(coreR - 2 * s, coreR + 6 * s, r)
			* arm
		let band = r > bandR
			? gauss(r - bandR, bandW)
			: gauss(r - bandR, bandW * 0.5)
		let light = (halo + look.bandLight * band) * doppler(theta)
		return (color, clamp01(light), 1)
	}

	// 5. Plane motion trail.
	if look.trails {
		let trails: [(offset: Float, start: Float, length: Float, width: Float, alpha: Float)] = [
			(-0.050, 0.02, 0.13, 0.0050, 0.30),
			(0.004, 0.00, 0.22, 0.0070, 0.36),
			(0.056, 0.03, 0.11, 0.0042, 0.24),
		]
		let px = -uy // Perpendicular.
		let py = ux
		let base = (x: Float(tailPoint.x) * 0.6 + Float(tipPoint.x) * 0.4,
			y: Float(tailPoint.y) * 0.6 + Float(tipPoint.y) * 0.4)
		for trail in trails {
			let startX = base.x + px * trail.offset * B - ux * (0.10 + trail.start) * B
			let startY = base.y + py * trail.offset * B - uy * (0.10 + trail.start) * B
			let length = trail.length * B
			let width = trail.width * B
			canvas.paint { x, y in
				let rx = x - startX
				let ry = y - startY
				let along = -(rx * ux + ry * uy) // Positive backwards.
				let across = rx * px + ry * py
				if along < -width * 2 || along > length + width * 2 {
					return nil
				}
				let t = clamp01(along / length)
				let taper = width * (1 - 0.7 * t)
				let a = trail.alpha
					* (1 - smoothstep(0.05, 1, t))
					* smoothstep(-width, width, along)
					* (1 - smoothstep(taper * 0.5, taper, abs(across)))
				return (RGB.hex(0xE8E0FF), a, 1)
			}
		}
	}

	// 6. Plane shadow and body, fading into the core.
	let planeFade = { (x: Float, y: Float) -> Float in
		let dx = x - hx
		let dy = y - hy
		let r = sqrtf(dx * dx + dy * dy)
		return smoothstep(
			coreR + look.planeFadeIn * B,
			coreR + look.planeFadeOut * B,
			r)
	}
	if v.detail == .full {
		let shadowMask = blur(planeMask, n, n, sigma: 0.012 * B)
		let offX = Int((0.003 * B).rounded())
		let offY = Int((0.010 * B).rounded())
		canvas.paint { x, y in
			let ix = Int(x) - offX
			let iy = Int(y) - offY
			if ix < 0 || iy < 0 || ix >= n || iy >= n {
				return nil
			}
			let a = shadowMask[iy * n + ix] * 0.65 * planeFade(x, y)
			return (RGB.hex(0x05030F), a, 0)
		}
	}
	let planeLight = RGB.white
	let planeDark = RGB.hex(0xD6CCFF)
	canvas.paint { x, y in
		let m = planeMask[Int(y) * n + Int(x)]
		if m <= 0 {
			return nil
		}
		// Gradient along the plane, lighter towards the tail.
		let along = ((x - Float(tailPoint.x)) * ux + (y - Float(tailPoint.y)) * uy) / dirLen
		var c = planeLight.mixed(planeDark, clamp01(along * 0.9))
		// Glow of the disk reflected near the hole.
		let dx = x - hx
		let dy = y - hy
		let r = sqrtf(dx * dx + dy * dy)
		let tint = gauss(r - coreR, 0.06 * B) * 0.25
		c = c.mixed(magenta.mixed(violet, 0.35), tint)
		return (c, m * planeFade(x, y), 0)
	}

	// 7. The black core with a crisp event horizon.
	canvas.paint { x, y in
		let dx = x - hx
		let dy = y - hy
		let r = sqrtf(dx * dx + dy * dy)
		if r > coreR + 0.03 * B {
			return nil
		}
		let edge = max(0.7 * s, 0.003 * B)
		let a = 1 - smoothstep(coreR - edge, coreR + edge, r)
		return (.black, a, 0)
	}

	// 8. Photon ring on top: thin and bright, it crosses the plane nose.
	canvas.paint { x, y in
		let dx = x - hx
		let dy = y - hy
		let r = sqrtf(dx * dx + dy * dy)
		if abs(r - photonR) > photonW * 4 {
			return nil
		}
		let theta = atan2f(dy, dx)
		let phase = theta / (2 * .pi) + 0.62
		let color = palette(phase).mixed(.white, look.photonWhite)
		let a = gauss(r - photonR, photonW) * doppler(theta) * 0.95
		return (color, clamp01(a), 1)
	}

	// 9. Subtle top highlight along the inner rim of the body.
	if look.rim {
		let rimWidth = max(1.0, CGFloat(0.004 * B))
		let rimPath = bodyPath.copy(
			strokingWithWidth: rimWidth * 2,
			lineCap: .round,
			lineJoin: .round,
			miterLimit: 1)
		let rimMask = rasterize(rimPath, n, n)
		canvas.paint { x, y in
			let i = Int(y) * n + Int(x)
			let m = rimMask[i] * bodyMask[i]
			if m <= 0 {
				return nil
			}
			let t = (y - Float(bodyRect.minY)) / B
			return (.white, m * (0.16 - 0.13 * smoothstep(0, 0.6, t)), 1)
		}
	}

	// Compose: shadow, then the clipped body.
	let out = Image(n, n)
	if v.shadow {
		let offset = Int((0.012 * fn).rounded())
		let shadow = blur(bodyMask, n, n, sigma: 0.014 * fn)
		for y in 0..<n {
			for x in 0..<n {
				let sy = y - offset
				if sy < 0 {
					continue
				}
				let a = shadow[sy * n + x] * 0.42
				out.over(y * n + x, .black, a)
			}
		}
	}
	for i in 0..<(n * n) {
		let m = bodyMask[i]
		if m <= 0 {
			continue
		}
		let c = RGB(r: canvas.px[i * 3], g: canvas.px[i * 3 + 1], b: canvas.px[i * 3 + 2])
		out.over(i, c, m)
	}
	return out.cgImage()
}

// MARK: - Resampling and output

func resized(_ image: CGImage, _ size: Int) -> CGImage {
	var current = image
	// Halve step by step for quality, then do the final step.
	while current.width / 2 >= size && current.width / 2 != size {
		current = draw(current, current.width / 2)
	}
	return (current.width == size) ? current : draw(current, size)
}

func draw(_ image: CGImage, _ size: Int) -> CGImage {
	let ctx = CGContext(
		data: nil,
		width: size,
		height: size,
		bitsPerComponent: 8,
		bytesPerRow: size * 4,
		space: CGColorSpace(name: CGColorSpace.sRGB)!,
		bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
	ctx.interpolationQuality = .high
	ctx.draw(image, in: CGRect(x: 0, y: 0, width: size, height: size))
	return ctx.makeImage()!
}

func pngData(_ image: CGImage) -> Data {
	let data = NSMutableData()
	let dest = CGImageDestinationCreateWithData(
		data as CFMutableData,
		UTType.png.identifier as CFString,
		1,
		nil)!
	CGImageDestinationAddImage(dest, image, nil)
	CGImageDestinationFinalize(dest)
	return data as Data
}

func writePng(_ image: CGImage, _ path: String) {
	let url = URL(fileURLWithPath: path)
	try! FileManager.default.createDirectory(
		at: url.deletingLastPathComponent(),
		withIntermediateDirectories: true)
	try! pngData(image).write(to: url)
	print("wrote \(path) (\(image.width)x\(image.height))")
}

// Straight (non-premultiplied) BGRA rows, top to bottom.
func bgraRows(_ image: CGImage) -> [UInt8] {
	let n = image.width
	var bytes = [UInt8](repeating: 0, count: n * n * 4)
	bytes.withUnsafeMutableBytes { raw in
		let ctx = CGContext(
			data: raw.baseAddress,
			width: n,
			height: n,
			bitsPerComponent: 8,
			bytesPerRow: n * 4,
			space: CGColorSpace(name: CGColorSpace.sRGB)!,
			bitmapInfo: CGImageAlphaInfo.premultipliedFirst.rawValue
				| CGBitmapInfo.byteOrder32Little.rawValue)!
		ctx.draw(image, in: CGRect(x: 0, y: 0, width: n, height: n))
	}
	// Unpremultiply.
	for i in stride(from: 0, to: bytes.count, by: 4) {
		let a = Int(bytes[i + 3])
		if a > 0 && a < 255 {
			for k in 0..<3 {
				bytes[i + k] = UInt8(min(255, (Int(bytes[i + k]) * 255 + a / 2) / a))
			}
		}
	}
	return bytes
}

// Windows .ico: BMP entries for small sizes and a PNG one for 256.
func writeIco(_ images: [CGImage], _ path: String) {
	var entries: [Data] = []
	for image in images {
		let n = image.width
		if n >= 256 {
			entries.append(pngData(image))
			continue
		}
		var d = Data()
		func u16(_ v: Int) { var x = UInt16(v).littleEndian; d.append(Data(bytes: &x, count: 2)) }
		func u32(_ v: Int) { var x = UInt32(v).littleEndian; d.append(Data(bytes: &x, count: 4)) }
		let maskStride = ((n + 31) / 32) * 4
		u32(40); u32(n); u32(n * 2); u16(1); u16(32); u32(0)
		u32(n * n * 4 + maskStride * n); u32(0); u32(0); u32(0); u32(0)
		let rows = bgraRows(image)
		for y in (0..<n).reversed() {
			d.append(contentsOf: rows[(y * n * 4)..<((y + 1) * n * 4)])
		}
		for y in (0..<n).reversed() {
			var line = [UInt8](repeating: 0, count: maskStride)
			for x in 0..<n where rows[(y * n + x) * 4 + 3] == 0 {
				line[x / 8] |= UInt8(0x80 >> (x % 8))
			}
			d.append(contentsOf: line)
		}
		entries.append(d)
	}
	var out = Data()
	func u8(_ v: Int) { out.append(UInt8(v)) }
	func u16(_ v: Int) { var x = UInt16(v).littleEndian; out.append(Data(bytes: &x, count: 2)) }
	func u32(_ v: Int) { var x = UInt32(v).littleEndian; out.append(Data(bytes: &x, count: 4)) }
	u16(0); u16(1); u16(images.count)
	var offset = 6 + 16 * images.count
	for (image, entry) in zip(images, entries) {
		let n = image.width
		u8(n >= 256 ? 0 : n); u8(n >= 256 ? 0 : n); u8(0); u8(0)
		u16(1); u16(32); u32(entry.count); u32(offset)
		offset += entry.count
	}
	for entry in entries {
		out.append(entry)
	}
	try! out.write(to: URL(fileURLWithPath: path))
	print("wrote \(path) (\(images.map { $0.width }))")
}

// MARK: - Variants

// macOS Big Sur+ squircle with the standard 100 px margin.
func macIcon(_ size: Int) -> CGImage {
	if size <= 16 {
		let v = Variant(size: size, shape: .squircle, margin: 1.0 / 16, shadow: false, detail: .tiny)
		return resized(render(v, resolution: 256), size)
	} else if size <= 32 {
		let v = Variant(size: size, shape: .squircle, margin: 1.0 / 16, shadow: false, detail: .small)
		return resized(render(v, resolution: 256), size)
	}
	let v = Variant(size: size, shape: .squircle, margin: 100.0 / 1024, shadow: true, detail: .full)
	return resized(render(v, resolution: max(1024, size)), size)
}

// Round variant (Linux / Windows / "round icon" option), like upstream.
func roundIcon(_ size: Int, noMargin: Bool = false) -> CGImage {
	let margin: CGFloat = noMargin ? 0 : (size <= 48 ? 1.0 / 32 : 56.0 / 1024)
	let detail: Detail = size <= 16 ? .tiny : size <= 48 ? .small : .full
	let v = Variant(size: size, shape: .circle, margin: margin, shadow: !noMargin && size > 48, detail: detail)
	let resolution = (detail == .full) ? max(1024, size) : 256
	return resized(render(v, resolution: resolution), size)
}

// White macOS icon body on a transparent canvas, used at runtime to shape
// custom pictures the same way (see oblivion_app_icon.cpp).
func squircleMask(_ size: Int) -> CGImage {
	let rect = CGRect(x: 100, y: 100, width: 824, height: 824)
	let ctx = CGContext(
		data: nil,
		width: size,
		height: size,
		bitsPerComponent: 8,
		bytesPerRow: size * 4,
		space: CGColorSpace(name: CGColorSpace.sRGB)!,
		bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
	ctx.scaleBy(x: CGFloat(size) / 1024, y: CGFloat(size) / 1024)
	ctx.setShouldAntialias(true)
	ctx.setFillColor(CGColor(red: 1, green: 1, blue: 1, alpha: 1))
	ctx.addPath(squircle(rect))
	ctx.fillPath()
	return ctx.makeImage()!
}

func contactSheet(_ path: String) {
	let sizes = [512, 256, 128, 64, 32, 16]
	let width = 1500
	let height = 1300
	let ctx = CGContext(
		data: nil,
		width: width,
		height: height,
		bitsPerComponent: 8,
		bytesPerRow: width * 4,
		space: CGColorSpace(name: CGColorSpace.sRGB)!,
		bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
	for (row, dark) in [false, true].enumerated() {
		let y0 = CGFloat(row) * 650
		ctx.setFillColor(dark
			? CGColor(red: 0.12, green: 0.12, blue: 0.13, alpha: 1)
			: CGColor(red: 0.93, green: 0.93, blue: 0.95, alpha: 1))
		ctx.fill(CGRect(x: 0, y: CGFloat(height) - y0 - 650, width: CGFloat(width), height: 650))
		var x: CGFloat = 20
		for size in sizes {
			let image = macIcon(size)
			ctx.interpolationQuality = .none
			ctx.draw(image, in: CGRect(
				x: x,
				y: CGFloat(height) - y0 - 20 - CGFloat(size),
				width: CGFloat(size),
				height: CGFloat(size)))
			x += CGFloat(size) + 20
		}
		// Magnified small sizes.
		for (i, size) in [32, 16].enumerated() {
			let image = macIcon(size)
			ctx.interpolationQuality = .none
			let scale = (size == 32) ? 5 : 8
			ctx.draw(image, in: CGRect(
				x: 560 + CGFloat(i) * 200,
				y: CGFloat(height) - y0 - 580,
				width: CGFloat(size * scale),
				height: CGFloat(size * scale)))
		}
		for (i, size) in [128, 48, 16].enumerated() {
			let image = roundIcon(size)
			ctx.draw(image, in: CGRect(
				x: 1000 + CGFloat(i) * 150,
				y: CGFloat(height) - y0 - 580,
				width: CGFloat(size),
				height: CGFloat(size)))
		}
	}
	writePng(ctx.makeImage()!, path)
}

// MARK: - Main

let args = CommandLine.arguments
guard args.count >= 3 else {
	print("usage: generate_icon (preview <dir> | install <repo-root>)")
	exit(1)
}
let mode = args[1]
let target = (args[2] as NSString).standardizingPath

if mode == "preview" {
	writePng(macIcon(1024), target + "/mac_1024.png")
	writePng(roundIcon(1024), target + "/round_1024.png")
	writePng(roundIcon(256, noMargin: true), target + "/round_nomargin_256.png")
	contactSheet(target + "/sheet.png")
} else if mode == "install" {
	let root = target + "/Telegram"
	let xcassets = root + "/Telegram/Images.xcassets"
	let art = root + "/Resources/art"
	let mac: [(String, String, Int)] = [
		("icon16.png", "icon_16x16.png", 16),
		("icon16@2x.png", "icon_16x16@2x.png", 32),
		("icon32.png", "icon_32x32.png", 32),
		("icon32@2x.png", "icon_32x32@2x.png", 64),
		("icon128.png", "icon_128x128.png", 128),
		("icon128@2x.png", "icon_128x128@2x.png", 256),
		("icon256.png", "icon_256x256.png", 256),
		("icon256@2x.png", "icon_256x256@2x.png", 512),
		("icon512.png", "icon_512x512.png", 512),
		("icon512@2x.png", "icon_512x512@2x.png", 1024),
	]
	var cache: [Int: CGImage] = [:]
	for (appiconset, iconset, size) in mac {
		let image = cache[size] ?? macIcon(size)
		cache[size] = image
		writePng(image, xcassets + "/Icon.appiconset/" + appiconset)
		writePng(image, xcassets + "/Icon.iconset/" + iconset)
	}
	writePng(squircleMask(1024), art + "/oblivion_icon/squircle_mask.png")

	let round: [(String, Int)] = [
		("icon16.png", 16), ("icon16@2x.png", 32),
		("icon32.png", 32), ("icon32@2x.png", 64),
		("icon48.png", 48), ("icon48@2x.png", 96),
		("icon64.png", 64), ("icon64@2x.png", 128),
		("icon128.png", 128), ("icon128@2x.png", 256),
		("icon256.png", 256), ("icon256@2x.png", 512),
		("icon512.png", 512), ("icon512@2x.png", 1024),
	]
	var roundCache: [Int: CGImage] = [:]
	for (name, size) in round {
		let image = roundCache[size] ?? roundIcon(size)
		roundCache[size] = image
		writePng(image, art + "/" + name)
	}
	writePng(roundCache[1024]!, art + "/icon_round512@2x.png")
	writePng(roundCache[256]!, art + "/logo_256.png")
	writePng(roundIcon(256, noMargin: true), art + "/logo_256_no_margin.png")
	writeIco(
		[roundIcon(16), roundIcon(24), roundCache[32]!, roundCache[48]!, roundCache[256]!],
		art + "/icon256.ico")
} else {
	print("unknown mode \(mode)")
	exit(1)
}
