#!/usr/bin/env python3
"""Which app opens which file type, for the installers.

apps/common/apps.cpp (extensionTable) is the one list of which app opens which
extension. This writes it out for the installers so each app registers its own
file types with the OS:
  packaging/file-types.tsv              ext, app, mime type, own (yes = a wetwaresuite format)
  packaging/windows/associations.iss    "Open with" entries (and the default for wetwaresuite's own formats)
linux (.desktop MimeType=) and macOS (CFBundleDocumentTypes) read the tsv when packaging.

  python3 packaging/file_types.py           rewrite both
  python3 packaging/file_types.py --check   fail when either is out of date (CI)
Only extensions an app opens straight away (a command in the table) are listed, and
only single extensions (.beats.json can't be told apart from .json by the OS).
"""
import re
import sys
from pathlib import Path

root = Path(__file__).resolve().parent.parent
here = root / "packaging"

# shared-mime-info / IANA names; wetwaresuite's own formats get application/x-wetwaresuite-<ext>
MIME = {
    ".png": "image/png", ".jpg": "image/jpeg", ".jpeg": "image/jpeg", ".bmp": "image/bmp", ".tga": "image/x-tga",
    ".ppm": "image/x-portable-pixmap", ".pgm": "image/x-portable-graymap", ".ora": "image/openraster", ".gif": "image/gif",
    ".webp": "image/webp", ".psd": "image/vnd.adobe.photoshop", ".tif": "image/tiff", ".tiff": "image/tiff",
    ".svg": "image/svg+xml", ".ai": "application/illustrator", ".eps": "application/postscript", ".ttf": "font/ttf", ".otf": "font/otf", ".woff": "font/woff", ".ufo": "font/x-ufo",
    ".excalidraw": "application/vnd.excalidraw+json", ".drawio": "application/vnd.jgraph.mxfile",
    ".obj": "model/obj", ".ply": "model/x-ply", ".off": "model/x-off", ".glb": "model/gltf-binary", ".gltf": "model/gltf+json",
    ".fbx": "application/x-fbx",
    ".kicad_sch": "application/x-kicad-schematic", ".kicad_pcb": "application/x-kicad-pcb", ".kicad_pro": "application/x-kicad-project",
    ".world": "application/x-tiled-world", ".aseprite": "image/x-aseprite", ".ase": "image/x-aseprite",
    ".tscn": "application/x-godot-scene", ".tmx": "application/x-tiled-tmx", ".tmj": "application/x-tiled-tmj", ".ldtk": "application/x-ldtk",
    ".stl": "model/stl", ".3mf": "model/3mf", ".dxf": "image/vnd.dxf", ".step": "model/step", ".stp": "model/step",
    ".wav": "audio/x-wav", ".mp3": "audio/mpeg", ".flac": "audio/flac", ".ogg": "audio/ogg", ".opus": "audio/opus",
    ".m4a": "audio/mp4", ".aac": "audio/aac", ".aiff": "audio/x-aiff", ".aif": "audio/x-aiff",
    ".docx": "application/vnd.openxmlformats-officedocument.wordprocessingml.document",
    ".odt": "application/vnd.oasis.opendocument.text", ".md": "text/markdown", ".txt": "text/plain", ".rtf": "application/rtf",
    ".html": "text/html", ".xlsx": "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet",
    ".ods": "application/vnd.oasis.opendocument.spreadsheet", ".csv": "text/csv",
    ".pptx": "application/vnd.openxmlformats-officedocument.presentationml.presentation",
    ".odp": "application/vnd.oasis.opendocument.presentation", ".pdf": "application/pdf",
    ".geojson": "application/geo+json", ".kml": "application/vnd.google-earth.kml+xml", ".gpx": "application/gpx+xml",
    ".shp": "application/x-esri-shape", ".fit": "application/vnd.ant.fit", ".osm": "application/vnd.openstreetmap.data+xml",
    ".c": "text/x-csrc", ".h": "text/x-chdr", ".cpp": "text/x-c++src", ".hpp": "text/x-c++hdr", ".cc": "text/x-c++src",
    ".cxx": "text/x-c++src", ".hh": "text/x-c++hdr", ".hxx": "text/x-c++hdr", ".m": "text/x-objcsrc", ".mm": "text/x-objc++src",
    ".rs": "text/rust", ".go": "text/x-go", ".zig": "text/x-zig", ".py": "text/x-python", ".pyw": "text/x-python",
    ".js": "text/javascript", ".mjs": "text/javascript", ".cjs": "text/javascript", ".ts": "text/x-typescript",
    ".tsx": "text/x-typescript", ".jsx": "text/javascript", ".vue": "text/x-vue", ".java": "text/x-java", ".kt": "text/x-kotlin",
    ".kts": "text/x-kotlin", ".scala": "text/x-scala", ".cs": "text/x-csharp", ".fs": "text/x-fsharp", ".swift": "text/x-swift",
    ".dart": "text/x-dart", ".lua": "text/x-lua", ".rb": "application/x-ruby", ".php": "application/x-php", ".pl": "application/x-perl",
    ".r": "text/x-r", ".jl": "text/x-julia", ".ex": "text/x-elixir", ".exs": "text/x-elixir", ".hs": "text/x-haskell",
    ".sh": "application/x-shellscript", ".bash": "application/x-shellscript", ".zsh": "application/x-shellscript",
    ".fish": "application/x-fishscript", ".ps1": "text/x-powershell", ".bat": "application/x-bat", ".cmd": "application/x-bat",
    ".css": "text/css", ".scss": "text/x-scss", ".less": "text/x-less", ".json": "application/json", ".jsonc": "application/json",
    ".yaml": "application/yaml", ".yml": "application/yaml", ".toml": "application/toml", ".ini": "text/x-ini", ".cfg": "text/plain",
    ".conf": "text/plain", ".xml": "application/xml", ".sql": "application/sql", ".cmake": "text/x-cmake", ".gradle": "text/x-gradle",
    ".mk": "text/x-makefile", ".log": "text/x-log", ".diff": "text/x-patch", ".patch": "text/x-patch",
    ".zip": "application/zip", ".tar": "application/x-tar", ".tgz": "application/x-compressed-tar", ".gz": "application/gzip",
}


def table():
    text = (root / "apps/common/apps.cpp").read_text()
    body = text[text.index("extensionTable()"):]
    body = body[:body.index("return table;")]
    return re.findall(r'\{"(\.[a-z0-9_.]+)", \{"([a-z]+)", "([a-z_.]*)"\}\}', body)


def rows():
    out = []
    for ext, app, command in table():
        if not command or ext.count(".") > 1:
            continue
        own = ext.startswith((".ww", ".ach"))
        mime = MIME.get(ext) or ("application/x-wetwaresuite-" + ext[1:] if own else "")
        if not mime:
            sys.exit(f"packaging/file_types.py: add a mime type for {ext}")
        out.append((ext, app, mime, "yes" if own else "no"))
    return out


def tsv(rs):
    lines = ["# ext\tapp\tmime\town  (made by packaging/file_types.py from apps/common/apps.cpp; don't edit by hand)"]
    lines += ["\t".join(r) for r in rs]
    return "\n".join(lines) + "\n"


def iss(rs):
    # HKA: the current user's classes for a per-user install, the machine's for all users.
    # OpenWithProgids only adds the app to "Open with"; the default is set for wetwaresuite's own formats only.
    out = ["; made by packaging/file_types.py from apps/common/apps.cpp; don't edit by hand", "[Registry]"]
    for app in sorted({r[1] for r in rs}):
        prog = f"wetwaresuite.{app}"
        exe = f"{{app}}\\wetwaresuite-{app}.exe"
        out.append(f'Root: HKA; Subkey: "Software\\Classes\\{prog}"; ValueType: string; ValueName: ""; '
                   f'ValueData: "wetwaresuite {app} file"; Flags: uninsdeletekey')
        out.append(f'Root: HKA; Subkey: "Software\\Classes\\{prog}\\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{exe},0"')
        out.append(f'Root: HKA; Subkey: "Software\\Classes\\{prog}\\shell\\open\\command"; ValueType: string; ValueName: ""; '
                   f'ValueData: """{exe}"" open ""%1"""')
        out.append(f'Root: HKA; Subkey: "Software\\Classes\\Applications\\wetwaresuite-{app}.exe"; ValueType: none; Flags: uninsdeletekey')
        out.append(f'Root: HKA; Subkey: "Software\\Classes\\Applications\\wetwaresuite-{app}.exe\\shell\\open\\command"; '
                   f'ValueType: string; ValueName: ""; ValueData: """{exe}"" open ""%1"""')
    for ext, app, mime, own in rs:
        prog = f"wetwaresuite.{app}"
        out.append(f'Root: HKA; Subkey: "Software\\Classes\\{ext}\\OpenWithProgids"; ValueType: string; ValueName: "{prog}"; '
                   f'ValueData: ""; Flags: uninsdeletevalue')
        out.append(f'Root: HKA; Subkey: "Software\\Classes\\Applications\\wetwaresuite-{app}.exe\\SupportedTypes"; '
                   f'ValueType: string; ValueName: "{ext}"; ValueData: ""')
        if own == "yes":
            out.append(f'Root: HKA; Subkey: "Software\\Classes\\{ext}"; ValueType: string; ValueName: ""; ValueData: "{prog}"; '
                       f'Flags: uninsdeletevalue')
    # every entry belongs to the installer's "file types" task, so people can leave it off
    out = out[:2] + [line + "; Tasks: filetypes" for line in out[2:]]
    return "\n".join(out) + "\n"


def main():
    rs = rows()
    files = {here / "file-types.tsv": tsv(rs), here / "windows/associations.iss": iss(rs)}
    if "--check" in sys.argv:
        stale = [str(p.relative_to(root)) for p, text in files.items() if not p.exists() or p.read_text() != text]
        if stale:
            sys.exit("out of date (run python3 packaging/file_types.py): " + ", ".join(stale))
        print("file types up to date:", len(rs))
        return
    for p, text in files.items():
        p.write_text(text)
    print("wrote", len(rs), "file types")


if __name__ == "__main__":
    main()
