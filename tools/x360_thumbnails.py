"""Rewrite Xbox 360 track thumbnails (thumbnail.thn) in the PC format.

Both platforms wrap the picture in the same header (u32 0x751e7ace, u8 1, u32 BE size of the rest),
but the 360 stores a JPEG XR (HD Photo) image and the PC game reads a JPEG:
  360: ce7a1e75 01 <u32 BE n> <u32 BE n> JPEG XR ...
  PC:  ce7a1e75 01 <u32 BE n> <u32 LE n> <u32 LE width> <u32 LE height> JPEG ...   (n = 12 + JPEG size)
Without this Track Central shows no screenshot for 360 tracks (and the game logs a bogus size).

JPEG XR is decoded with Windows' own codec (WPF WmpBitmapDecoder) through PowerShell, so it only
runs on Windows. The 360 wrote its pixels big-endian (A R G B), so the decoded "BGRA" bytes hold
A, R, G, B, and its rows are stored bottom-up: both are fixed before the JPEG is written (decoded
as-is, thumbnails came out green/yellow with blue ~0, and upside down). Folders whose thumbnail is already a PC one are left alone, unless
--packages points at the original 360 packages to take the thumbnails from.

Usage: python tools/x360_thumbnails.py <folder of track folders> [more folders...]
                                       [--packages <folder of 360 packages>]
"""
import os
import struct
import subprocess
import sys
import tempfile

THN_MAGIC = bytes.fromhex("ce7a1e75")
JXR_MAGIC = b"II\xbc"

# Converts every *.jxr in a folder to a same-named .jpg (inline -Command: no script file needed).
CONVERT = r"""
Add-Type -AssemblyName PresentationCore
Add-Type -TypeDefinition 'public static class X360Pixels {{ public static byte[] ToBgr(byte[] argbAsBgra, int w, int h) {{ var o = new byte[w * h * 3]; for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {{ int i = (y * w + x) * 4, j = ((h - 1 - y) * w + x) * 3; o[j] = argbAsBgra[i+3]; o[j+1] = argbAsBgra[i+2]; o[j+2] = argbAsBgra[i+1]; }} return o; }} }}'
$ok = 0; $bad = 0
foreach ($f in Get-ChildItem -LiteralPath '{dir}' -Filter *.jxr) {{
  $in = $null; $out = $null
  try {{
    $in = [IO.File]::OpenRead($f.FullName)
    $dec = New-Object System.Windows.Media.Imaging.WmpBitmapDecoder($in, 1, 1)
    $src = New-Object System.Windows.Media.Imaging.FormatConvertedBitmap($dec.Frames[0], [System.Windows.Media.PixelFormats]::Bgra32, $null, 0)
    $w = $src.PixelWidth; $h = $src.PixelHeight
    $raw = New-Object byte[] ($w * 4 * $h); $src.CopyPixels($raw, $w * 4, 0)
    $rgb = [X360Pixels]::ToBgr($raw, $w, $h)
    $px = [System.Windows.Media.Imaging.BitmapSource]::Create($w, $h, 96, 96, [System.Windows.Media.PixelFormats]::Bgr24, $null, $rgb, $w * 3)
    $enc = New-Object System.Windows.Media.Imaging.JpegBitmapEncoder
    $enc.QualityLevel = 85
    $enc.Frames.Add([System.Windows.Media.Imaging.BitmapFrame]::Create($px))
    $out = [IO.File]::Create([IO.Path]::ChangeExtension($f.FullName, '.jpg'))
    $enc.Save($out)
    $out.Close(); $out = $null
    [IO.File]::WriteAllText([IO.Path]::ChangeExtension($f.FullName, '.size'), "$($px.PixelWidth) $($px.PixelHeight)")
    $ok++
  }} catch {{ $bad++ }} finally {{ if ($out) {{ $out.Close() }}; if ($in) {{ $in.Close() }} }}
}}
"converted $ok, failed $bad"
"""


def jxr_payload(thn):
    if thn[:4] != THN_MAGIC or len(thn) < 17:
        return None
    i = thn.find(JXR_MAGIC, 9, 32)
    return thn[i:] if i >= 0 else None


def pc_thumbnail(jpeg, width, height):
    n = 12 + len(jpeg)
    return THN_MAGIC + b"\x01" + struct.pack(">I", n) + struct.pack("<III", n, width, height) + jpeg


def package_thumbnails(package_root):
    """Folder name (= package file name) -> original 360 thumbnail.thn bytes, from the packages."""
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from stfs_extract import STFS
    out = {}
    for dp, _, files in os.walk(package_root):
        for name in files:
            try:
                pkg = STFS(os.path.join(dp, name))
                ents = pkg.entries()
                e = next(e for e in ents if e and e["name"] == "thumbnail.thn")
                data = b"".join(pkg.read_block(b) for b in pkg.blocks(e["start"], e["blocks"], e["consecutive"]))
                out[name] = data[:e["size"]]
            except Exception:                                # noqa: BLE001 (not a package / no thumbnail)
                continue
    return out


def convert_folders(roots, log=print, originals=None):
    """Rewrite every 360-format thumbnail.thn under the given folders (or, with `originals`
    {folder name: original 360 thn}, every listed folder's). Returns (converted, failed)."""
    jobs = {}
    for root in roots:
        if originals is not None:
            for name in os.listdir(root):
                path = os.path.join(root, name, "thumbnail.thn")
                payload = jxr_payload(originals.get(name, b""))
                if payload and os.path.isfile(path):
                    jobs.setdefault(name, []).append((path, payload))
            continue
        for name in os.listdir(root):
            path = os.path.join(root, name, "thumbnail.thn")
            if os.path.isfile(path):
                payload = jxr_payload(open(path, "rb").read())
                if payload:
                    jobs.setdefault(name, []).append((path, payload))
    if not jobs:
        return 0, 0
    with tempfile.TemporaryDirectory() as tmp:
        for name, items in jobs.items():
            open(os.path.join(tmp, name + ".jxr"), "wb").write(items[0][1])
        result = subprocess.run(["powershell", "-NoProfile", "-NonInteractive", "-Command",
                                 CONVERT.format(dir=tmp.replace("'", "''"))],
                                capture_output=True, text=True)
        log(result.stdout.strip() or result.stderr.strip())
        done = failed = 0
        for name, items in jobs.items():
            jpg, size = os.path.join(tmp, name + ".jpg"), os.path.join(tmp, name + ".size")
            if not (os.path.exists(jpg) and os.path.exists(size)):
                failed += len(items)
                continue
            width, height = map(int, open(size).read().split())
            thn = pc_thumbnail(open(jpg, "rb").read(), width, height)
            for path, _ in items:
                with open(path + ".tmp", "wb") as f:
                    f.write(thn)
                os.replace(path + ".tmp", path)
                done += 1
    return done, failed


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    args = sys.argv[1:]
    originals = None
    if "--packages" in args:
        i = args.index("--packages")
        originals = package_thumbnails(args[i + 1])
        print(f"{len(originals)} original thumbnails read from the packages")
        del args[i:i + 2]
    done, failed = convert_folders(args, originals=originals)
    print(f"{done} thumbnails rewritten, {failed} failed")
