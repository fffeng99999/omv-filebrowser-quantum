#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Shared implementation of the FileBrowser Quantum deployment checks.

This file is part of OpenMediaVault.

@license   https://www.gnu.org/licenses/gpl.html GPL Version 3
@author    ${GITHUB_USER} <${GITHUB_USER}@users.noreply.github.com>

Everything that decides whether the container may be started, and every
write to the upstream ``config.yaml``, lives here instead of inside a shell
heredoc. The ctl (``/usr/sbin/omv-filebrowserquantum-ctl``) and the test
suite (``scripts/test_anti_recursion.py``) therefore execute exactly the
same code - the tests exercise the production algorithms instead of a
copy of them.

Commands (see main()):

  gate            fail-closed preconditions for starting the container
  prepare-config  declarative synchronisation of the upstream config.yaml
  maskcheck       verify the rendered stack against the live storage layout
  maskreport      print the anti-recursion protection report
  verify          verify the running container (used by the Salt state)
"""

from __future__ import annotations

import json
import os
import posixpath
import re
import shutil
import subprocess
import sys
import tempfile

try:
    import yaml
except ImportError:  # pragma: no cover - PyYAML ships with Salt on OMV
    yaml = None

CONF_MAIN = "conf.service.filebrowserquantum"
CONF_SOURCE = "conf.service.filebrowserquantum.source"
CONF_SHAREDFOLDER = "conf.system.sharedfolder"
CONF_MOUNT = "conf.system.filesystem.mountpoint"
CONF_COMPOSE = "conf.service.compose"
CONF_RUSTFS = "conf.service.rustfs"

DATA_MOUNT = "/home/filebrowser/data"
CACHE_DIR = DATA_MOUNT + "/tmp"
CONFIG_FILE = "config.yaml"
CONFIG_BACKUP_SUFFIX = ".omv-bak"
MAX_MASKS = 32
DEFAULT_PROXY_HEADER = "X-Forwarded-User"

#: Characters that must not appear in http.baseURL.
_FORBIDDEN_BASE_URL_CHARS = ('\\', ' ', '\t', '?', '#', '"', "'")

#: Characters that are not allowed in http.baseURL.

#: Container paths that must never be used as a bind mount target (R-6).
RESERVED_PREFIXES = (
    "/app", "/etc", "/proc", "/sys", "/dev", "/var",
    "/usr", "/bin", "/sbin", "/lib", "/lib64", "/tmp",
    "/home/filebrowser/data", "/home/filebrowser",
)

AUTH_METHODS = ("password", "noauth", "proxy")


class FbqError(Exception):
    """Base class for all user-visible failures of this module."""


class GateError(FbqError):
    """The configuration does not allow the container to be started."""


class MaskError(FbqError):
    """The anti-recursion invariants are violated."""


class ConfigFileError(FbqError):
    """``config.yaml`` cannot be read safely."""


# ---------------------------------------------------------------------
# Configuration database access
# ---------------------------------------------------------------------

def run_capture(argv):
    """Run a command and return ``(exit code, stdout)``.

    The output is collected through a pipe and, when the environment does
    not allow that (restricted sandbox), through a temporary file.
    """
    try:
        result = subprocess.run(
            argv, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True
        )
        return result.returncode, result.stdout
    except OSError:
        pass
    handle, path = tempfile.mkstemp(prefix="fbq-cmd.")
    os.close(handle)
    try:
        try:
            with open(path, "w", encoding="utf-8") as out:
                result = subprocess.run(
                    argv, stdout=out, stderr=subprocess.DEVNULL, text=True
                )
        except OSError:
            # The command does not exist (e.g. docker not installed).
            return 1, ""
        with open(path, "r", encoding="utf-8") as captured:
            return result.returncode, captured.read()
    finally:
        try:
            os.unlink(path)
        except OSError:
            pass


def omv_reader(config_id):
    """Read a configuration object through omv-confdbadm."""
    code, output = run_capture(["omv-confdbadm", "read", config_id])
    if 0 != code or not output.strip():
        return None
    try:
        return json.loads(output)
    except ValueError:
        return None


class DictReader(object):
    """Reader used by the test suite (no OMV host required)."""

    def __init__(self, data):
        self._data = data or {}

    def __call__(self, config_id):
        return self._data.get(config_id)


def as_item_list(value, key):
    """Normalise a confdb read that returns either a list or a wrapper dict."""
    if isinstance(value, dict):
        value = value.get(key, [])
    if not isinstance(value, list):
        return []
    return [item for item in value if isinstance(item, dict)]


def as_bool(value, default=False):
    if isinstance(value, bool):
        return value
    if value is None or value == "":
        return default
    if isinstance(value, (int, float)):
        return value != 0
    text = str(value).strip().lower()
    if text in ("1", "true", "yes", "on"):
        return True
    if text in ("0", "false", "no", "off"):
        return False
    return default


def as_text(value, default=""):
    if value is None:
        return default
    if isinstance(value, str):
        return value
    return str(value)


# ---------------------------------------------------------------------
# Path helpers
# ---------------------------------------------------------------------

def is_strict_descendant(child, parent):
    """True when ``child`` strictly lives below ``parent``."""
    if not child or not parent:
        return False
    c = posixpath.normpath(child).rstrip("/")
    p = posixpath.normpath(parent).rstrip("/")
    return (c != p) and c.startswith(p + "/")


def get_rel_path(child, parent):
    """Relative path of ``child`` below ``parent`` (empty when not nested)."""
    if not child or not parent:
        return ""
    c = posixpath.normpath(child).rstrip("/")
    p = posixpath.normpath(parent).rstrip("/")
    if c.startswith(p + "/"):
        return c[len(p) + 1:]
    return ""


def resolve_sf_path(ref, sf_list, mnt_list):
    """Resolve a shared folder reference to its host path."""
    if not ref:
        return ""
    mntref = ""
    reldir = ""
    for sf in sf_list:
        if as_text(sf.get("uuid")) == ref:
            mntref = as_text(sf.get("mntentref"))
            reldir = as_text(sf.get("reldirpath")).strip("/")
            break
    if not mntref:
        return ""
    dirpath = ""
    for mnt in mnt_list:
        if as_text(mnt.get("uuid")) == mntref:
            dirpath = as_text(mnt.get("dir")).rstrip("/")
            break
    if not dirpath:
        return ""
    # OMV paths are always POSIX, keep the join independent of the host
    # the module is executed on (the test suite runs on Windows as well).
    full = posixpath.normpath(posixpath.join(dirpath, reldir) if reldir else dirpath)
    if os.name == "posix":
        try:
            return os.path.realpath(full)
        except OSError:
            return full
    return full


def parse_volume(value):
    """Return ``(container target, read_only)`` of a compose volume entry."""
    text = value.strip().strip('"').strip("'")
    if text.startswith("${{"):
        idx = text.rfind("}}:")
        if idx != -1:
            rest = text[idx + 3:]
            parts = rest.split(":")
            target = parts[0]
            is_ro = len(parts) > 1 and parts[1] == "ro"
            return target, is_ro
    parts = text.split(":")
    if len(parts) >= 2:
        target = parts[1]
        is_ro = len(parts) >= 3 and parts[2] == "ro"
        return target, is_ro
    return "", False


# ---------------------------------------------------------------------
# Topology
# ---------------------------------------------------------------------

def docker_root():
    """Docker data-root of the host."""
    code, output = run_capture(
        ["docker", "info", "--format", "{{.DockerRootDir}}"]
    )
    root = output.strip()
    if 0 == code and root:
        return root
    return "/var/lib/docker"


def build_topology(reader, docker_root_fn=docker_root, existing_roots_fn=None):
    """Collect everything the checks need from the configuration database."""
    main = reader(CONF_MAIN) or {}
    if not isinstance(main, dict):
        main = {}
    sources_raw = as_item_list(reader(CONF_SOURCE), "source")
    sf_list = as_item_list(reader(CONF_SHAREDFOLDER), "sharedfolder")
    mnt_list = as_item_list(reader(CONF_MOUNT), "mntent")
    compose = reader(CONF_COMPOSE) or {}
    if not isinstance(compose, dict):
        compose = {}
    rustfs = reader(CONF_RUSTFS) or {}
    if not isinstance(rustfs, dict):
        rustfs = {}

    data_ref = as_text(main.get("dataRef"))
    data_host = resolve_sf_path(data_ref, sf_list, mnt_list)
    compose_ref = as_text(compose.get("sharedfolderref"))
    compose_host = resolve_sf_path(compose_ref, sf_list, mnt_list)
    rustfs_host = resolve_sf_path(as_text(rustfs.get("dataRef")), sf_list, mnt_list)
    docker_host = docker_root_fn() or "/var/lib/docker"

    storage = []
    for item in sources_raw:
        ref = as_text(item.get("sharedfolderref"))
        cpath = as_text(item.get("path"))
        storage.append({
            "uuid": as_text(item.get("uuid")),
            "ref": ref,
            "host": resolve_sf_path(ref, sf_list, mnt_list),
            "path": cpath,
            "name": as_text(item.get("name")) or cpath,
            "readonly": as_bool(item.get("readonly")),
            "defaultEnabled": as_bool(item.get("defaultEnabled"), True),
        })

    mask_protected = as_bool(main.get("maskProtected"), True)
    protected = {}
    if data_host:
        protected["data"] = ("FileBrowser data folder", data_host)
    if mask_protected:
        if compose_host:
            protected["compose"] = ("Compose shared folder", compose_host)
        if rustfs_host:
            protected["rustfs"] = ("RustFS object store", rustfs_host)
        if docker_host:
            protected["docker"] = ("Docker data-root", docker_host)

    if existing_roots_fn is None:
        existing = set(
            key for key, (_, path) in protected.items() if path and os.path.isdir(path)
        )
    else:
        existing = set(existing_roots_fn(protected))

    topology = {
        "main": main,
        "sources": storage,
        "sharedfolders": sf_list,
        "mountpoints": mnt_list,
        "data_ref": data_ref,
        "data_host": data_host,
        "compose_ref": compose_ref,
        "compose_host": compose_host,
        "rustfs_host": rustfs_host,
        "docker_root": docker_host,
        "mask_protected": mask_protected,
        "protected": protected,
        "existing_roots": existing,
    }
    topology["masks"] = compute_masks(storage, protected, existing_roots=existing)
    return topology


def compute_masks(sources, protected, existing_roots=None):
    """Compute the tmpfs masks that hide protected roots below a source.

    ``protected`` maps a key to a ``(label, host path)`` tuple. When
    ``existing_roots`` is given, roots that do not exist on the host are
    skipped (the CLI passes the real filesystem, the tests pass a set).
    """
    masks = []
    for source in sorted(sources, key=lambda item: item.get("path", "")):
        host = source.get("host", "")
        cpath = source.get("path", "")
        if not host or not cpath:
            continue
        for key in sorted(protected):
            label, protected_path = protected[key]
            if not protected_path:
                continue
            if existing_roots is not None and key not in existing_roots:
                continue
            if protected_path == host:
                raise MaskError(
                    "Shared folder '{0}' is identical to the protected root "
                    "'{1}'. Masking requires a path below it (R-3).".format(
                        host, label
                    )
                )
            if is_strict_descendant(protected_path, host):
                rel = get_rel_path(protected_path, host)
                if rel:
                    target = cpath.rstrip("/") + "/" + rel.lstrip("/")
                    if target not in masks:
                        masks.append(target)
    masks.sort()
    if len(masks) > MAX_MASKS:
        raise MaskError(
            "Too many protected directories to mask ({0} > {1}). Choose "
            "narrower shared folders.".format(len(masks), MAX_MASKS)
        )
    return masks


# ---------------------------------------------------------------------
# Fail-closed gate
# ---------------------------------------------------------------------

def check_gate(topology):
    """Raise GateError unless the container may be started.

    Mirrors the project rule "the service must not start before the data
    shared folder and at least one storage source are configured". The
    ctl ``up``/``restart``/``upgrade`` commands and the RPC
    ``start``/``restart``/``upgrade`` methods both run this check, so a
    direct RPC call cannot bypass the enabled flag or an empty
    configuration either.
    """
    main = topology["main"]
    if not as_bool(main.get("enable")):
        raise GateError(
            "The FileBrowser Quantum service is disabled. Enable it, save "
            "and apply the configuration before starting the container."
        )
    if not topology["compose_ref"]:
        raise GateError(
            "The openmediavault-compose shared folder is not configured. Set "
            "it in Services -> Compose -> Settings first."
        )
    if not topology["compose_host"]:
        raise GateError(
            "The openmediavault-compose shared folder could not be resolved. "
            "Check Services -> Compose -> Settings."
        )
    if not topology["data_ref"]:
        raise GateError(
            "No data shared folder is configured. Select the data shared "
            "folder before starting the container."
        )
    if not topology["data_host"]:
        raise GateError(
            "The configured data shared folder could not be resolved. Check "
            "the shared folder and the mounted file system."
        )
    if not os.path.isdir(topology["data_host"]):
        raise GateError(
            "The data folder '{0}' does not exist on this host. Mount the "
            "file system or select another shared folder.".format(
                topology["data_host"]
            )
        )
    if not topology["sources"]:
        raise GateError(
            "At least one storage source must be configured before the "
            "container can be started."
        )
    for source in topology["sources"]:
        if not source["ref"]:
            raise GateError(
                "A storage source has no shared folder assigned. Fix it on "
                "the Sources tab."
            )
        if not source["host"]:
            raise GateError(
                "A storage source shared folder could not be resolved. Check "
                "the shared folder '({0})'.".format(source["path"] or source["uuid"])
            )
        if not os.path.isdir(source["host"]):
            raise GateError(
                "The storage source folder '{0}' does not exist on this "
                "host. Mount the file system or remove the source.".format(
                    source["host"]
                )
            )


# ---------------------------------------------------------------------
# config.yaml synchronisation
# ---------------------------------------------------------------------

def normalise_base_url(value):
    """Normalise the reverse proxy sub path ('' when disabled)."""
    text = as_text(value).strip()
    if text in ("", "/"):
        return ""
    if not text.startswith("/"):
        raise ConfigFileError(
            "The base URL '{0}' must start with '/'.".format(text)
        )
    if any(ch in text for ch in _FORBIDDEN_BASE_URL_CHARS):
        raise ConfigFileError(
            "The base URL '{0}' must not contain whitespace, quotes, '?' or "
            "'#'.".format(text)
        )
    text = re.sub(r"/+", "/", text).rstrip("/")
    if len(text) > 128:
        raise ConfigFileError("The base URL is too long (max 128 characters).")
    return text


def validate_app_url(value):
    """Validate an optional public base URL (scheme and host)."""
    text = as_text(value).strip().rstrip("/")
    if "" == text:
        return ""
    if not re.match(r"^https?://[^/\s]+(?:/[^\s]*)?$", text, re.IGNORECASE):
        raise ConfigFileError(
            "The application URL '{0}' must start with http:// or https:// "
            "and must not contain whitespace.".format(text)
        )
    return text


def app_url_origin(app_url):
    """Return ``scheme://host[:port]`` of a public URL ('' when empty).

    Upstream appends ``http.baseURL`` to ``http.externalUrl`` on its own,
    so only the origin may be written into the configuration - otherwise
    the generated links would contain the sub path twice (/files/files).
    """
    text = as_text(app_url).strip()
    if "" == text:
        return ""
    match = re.match(r"^(https?://[^/\s]+)", text, re.IGNORECASE)
    return match.group(1) if match else ""


def build_config_document(existing, main, sources):
    """Merge the plugin settings into an upstream config.yaml document.

    Only the keys owned by the plugin are touched: everything the operator
    added per hand (branding, access rules, other auth methods, ...) is
    preserved.
    """
    document = dict(existing) if isinstance(existing, dict) else {}

    http_cfg = document.get("http")
    if not isinstance(http_cfg, dict):
        http_cfg = {}
        document["http"] = http_cfg
    server_cfg = document.get("server")
    if not isinstance(server_cfg, dict):
        server_cfg = {}
        document["server"] = server_cfg
    auth_cfg = document.get("auth")
    if not isinstance(auth_cfg, dict):
        auth_cfg = {}
        document["auth"] = auth_cfg
    methods_cfg = auth_cfg.get("methods")
    if not isinstance(methods_cfg, dict):
        methods_cfg = {}
        auth_cfg["methods"] = methods_cfg

    base_url = normalise_base_url(main.get("baseURL"))
    app_url = validate_app_url(main.get("appUrl"))

    # http.* - always written, so clearing a value really clears it.
    http_cfg["baseURL"] = base_url if base_url else "/"
    http_cfg["externalUrl"] = app_url_origin(app_url)
    http_cfg["trustProxyHeaders"] = as_bool(main.get("trustProxyHeaders"))

    method = as_text(main.get("authMethod"), "password").strip() or "password"
    if method not in AUTH_METHODS:
        raise ConfigFileError(
            "Unknown authentication method '{0}'.".format(method)
        )
    proxy_header = as_text(main.get("authProxyHeader"), DEFAULT_PROXY_HEADER).strip()
    if not proxy_header:
        proxy_header = DEFAULT_PROXY_HEADER

    # auth.methods.* - the keys upstream 2.x really reads.
    methods_cfg["noauth"] = ("noauth" == method)
    password_cfg = methods_cfg.get("password")
    if not isinstance(password_cfg, dict):
        password_cfg = {}
        methods_cfg["password"] = password_cfg
    password_cfg["enabled"] = ("password" == method)
    proxy_cfg = methods_cfg.get("proxy")
    if not isinstance(proxy_cfg, dict):
        proxy_cfg = {}
        methods_cfg["proxy"] = proxy_cfg
    proxy_cfg["enabled"] = ("proxy" == method)
    if "proxy" == method:
        proxy_cfg["header"] = proxy_header

    # Legacy (v1.4 / <= 8.1.4) keys never understood by upstream.
    server_cfg.pop("baseURL", None)
    server_cfg.pop("trustProxyHeaders", None)
    auth_cfg.pop("methods_noauth", None)

    server_cfg["cacheDir"] = CACHE_DIR
    server_cfg["sources"] = [
        {
            "path": source["path"],
            "name": source.get("name") or source["path"].rstrip("/").split("/")[-1] or "Storage",
            "config": {"defaultEnabled": as_bool(source.get("defaultEnabled"), True)},
        }
        for source in sorted(sources, key=lambda item: as_text(item.get("path")))
        if source.get("path")
    ]
    return document


def read_config_document(path):
    """Read ``config.yaml``; raise instead of silently treating it as empty."""
    if not os.path.isfile(path):
        return {}
    if yaml is None:  # pragma: no cover - PyYAML is a Salt dependency
        raise ConfigFileError(
            "PyYAML is not installed, so {0} cannot be checked before it is "
            "rewritten. Install python3-yaml.".format(path)
        )
    try:
        with open(path, "r", encoding="utf-8") as handle:
            document = yaml.safe_load(handle)
    except Exception as exc:  # noqa: BLE001 - YAML raises several types
        raise ConfigFileError(
            "{0} cannot be parsed ({1}). The file was left untouched so no "
            "upstream setting is lost; repair or remove it and try "
            "again.".format(path, exc)
        )
    if document is None:
        return {}
    if not isinstance(document, dict):
        raise ConfigFileError(
            "{0} does not contain a YAML mapping. The file was left "
            "untouched.".format(path)
        )
    return document


def dump_config_document(document):
    """Serialise the document in the upstream (2 space, block) style."""
    if yaml is None:  # pragma: no cover - PyYAML is a Salt dependency
        raise ConfigFileError("PyYAML is not installed, cannot write config.yaml.")
    return yaml.safe_dump(
        document, default_flow_style=False, sort_keys=False, allow_unicode=True
    )


def write_config_document(path, document, backup=True):
    """Atomically replace ``config.yaml`` (temp file + rename).

    Returns ``True`` when the content changed.
    """
    content = dump_config_document(document)
    old = None
    if os.path.isfile(path):
        with open(path, "r", encoding="utf-8") as handle:
            old = handle.read()
        if old == content:
            return False
        if backup:
            try:
                shutil.copy2(path, path + CONFIG_BACKUP_SUFFIX)
            except OSError:
                pass
    directory = os.path.dirname(path) or "."
    os.makedirs(directory, exist_ok=True)
    handle, tmp_path = tempfile.mkstemp(prefix=".config.yaml.", dir=directory)
    try:
        with os.fdopen(handle, "w", encoding="utf-8") as tmp:
            tmp.write(content)
            tmp.flush()
            os.fsync(tmp.fileno())
        os.replace(tmp_path, path)
    except Exception:
        try:
            os.unlink(tmp_path)
        except OSError:
            pass
        raise
    return True


def sync_config(data_dir, main, sources):
    """Synchronise ``<data_dir>/config.yaml``; returns ``(path, changed)``."""
    if not data_dir or not os.path.isdir(data_dir):
        raise ConfigFileError(
            "The data folder is not available, config.yaml was not written."
        )
    path = os.path.join(data_dir, CONFIG_FILE)
    existing = read_config_document(path)
    document = build_config_document(existing, main, sources)
    changed = write_config_document(path, document)
    return path, changed


# ---------------------------------------------------------------------
# Stack verification (M1 - M7)
# ---------------------------------------------------------------------

def parse_stack_file(yml_path):
    """Split the rendered stack file into volume and tmpfs entries."""
    volumes = []
    tmpfs = []
    with open(yml_path, "r", encoding="utf-8") as handle:
        content = handle.read()
    in_volumes = False
    in_tmpfs = False
    for raw in content.splitlines():
        stripped = raw.strip()
        if stripped == "volumes:":
            in_volumes, in_tmpfs = True, False
            continue
        if stripped == "tmpfs:":
            in_volumes, in_tmpfs = False, True
            continue
        if stripped.startswith("services:") or (
            stripped.endswith(":") and not stripped.startswith("-")
        ):
            in_volumes, in_tmpfs = False, False
        if in_volumes and stripped.startswith("- "):
            volumes.append(stripped[2:].strip())
        elif in_tmpfs and stripped.startswith("- "):
            tmpfs.append(stripped[2:].strip())
    return volumes, tmpfs


def check_stack(topology, yml_path):
    """Return the list of violations of the rendered stack file."""
    errors = []
    if not yml_path or not os.path.isfile(yml_path):
        return [
            "The rendered stack file was not found. Save the settings and "
            "apply the configuration first."
        ]
    volumes, tmpfs = parse_stack_file(yml_path)
    parsed = [(entry, parse_volume(entry)) for entry in volumes]
    targets = [(target, is_ro) for _, (target, is_ro) in parsed if target]

    # M1: writable data mount.
    if not any(target == DATA_MOUNT and not is_ro for target, is_ro in targets):
        errors.append(
            "[M1] The stack has no writable mount for the data folder "
            "({0}).".format(DATA_MOUNT)
        )

    # M2: the bind set must match the configured sources in both directions.
    configured = {}
    for source in topology["sources"]:
        if source["path"]:
            configured[source["path"]] = bool(source["readonly"])
    rendered = {}
    for target, is_ro in targets:
        if target == DATA_MOUNT:
            continue
        rendered[target] = is_ro
    for target in sorted(rendered):
        if target not in configured:
            errors.append(
                "[M2] The stack mounts '{0}', but no storage source uses that "
                "container path. The rendered stack no longer matches the "
                "configuration - save the settings and apply again.".format(target)
            )
            continue
        if configured[target] != rendered[target]:
            errors.append(
                "[M2] The stack renders '{0}' {1}, the source is configured "
                "{2}.".format(
                    target,
                    "read-only" if rendered[target] else "read-write",
                    "read-only" if configured[target] else "read-write",
                )
            )
    for target in sorted(configured):
        if target not in rendered:
            errors.append(
                "[M2] The storage source '{0}' is configured but not mounted "
                "by the stack.".format(target)
            )

    # M3/M4: tmpfs targets must not collide with bind targets; no bind
    # mount inside reserved container paths.
    bind_targets = set(rendered)
    tmpfs_paths = []
    for entry in tmpfs:
        path = entry.split(":")[0]
        tmpfs_paths.append(path)
        if path in bind_targets or path == DATA_MOUNT:
            errors.append(
                "[M3] The mask '{0}' collides with a bind mount target.".format(path)
            )
    for target in sorted(bind_targets):
        if target == "/" or any(
            target == prefix or target.startswith(prefix + "/")
            for prefix in RESERVED_PREFIXES
        ):
            errors.append(
                "[M4] '{0}' is a reserved container path and must not be used "
                "as a storage source.".format(target)
            )

    # M5: mask count and parameters.
    if len(tmpfs) > MAX_MASKS:
        errors.append(
            "[M5] The stack contains {0} masks, the limit is {1}.".format(
                len(tmpfs), MAX_MASKS
            )
        )
    for entry in tmpfs:
        if "mode=0500" not in entry or "size=" not in entry:
            errors.append(
                "[M5] The mask '{0}' is missing mode=0500 or a size.".format(entry)
            )

    # M6: the mask set must match the live storage layout exactly. A
    # protected root that disappeared (or a source that was moved) shows up
    # here either as a missing or as a stale mask, because the expectation
    # is recomputed from the current configuration and file system.
    expected = sorted(topology.get("masks") or [])
    rendered_masks = sorted(tmpfs_paths)
    for path in expected:
        if path not in rendered_masks:
            errors.append(
                "[M6] The mask '{0}' is missing. The storage layout changed "
                "since the stack was rendered.".format(path)
            )
    for path in rendered_masks:
        if path not in expected:
            errors.append(
                "[M6] The mask '{0}' is stale. The storage layout changed "
                "since the stack was rendered.".format(path)
            )
    if errors:
        errors.append(
            "Remedy: open Services -> FileBrowser Quantum -> Settings, click "
            "Save and then Apply."
        )
    return errors


def mask_report(topology, yml_path):
    """Human readable anti-recursion report."""
    lines = []
    lines.append("=" * 78)
    lines.append("FileBrowser Quantum anti-recursion mask report")
    lines.append("=" * 78)
    lines.append("Protected roots:")
    if not topology["protected"]:
        lines.append("  (none - automatic masking is disabled)")
    for key in sorted(topology["protected"]):
        label, path = topology["protected"][key]
        state = "exists" if os.path.isdir(path) else "not found on host"
        lines.append("  - {0:<26}: {1} ({2})".format(label, path, state))
    lines.append("")
    lines.append("Configured storage sources:")
    if not topology["sources"]:
        lines.append("  (none)")
    for source in topology["sources"]:
        mode = "ro" if source["readonly"] else "rw (WARNING: writable)"
        lines.append(
            "  - [{0}] -> {1} ({2})".format(source["path"], source["host"], mode)
        )
    lines.append("")
    lines.append("Expected tmpfs masks:")
    masks = topology.get("masks") or []
    if not masks:
        lines.append("  (no protected path is exposed below a storage source)")
    for path in masks:
        lines.append("  - {0} (mode=0500, size=1MiB)".format(path))
    lines.append("")
    lines.append("Masks rendered in the stack file:")
    rendered = []
    if yml_path and os.path.isfile(yml_path):
        _, rendered = parse_stack_file(yml_path)
    if not rendered:
        lines.append("  (none)")
    for entry in rendered:
        lines.append("  - {0}".format(entry))
    lines.append("=" * 78)
    return "\n".join(lines)


# ---------------------------------------------------------------------
# Runtime verification (Salt state)
# ---------------------------------------------------------------------

def container_status():
    """'running' | 'stopped' | 'notinstalled'."""
    code, output = run_capture(
        ["docker", "inspect", "--format", "{{.State.Status}}", "filebrowserquantum"]
    )
    state = output.strip()
    if 0 != code or not state:
        return "notinstalled"
    return "running" if "running" == state else "stopped"


def container_health():
    """Health state reported by Docker ('none' when no health check)."""
    code, output = run_capture(
        ["docker", "inspect", "--format",
         "{{if .State.Health}}{{.State.Health.Status}}{{else}}none{{end}}",
         "filebrowserquantum"]
    )
    state = output.strip()
    return state if (0 == code and state) else "unknown"


def check_runtime():
    """Raise MaskError unless the container is running and healthy."""
    status = container_status()
    if "running" != status:
        raise MaskError(
            "The FileBrowser Quantum container is not running (state: {0}). "
            "Check the container log.".format(status)
        )
    health = container_health()
    if health in ("unhealthy", "unknown"):
        raise MaskError(
            "The FileBrowser Quantum container is not healthy (health: "
            "{0}). Check the container log.".format(health)
        )
    return health


# ---------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------

def _fail(message):
    sys.stderr.write("ERROR: " + message + "\n")
    return 1


def _topology(reader=None):
    return build_topology(reader or omv_reader)


def main(argv):
    command = argv[1] if len(argv) > 1 else ""
    if command not in ("gate", "prepare-config", "maskcheck", "maskreport", "verify"):
        sys.stderr.write(
            "Usage: fbq_tools.py <gate|prepare-config|maskcheck|maskreport|verify>\n"
        )
        return 64
    try:
        topology = _topology()
        if "gate" == command:
            check_gate(topology)
            return 0
        if "verify" == command:
            health = check_runtime()
            print("running ({0})".format(health))
            return 0
        if "prepare-config" == command:
            if not topology["data_host"] or not os.path.isdir(topology["data_host"]):
                raise GateError(
                    "The data folder is not configured or does not exist, "
                    "config.yaml was not written."
                )
            path, changed = sync_config(
                topology["data_host"], topology["main"], topology["sources"]
            )
            print("{0} ({1})".format(path, "updated" if changed else "unchanged"))
            return 0
        if "maskreport" == command:
            print(mask_report(topology, argv[2] if len(argv) > 2 else ""))
            return 0
        # maskcheck
        yml_path = argv[2] if len(argv) > 2 else ""
        check_gate(topology)
        errors = check_stack(topology, yml_path)
        if errors:
            for error in errors:
                sys.stderr.write(error + "\n")
            sys.stderr.write(
                "The stack was NOT started: the anti-recursion invariants "
                "are violated.\n"
            )
            return 1
        return 0
    except FbqError as exc:
        return _fail(str(exc))


if __name__ == "__main__":
    sys.exit(main(sys.argv))
