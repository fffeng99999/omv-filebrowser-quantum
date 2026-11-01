#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Test suite for openmediavault-filebrowser-quantum (8.1.5).

The tests execute the production code of the plugin:
``usr/share/openmediavault/filebrowserquantum/fbq_tools.py`` is imported and
its functions are called directly. The ctl runs exactly these functions
(gate / prepare-config / maskcheck / verify), so a passing test suite says
something about the shipped behaviour instead of describing a second copy
of the algorithm.

Covered:
  * path helpers and compose volume parsing
  * anti-recursion mask computation (R-3, R-7) and the M1..M6 stack checks
  * fail-closed start gate (enabled, data folder, at least one source)
  * declarative config.yaml synchronisation: auth methods, baseURL /
    externalUrl round trip, merge with operator settings, refusing to
    overwrite an unparsable file, atomic write + backup
  * the shipped CLI entry point (gate / prepare-config / maskcheck),
    with the configuration database reader replaced by a stub

Run:
    python3 scripts/test_anti_recursion.py
"""

from __future__ import annotations

import contextlib
import importlib.util
import io
import os
import pathlib
import shutil
import unittest
import uuid

import yaml

ROOT = pathlib.Path(__file__).resolve().parent.parent
TMP_ROOT = ROOT / ".tmp-tests"
TOOLS_PATH = ROOT / "usr" / "share" / "openmediavault" / "filebrowserquantum" / "fbq_tools.py"

_spec = importlib.util.spec_from_file_location("fbq_tools", TOOLS_PATH)
fbq = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(fbq)

RESTRICTED_PREFIX = "/srv/dev-disk-by-uuid-6dab12c5-4b76-461a-9c3a-7f610aca2f74"


def tempdir():
    """Temporary directory below the repository (works in the sandbox)."""
    TMP_ROOT.mkdir(parents=True, exist_ok=True)
    path = TMP_ROOT / ("case-" + uuid.uuid4().hex[:12])
    path.mkdir(parents=True, exist_ok=True)
    return str(path)


def make_topology(data_host="/srv/arr/data", sources=None, data_ref="sf-data",
                  compose_host="/srv/arr/compose", mask_protected=True,
                  existing=("data", "compose")):
    """Build a topology dict the way build_topology() would (no confdb)."""
    storage = sources if sources is not None else [
        {"uuid": "s1", "ref": "sf-pool", "host": "/srv/arr/pool",
         "path": "/srv/pool", "name": "pool", "readonly": False,
         "defaultEnabled": True},
    ]
    protected = {}
    if data_host:
        protected["data"] = ("FileBrowser data folder", data_host)
    if mask_protected and compose_host:
        protected["compose"] = ("Compose shared folder", compose_host)
    existing = set(existing)
    return {
        "main": {
            "enable": 1,
            "dataRef": data_ref,
            "maskProtected": 1 if mask_protected else 0,
            "authMethod": "password",
            "baseURL": "",
            "appUrl": "",
        },
        "sources": storage,
        "sharedfolders": [],
        "mountpoints": [],
        "data_ref": data_ref,
        "data_host": data_host,
        "compose_ref": "sf-compose",
        "compose_host": compose_host,
        "rustfs_host": "",
        "docker_root": "/var/lib/docker",
        "mask_protected": mask_protected,
        "protected": protected,
        "existing_roots": existing,
        "masks": fbq.compute_masks(storage, protected, existing_roots=existing),
    }


class TestPathHelpers(unittest.TestCase):
    def test_strict_descendant(self):
        self.assertTrue(fbq.is_strict_descendant("/srv/pool/data", "/srv/pool"))
        self.assertTrue(fbq.is_strict_descendant("/srv/pool/sub/data", "/srv/pool"))
        self.assertFalse(fbq.is_strict_descendant("/srv/pool", "/srv/pool"))
        self.assertFalse(fbq.is_strict_descendant("/srv/pool2", "/srv/pool"))
        self.assertFalse(fbq.is_strict_descendant("/srv", "/srv/pool"))
        self.assertFalse(fbq.is_strict_descendant("", "/srv/pool"))

    def test_get_rel_path(self):
        self.assertEqual(fbq.get_rel_path("/srv/pool/data", "/srv/pool"), "data")
        self.assertEqual(fbq.get_rel_path("/srv/pool/a/b", "/srv/pool"), "a/b")
        self.assertEqual(fbq.get_rel_path("/srv/pool", "/srv/pool"), "")

    def test_parse_volume(self):
        self.assertEqual(fbq.parse_volume('${{ sf:"data" }}:/home/filebrowser/data'),
                         ("/home/filebrowser/data", False))
        self.assertEqual(fbq.parse_volume('${{ sf:"media" }}:/srv/media:ro'),
                         ("/srv/media", True))
        self.assertEqual(fbq.parse_volume("/var/srv/docs:/srv/docs"),
                         ("/srv/docs", False))
        self.assertEqual(fbq.parse_volume("/var/srv/docs:/srv/docs:ro"),
                         ("/srv/docs", True))

    def test_resolve_shared_folder_path(self):
        sharedfolders = [
            {"uuid": "sf-1", "mntentref": "mnt-1", "reldirpath": "pool/"},
        ]
        mountpoints = [{"uuid": "mnt-1", "dir": "/srv/arr/"}]
        self.assertEqual(
            fbq.resolve_sf_path("sf-1", sharedfolders, mountpoints),
            "/srv/arr/pool",
        )
        self.assertEqual(fbq.resolve_sf_path("", sharedfolders, mountpoints), "")
        self.assertEqual(fbq.resolve_sf_path("nope", sharedfolders, mountpoints), "")


class TestMaskEngine(unittest.TestCase):
    def test_mask_generation(self):
        sources = [{"host": "/srv/pool", "path": "/srv/pool", "readonly": False}]
        protected = {
            "data": ("data", "/srv/pool/fbq-data"),
            "compose": ("compose", "/srv/pool/compose"),
            "rustfs": ("rustfs", "/srv/pool/rustfs-data"),
            "docker": ("docker", "/var/lib/docker"),
        }
        masks = fbq.compute_masks(sources, protected)
        self.assertEqual(masks, [
            "/srv/pool/compose",
            "/srv/pool/fbq-data",
            "/srv/pool/rustfs-data",
        ])

    def test_protected_root_not_on_host_is_skipped(self):
        sources = [{"host": "/srv/pool", "path": "/srv/pool"}]
        protected = {
            "data": ("data", "/srv/pool/fbq-data"),
            "docker": ("docker", "/srv/pool/docker"),
        }
        masks = fbq.compute_masks(sources, protected, existing_roots={"data"})
        self.assertEqual(masks, ["/srv/pool/fbq-data"])

    def test_identical_protected_root_is_rejected(self):
        # R-3: a protected root can never be identical to the source root,
        # masking requires a path below it.
        sources = [{"host": "/srv/pool/compose", "path": "/srv/compose"}]
        protected = {"compose": ("compose", "/srv/pool/compose")}
        with self.assertRaises(fbq.MaskError) as ctx:
            fbq.compute_masks(sources, protected)
        self.assertIn("R-3", str(ctx.exception))

    def test_max_masks_limit(self):
        # R-7: more than 32 masks must be rejected instead of silently
        # rendering a huge tmpfs list.
        protected = {
            "p%02d" % index: ("root%d" % index, "/srv/pool/sensitive%02d" % index)
            for index in range(33)
        }
        sources = [{"host": "/srv/pool", "path": "/srv/pool"}]
        with self.assertRaises(fbq.MaskError) as ctx:
            fbq.compute_masks(sources, protected)
        self.assertIn("Too many", str(ctx.exception))

    def test_red_line_volume_is_allowed(self):
        # Red line #9 only restricts the AI agent, not the operator: the
        # production volume may be used as data folder and as source.
        topology = make_topology(data_host=RESTRICTED_PREFIX + "/data")
        self.assertIsInstance(topology["masks"], list)


class TestGate(unittest.TestCase):
    def test_complete_configuration_passes(self):
        tmp = tempdir()
        try:
            os.makedirs(os.path.join(tmp, "data"))
            os.makedirs(os.path.join(tmp, "pool"))
            os.makedirs(os.path.join(tmp, "compose"))
            topology = make_topology(
                data_host=os.path.join(tmp, "data"),
                compose_host=os.path.join(tmp, "compose"),
            )
            topology["sources"][0]["host"] = os.path.join(tmp, "pool")
            fbq.check_gate(topology)
        finally:
            shutil.rmtree(tmp, ignore_errors=True)

    def test_disabled_service_is_rejected(self):
        tmp = tempdir()
        try:
            for name in ("data", "pool", "compose"):
                os.makedirs(os.path.join(tmp, name))
            topology = make_topology(
                data_host=os.path.join(tmp, "data"),
                compose_host=os.path.join(tmp, "compose"),
            )
            topology["sources"][0]["host"] = os.path.join(tmp, "pool")
            topology["main"]["enable"] = 0
            with self.assertRaises(fbq.GateError) as ctx:
                fbq.check_gate(topology)
            self.assertIn("disabled", str(ctx.exception))
        finally:
            shutil.rmtree(tmp, ignore_errors=True)

    def test_missing_data_folder_is_rejected(self):
        topology = make_topology(data_host="", data_ref="")
        with self.assertRaises(fbq.GateError) as ctx:
            fbq.check_gate(topology)
        self.assertIn("data shared folder", str(ctx.exception))

    def test_missing_storage_source_is_rejected(self):
        tmp = tempdir()
        try:
            os.makedirs(os.path.join(tmp, "data"))
            os.makedirs(os.path.join(tmp, "compose"))
            topology = make_topology(
                data_host=os.path.join(tmp, "data"),
                compose_host=os.path.join(tmp, "compose"),
                sources=[],
            )
            with self.assertRaises(fbq.GateError) as ctx:
                fbq.check_gate(topology)
            self.assertIn("storage source", str(ctx.exception))
        finally:
            shutil.rmtree(tmp, ignore_errors=True)

    def test_missing_compose_folder_is_rejected(self):
        tmp = tempdir()
        try:
            for name in ("data", "pool"):
                os.makedirs(os.path.join(tmp, name))
            topology = make_topology(
                data_host=os.path.join(tmp, "data"),
                compose_host="",
            )
            topology["sources"][0]["host"] = os.path.join(tmp, "pool")
            topology["compose_ref"] = ""
            with self.assertRaises(fbq.GateError) as ctx:
                fbq.check_gate(topology)
            self.assertIn("Compose", str(ctx.exception))
        finally:
            shutil.rmtree(tmp, ignore_errors=True)


class TestStackChecks(unittest.TestCase):
    def setUp(self):
        self.tmp = tempdir()
        self.yml = os.path.join(self.tmp, "filebrowserquantum.yml")

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def write_stack(self, volumes, tmpfs=None):
        lines = ["name: filebrowserquantum", "", "services:", "  filebrowserquantum:",
                 "    image: ghcr.io/gtsteffaniak/filebrowser:2.0.10-beta",
                 "    volumes:"]
        lines += ["      - " + volume for volume in volumes]
        if tmpfs:
            lines.append("    tmpfs:")
            lines += ["      - " + entry for entry in tmpfs]
        pathlib.Path(self.yml).write_text("\n".join(lines) + "\n", encoding="utf-8")

    def test_consistent_stack_passes(self):
        topology = make_topology(compose_host="/srv/arr/pool/compose")
        self.write_stack(
            ['${{ sf:"data" }}:/home/filebrowser/data',
             '${{ sf:"pool" }}:/srv/pool:ro'],
            ["/srv/pool/compose:size=1048576,mode=0500"],
        )
        topology["sources"][0]["readonly"] = True
        self.assertEqual(fbq.check_stack(topology, self.yml), [])

    def test_unregistered_bind_mount_is_rejected(self):
        # M2 (both directions): a mount that no storage source uses means
        # the stack no longer matches the configuration.
        topology = make_topology()
        self.write_stack(
            ['${{ sf:"data" }}:/home/filebrowser/data',
             '${{ sf:"pool" }}:/srv/pool',
             '${{ sf:"old" }}:/srv/old-pool'],
            ["/srv/pool/compose:size=1048576,mode=0500"],
        )
        errors = "\n".join(fbq.check_stack(topology, self.yml))
        self.assertIn("[M2]", errors)
        self.assertIn("/srv/old-pool", errors)

    def test_missing_source_mount_is_rejected(self):
        topology = make_topology()
        self.write_stack(
            ['${{ sf:"data" }}:/home/filebrowser/data'],
            ["/srv/pool/compose:size=1048576,mode=0500"],
        )
        errors = "\n".join(fbq.check_stack(topology, self.yml))
        self.assertIn("[M2]", errors)
        self.assertIn("/srv/pool", errors)

    def test_readonly_drift_is_rejected(self):
        topology = make_topology()
        topology["sources"][0]["readonly"] = True
        self.write_stack(
            ['${{ sf:"data" }}:/home/filebrowser/data',
             '${{ sf:"pool" }}:/srv/pool'],
            ["/srv/pool/compose:size=1048576,mode=0500"],
        )
        errors = "\n".join(fbq.check_stack(topology, self.yml))
        self.assertIn("read-only", errors)

    def test_missing_data_mount_is_rejected(self):
        topology = make_topology()
        self.write_stack(['${{ sf:"pool" }}:/srv/pool'])
        errors = "\n".join(fbq.check_stack(topology, self.yml))
        self.assertIn("[M1]", errors)

    def test_stale_mask_is_rejected(self):
        topology = make_topology()
        self.write_stack(
            ['${{ sf:"data" }}:/home/filebrowser/data',
             '${{ sf:"pool" }}:/srv/pool'],
            ["/srv/pool/compose:size=1048576,mode=0500",
             "/srv/old/compose:size=1048576,mode=0500"],
        )
        errors = "\n".join(fbq.check_stack(topology, self.yml))
        self.assertIn("[M6]", errors)

    def test_missing_stack_file(self):
        topology = make_topology()
        errors = fbq.check_stack(topology, os.path.join(self.tmp, "missing.yml"))
        self.assertEqual(len(errors), 1)
        self.assertIn("not found", errors[0])


class TestConfigSync(unittest.TestCase):
    def setUp(self):
        self.tmp = tempdir()

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def read(self):
        return yaml.safe_load(
            pathlib.Path(self.tmp, "config.yaml").read_text(encoding="utf-8"))

    def main(self, **kwargs):
        base = {"baseURL": "", "appUrl": "", "authMethod": "password",
                "trustProxyHeaders": 0, "authProxyHeader": "X-Forwarded-User"}
        base.update(kwargs)
        return base

    def sources(self, *paths):
        return [{"path": path, "name": path.strip("/").split("/")[-1],
                 "defaultEnabled": True} for path in paths]

    def test_first_sync_writes_upstream_keys(self):
        fbq.sync_config(self.tmp, self.main(), self.sources("/srv/b", "/srv/a"))
        document = self.read()
        self.assertEqual(document["server"]["sources"][0]["path"], "/srv/a")
        self.assertEqual(document["server"]["sources"][1]["path"], "/srv/b")
        self.assertEqual(document["server"]["cacheDir"], "/home/filebrowser/data/tmp")
        self.assertEqual(document["http"]["baseURL"], "/")
        self.assertEqual(document["http"]["externalUrl"], "")
        self.assertTrue(document["auth"]["methods"]["password"]["enabled"])
        self.assertFalse(document["auth"]["methods"]["noauth"])
        self.assertFalse(document["auth"]["methods"]["proxy"]["enabled"])

    def test_urls_are_written_and_cleared(self):
        fbq.sync_config(
            self.tmp,
            self.main(baseURL="/files/", appUrl="https://files.example.com/files"),
            self.sources("/srv/a"),
        )
        document = self.read()
        self.assertEqual(document["http"]["baseURL"], "/files")
        # externalUrl carries the origin only - upstream appends baseURL.
        self.assertEqual(document["http"]["externalUrl"], "https://files.example.com")

        # Clearing the base URL must really clear it (8.1.4 kept the old one).
        fbq.sync_config(self.tmp, self.main(baseURL="", appUrl=""),
                        self.sources("/srv/a"))
        document = self.read()
        self.assertEqual(document["http"]["baseURL"], "/")
        self.assertEqual(document["http"]["externalUrl"], "")

    def test_auth_methods_are_mapped_to_upstream_config(self):
        fbq.sync_config(self.tmp, self.main(authMethod="noauth"),
                        self.sources("/srv/a"))
        methods = self.read()["auth"]["methods"]
        self.assertTrue(methods["noauth"])
        self.assertFalse(methods["password"]["enabled"])

        fbq.sync_config(
            self.tmp,
            self.main(authMethod="proxy", authProxyHeader="X-Remote-User"),
            self.sources("/srv/a"),
        )
        methods = self.read()["auth"]["methods"]
        self.assertFalse(methods["noauth"])
        self.assertTrue(methods["proxy"]["enabled"])
        self.assertEqual(methods["proxy"]["header"], "X-Remote-User")
        self.assertFalse(methods["password"]["enabled"])

    def test_operator_settings_are_preserved(self):
        fbq.sync_config(self.tmp, self.main(), self.sources("/srv/a"))
        path = pathlib.Path(self.tmp, "config.yaml")
        document = self.read()
        document["branding"] = {"name": "Home"}
        document["server"]["maxUploadSize"] = 104857600
        document["auth"]["methods"]["password"]["minLength"] = 7
        path.write_text(yaml.safe_dump(document), encoding="utf-8")

        fbq.sync_config(self.tmp, self.main(), self.sources("/srv/a", "/srv/b"))
        document = self.read()
        self.assertEqual(document["branding"], {"name": "Home"})
        self.assertEqual(document["server"]["maxUploadSize"], 104857600)
        self.assertEqual(document["auth"]["methods"]["password"]["minLength"], 7)
        self.assertEqual(len(document["server"]["sources"]), 2)

    def test_unparsable_file_is_not_overwritten(self):
        path = pathlib.Path(self.tmp, "config.yaml")
        broken = "server:\n  sources: [unclosed\n"
        path.write_text(broken, encoding="utf-8")
        with self.assertRaises(fbq.ConfigFileError):
            fbq.sync_config(self.tmp, self.main(), self.sources("/srv/a"))
        self.assertEqual(path.read_text(encoding="utf-8"), broken)

    def test_write_is_atomic_and_keeps_a_backup(self):
        fbq.sync_config(self.tmp, self.main(), self.sources("/srv/a"))
        backup = pathlib.Path(self.tmp, "config.yaml.omv-bak")
        fbq.sync_config(self.tmp, self.main(), self.sources("/srv/a", "/srv/b"))
        self.assertTrue(backup.is_file())
        self.assertIn("/srv/a", backup.read_text(encoding="utf-8"))
        leftovers = [name for name in os.listdir(self.tmp)
                     if name.startswith(".config.yaml.")]
        self.assertEqual(leftovers, [])

    def test_invalid_urls_are_rejected(self):
        for kwargs in ({"baseURL": "files"}, {"baseURL": "/a b"},
                       {"appUrl": "files.example.com"},
                       {"appUrl": "https://example.com/a b"}):
            with self.assertRaises(fbq.ConfigFileError):
                fbq.build_config_document({}, self.main(**kwargs), [])

    def test_sources_are_removed_from_config(self):
        fbq.sync_config(self.tmp, self.main(), self.sources("/srv/a"))
        fbq.sync_config(self.tmp, self.main(), [])
        self.assertEqual(self.read()["server"]["sources"], [])


class TestCli(unittest.TestCase):
    def setUp(self):
        self.root = pathlib.Path(tempdir())
        self.data_dir = self.root / "data"
        self.source_dir = self.root / "pool"
        self.compose_dir = self.root / "compose"
        for directory in (self.data_dir, self.source_dir, self.compose_dir):
            directory.mkdir()
        self._saved_reader = fbq.omv_reader
        self.confdb = {}

    def tearDown(self):
        fbq.omv_reader = self._saved_reader
        shutil.rmtree(self.root, ignore_errors=True)

    def posix(self, path: pathlib.Path) -> str:
        return str(path).replace("\\", "/")

    def write_confdb(self, main=None, sources=None):
        settings = {
            "enable": 1,
            "dataRef": "sf-data",
            "maskProtected": 1,
            "authMethod": "password",
            "baseURL": "",
            "appUrl": "",
        }
        settings.update(main or {})
        if sources is None:
            sources = [
                {"uuid": "sf-pool-src", "sharedfolderref": "sf-pool",
                 "path": "/srv/pool", "name": "pool", "readonly": 0,
                 "defaultEnabled": 1},
            ]
        self.confdb = {
            "conf.service.filebrowserquantum": settings,
            "conf.service.filebrowserquantum.source": sources,
            "conf.system.sharedfolder": [
                {"uuid": "sf-data", "mntentref": "mnt-1",
                 "reldirpath": self.data_dir.name},
                {"uuid": "sf-pool", "mntentref": "mnt-1",
                 "reldirpath": self.source_dir.name},
                {"uuid": "sf-compose", "mntentref": "mnt-1",
                 "reldirpath": self.compose_dir.name},
            ],
            "conf.system.filesystem.mountpoint": [
                {"uuid": "mnt-1", "dir": self.posix(self.root)},
            ],
            "conf.service.compose": {"sharedfolderref": "sf-compose"},
            "conf.service.rustfs": {},
        }

    def run_tools(self, *args):
        confdb = self.confdb
        fbq.omv_reader = lambda config_id: confdb.get(config_id)
        stdout, stderr = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
            code = fbq.main(["fbq_tools.py", *args])
        return code, stdout.getvalue(), stderr.getvalue()

    def test_gate_passes_for_complete_configuration(self):
        self.write_confdb()
        code, _, err = self.run_tools("gate")
        self.assertEqual(code, 0, err)

    def test_gate_fails_when_disabled(self):
        self.write_confdb(main={"enable": 0})
        code, _, err = self.run_tools("gate")
        self.assertEqual(code, 1)
        self.assertIn("disabled", err)

    def test_gate_fails_without_data_folder(self):
        self.write_confdb(main={"dataRef": ""})
        code, _, err = self.run_tools("gate")
        self.assertEqual(code, 1)
        self.assertIn("data shared folder", err)

    def test_gate_fails_without_storage_source(self):
        self.write_confdb(sources=[])
        code, _, err = self.run_tools("gate")
        self.assertEqual(code, 1)
        self.assertIn("storage source", err)

    def test_gate_fails_when_the_source_folder_is_missing(self):
        self.write_confdb()
        self.confdb["conf.system.sharedfolder"][1]["reldirpath"] = "gone"
        code, _, err = self.run_tools("gate")
        self.assertEqual(code, 1)
        self.assertIn("does not exist", err)

    def test_gate_fails_without_compose_folder(self):
        self.write_confdb()
        self.confdb["conf.service.compose"] = {"sharedfolderref": ""}
        code, _, err = self.run_tools("gate")
        self.assertEqual(code, 1)
        self.assertIn("Compose", err)

    def test_prepare_config_writes_config_yaml(self):
        self.write_confdb(main={"baseURL": "/files",
                                "appUrl": "https://files.example.com/files",
                                "authMethod": "proxy"})
        code, out, err = self.run_tools("prepare-config")
        self.assertEqual(code, 0, err)
        self.assertIn("config.yaml", out)
        config = self.data_dir / "config.yaml"
        self.assertTrue(config.is_file())
        text = config.read_text(encoding="utf-8")
        self.assertIn("/srv/pool", text)
        self.assertIn("baseURL: /files", text)
        self.assertIn("externalUrl: https://files.example.com", text)
        self.assertIn("proxy:", text)

    def test_prepare_config_refuses_broken_yaml(self):
        self.write_confdb()
        config = self.data_dir / "config.yaml"
        config.write_text("server: [unclosed\n", encoding="utf-8")
        code, _, err = self.run_tools("prepare-config")
        self.assertEqual(code, 1)
        self.assertIn("cannot be parsed", err)
        self.assertEqual(config.read_text(encoding="utf-8"), "server: [unclosed\n")

    def test_prepare_config_fails_without_data_folder(self):
        self.write_confdb(main={"dataRef": ""})
        code, _, err = self.run_tools("prepare-config")
        self.assertEqual(code, 1)
        self.assertIn("data folder", err)

    def test_maskcheck_fails_without_stack_file(self):
        self.write_confdb()
        code, _, err = self.run_tools(
            "maskcheck", str(self.compose_dir / "missing.yml"))
        self.assertEqual(code, 1)
        self.assertIn("not found", err)

    def test_maskcheck_passes_for_a_consistent_stack(self):
        self.write_confdb()
        stack = self.compose_dir / "filebrowserquantum.yml"
        stack.write_text(
            "name: filebrowserquantum\n"
            "services:\n"
            "  filebrowserquantum:\n"
            "    volumes:\n"
            '      - ${{ sf:"data" }}:/home/filebrowser/data\n'
            '      - ${{ sf:"pool" }}:/srv/pool\n',
            encoding="utf-8",
        )
        code, _, err = self.run_tools("maskcheck", str(stack))
        self.assertEqual(code, 0, err)

    def test_maskcheck_fails_for_an_unregistered_mount(self):
        self.write_confdb()
        stack = self.compose_dir / "filebrowserquantum.yml"
        stack.write_text(
            "name: filebrowserquantum\n"
            "services:\n"
            "  filebrowserquantum:\n"
            "    volumes:\n"
            '      - ${{ sf:"data" }}:/home/filebrowser/data\n'
            '      - ${{ sf:"pool" }}:/srv/pool\n'
            '      - ${{ sf:"old" }}:/srv/old\n',
            encoding="utf-8",
        )
        code, _, err = self.run_tools("maskcheck", str(stack))
        self.assertEqual(code, 1)
        self.assertIn("[M2]", err)

    def test_maskcheck_fails_when_disabled(self):
        self.write_confdb(main={"enable": 0})
        stack = self.compose_dir / "filebrowserquantum.yml"
        stack.write_text("name: filebrowserquantum\n", encoding="utf-8")
        code, _, err = self.run_tools("maskcheck", str(stack))
        self.assertEqual(code, 1)
        self.assertIn("disabled", err)

    def test_unknown_command(self):
        code, _, err = self.run_tools("frobnicate")
        self.assertEqual(code, 64)
        self.assertIn("Usage", err)


if __name__ == "__main__":
    unittest.main(verbosity=2)
