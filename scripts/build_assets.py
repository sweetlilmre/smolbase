# /// script
# requires-python = ">=3.11"
# dependencies = ["fonttools>=4.53", "pillow>=10", "resvg-cli>=0.2", "cryptography>=42"]
# ///
"""Weather dashboard asset pipeline (#71; design: docs/research/
dashboard-asset-pipeline.md). Reads scripts/assets.toml, fetches the pinned
inputs (hash-locked in scripts/assets.lock.toml — a drifted upstream is a
hard error), instances the variable fonts, renders faces with
lv_font_conv@1.5.3 (npx) to LVGL bin format (LovyanGFX's BFFfont loads it
natively), rasterizes the Meteocons SVGs with resvg and quantizes to 4-bit
indexed, then emits src/app-weather/assets/wx_assets.{h,cpp}.

    uv run scripts/build_assets.py

Needs node/npx on PATH for lv_font_conv. Downloads cache in
scripts/.assets-cache/ (gitignored); only the emitted sources are committed.
"""
import hashlib
import io
import shutil
import subprocess
import sys
import tomllib
import urllib.request
from pathlib import Path

from fontTools.ttLib import TTFont
from fontTools.varLib.instancer import instantiateVariableFont
from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
CACHE = ROOT / "scripts" / ".assets-cache"
OUT = ROOT / "src" / "app-weather" / "assets"
LOCK = ROOT / "scripts" / "assets.lock.toml"
LICDIR = ROOT / "docs" / "licenses"
LV_FONT_CONV = "lv_font_conv@1.5.3"


def fetch(url: str, name: str, lock: dict, lock_out: dict) -> Path:
    dest = CACHE / name
    if not dest.exists():
        print(f"  fetch {url}")
        with urllib.request.urlopen(url) as r:
            dest.write_bytes(r.read())
    digest = hashlib.sha256(dest.read_bytes()).hexdigest()
    if name in lock and lock[name] != digest:
        sys.exit(f"HASH DRIFT for {name}: lock {lock[name][:12]}… got {digest[:12]}… "
                 f"— upstream changed; delete the cache and re-verify deliberately.")
    lock_out[name] = digest
    return dest


def instance_font(src: Path, weight: int) -> Path:
    out = CACHE / f"{src.stem}-w{weight}.ttf"
    if not out.exists():
        print(f"  instance {src.name} @ wght={weight}")
        f = TTFont(src)
        instantiateVariableFont(f, {"wght": weight}, inplace=True)
        f.save(out)
    return out


def render_font(name: str, ttf: Path, spec: dict) -> bytes:
    out = CACHE / f"{name}.bin"
    npx = shutil.which("npx") or shutil.which("npx.cmd")
    if not npx:
        sys.exit("npx not found — node is required for lv_font_conv")
    # --no-compress: first flight (#74) showed LovyanGFX 1.2.26's BFF RLE
    # decode double-striking glyphs; raw bits render correctly and cost only
    # ~15 KB more flash. The research doc named this exact fallback switch.
    cmd = [npx, "--yes", LV_FONT_CONV, "--font", str(ttf), "--size", str(spec["size"]),
           "--bpp", "4", "-r", spec["range"], "--format", "bin", "--no-compress",
           "-o", str(out)]
    if not spec.get("kerning", True):
        cmd.append("--no-kerning")
    print(f"  lv_font_conv {name} ({spec['size']} px, {spec['range']})")
    subprocess.run(cmd, check=True)
    return out.read_bytes()


def render_icon(name: str, svg: Path, w: int, h: int, trim: bool,
                rotate: float = 0) -> tuple[list[int], bytes]:
    """SVG → (16-entry RGB565 palette, 4bpp data). Index 0 = transparent;
    AA is baked against black (the panel background)."""
    png = CACHE / f"{name}.png"
    scale = 4  # supersample, then downscale for clean edges at small sizes
    # resvg-cli installs a `resvg` console script into the venv's Scripts dir.
    resvg = shutil.which("resvg") or str(Path(sys.executable).parent / "resvg")
    subprocess.run([resvg, str(svg), str(png),
                    "--width", str(w * scale), "--height", str(h * scale)], check=True)
    img = Image.open(png).convert("RGBA")
    if rotate:  # CCW degrees, before trim so the crop hugs the rotated ink
        img = img.rotate(rotate, expand=True, resample=Image.BICUBIC)
    if trim:
        bbox = img.getchannel("A").getbbox()
        if bbox:
            img = img.crop(bbox)
    # Fit into the slot PRESERVING ASPECT (resvg renders the square viewBox
    # regardless of --height; a blind resize stretched the thermometer into
    # a blob — #74 round 4). Centered on transparent.
    f = min(w / img.width, h / img.height)
    nw, nh = max(1, round(img.width * f)), max(1, round(img.height * f))
    img = img.resize((nw, nh), Image.LANCZOS)
    canvas = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    canvas.paste(img, ((w - nw) // 2, (h - nh) // 2))
    img = canvas

    # Composite over black, quantize the opaque pixels to 15 colors.
    alpha = img.getchannel("A")
    black = Image.new("RGBA", img.size, (0, 0, 0, 255))
    flat = Image.alpha_composite(black, img).convert("RGB")
    quant = flat.quantize(colors=15, method=Image.MEDIANCUT)
    qpal = quant.getpalette()[: 15 * 3]

    pal565 = [0]  # index 0: transparent (drawn with the transparent-index overload)
    for i in range(15):
        r, g, b = qpal[i * 3 : i * 3 + 3]
        pal565.append(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3))

    px = quant.load()
    apx = alpha.load()
    nibbles = []
    for y in range(h):
        for x in range(w):
            nibbles.append(0 if apx[x, y] < 128 else px[x, y] + 1)
    if len(nibbles) % 2:
        nibbles.append(0)
    data = bytes((nibbles[i] << 4) | nibbles[i + 1] for i in range(0, len(nibbles), 2))
    return pal565, data


def c_array(data: bytes) -> str:
    lines = []
    for i in range(0, len(data), 16):
        lines.append("  " + ", ".join(f"0x{b:02x}" for b in data[i : i + 16]) + ",")
    return "\n".join(lines)


def main() -> None:
    spec = tomllib.loads((ROOT / "scripts" / "assets.toml").read_text())
    CACHE.mkdir(exist_ok=True)
    OUT.mkdir(exist_ok=True)
    LICDIR.mkdir(exist_ok=True)
    lock = tomllib.loads(LOCK.read_text()) if LOCK.exists() else {}
    lock_out: dict[str, str] = {}

    print("fetch:")
    fonts_ttf = {}
    for fam in ("teko", "montserrat"):
        src = spec["sources"][fam]
        fonts_ttf[fam] = fetch(src["url"], f"{fam}.ttf", lock, lock_out)
        lic = fetch(src["license_url"], f"{fam}-OFL.txt", lock, lock_out)
        shutil.copy(lic, LICDIR / f"OFL-{fam}.txt")
    svgs = {}
    for ic in spec["icons"].values():
        key = (ic.get("set", "meteocons"), ic["icon"])
        if key not in svgs:
            svgs[key] = fetch(f"{spec['sources'][key[0]]['base']}/{key[1]}.svg",
                              f"{key[0]}-{key[1]}.svg", lock, lock_out)
    lic = fetch(spec["sources"]["meteocons"]["license_url"], "meteocons-LICENSE", lock, lock_out)
    shutil.copy(lic, LICDIR / "MIT-meteocons.txt")
    lic = fetch(spec["sources"]["twemoji"]["license_url"], "twemoji-LICENSE", lock, lock_out)
    shutil.copy(lic, LICDIR / "CC-BY-twemoji.txt")
    cacert = fetch(spec["sources"]["ca_bundle"]["cacert_url"], "cacert.pem", lock, lock_out)
    genscript = fetch(spec["sources"]["ca_bundle"]["gen_url"], "gen_crt_bundle.py", lock, lock_out)

    print("ca bundle:")
    subprocess.run([sys.executable, str(genscript), "--input", str(cacert)],
                   cwd=CACHE, check=True)
    ca_bundle = (CACHE / "x509_crt_bundle").read_bytes()
    print(f"  {len(ca_bundle)} B from {cacert.name}")

    print("fonts:")
    font_bins = {}
    for name, f in spec["fonts"].items():
        ttf = instance_font(fonts_ttf[f["family"]], f["weight"])
        font_bins[name] = render_font(name, ttf, f)

    print("icons:")
    icons = {}
    for name, ic in spec["icons"].items():
        w = ic.get("width", ic.get("size"))
        h = ic.get("height", ic.get("size"))
        svg = svgs[(ic.get("set", "meteocons"), ic["icon"])]
        icons[name] = (w, h) + render_icon(name, svg, w, h, ic.get("trim", False),
                                           ic.get("rotate", 0))

    print("emit:")
    banner = ("// GENERATED by scripts/build_assets.py from scripts/assets.toml — DO NOT EDIT.\n"
              "// Fonts: Teko, Montserrat (OFL-1.1); icons: Meteocons by Bas Milius (MIT).\n"
              "// See docs/THIRD-PARTY.md and docs/licenses/.\n")
    codes = spec["order"]["wx_codes"]

    h_lines = [banner, "#pragma once", "#include <cstddef>", "#include <cstdint>", "",
               "// 4bpp indexed image, high nibble first; palette is RGB565, index 0 transparent.",
               "struct WxIcon { uint8_t w, h; const uint16_t* palette; const uint8_t* data; };", ""]
    for name, blob in font_bins.items():
        h_lines.append(f"extern const uint8_t {name}[]; // lv_font_conv bin, {len(blob)} B")
    h_lines += ["", f"constexpr size_t WX_ICON_COUNT = {len(codes)};",
                "extern const uint8_t WX_ICON_CODES[WX_ICON_COUNT]; // OWM icon-prefix codes",
                "extern const WxIcon WX_ICONS[WX_ICON_COUNT];       // lock-step with codes",
                "extern const WxIcon WX_GAUGE_TEMP;", "extern const WxIcon WX_GAUGE_HUMI;", "",
                "// Mozilla root store in ESP bundle format (gen_crt_bundle.py) — feed to",
                "// NetworkClientSecure::setCACertBundle for HTTPS that verifies 2026 chains.",
                f"constexpr size_t WX_CA_BUNDLE_LEN = {len(ca_bundle)};",
                "extern const uint8_t WX_CA_BUNDLE[WX_CA_BUNDLE_LEN];", ""]

    c_lines = [banner, '#include "wx_assets.h"', ""]
    for name, blob in font_bins.items():
        c_lines += [f"alignas(4) const uint8_t {name}[{len(blob)}] = {{", c_array(blob), "};", ""]
    for name, (w, h, pal, data) in icons.items():
        c_lines += [f"static const uint16_t {name}_PAL[16] = {{ "
                    + ", ".join(f"0x{v:04x}" for v in pal) + " };",
                    f"alignas(4) static const uint8_t {name}_DATA[{len(data)}] = {{",
                    c_array(data), "};", ""]
    c_lines += ["const uint8_t WX_ICON_CODES[WX_ICON_COUNT] = { "
                + ", ".join(str(c) for c in codes) + " };",
                "const WxIcon WX_ICONS[WX_ICON_COUNT] = {"]
    for code in codes:
        name = f"WX_ICON_{code:02d}"
        w, h, _, _ = icons[name]
        c_lines.append(f"  {{ {w}, {h}, {name}_PAL, {name}_DATA }},")
    c_lines += ["};"]
    for name in ("WX_GAUGE_TEMP", "WX_GAUGE_HUMI"):
        w, h, _, _ = icons[name]
        c_lines.append(f"const WxIcon {name} = {{ {w}, {h}, {name}_PAL, {name}_DATA }};")
    c_lines += ["", f"alignas(4) const uint8_t WX_CA_BUNDLE[{len(ca_bundle)}] = {{",
                c_array(ca_bundle), "};", ""]

    # encoding pinned: without it Windows writes the locale codepage and the
    # banner's punctuation lands in the repo as mojibake.
    (OUT / "wx_assets.h").write_text("\n".join(h_lines), newline="\n", encoding="utf-8")
    (OUT / "wx_assets.cpp").write_text("\n".join(c_lines), newline="\n", encoding="utf-8")
    LOCK.write_text("# sha256 of every fetched input — build_assets.py hard-errors on drift.\n"
                    + "".join(f'"{k}" = "{v}"\n' for k, v in sorted(lock_out.items())),
                    newline="\n", encoding="utf-8")
    total = sum(len(b) for b in font_bins.values()) + sum(len(d) + 32 for *_ , d in icons.values())
    print(f"done: {len(font_bins)} fonts + {len(icons)} icons ~= {total // 1024} KB flash "
          f"-> {OUT.relative_to(ROOT)}")


if __name__ == "__main__":
    main()
