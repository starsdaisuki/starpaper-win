#!/usr/bin/env python3
"""Verify the final StarPaper MSIX files against the current source tree.

This is deliberately an end-of-pipeline check. It opens each MSIX and inspects
the packaged executable instead of trusting the intermediate build output.
"""

from __future__ import annotations

import argparse
import ast
import hashlib
import re
import struct
import subprocess
import sys
import zipfile
from pathlib import Path
from xml.etree import ElementTree


ROOT = Path(__file__).resolve().parents[1]
MACHINES = {0x8664: "x64", 0xAA64: "arm64"}
LOCAL_EXE = {"x64": ROOT / "StarPaper.exe", "arm64": ROOT / "StarPaper-arm64.exe"}
FORBIDDEN_PACKAGE_SUFFIXES = {".pdb", ".map", ".pfx", ".cer", ".env"}
EXPECTED_PACKAGE_ENTRIES = {
    "AppxBlockMap.xml",
    "AppxManifest.xml",
    "Assets/Square150x150Logo.png",
    "Assets/Square44x44Logo.png",
    "Assets/Square44x44Logo.targetsize-16.png",
    "Assets/Square44x44Logo.targetsize-24.png",
    "Assets/Square44x44Logo.targetsize-32.png",
    "Assets/Square44x44Logo.targetsize-48.png",
    "Assets/Square44x44Logo.targetsize-256.png",
    "Assets/Square71x71Logo.png",
    "Assets/StoreLogo.png",
    "StarPaper.exe",
    "[Content_Types].xml",
}
PRIVATE_PATTERNS = (
    # llvm-mingw embeds its public CI runner path; other macOS user roots fail.
    re.compile("/" + "Users" + r"/(?!runner(?:/|$))[^/\\\s]+"),
    re.compile(r"[A-Za-z]:\\" + "Users" + r"\\[^\\\s]+", re.I),
    re.compile(r"Documents[/\\](?:Research|Code)", re.I),
    re.compile(r"[A-Z0-9._%+-]+@[A-Z0-9.-]+\.[A-Z]{2,}", re.I),
)


class VerificationError(RuntimeError):
    pass


def fail(message: str) -> None:
    raise VerificationError(message)


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def scan_private_shapes(label: str, data: bytes) -> None:
    decoded_views = [
        data.decode("latin-1", errors="ignore"),
        data[0::2].decode("latin-1", errors="ignore"),
        data[1::2].decode("latin-1", errors="ignore"),
    ]
    for pattern in PRIVATE_PATTERNS:
        if any(pattern.search(view) for view in decoded_views):
            fail(f"{label}: private-looking value embedded in artifact: {pattern.pattern}")


def verify_png_metadata(label: str, data: bytes) -> None:
    if not data.startswith(b"\x89PNG\r\n\x1a\n"):
        fail(f"{label}: invalid PNG signature")
    offset = 8
    allowed = {b"IHDR", b"IDAT", b"IEND", b"sRGB", b"eXIf"}
    while offset + 12 <= len(data):
        size = struct.unpack_from(">I", data, offset)[0]
        chunk_type = data[offset + 4 : offset + 8]
        chunk_data = data[offset + 8 : offset + 8 + size]
        if chunk_type not in allowed:
            fail(f"{label}: unreviewed PNG metadata chunk {chunk_type!r}")
        if chunk_type == b"eXIf":
            # sips adds a fixed 68-byte TIFF block containing orientation and
            # pixel dimensions only. Text-bearing or larger EXIF is rejected.
            if len(chunk_data) != 68 or re.search(rb"[ -~]{4,}", chunk_data):
                fail(f"{label}: PNG EXIF is not the reviewed dimensions-only form")
        offset += size + 12
        if chunk_type == b"IEND":
            return
    fail(f"{label}: PNG has no valid IEND")


def project_version() -> str:
    text = (ROOT / "Makefile").read_text(encoding="utf-8")
    match = re.search(r"^VERSION\s*\?=\s*([0-9]+\.[0-9]+\.[0-9]+)\s*$", text, re.M)
    if not match:
        fail("Makefile VERSION not found")
    return match.group(1)


def cpp_string_sequence(source: str) -> str:
    tokens = re.findall(r'L("(?:\\.|[^"\\])*")', source)
    if not tokens:
        fail(f"cannot parse C++ string sequence: {source!r}")
    return "".join(ast.literal_eval(token) for token in tokens)


def verify_translation_table() -> list[str]:
    header = (ROOT / "src/theme.h").read_text(encoding="utf-8")
    source = (ROOT / "src/theme.cpp").read_text(encoding="utf-8")
    enum_match = re.search(r"enum StrId\s*\{(.*?)S_COUNT", header, re.S)
    table_match = re.search(r"const Str kStr\[\]\s*=\s*\{(.*?)\n\};", source, re.S)
    if not enum_match or not table_match:
        fail("cannot locate StrId or kStr")

    enum_text = re.sub(r"//.*", "", enum_match.group(1))
    enum_ids = []
    for item in enum_text.split(","):
        item = item.strip()
        if not item:
            continue
        enum_ids.append(item.split("=", 1)[0].strip())
    table_ids = re.findall(r"//\s*(S_[A-Z0-9_]+)\s*$", table_match.group(1), re.M)
    if enum_ids != table_ids:
        for index, (enum_id, table_id) in enumerate(zip(enum_ids, table_ids)):
            if enum_id != table_id:
                fail(f"translation order mismatch at {index}: {enum_id} != {table_id}")
        fail(f"translation count mismatch: enum={len(enum_ids)} table={len(table_ids)}")

    entry_pattern = re.compile(
        r"\{\s*((?:L\"(?:\\.|[^\"\\])*\"\s*)+),\s*"
        r"((?:L\"(?:\\.|[^\"\\])*\"\s*)+)\}\s*,?",
        re.S,
    )
    entries = entry_pattern.findall(table_match.group(1))
    if len(entries) != len(enum_ids):
        fail(f"cannot parse all translations: entries={len(entries)} ids={len(enum_ids)}")

    # Visible Chinese text must not bypass the translation table.
    for relative in ("src/main.cpp", "src/settings.cpp"):
        text = (ROOT / relative).read_text(encoding="utf-8")
        if re.search(r'L"[^"\n]*[\u3400-\u9fff]', text):
            fail(f"hard-coded Chinese UI string outside theme.cpp: {relative}")

    app_h = (ROOT / "src/app.h").read_text(encoding="utf-8")
    main_cpp = (ROOT / "src/main.cpp").read_text(encoding="utf-8")
    if not re.search(r"bool\s+english\s*=\s*true", app_h):
        fail("AppState no longer defaults to English")
    language_contract = (
        'if (v == L"zh") return false;',
        'if (v == L"en") return true;',
        "PRIMARYLANGID(GetUserDefaultUILanguage()) != LANG_CHINESE",
    )
    if any(fragment not in main_cpp for fragment in language_contract):
        fail("LoadEnglishPref no longer uses Chinese-only detection with English fallback")

    # Verify both columns. Checking only English would miss a source change that
    # updates Chinese copy after the package was built (and vice versa).
    return [cpp_string_sequence(text) for entry in entries for text in entry]


def pe_arch(exe: bytes) -> str:
    if exe[:2] != b"MZ":
        fail("packaged StarPaper.exe is not a PE file")
    pe_offset = struct.unpack_from("<I", exe, 0x3C)[0]
    if exe[pe_offset : pe_offset + 4] != b"PE\0\0":
        fail("packaged StarPaper.exe has an invalid PE header")
    machine = struct.unpack_from("<H", exe, pe_offset + 4)[0]
    if machine not in MACHINES:
        fail(f"unsupported PE machine: 0x{machine:04x}")
    return MACHINES[machine]


def verify_package(path: Path, version: str, translation_strings: list[str]) -> dict[str, str]:
    if not path.is_file():
        fail(f"package not found: {path}")
    with zipfile.ZipFile(path) as package:
        names = package.namelist()
        if len(names) != len(set(names)):
            fail(f"duplicate path in package: {path.name}")
        if set(names) != EXPECTED_PACKAGE_ENTRIES:
            missing_entries = sorted(EXPECTED_PACKAGE_ENTRIES - set(names))
            extra_entries = sorted(set(names) - EXPECTED_PACKAGE_ENTRIES)
            fail(f"{path.name}: unexpected package inventory; missing={missing_entries}, extra={extra_entries}")
        lowered = [name.lower() for name in names]
        if "appxsignature.p7x" in lowered:
            fail(f"Store submission package must be unsigned: {path.name}")
        for name in names:
            suffix = Path(name).suffix.lower()
            if suffix in FORBIDDEN_PACKAGE_SUFFIXES or "identity.env" in name.lower():
                fail(f"private/debug file in package {path.name}: {name}")
            payload = package.read(name)
            if suffix == ".png":
                verify_png_metadata(f"{path.name}:{name}", payload)
            else:
                scan_private_shapes(f"{path.name}:{name}", payload)
        try:
            manifest_bytes = package.read("AppxManifest.xml")
            exe = package.read("StarPaper.exe")
        except KeyError as error:
            fail(f"missing required package entry in {path.name}: {error}")

    manifest_text = manifest_bytes.decode("utf-8-sig")
    if re.search(r"REPLACE-|__[A-Z0-9_]+__", manifest_text):
        fail(f"identity placeholder remains in {path.name}")
    root = ElementTree.fromstring(manifest_text)
    ns = {"p": "http://schemas.microsoft.com/appx/manifest/foundation/windows10"}
    identity = root.find("p:Identity", ns)
    if identity is None:
        fail(f"Identity missing in {path.name}")
    if not identity.attrib.get("Name") or not identity.attrib.get("Publisher"):
        fail(f"Store identity is incomplete in {path.name}")
    manifest_version = identity.attrib.get("Version", "")
    manifest_arch = identity.attrib.get("ProcessorArchitecture", "")
    expected_manifest_version = f"{version}.0"
    if manifest_version != expected_manifest_version:
        fail(f"{path.name}: manifest version {manifest_version} != {expected_manifest_version}")
    actual_arch = pe_arch(exe)
    if manifest_arch != actual_arch:
        fail(f"{path.name}: manifest arch {manifest_arch} != PE arch {actual_arch}")

    version_bytes = version.encode("utf-16-le")
    if exe.count(version_bytes) < 2:
        fail(f"{path.name}: EXE FileVersion/ProductVersion does not contain {version}")
    missing = [text for text in translation_strings if text and text.encode("utf-16-le") not in exe]
    if missing:
        sample = ", ".join(repr(item) for item in missing[:5])
        fail(f"{path.name}: {len(missing)} translation strings missing from final EXE: {sample}")

    local_exe = LOCAL_EXE[actual_arch]
    if not local_exe.is_file():
        fail(f"local {actual_arch} EXE missing: {local_exe}")
    if local_exe.read_bytes() != exe:
        fail(f"{path.name}: packaged EXE differs from the current local {actual_arch} EXE")

    return {
        "path": str(path),
        "arch": actual_arch,
        "identity": identity.attrib.get("Name", ""),
        "publisher": identity.attrib.get("Publisher", ""),
        "package_sha256": sha256(path.read_bytes()),
        "exe_sha256": sha256(exe),
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("packages", nargs="*", type=Path, help="MSIX paths; defaults to build/submit/*.msix")
    parser.add_argument("--require-clean", action="store_true", help="fail unless the source repository is clean")
    args = parser.parse_args()

    if args.require_clean:
        status = subprocess.check_output(
            ["git", "-C", str(ROOT), "status", "--porcelain"], text=True
        )
        if status:
            fail("source repository is dirty; release artifacts would have no committed provenance")

    version = project_version()
    packages = args.packages or sorted(
        (ROOT / "build/submit").glob(f"StarPaper-{version}.0-*.msix")
    )
    if not packages:
        fail(f"no StarPaper {version} MSIX packages supplied or found in build/submit")

    translation_strings = verify_translation_table()
    results = [verify_package(path.resolve(), version, translation_strings) for path in packages]
    architectures = [result["arch"] for result in results]
    if len(results) > 1 and sorted(architectures) != ["arm64", "x64"]:
        fail(f"expected one x64 and one arm64 package, got: {architectures}")
    identities = {(result["identity"], result["publisher"]) for result in results}
    if len(identities) != 1:
        fail("packages do not use the same Store identity")

    print(f"PASS: StarPaper {version}; {len(translation_strings) // 2} translation pairs verified in final EXE")
    for result in sorted(results, key=lambda item: item["arch"]):
        print(f"  {result['arch']}: {result['path']}")
        print(f"    package sha256: {result['package_sha256']}")
        print(f"    exe sha256:     {result['exe_sha256']}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except VerificationError as error:
        print(f"FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
