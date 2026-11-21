"""Builds SteelTemple Sanctum for Windows: build/SteelTemple-Sanctum.exe with its assets beside it.

What: every src/*.cpp and Dear ImGui (third_party/imgui, its SDL3 and SDL_Renderer back ends) compiled with
llvm-mingw's clang++ and linked statically against SDL3, so the program needs no DLL next to it.
Needs: llvm-mingw (--llvm-mingw or SANCTUM_LLVM_MINGW: the folder holding bin/) and a static SDL3 3.4 (--sdl or
SANCTUM_SDL: a prefix with include/SDL3 and lib/libSDL3.a, e.g. built with -DSDL_STATIC=ON).
How: `python tools/build.py [--debug]`. Objects are kept in build/obj and rebuilt only when their source or a header
changed. Nothing personal is packaged: the player's settings live in %APPDATA%, never in build/.
"""

import argparse
import os
import shutil
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build"
IMGUI = ROOT / "third_party/imgui"
SOURCES = sorted((ROOT / "src").glob("*.cpp")) + [
    IMGUI / "imgui.cpp", IMGUI / "imgui_draw.cpp", IMGUI / "imgui_tables.cpp", IMGUI / "imgui_widgets.cpp",
    IMGUI / "backends/imgui_impl_sdl3.cpp", IMGUI / "backends/imgui_impl_sdlrenderer3.cpp"]
LIBS = ["-lsetupapi", "-lwinmm", "-limm32", "-lversion", "-lole32", "-loleaut32", "-luuid", "-lgdi32", "-luser32",
        "-lshell32", "-ladvapi32", "-lcfgmgr32", "-lhid", "-lwinhttp", "-lbcrypt"]


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--llvm-mingw", default=os.environ.get("SANCTUM_LLVM_MINGW"))
    ap.add_argument("--sdl", default=os.environ.get("SANCTUM_SDL"))
    ap.add_argument("--debug", action="store_true", help="no optimisation, a console for logs")
    ap.add_argument("--name", default="SteelTemple-Sanctum", help="the program's file name (a test copy while one runs)")
    a = ap.parse_args()
    if not a.llvm_mingw or not a.sdl:
        sys.exit("give --llvm-mingw and --sdl (or set SANCTUM_LLVM_MINGW and SANCTUM_SDL)")
    exe = ".exe" if sys.platform == "win32" else ""
    cxx = Path(a.llvm_mingw) / f"bin/x86_64-w64-mingw32-clang++{exe}"
    sdl = Path(a.sdl)
    flags = ["-std=c++20", "-DUNICODE", "-D_UNICODE", f"-I{sdl / 'include'}", f"-I{IMGUI}", f"-I{IMGUI / 'backends'}",
             f"-I{ROOT / 'third_party/stb'}", "-Wall", "-Wno-unused-function"]
    flags += ["-O0", "-g"] if a.debug else ["-O2"]
    obj_dir = OUT / ("obj-debug" if a.debug else "obj")
    obj_dir.mkdir(parents=True, exist_ok=True)
    headers = max(p.stat().st_mtime for p in (ROOT / "src").glob("*.h"))

    def compile_one(src: Path) -> Path:
        obj = obj_dir / (src.stem + ".o")
        newest = max(src.stat().st_mtime, headers if src.parent.name == "src" else 0)
        if obj.exists() and obj.stat().st_mtime >= newest:
            return obj
        r = subprocess.run([str(cxx), *flags, "-c", str(src), "-o", str(obj)], capture_output=True, text=True)
        if r.returncode:
            sys.exit(f"{src.name}:\n{r.stderr}")
        if r.stderr.strip():
            print(r.stderr, file=sys.stderr)
        print(f"  {src.name}")
        return obj

    with ThreadPoolExecutor() as pool:
        objs = list(pool.map(compile_one, SOURCES))
    # the icon, the manifest (per-monitor DPI, UTF-8) and the version, from assets/sanctum.rc
    res = obj_dir / "sanctum.res.o"
    windres = Path(a.llvm_mingw) / f"bin/x86_64-w64-mingw32-windres{exe}"
    subprocess.run([str(windres), "-O", "coff", "-I", str(ROOT / "assets"), str(ROOT / "assets/sanctum.rc"), "-o", str(res)],
                   check=True)
    target = OUT / f"{a.name}.exe"
    link = [str(cxx), *objs, str(res), "-o", str(target), "-static", f"-L{sdl / 'lib'}", "-lSDL3", *LIBS]
    link.append("-mconsole" if a.debug else "-mwindows")
    subprocess.run(link, check=True)
    shutil.copytree(ROOT / "assets", OUT / "assets", dirs_exist_ok=True)
    print(f"{target}: {target.stat().st_size / 2**20:.1f} MiB")


if __name__ == "__main__":
    main()
