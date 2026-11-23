"""Packs a built Sanctum into its release zip: dist/SteelTemple-Sanctum-<version>-Portable.zip, holding one folder
"SteelTemple Sanctum" with the program and its assets. Nothing of the player's goes in: their settings live in
%APPDATA% and %LOCALAPPDATA%, never beside the program.

    python tools/build.py ...        # first
    python tools/package.py          # the version comes from src/selfupdate.h (SANCTUM_VERSION)
"""
import hashlib
import re
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BUILD = ROOT / "build"
SKIP = {".rc", ".manifest"}   # build inputs, not needed beside the program

README = """SteelTemple Sanctum {v}

Start SteelTemple-Sanctum.exe. Keep this folder together: the program needs its assets folder.
Sanctum updates itself from GitHub (System Settings > About), and never downloads game files.

Tip: Press the controller Guide / PS / Home button anytime (even in-game) to slide open
Sanctum's Quick Overlay (Quick Suspend & Resume, System Volume, Close / Force Quit). If a game
ever freezes, you can also hold Back + Start + L + R for 2 seconds to force-close it cleanly.

https://github.com/STS78721/SteelTempleSanctum
"""


def main():
    v = re.search(r'#define SANCTUM_VERSION "([^"]+)"', (ROOT / "src/selfupdate.h").read_text(encoding="utf-8")).group(1)
    exe = BUILD / "SteelTemple-Sanctum.exe"
    if not exe.is_file():
        raise SystemExit("build first: python tools/build.py ...")
    out = ROOT / "dist"
    out.mkdir(exist_ok=True)
    zpath = out / f"SteelTemple-Sanctum-{v}-Portable.zip"
    top = "SteelTemple Sanctum"
    with zipfile.ZipFile(zpath, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        z.write(exe, f"{top}/SteelTemple-Sanctum.exe")
        for f in sorted((BUILD / "assets").rglob("*")):
            if f.is_file() and f.suffix not in SKIP:
                z.write(f, f"{top}/assets/{f.relative_to(BUILD / 'assets').as_posix()}")
        z.writestr(f"{top}/README.txt", README.format(v=v).replace("\n", "\r\n"))
        for lic in sorted((ROOT / "third_party").rglob("LICENSE*")):
            z.write(lic, f"{top}/licenses/{lic.parent.name}-{lic.name}")
    digest = hashlib.sha256(zpath.read_bytes()).hexdigest()
    print(f"{zpath}  {zpath.stat().st_size / 1e6:.1f} MB  sha256 {digest}")


if __name__ == "__main__":
    main()
