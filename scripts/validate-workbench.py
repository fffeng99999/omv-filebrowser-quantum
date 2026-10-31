#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Host-independent Workbench validation for openmediavault-filebrowser-quantum.

It checks the things that would otherwise only fail on an OMV host when
omv-mkworkbench runs:

  1. Every component.d / route.d / navigation.d manifest parses as YAML
     and has the expected version/type/data envelope.
  2. Component names are globally unique.
  3. Every route references a component that is defined in component.d.
  4. editing: true form pages only live on leaf URLs (the frpc 8.0.11
     bug: an editing form page under a URL with child routes never loads
     its data because the synthetic child route drops the editing flag).
  5. Every _("...") msgid used in the YAML exists in the .pot template
     and in the zh_CN / zh_TW .po files.
  6. No _("...") string contains a {{ token }} (translate runs before
     interpolation, so such strings can never match).
  7. All shipped text files use LF line endings.

Run:  python3 scripts/validate-workbench.py
"""

from __future__ import annotations

import pathlib
import re
import sys

import yaml

ROOT = pathlib.Path(__file__).resolve().parent.parent
WB = ROOT / "usr" / "share" / "openmediavault" / "workbench"
LOCALE = ROOT / "usr" / "share" / "openmediavault" / "locale"
PACKAGE = "openmediavault-filebrowser-quantum"

# Matches the single-line _("...") / _('...') form used in the manifests.
GETTEXT_RE = re.compile(r'_\(\s*(["\'])(.*?)\1\s*\)', re.DOTALL)
TOKEN_RE = re.compile(r"\{\{.*?\}\}")

failures: list[str] = []


def fail(message: str) -> None:
    failures.append(message)


def load_yaml(path: pathlib.Path) -> dict:
    try:
        with path.open("r", encoding="utf-8") as fh:
            data = yaml.safe_load(fh)
    except yaml.YAMLError as exc:
        fail(f"YAML parse error in {path.relative_to(ROOT)}: {exc}")
        return {}
    if not isinstance(data, dict) or data.get("version") != "1.0" \
            or "type" not in data or "data" not in data:
        fail(f"Malformed envelope in {path.relative_to(ROOT)}")
    return data


def collect_msgids(path: pathlib.Path) -> list[str]:
    text = path.read_text(encoding="utf-8")
    return [m.group(2) for m in GETTEXT_RE.finditer(text)]


def parse_po_msgids(path: pathlib.Path) -> set[str]:
    """Extract msgid values from a .po/.pot file without gettext tools."""
    msgids: set[str] = set()
    if not path.exists():
        fail(f"Missing translation file: {path.relative_to(ROOT)}")
        return msgids
    current: list[str] = []
    in_msgid = False
    for raw in path.read_text(encoding="utf-8").splitlines():
        if raw.startswith("msgid "):
            in_msgid = True
            current = [raw[len("msgid "):]]
        elif raw.startswith("msgstr "):
            if in_msgid:
                joined = " ".join(current).strip()
                value = _unpo(joined)
                if value:
                    msgids.add(value)
            in_msgid = False
        elif in_msgid and raw.startswith('"'):
            current.append(raw)
    return msgids


def _unpo(token: str) -> str:
    try:
        parts = yaml.safe_load(f"[{token}]")
        return "".join(parts) if parts else ""
    except yaml.YAMLError:
        return token.strip('"')


def main() -> int:
    component_files = sorted((WB / "component.d").glob("*.yaml"))
    route_files = sorted((WB / "route.d").glob("*.yaml"))
    nav_files = sorted((WB / "navigation.d").glob("*.yaml"))

    # 1-2. Components: parse and enforce unique names.
    components: dict[str, pathlib.Path] = {}
    component_types: dict[str, str] = {}
    for path in component_files:
        doc = load_yaml(path)
        if doc.get("type") != "component":
            fail(f"{path.name}: expected type 'component'")
        data = doc.get("data", {})
        name = data.get("name")
        ctype = data.get("type")
        if not name:
            fail(f"{path.name}: missing data.name")
        elif name in components:
            fail(f"Duplicate component name '{name}' in {path.name} "
                 f"and {components[name].name}")
        else:
            components[name] = path
            component_types[name] = ctype or ""

    # Routes: parse, map URLs and validate component references.
    routes: dict[str, dict] = {}
    for path in route_files:
        doc = load_yaml(path)
        if doc.get("type") != "route":
            fail(f"{path.name}: expected type 'route'")
        data = doc.get("data", {})
        url = data.get("url", "")
        routes[url] = data
        comp = data.get("component")
        if comp not in components:
            fail(f"{path.name}: route '{url}' references unknown "
                 f"component '{comp}'")

    # Navigation: parse and ensure every url has a matching route.
    nav_paths: set[str] = set()
    for path in nav_files:
        doc = load_yaml(path)
        if doc.get("type") != "navigation-item":
            fail(f"{path.name}: expected type 'navigation-item'")
        data = doc.get("data", {})
        url = data.get("url", "")
        nav_paths.add(url)
        if url and url not in routes:
            fail(f"{path.name}: navigation url '{url}' has no route")
        for role in (data.get("permissions", {}) or {}).get("role", []):
            if role not in ("admin", "user"):
                fail(f"{path.name}: unexpected role '{role}'")

    # 4. editing:true must only be on leaf URLs (no child routes).
    for url, data in routes.items():
        if data.get("editing"):
            prefix = url.rstrip("/") + "/"
            children = [u for u in routes if u != url and u.startswith(prefix)]
            if children:
                fail(f"Route '{url}' has editing:true but child routes "
                     f"{children}; move the form page to a leaf URL")

    # Every child route path should have a navigation entry or be a
    # create/edit action route reached from a datatable.
    action_suffixes = ("/create", "/edit")
    for url in routes:
        if url in nav_paths or url == "/services/filebrowserquantum":
            continue
        if not any(url.endswith(s) or "/edit/" in url for s in action_suffixes):
            # Datatable listing pages must have a navigation entry.
            if not any(url.startswith(n.rstrip("/") + "/") for n in nav_paths) \
                    and url not in nav_paths:
                fail(f"Route '{url}' has no navigation entry and is not a "
                     f"create/edit route")

    # 5-6. i18n: gather msgids and cross-check with the catalogs.
    used_msgids: dict[str, list[str]] = {}
    for path in component_files + route_files + nav_files:
        for msgid in collect_msgids(path):
            if TOKEN_RE.search(msgid):
                fail(f"{path.name}: _() msgid contains a {{{{ token }}}}: "
                     f"{msgid!r}")
            used_msgids.setdefault(msgid, []).append(path.name)

    pot = parse_po_msgids(LOCALE / f"{PACKAGE}.pot")
    po_zh_cn = parse_po_msgids(LOCALE / "zh_CN" / f"{PACKAGE}.po")
    po_zh_tw = parse_po_msgids(LOCALE / "zh_TW" / f"{PACKAGE}.po")

    for catalog_name, catalog in ((".pot", pot),
                                  ("zh_CN .po", po_zh_cn),
                                  ("zh_TW .po", po_zh_tw)):
        missing = sorted(m for m in used_msgids if m not in catalog)
        if missing:
            for msgid in missing:
                fail(f"msgid missing from {catalog_name}: {msgid!r} "
                     f"(used in {', '.join(sorted(set(used_msgids[msgid])))})")

    # Every translated catalog must carry a non-empty translation.
    for label, path in (("zh_CN", LOCALE / "zh_CN" / f"{PACKAGE}.po"),
                        ("zh_TW", LOCALE / "zh_TW" / f"{PACKAGE}.po")):
        if not path.exists():
            continue
        text = path.read_text(encoding="utf-8")
        blocks = re.split(r"\n(?=msgid )", text)
        for block in blocks:
            mid = re.search(r"msgid (.*?)(?:\n|\Z)", block, re.DOTALL)
            mstr = re.search(r"msgstr (.*?)(?:\n|\Z)", block, re.DOTALL)
            if not mid or not mstr:
                continue
            mid_v = _unpo(mid.group(1).strip())
            mstr_v = _unpo(mstr.group(1).strip())
            if mid_v and not mstr_v:
                fail(f"{label} .po has empty translation for {mid_v!r}")

    # 7. LF line endings for all shipped text files.
    text_globs = ("**/*.yaml", "**/*.yml", "**/*.json", "**/*.inc",
                  "**/*.php", "**/*.po", "**/*.pot", "**/*.sh", "**/*.sls",
                  "**/*.md", "**/Makefile", "Makefile")
    checked = 0
    for pattern in text_globs:
        for path in ROOT.glob(pattern):
            if not path.is_file() or ".git" in path.parts:
                continue
            checked += 1
            raw = path.read_bytes()
            if b"\r\n" in raw:
                fail(f"CRLF line ending in {path.relative_to(ROOT)}")

    print(f"Validated {len(component_files)} components, {len(route_files)} "
          f"routes, {len(nav_files)} navigation items, "
          f"{len(used_msgids)} msgids, {checked} text files (LF).")

    if failures:
        print("\nFAILED:")
        for item in failures:
            print(f"  - {item}")
        return 1
    print("OK: workbench manifests and i18n catalogs are consistent.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
