// Renders a TrueType font into a smooth (anti-aliased, 4 bits per pixel)
// bitmap font module for the PiBody display (drawn by st7789 text()).
//
//   swift makefont.swift --list font.ttf...            print PostScript names
//   swift makefont.swift font.ttf SIZE out.py [NAME]   write a font module
//   swift makefont.swift font.ttf hN out.py [NAME]     largest size with N-row lines
//
// Characters: ASCII 32..126, Cyrillic U+0400..U+045F and the Kazakh, Tajik, Tatar
// and Bashkir letters (when the font has them). Glyphs are cropped to their ink,
// advances are rounded to whole pixels. Module fields: NAME, HEIGHT, BASELINE,
// WIDTH (widest ASCII advance), CHARS (str),
// ADV, XOFF (bearing + 128), GW (bitmap width), YOFF (first inked row), GH
// (inked rows), OFFS (uint16 LE byte offsets into BITMAP), BITMAP (nibbles,
// high first, row-major, GH rows per glyph).
import CoreGraphics
import CoreText
import Foundation

func load(_ path: String, _ size: CGFloat) -> CTFont {
    guard let prov = CGDataProvider(url: URL(fileURLWithPath: path) as CFURL),
          let cg = CGFont(prov) else { fatalError("cannot read \(path)") }
    return CTFontCreateWithGraphicsFont(cg, size, nil, nil)
}

let args = Array(CommandLine.arguments.dropFirst())
if args.first == "--list" {
    for p in args.dropFirst() {
        print(CTFontCopyPostScriptName(load(p, 12)) as String, p)
    }
    exit(0)
}
// SIZE is the em size in pixels, or hN: the largest size whose glyphs fit N rows
let fit = args.count >= 3 && args[1].hasPrefix("h") ? Int(args[1].dropFirst()) : nil
guard args.count >= 3, let size = fit != nil ? 8.0 : Double(args[1]) else {
    print("usage: makefont.swift font.ttf SIZE|hN out.py [NAME]"); exit(2)
}
func box(_ f: CTFont, _ cs: [UniChar]) -> (Int, Int) {
    var gs = [CGGlyph](repeating: 0, count: cs.count)
    var cc = cs
    _ = CTFontGetGlyphsForCharacters(f, &cc, &gs, cs.count)
    var rs = [CGRect](repeating: .zero, count: gs.count)
    CTFontGetBoundingRectsForGlyphs(f, .horizontal, gs, &rs, gs.count)
    let ink = rs.filter { !$0.isEmpty }
    return (Int(ceil(ink.map { $0.maxY }.max()!)), Int(floor(ink.map { $0.minY }.min()!)))
}
var font = load(args[0], CGFloat(size))
var name: String { args.count > 3 ? args[3] : "\(CTFontCopyPostScriptName(font)) \(CTFontGetSize(font))" }

var chars: [UniChar] = Array(32...126)
// Cyrillic U+0400..U+045F: Russian, Ukrainian, Belarusian, Serbian, Bulgarian, Macedonian
chars += Array(0x0400...0x045F)
// Central Asian and Volga letters: Kazakh Ә Ғ Қ Ң Ө Ұ Ү Һ, Tajik Ӣ Ӯ Ҳ Ҷ,
// Tatar Җ, Bashkir Ҙ Ҡ Ҫ (upper and lower case)
for c in [0x04D8, 0x0492, 0x049A, 0x04A2, 0x04E8, 0x04B0, 0x04AE, 0x04BA,
          0x04E2, 0x04EE, 0x04B2, 0x04B6, 0x0496, 0x0498, 0x04A0, 0x04AA] {
    chars += [UniChar(c), UniChar(c + 1)]
}
var glyphs = [CGGlyph](repeating: 0, count: chars.count)
for (i, c) in chars.enumerated() {
    var cc = c
    var g: CGGlyph = 0
    if CTFontGetGlyphsForCharacters(font, &cc, &g, 1) { glyphs[i] = g }
}
if let target = fit {
    var s = 8.0
    while true {
        let f = load(args[0], CGFloat(s + 0.25))
        let (t, b) = box(f, chars)
        if t - b > target { break }
        s += 0.25
    }
    font = load(args[0], CGFloat(s))
}
// drop characters the font lacks (except space)
let keep = chars.indices.filter { glyphs[$0] != 0 || chars[$0] == 32 }
chars = keep.map { chars[$0] }
glyphs = keep.map { glyphs[$0] }

// Line box from the ink of all glyphs: top of the tallest, bottom of the lowest.
var rects = [CGRect](repeating: .zero, count: glyphs.count)
CTFontGetBoundingRectsForGlyphs(font, .horizontal, glyphs, &rects, glyphs.count)
var advs = [CGSize](repeating: .zero, count: glyphs.count)
CTFontGetAdvancesForGlyphs(font, .horizontal, glyphs, &advs, glyphs.count)
let top = Int(ceil(rects.map { $0.maxY }.max()!))
let bottom = Int(floor(rects.map { $0.minY }.min()!))
let pad = fit != nil ? max(0, fit! - (top - bottom)) : 0
let height = top - bottom + pad
let baseline = top + pad / 2

var adv = [UInt8](), xoff = [UInt8](), gw = [UInt8](), offs = [UInt8](), bitmap = [UInt8]()
var yoff = [UInt8](), gh = [UInt8]()
for (i, g) in glyphs.enumerated() {
    let r = rects[i]
    let x0 = r.isEmpty ? 0 : Int(floor(r.minX))
    let w = r.isEmpty ? 0 : max(1, Int(ceil(r.maxX)) - x0)
    adv.append(UInt8(max(0, min(255, Int((advs[i].width).rounded())))))
    xoff.append(UInt8(x0 + 128))
    gw.append(UInt8(w))
    let o = bitmap.count
    offs += [UInt8(o & 0xFF), UInt8(o >> 8)]
    if w == 0 { yoff.append(0); gh.append(0); continue }
    var px = [UInt8](repeating: 0, count: w * height)
    let ctx = CGContext(data: &px, width: w, height: height, bitsPerComponent: 8,
                        bytesPerRow: w, space: CGColorSpaceCreateDeviceGray(),
                        bitmapInfo: CGImageAlphaInfo.none.rawValue)!
    ctx.setAllowsAntialiasing(true)
    ctx.setShouldAntialias(true)
    ctx.setAllowsFontSmoothing(false)
    ctx.setShouldSmoothFonts(false)
    ctx.setAllowsFontSubpixelPositioning(false)
    ctx.setAllowsFontSubpixelQuantization(false)
    ctx.setFillColor(gray: 1, alpha: 1)
    var gg = g
    var pos = CGPoint(x: CGFloat(-x0), y: CGFloat(-bottom + (pad - pad / 2)))
    CTFontDrawGlyphs(font, &gg, &pos, 1, ctx)
    // CGContext rows run bottom-up in memory? No: row 0 is the top row.
    var nib = [UInt8]()
    for v in px {
        // slight contrast boost so thin stems stay dark on a small screen
        let a = pow(Double(v) / 255.0, 0.8)
        nib.append(UInt8(min(15, Int((a * 15).rounded()))))
    }
    // keep only the rows with ink (YOFF, GH): pixels unchanged, much smaller
    let inked = (0..<height).filter { r in nib[(r * w)..<((r + 1) * w)].contains { $0 != 0 } }
    let y0 = inked.first ?? 0
    let y1 = inked.last ?? -1
    yoff.append(UInt8(y0))
    gh.append(UInt8(y1 - y0 + 1))
    nib = y1 >= y0 ? Array(nib[(y0 * w)..<((y1 + 1) * w)]) : []
    if nib.count % 2 == 1 { nib.append(0) }
    for k in stride(from: 0, to: nib.count, by: 2) { bitmap.append(nib[k] << 4 | nib[k + 1]) }
}

func lit(_ b: [UInt8]) -> String {
    var s = "b'"
    for (i, v) in b.enumerated() {
        if i > 0 && i % 40 == 0 { s += "'\\\nb'" }
        s += String(format: "\\x%02x", v)
    }
    return s + "'"
}
let text = String(utf16CodeUnits: chars, count: chars.count)
    .replacingOccurrences(of: "\\", with: "\\\\").replacingOccurrences(of: "\"", with: "\\\"")
var out = "# Generated by tools/makefont.swift from \(name) (Apache-2.0 font). Do not edit.\n"
// WIDTH: widest ASCII advance, for code that counts characters per line
let maxw = chars.indices.filter { chars[$0] < 127 }.map { Int(adv[$0]) }.max()!
out += "NAME = \"\(name)\"\nHEIGHT = \(height)\nBASELINE = \(baseline)\nWIDTH = \(maxw)\n"
out += "CHARS = \"\(text)\"\n"
out += "ADV = \(lit(adv))\nXOFF = \(lit(xoff))\nGW = \(lit(gw))\nYOFF = \(lit(yoff))\nGH = \(lit(gh))\nOFFS = \(lit(offs))\n"
out += "BITMAP =\\\n\(lit(bitmap))\n"
try! out.write(toFile: args[2], atomically: true, encoding: .utf8)
print("\(name): em \(CTFontGetSize(font)) px, height \(height), \(chars.count) glyphs, \(bitmap.count) bytes")
