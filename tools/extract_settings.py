"""Reads each port's in-game settings menu from its source tree into assets/settings/<port>.json for Sanctum.

What: for every port in tools/settings_sources.local.json (the player's own source paths; never committed), it reads
  - runtime cfg.c        metas[] (key, default, accepted values, live or at restart) and sections[] (the ini section
                         a key is written to when the file lacks it)
  - the menu's tabs      tab_<name>() functions in menu order (kTabNames), and in them, in order, every
                         opt_group / opt_toggle / opt_choice / opt_choice_list / opt_slider row, following calls to
                         helper functions that draw rows; each row's label and description id and its value names
  - the English text     ui_text.c's text_en[] table, or i18n/<name>.pot (THWOMP)
and writes one JSON list of pages and rows. Rows the menu draws by hand (a window-size field, a GPU picker) and
free-text keys are left out: Sanctum shows only what it can set with a controller.
Why: Sanctum's Settings tab draws each port's own settings, in its own order and words, with no change to the ports
(DESIGN.md, SETTINGS_AND_DIRECT_START.md option B). Each file names the commit it was read from.
How: `python tools/extract_settings.py` (all ports) or `python tools/extract_settings.py shadowracket`.
settings_sources.local.json: {"<port id>": {"root": "<source tree>", "ini": "mtf.ini"}, ...}
"""

import json
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "assets/settings"


def first(root: Path, *rels: str) -> Path | None:
    for r in rels:
        p = root / r
        if p.is_file():
            return p
    return None


def strip_comments(src: str) -> str:
    src = re.sub(r"/\*.*?\*/", " ", src, flags=re.S)
    return re.sub(r"//[^\n]*", "", src)


# ---- cfg.c
def read_cfg(path: Path) -> tuple[dict, dict]:
    src = strip_comments(path.read_text(encoding="utf-8", errors="replace"))
    metas, sections = {}, {}
    m = re.search(r"metas\[\]\s*=\s*\{(.*?)\n\};", src, re.S)
    for key, d, vals, live in re.findall(r'\{\s*"([A-Z0-9_]+)"\s*,\s*"([^"]*)"\s*,\s*(NULL|"[^"]*")\s*,\s*(\w+)\s*\}', m.group(1)):
        metas[key] = {"default": d, "values": None if vals == "NULL" else vals.strip('"'), "live": live in ("LIVE", "1")}
    m = re.search(r"sections\[\]\s*=\s*\{(.*?)\};", src, re.S)
    if m:
        for key, sec in re.findall(r'\{\s*"([A-Z0-9_]+)"\s*,\s*"([^"]*)"\s*\}', m.group(1)):
            sections[key] = sec
    return metas, sections


# ---- English text
def read_text(root: Path) -> dict:
    text = {}
    pot = next(iter(sorted((root / "i18n").glob("*.pot"))), None) if (root / "i18n").is_dir() else None
    if pot:
        for ctx, msg in re.findall(r'msgctxt "([A-Z0-9_]+)"\s*\nmsgid ((?:"(?:[^"\\]|\\.)*"\s*)+)', pot.read_text(encoding="utf-8")):
            text[ctx] = "".join(json.loads(part) for part in re.findall(r'"(?:[^"\\]|\\.)*"', msg))
        return text
    uit = first(root, "runtime/ui_text.c", "runtime/gfx/nvn/host/ui_text.c")
    src = uit.read_text(encoding="utf-8", errors="replace")
    m = re.search(r"text_en\[[^\]]*\]\s*=\s*\{(.*?)\n\};", src, re.S)
    body = m.group(1)
    # the macros the table starts with (SWKBD(...), LANG_NAMES) hold [ID] = "..." pairs too
    for mac in re.findall(r"#define \w+(?:\([^)]*\))?\s*\\?\n?((?:.*\\\n)*.*)", src):
        body += "\n" + mac
    for tid, s in re.findall(r'\[(UI_[A-Z0-9_]+)\]\s*=\s*((?:"(?:[^"\\]|\\.)*"\s*)+)', body):
        if tid not in text:
            text[tid] = "".join(json.loads(part) for part in re.findall(r'"(?:[^"\\]|\\.)*"', s))
    return text


# ---- the menu
def functions(src: str) -> dict:
    """name -> body of every function in the file (brace matched)."""
    out = {}
    for m in re.finditer(r"\n(?:static\s+)?(?:inline\s+)?[\w:<>\*\s]+?\b(\w+)\s*\([^;{)]*\)\s*\{", src):
        i, depth = m.end(), 1
        while i < len(src) and depth:
            depth += {"{": 1, "}": -1}.get(src[i], 0)
            i += 1
        out.setdefault(m.group(1), src[m.end():i - 1])
    return out


def split_args(s: str) -> list[str]:
    args, depth, cur, q = [], 0, "", False
    for c in s:
        if c == '"':
            q = not q
        if not q and c in "([{":
            depth += 1
        if not q and c in ")]}":
            depth -= 1
        if c == "," and depth == 0 and not q:
            args.append(cur.strip())
            cur = ""
        else:
            cur += c
    if cur.strip():
        args.append(cur.strip())
    return args


def call_args(body: str, start: int) -> tuple[list[str], int]:
    i, depth = start, 1
    while depth and i < len(body):
        depth += {"(": 1, ")": -1}.get(body[i], 0)
        i += 1
    return split_args(body[start:i - 1]), i


def array_items(body: str, src: str, name: str, pos: int) -> list[str]:
    """The array `name` as the row at pos sees it: the nearest definition before it in the function (one function may
    define `vals` in several blocks), else one at file scope."""
    pat = re.compile(r"\b" + re.escape(name) + r"\s*\[[^\]]*\]\s*=\s*\{(.*?)\};", re.S)
    before = [m for m in pat.finditer(body) if m.start() < pos]
    if before:
        return split_args(before[-1].group(1))
    m = pat.search(src)
    return split_args(m.group(1)) if m else []


# names the catalogue does not carry (THWOMP keeps language names outside its .pot)
LANG_NAMES = {"EN": "English", "JA": "日本語", "FR": "Français", "DE": "Deutsch", "IT": "Italiano", "ES": "Español",
              "ES_419": "Español (Latinoamérica)", "NL": "Nederlands", "RU": "Русский", "KO": "한국어",
              "ZH_HANS": "简体中文", "ZH_HANT": "繁體中文", "CA": "Català", "EU": "Euskara", "GL": "Galego",
              "PT": "Português", "PT_BR": "Português (Brasil)", "PL": "Polski"}


def name_of(item: str, text: dict) -> str:
    item = item.strip()
    m = re.fullmatch(r'"((?:[^"\\]|\\.)*)"', item)
    if m:
        return json.loads(item)
    m = re.search(r"\b(UI_[A-Z0-9_]+)", item)
    if not m:
        return item
    tid = m.group(1)
    if tid in text:
        return text[tid]
    lang = re.fullmatch(r"UI_MENU_LANGNAME_(\w+)", tid)
    return LANG_NAMES.get(lang.group(1), lang.group(1)) if lang else tid


def num(s: str) -> float:
    return float(re.sub(r"[fF]$", "", s.strip()))


def rows_of(fn: str, fns: dict, src: str, text: dict, seen: set) -> list[dict]:
    if fn in seen or fn not in fns:
        return []
    seen = seen | {fn}
    body, rows = fns[fn], []
    for m in re.finditer(r"\b(opt_group|opt_toggle|opt_choice_list|opt_choice|opt_slider|(\w+))\s*\(", body):
        kind = m.group(1)
        if m.group(2):   # a helper that may draw rows
            if m.group(2) in fns and m.group(2) != fn and re.search(r"\bopt_(toggle|choice|slider|group)\b", fns[m.group(2)]):
                rows += rows_of(m.group(2), fns, src, text, seen)
            continue
        a, _ = call_args(body, m.end())
        if kind == "opt_group":
            rows.append({"type": "group", "label": name_of(a[0], text)})
            continue
        key = a[0].strip('"')
        if not re.fullmatch(r"[A-Z0-9]+_[A-Z0-9_]+", key):
            continue
        r = {"key": key, "label": name_of(a[1], text)}
        if kind == "opt_toggle":
            r.update(type="toggle", desc=name_of(a[2], text), inverted=len(a) > 3 and a[3].strip() == "true")
        elif kind == "opt_choice":
            r.update(type="choice", names=[name_of(x, text) for x in array_items(body, src, a[2], m.start())], desc=name_of(a[4], text))
        elif kind == "opt_choice_list":
            r.update(type="choice", values=[json.loads(x) for x in array_items(body, src, a[2], m.start()) if x.startswith('"')],
                     names=[name_of(x, text) for x in array_items(body, src, a[3], m.start())], desc=name_of(a[5], text))
        elif kind == "opt_slider":
            r.update(type="slider", lo=num(a[2]), hi=num(a[3]), desc=name_of(a[4], text),
                     step=num(a[5]) if len(a) > 5 else 1.0, fmt=json.loads(a[6]) if len(a) > 6 and a[6].startswith('"') else "%.0f")
        rows.append(r)
    return rows


def read_menu(root: Path, text: dict) -> list[dict]:
    path = first(root, "runtime/ui/menu/settings_tabs.cpp", "runtime/ui/menu.cpp")
    src = strip_comments(path.read_text(encoding="utf-8", errors="replace"))
    fns = functions(src)
    tabs = re.search(r"kTabNames\[[^\]]*\]\s*=\s*\{(.*?)\};", src, re.S).group(1)
    pages = []
    for tid in re.findall(r"UI_MENU_TAB_([A-Z0-9_]+)", tabs):
        fn = "tab_" + tid.lower()
        rows = rows_of(fn, fns, src, text, set())
        if any(r["type"] != "group" for r in rows):
            pages.append({"title": text.get("UI_MENU_TAB_" + tid, tid.title()), "rows": rows})
    return pages


def finish(pages: list[dict], metas: dict, sections: dict) -> tuple[list[dict], str]:
    """Each row gets its ini key and section, default, values and live flag; rows Sanctum cannot set are dropped."""
    prefix = ""
    for p in pages:
        for r in p["rows"]:
            if "key" in r:
                prefix = r["key"].split("_", 1)[0] + "_"
    out = []
    for p in pages:
        rows = []
        for r in p["rows"]:
            if r["type"] == "group":
                rows.append(r)
                continue
            name = r.pop("key")[len(prefix):]
            meta = metas.get(name)
            if not meta:
                continue
            r.update(key=name, ini=name.lower(), section=sections.get(name, ""), default=meta["default"], live=meta["live"])
            vals = meta["values"]
            if r["type"] == "toggle":
                r["values"] = ["0", "1"]
            elif r["type"] == "choice":
                if "values" not in r:
                    listed = vals.split("|") if vals and ".." not in vals else []
                    if meta["default"] == "" and len(r["names"]) == len(listed) + 1:
                        listed = [""] + listed   # names[0] is the automatic choice (the key unset)
                    r["values"] = listed
                n = min(len(r["values"]), len(r["names"]))
                if n < 2:
                    continue
                r["values"], r["names"] = r["values"][:n], r["names"][:n]
            rows.append(r)
        # no empty groups (a heading followed by nothing, or by another heading)
        clean = [r for i, r in enumerate(rows) if r["type"] != "group" or (i + 1 < len(rows) and rows[i + 1]["type"] != "group")]
        if any(r["type"] != "group" for r in clean):
            out.append({"title": p["title"], "rows": clean})
    return out, prefix


def source_of(root: Path) -> str:
    try:
        g = lambda *a: subprocess.run(["git", "-C", str(root), *a], capture_output=True, text=True, check=True).stdout.strip()  # noqa: E731
        return f"{g('rev-parse', '--short', 'HEAD')} ({g('log', '-1', '--format=%cd', '--date=short')}, {g('log', '-1', '--format=%s')[:70]})"
    except (OSError, subprocess.CalledProcessError):
        return "unknown"


def main() -> None:
    cfgf = ROOT / "tools/settings_sources.local.json"
    if not cfgf.is_file():
        sys.exit(f"{cfgf}: list each port's source tree there (see this file's docstring)")
    sources = json.loads(cfgf.read_text(encoding="utf-8"))
    OUT.mkdir(parents=True, exist_ok=True)
    for port, s in sources.items():
        if len(sys.argv) > 1 and port not in sys.argv[1:]:
            continue
        root = Path(s["root"])
        cfgc = first(root, "runtime/cfg.c", "runtime/core/base/cfg.c")
        metas, sections = read_cfg(cfgc)
        text = read_text(root)
        pages, prefix = finish(read_menu(root, text), metas, sections)
        data = {"port": port, "ini": s["ini"], "prefix": prefix, "read_from": source_of(root), "pages": pages}
        (OUT / f"{port}.json").write_text(json.dumps(data, indent=1, ensure_ascii=False) + "\n", encoding="utf-8")
        n = sum(1 for p in pages for r in p["rows"] if r["type"] != "group")
        print(f"{port}: {n} settings on {len(pages)} pages ({', '.join(p['title'] for p in pages)}), {len(metas)} keys in cfg.c")


if __name__ == "__main__":
    main()
