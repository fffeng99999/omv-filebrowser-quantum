<?php

/**
 * This file is part of OpenMediaVault.
 *
 * @license   https://www.gnu.org/licenses/gpl.html GPL Version 3
 * @author    ${GITHUB_USER} <${GITHUB_USER}@users.noreply.github.com>
 *
 * OpenMediaVault is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 *
 * OpenMediaVault is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

namespace Engined\Rpc;

/**
 * FileBrowser Quantum RPC service.
 *
 * Settings (conf.service.filebrowserquantum) and dynamic multi-storage
 * sources (conf.service.filebrowserquantum.source) live in the OMV
 * configuration database. The FileBrowser Quantum Docker container is
 * owned by the openmediavault-compose plugin: on save the plugin
 * publishes (or updates) the stack through the Compose RPC
 * (Compose.setFile). The stack follows the Compose plugin conventions
 * (<compose shared folder>/filebrowserquantum/filebrowserquantum.yml +
 * filebrowserquantum.env) and shows up in the Compose web UI. The plugin
 * never renders those files directly to disk, so the Compose plugin stays
 * the single writer.
 *
 * The stack is controlled through 'docker compose' (via the
 * /usr/sbin/omv-compose-run helper used by the Compose plugin) with the
 * /usr/sbin/omv-filebrowserquantum-ctl helper.
 */
class OMVRpcServiceFileBrowserQuantum extends \OMV\Rpc\ServiceAbstract
{
	const CTL = "/usr/sbin/omv-filebrowserquantum-ctl";

	/**
	 * Name of the stack in the openmediavault-compose plugin. It is
	 * also the name of the stack directory below the compose shared
	 * folder and the base name of the generated stack files.
	 */
	const STACK_NAME = "filebrowserquantum";

	/**
	 * Docker image repository (the tag is appended from the version and
	 * image variant settings).
	 */
	const IMAGE_REPO = "ghcr.io/gtsteffaniak/filebrowser";

	/**
	 * Production NAS restricted volume prefix (Red line #9).
	 * Any access or inspection is strictly forbidden.
	 */
	const RESTRICTED_VOLUME_PREFIX = "/srv/dev-disk-by-uuid-6dab12c5-4b76-461a-9c3a-7f610aca2f74";

	/**
	 * Local cache file for latest upstream release tag (Zero network I/O in get()).
	 */
	const LATEST_CACHE_FILE = "/var/lib/openmediavault/filebrowserquantum-latest.json";

	/**
	 * Maximum allowed tmpfs anti-recursion masking mounts (R-7).
	 */
	const MAX_MASKS = 32;

	/**
	 * Reserved container path prefixes (R-6).
	 */
	const RESERVED_PREFIXES = [
		"/app", "/etc", "/proc", "/sys", "/dev", "/var",
		"/usr", "/bin", "/sbin", "/lib", "/lib64", "/tmp",
		"/home/filebrowser/data", "/home/filebrowser"
	];

	/**
	 * Reserved environment variable keys.
	 */
	const EXTRA_OPTIONS_BLACK_LIST = [
		"PUID", "PGID", "PORT", "VERSION", "BASEURL", "UMASK"
	];

	/**
	 * Settings fields allowed to be persisted by set().
	 */
	private $settingsFields = [
		"enable", "version", "port", "imageVariant",
		"dataRef", "sourceRef", "baseURL", "appUrl",
		"trustProxyHeaders", "authMethod", "puid", "pgid",
		"maskProtected", "extraOptions"
	];

	public function getName()
	{
		return "FileBrowserQuantum";
	}

	public function initialize()
	{
		$this->registerMethod("get");
		$this->registerMethod("set");
		$this->registerMethod("getLatestVersion");
		$this->registerMethod("getSources");
		$this->registerMethod("getSource");
		$this->registerMethod("setSource");
		$this->registerMethod("deleteSource");
		$this->registerMethod("start");
		$this->registerMethod("stop");
		$this->registerMethod("restart");
		$this->registerMethod("upgrade");
		$this->registerMethod("getLog");
		$this->registerMethod("getShowConfig");
	}

	/**
	 * Run omv-filebrowserquantum-ctl with the given arguments and return
	 * the trimmed stdout. Throws on failure when $throwOnError is set.
	 */
	private function runCtl(array $args, bool $throwOnError = true): array
	{
		$cmd = new \OMV\System\Process(self::CTL, $args);
		$cmd->setRedirect2to1();
		$cmd->setQuiet(true);
		$output = [];
		$exitStatus = 0;
		$cmd->execute($output, $exitStatus);
		if (0 !== $exitStatus && $throwOnError) {
			throw new \OMV\ExecException($cmd, $output, $exitStatus);
		}
		return [$exitStatus, $output];
	}

	private function getSettingsObject($context): array
	{
		return \OMV\Rpc\Rpc::call("Config", "get", [
			"id" => "conf.service.filebrowserquantum"
		], $context);
	}

	private function channelOf(string $version): string
	{
		return (false !== strpos($version, "-beta")) ? "beta" : "stable";
	}

	private function imageOf(string $version, string $variant): string
	{
		$tag = trim($version);
		if ("slim" === $variant) {
			$tag .= "-slim";
		}
		return self::IMAGE_REPO . ":" . $tag;
	}

	/**
	 * Read cached latest version from disk without any network requests.
	 */
	private function readCachedLatestVersion(): array
	{
		if (is_readable(self::LATEST_CACHE_FILE)) {
			$data = json_decode((string)file_get_contents(self::LATEST_CACHE_FILE), true);
			if (is_array($data) && !empty($data["version"])) {
				return $data;
			}
		}
		return ["version" => "", "timestamp" => 0];
	}

	/**
	 * Cache latest upstream version tag to disk.
	 */
	private function writeCachedLatestVersion(string $tag): void
	{
		@file_put_contents(self::LATEST_CACHE_FILE, json_encode([
			"version" => $tag,
			"timestamp" => time()
		]));
	}

	/**
	 * Compute whether an update is available between running and latest.
	 */
	private function computeUpdateAvailable(string $runningVersion, string $latestVersion): bool
	{
		$a = preg_replace('/-slim$/', "", trim($runningVersion));
		$a = ltrim($a, "v");
		$b = preg_replace('/-slim$/', "", trim($latestVersion));
		$b = ltrim($b, "v");
		if (("" !== $a) && ("" !== $b)) {
			return version_compare($a, $b, "<");
		}
		return false;
	}

	/**
	 * Resolve shared folder UUID to host absolute path.
	 */
	private function resolveSharedFolderPath(string $ref): string
	{
		if (empty($ref)) {
			return "";
		}
		$db = \OMV\Config\Database::getInstance();
		$sf = $db->get("conf.system.sharedfolder", $ref);
		if (!$sf) {
			return "";
		}
		$mntent = $db->get("conf.system.filesystem.mountpoint", $sf->get("mntentref"));
		if (!$mntent) {
			return "";
		}
		$dirpath = rtrim($mntent->get("dir"), "/");
		$reldirpath = ltrim($sf->get("reldirpath"), "/");
		$path = $dirpath . "/" . $reldirpath;
		$real = realpath($path);
		return ($real !== false) ? $real : $path;
	}

	/**
	 * Name of a shared folder by reference (empty when unknown).
	 */
	private function sharedFolderName($context, string $ref): string
	{
		if ("" === $ref) {
			return "";
		}
		$folders = \OMV\Rpc\Rpc::call("Config", "get", [
			"id" => "conf.system.sharedfolder"
		], $context);
		if (!is_array($folders)) {
			return "";
		}
		foreach ($folders as $folder) {
			if (($folder["uuid"] ?? "") === $ref) {
				return (string)($folder["name"] ?? "");
			}
		}
		return "";
	}

	/**
	 * Red line #9: Assert host path does not touch the restricted production volume.
	 */
	private function assertNotRestrictedVolume(string $hostPath): void
	{
		if (str_starts_with($hostPath, self::RESTRICTED_VOLUME_PREFIX)) {
			throw new \OMV\Exception(
				"The selected shared folder points to a restricted volume and cannot be configured in FileBrowser Quantum."
			);
		}
	}

	/**
	 * Check if $child is a strict descendant of $parent.
	 */
	private function isStrictDescendant(string $child, string $parent): bool
	{
		$c = rtrim($child, "/");
		$p = rtrim($parent, "/");
		return ($c !== $p) && str_starts_with($c, $p . "/");
	}

	/**
	 * Get relative path of $child under $parent.
	 */
	private function getRelativePath(string $child, string $parent): string
	{
		$c = rtrim($child, "/");
		$p = rtrim($parent, "/");
		if (str_starts_with($c, $p . "/")) {
			return substr($c, strlen($p) + 1);
		}
		return "";
	}

	/**
	 * Resolve Compose shared folder path.
	 */
	private function getComposeSharedFolderPath(): string
	{
		try {
			$db = \OMV\Config\Database::getInstance();
			$compose = $db->get("conf.service.compose");
			if ($compose && ($ref = $compose->get("sharedfolderref"))) {
				return $this->resolveSharedFolderPath($ref);
			}
		} catch (\Throwable $e) {
		}
		return "";
	}

	/**
	 * Resolve RustFS dataRef path.
	 */
	private function getRustfsDataPath(): string
	{
		try {
			$db = \OMV\Config\Database::getInstance();
			$rustfs = $db->get("conf.service.rustfs");
			if ($rustfs && ($ref = $rustfs->get("dataRef"))) {
				return $this->resolveSharedFolderPath($ref);
			}
		} catch (\Throwable $e) {
		}
		return "";
	}

	/**
	 * Resolve Docker data-root.
	 */
	private function getDockerDataRoot(): string
	{
		list($status, $out) = $this->runCtl(["dockerroot"], false);
		$root = trim(implode("\n", $out));
		if (0 === $status && !empty($root) && is_dir($root)) {
			return $root;
		}
		return "/var/lib/docker";
	}

	/**
	 * Get all configured sources from the configuration database.
	 */
	private function getAllSources(): array
	{
		$db = \OMV\Config\Database::getInstance();
		$sources = [];
		try {
			$objs = $db->get("conf.service.filebrowserquantum.source");
			if (is_iterable($objs)) {
				foreach ($objs as $obj) {
					$sources[] = $obj->getAssoc();
				}
			}
		} catch (\Throwable $e) {
		}
		return $sources;
	}

	/**
	 * Normalize a container path by trimming whitespace, collapsing
	 * duplicate slashes and removing a trailing slash. The root path is
	 * kept as is. An empty string is returned unchanged.
	 */
	private function normalizeContainerPath(string $path): string
	{
		$path = trim($path);
		if ("" === $path) {
			return "";
		}
		$normalized = preg_replace('#/+#', '/', $path);
		if (!is_string($normalized)) {
			return "";
		}
		if (strlen($normalized) > 1) {
			$normalized = rtrim($normalized, "/");
		}
		return $normalized;
	}

	/**
	 * Derive the container path '/srv/<shared folder name>' from the
	 * name of the selected shared folder. Characters that are not
	 * allowed in a container path are replaced with an underscore;
	 * Unicode letters and digits are preserved so that localized shared
	 * folder names stay readable. If the sanitized name is empty a
	 * unique segment derived from the shared folder UUID is used.
	 */
	private function deriveContainerPath(string $sfName, string $sfRef): string
	{
		$name = trim($sfName);
		$segment = "";
		if ("" !== $name) {
			// Keep Unicode letters and digits, replace everything else.
			$sanitized = preg_replace('/[^\p{L}\p{N}._-]+/u', '_', $name);
			if (!is_string($sanitized)) {
				// Fallback for invalid UTF-8 input.
				$sanitized = preg_replace('/[^A-Za-z0-9._-]+/', '_', $name);
			}
			$segment = trim((string)preg_replace('/_+/', '_', (string)$sanitized), "._-");
		}
		if ("" === $segment) {
			$hex = (string)preg_replace('/[^0-9a-f]/i', '', $sfRef);
			$segment = "share-" . substr($hex, 0, 8);
		}
		// Keep the derived path well below the 200 byte limit.
		if (preg_match('/^(.{48})(.+)$/us', $segment, $matches)) {
			$segment = rtrim($matches[1], "._-") . "-" . substr(sha1($name . $sfRef), 0, 8);
		}
		return "/srv/" . $segment;
	}

	/**
	 * Validate container path format and reserved prefixes (R-6).
	 */
	private function validateContainerPath(string $path): void
	{
		if (empty($path)) {
			throw new \OMV\Exception("The container path must not be empty.");
		}
		if (!str_starts_with($path, "/") || str_contains($path, ":") || $path === "/" || strlen($path) > 200) {
			throw new \OMV\Exception(sprintf(
				"The container path '%s' must be an absolute path without a colon and at most 200 characters.",
				$path
			));
		}
		if (!preg_match('#^/[\p{L}\p{N}._/-]+$#u', $path)) {
			throw new \OMV\Exception(sprintf(
				"The container path '%s' contains invalid characters. Only letters, digits, '.', '_', '-' and '/' are allowed.",
				$path
			));
		}
		foreach (explode("/", trim($path, "/")) as $segment) {
			if ("." === $segment || ".." === $segment) {
				throw new \OMV\Exception(sprintf(
					"The container path '%s' must not contain '.' or '..' segments.",
					$path
				));
			}
		}
		$trimmed = rtrim($path, "/");
		foreach (self::RESERVED_PREFIXES as $res) {
			if ($trimmed === $res || str_starts_with($trimmed, $res . "/")) {
				throw new \OMV\Exception(sprintf(
					"The container path '%s' is reserved by the container system. Use another path like /srv/...",
					$path
				));
			}
		}
	}

	/**
	 * Validate storage topology against R-1..R-7 hard rejection rules.
	 */
	private function validateTopology(string $dataRef, array $sources, bool $maskProtected, bool $enabling = false): void
	{
		if ($enabling) {
			if (empty($dataRef) || empty($sources)) {
				throw new \OMV\Exception(
					"Select the data shared folder and at least one storage source before enabling FileBrowser Quantum."
				);
			}
		}

		$dataHost = "";
		if (!empty($dataRef)) {
			$dataHost = $this->resolveSharedFolderPath($dataRef);
			if (empty($dataHost)) {
				throw new \OMV\Exception("The data shared folder could not be resolved.");
			}
			$this->assertNotRestrictedVolume($dataHost);
			if ($dataHost === "/") {
				throw new \OMV\Exception("The root filesystem cannot be used as the data shared folder.");
			}
		}

		$composeSfHost = $this->getComposeSharedFolderPath();
		$rustfsDataHost = $this->getRustfsDataPath();
		$dockerRootHost = $this->getDockerDataRoot();

		$resolvedSources = [];
		foreach ($sources as $s) {
			$ref = (string)($s["sharedfolderref"] ?? "");
			if (empty($ref)) {
				throw new \OMV\Exception("A shared folder must be selected for every storage source.");
			}
			$host = $this->resolveSharedFolderPath($ref);
			if (empty($host)) {
				throw new \OMV\Exception("A selected storage source shared folder could not be resolved.");
			}
			$this->assertNotRestrictedVolume($host);
			if ($host === "/") {
				throw new \OMV\Exception("The root filesystem cannot be used as a storage source.");
			}

			// R-3: Host path must not equal any protected root
			if (!empty($composeSfHost) && ($host === $composeSfHost)) {
				throw new \OMV\Exception(
					"The selected shared folder is the Compose plugin shared folder and must not be exposed to FileBrowser Quantum (it holds stack .env credentials). Choose a specific data subfolder below it instead."
				);
			}
			if (!empty($dockerRootHost) && ($host === $dockerRootHost)) {
				throw new \OMV\Exception(
					"The selected shared folder is the Docker data-root and must not be exposed to FileBrowser Quantum."
				);
			}
			if (!empty($rustfsDataHost) && ($host === $rustfsDataHost)) {
				throw new \OMV\Exception(
					"The selected shared folder holds the RustFS object store and must not be exposed to FileBrowser Quantum directly."
				);
			}
			if (!empty($dataHost)) {
				if ($host === $dataHost) {
					throw new \OMV\Exception(
						"A storage source shared folder cannot be identical to the FileBrowser Quantum data folder."
					);
				}
				// R-4: Data folder cannot be ancestor of source folder
				if ($this->isStrictDescendant($host, $dataHost)) {
					throw new \OMV\Exception(
						"The storage source shared folder lies inside the FileBrowser Quantum data folder. Move the data folder elsewhere."
					);
				}
				// If data folder lies inside source folder, auto-masking must be enabled
				if ($this->isStrictDescendant($dataHost, $host) && !$maskProtected) {
					throw new \OMV\Exception(
						"The FileBrowser Quantum data folder lies inside this storage source and automatic masking is disabled. Enable auto-masking or choose a narrower source folder."
					);
				}
			}

			$resolvedSources[] = array_merge($s, ["hostPath" => $host]);
		}

		// R-5: Sources must not nest inside each other
		$count = count($resolvedSources);
		for ($i = 0; $i < $count; $i++) {
			for ($j = $i + 1; $j < $count; $j++) {
				$h1 = $resolvedSources[$i]["hostPath"];
				$h2 = $resolvedSources[$j]["hostPath"];
				if ($h1 === $h2 || $this->isStrictDescendant($h1, $h2) || $this->isStrictDescendant($h2, $h1)) {
					throw new \OMV\Exception(
						"Storage source shared folders must not be identical or nested inside each other."
					);
				}
			}
		}

		// R-7: Verify mask computation does not exceed MAX_MASKS
		$this->computeMasks($resolvedSources, $dataHost, $maskProtected);
	}

	/**
	 * Compute tmpfs masking set for sensitive nested directories (R-7).
	 */
	private function computeMasks(array $sources, string $dataHost, bool $maskProtected): array
	{
		$masks = [];
		$protectedRoots = [];
		if (!empty($dataHost)) {
			$protectedRoots["data"] = $dataHost;
		}
		if ($maskProtected) {
			$composeSf = $this->getComposeSharedFolderPath();
			if (!empty($composeSf)) {
				$protectedRoots["compose"] = $composeSf;
			}
			$rustfsData = $this->getRustfsDataPath();
			if (!empty($rustfsData)) {
				$protectedRoots["rustfs"] = $rustfsData;
			}
			$dockerRoot = $this->getDockerDataRoot();
			if (!empty($dockerRoot)) {
				$protectedRoots["docker"] = $dockerRoot;
			}
		}

		// Sort sources by container path ascending
		usort($sources, function ($a, $b) {
			return strcmp((string)($a["path"] ?? ""), (string)($b["path"] ?? ""));
		});

		foreach ($sources as $s) {
			$sHost = $s["hostPath"] ?? $this->resolveSharedFolderPath((string)($s["sharedfolderref"] ?? ""));
			if (empty($sHost)) {
				continue;
			}
			$cPath = rtrim((string)($s["path"] ?? ""), "/");

			foreach ($protectedRoots as $p) {
				if (!is_dir($p)) {
					continue;
				}
				if ($p === $sHost) {
					throw new \OMV\Exception(sprintf(
						"The shared folder '%s' is identical to a protected root and cannot be masked.",
						$sHost
					));
				}
				if ($this->isStrictDescendant($p, $sHost)) {
					$rel = $this->getRelativePath($p, $sHost);
					if (!empty($rel)) {
						$tgt = $cPath . "/" . ltrim($rel, "/");
						$masks[] = $tgt;
					}
				}
			}
		}

		$masks = array_values(array_unique($masks));
		sort($masks);
		if (count($masks) > self::MAX_MASKS) {
			throw new \OMV\Exception(
				"The selected shared folders contain too many protected directories to mask (>32). Choose narrower shared folders."
			);
		}
		return $masks;
	}

	/**
	 * Build Compose YAML body.
	 */
	private function buildComposeBody(
		array $config,
		string $dataSfName,
		array $sources,
		array $masks
	): string {
		$image = $this->imageOf(
			(string)($config["version"] ?? ""),
			(string)($config["imageVariant"] ?? "full")
		);
		$port = (int)($config["port"] ?? 8085);
		$puid = (int)($config["puid"] ?? 1000);
		$pgid = (int)($config["pgid"] ?? 100);

		// Volumes: data folder is ALWAYS first, then sources sorted by path
		usort($sources, function ($a, $b) {
			return strcmp((string)($a["path"] ?? ""), (string)($b["path"] ?? ""));
		});

		$volumeLines = [];
		$volumeLines[] = '      - ${{ sf:"' . $dataSfName . '" }}:/home/filebrowser/data';
		foreach ($sources as $s) {
			$sfName = $s["sharedFolderName"];
			$cPath = $s["path"];
			$ro = !empty($s["readonly"]) ? ":ro" : "";
			$volumeLines[] = '      - ${{ sf:"' . $sfName . '" }}:' . $cPath . $ro;
		}
		$volumeSection = implode("\n", $volumeLines);

		// Tmpfs: omit section completely when masks is empty
		$tmpfsSection = "";
		if (!empty($masks)) {
			$tmpfsLines = ["    tmpfs:"];
			foreach ($masks as $m) {
				$tmpfsLines[] = "      - " . $m . ":size=1048576,mode=0500";
			}
			$tmpfsSection = "\n" . implode("\n", $tmpfsLines);
		}

		// Environment
		$envLines = [];
		$envLines[] = "      - PUID=" . $puid;
		$envLines[] = "      - PGID=" . $pgid;
		$envLines[] = "      - UMASK=002";

		$baseURL = trim((string)($config["baseURL"] ?? ""));
		if (!empty($baseURL)) {
			$envLines[] = "      - FILEBROWSER_BASEURL=" . $baseURL;
		}

		$appUrl = trim((string)($config["appUrl"] ?? ""));
		if (!empty($appUrl)) {
			$envLines[] = "      - APPLICATION_URL=" . $appUrl;
		}

		$authMethod = trim((string)($config["authMethod"] ?? "password"));
		if ("noauth" === $authMethod) {
			$envLines[] = "      - FILEBROWSER_NOAUTH=true";
		}

		$extra = trim((string)($config["extraOptions"] ?? ""));
		if (!empty($extra)) {
			foreach (preg_split('/\r?\n/', $extra) as $line) {
				$line = trim($line);
				if (!empty($line) && !str_starts_with($line, "#")) {
					$envLines[] = "      - " . $line;
				}
			}
		}
		$envSection = "\n    environment:\n" . implode("\n", $envLines);

		$template = <<<'YAML'
name: filebrowserquantum

services:
  filebrowserquantum:
    container_name: filebrowserquantum
    image: %%IMAGE%%
    user: "%%PUID%%:%%PGID%%"
    restart: unless-stopped
    ports:
      - '%%PORT%%:80'
    volumes:
%%VOLUMES%%%%TMPFS%%%%ENVIRONMENT%%
    security_opt:
      - no-new-privileges:true
YAML;

		$body = strtr($template, [
			"%%IMAGE%%" => $image,
			"%%PUID%%" => (string)$puid,
			"%%PGID%%" => (string)$pgid,
			"%%PORT%%" => (string)$port,
			"%%VOLUMES%%" => $volumeSection,
			"%%TMPFS%%" => $tmpfsSection,
			"%%ENVIRONMENT%%" => $envSection
		]);

		return $body . "\n";
	}

	/**
	 * UUID of the stack in the Compose plugin (empty when not registered).
	 */
	private function findStackUuid($context): string
	{
		$files = \OMV\Rpc\Rpc::call("Config", "get", [
			"id" => "conf.service.compose.file"
		], $context);
		if (!is_array($files)) {
			return "";
		}
		foreach ($files as $file) {
			if (($file["name"] ?? "") === self::STACK_NAME) {
				return (string)($file["uuid"] ?? "");
			}
		}
		return "";
	}

	/**
	 * Create or update the FileBrowser Quantum stack in the openmediavault-compose plugin.
	 */
	private function syncComposeStack($context, array $config): void
	{
		$dataRef = (string)($config["dataRef"] ?? "");
		$sources = $this->getAllSources();

		if (empty($dataRef) || empty($sources)) {
			// Configuration incomplete - do not publish yet
			return;
		}

		$dataSfName = $this->sharedFolderName($context, $dataRef);
		if (empty($dataSfName)) {
			throw new \OMV\Exception("The data shared folder could not be resolved.");
		}

		$enrichedSources = [];
		foreach ($sources as $s) {
			$ref = (string)($s["sharedfolderref"] ?? "");
			$name = $this->sharedFolderName($context, $ref);
			$host = $this->resolveSharedFolderPath($ref);
			if (empty($name) || empty($host)) {
				throw new \OMV\Exception("A selected storage source shared folder could not be resolved.");
			}
			$enrichedSources[] = array_merge($s, [
				"sharedFolderName" => $name,
				"hostPath" => $host
			]);
		}

		$dataHost = $this->resolveSharedFolderPath($dataRef);
		$maskProtected = !empty($config["maskProtected"]);
		$masks = $this->computeMasks($enrichedSources, $dataHost, $maskProtected);

		$body = $this->buildComposeBody($config, $dataSfName, $enrichedSources, $masks);

		$params = [
			"name" => self::STACK_NAME,
			"description" => "FileBrowser Quantum (managed by the openmediavault-filebrowser-quantum plugin)",
			"body" => $body,
			"showenv" => false,
			"env" => "",
			"showoverride" => false,
			"override" => ""
		];

		$uuid = $this->findStackUuid($context);
		if (!empty($uuid)) {
			$params["uuid"] = $uuid;
		}

		try {
			\OMV\Rpc\Rpc::call("Compose", "setFile", $params, $context);
		} catch (\Exception $e) {
			throw new \OMV\Exception(sprintf(
				"The FileBrowser Quantum stack could not be registered in the Compose plugin: %s. " .
				"Ensure openmediavault-compose is installed and its shared folder is configured (Services -> Compose -> Settings).",
				$e->getMessage()
			));
		}
	}

	/**
	 * Settings object enriched with local runtime and derived observability fields.
	 * (Zero network I/O in get()).
	 */
	public function get($params, $context)
	{
		$this->validateMethodContext($context, ["role" => OMV_ROLE_ADMINISTRATOR]);
		$object = $this->getSettingsObject($context);

		// Container state
		list(, $statusOut) = $this->runCtl(["status"], false);
		$state = trim(implode("\n", $statusOut));
		if (!in_array($state, ["running", "stopped", "notinstalled"])) {
			$state = "stopped";
		}
		$object["running"] = ("running" === $state);
		$object["statusText"] = [
			"running" => "Running",
			"stopped" => "Stopped",
			"notinstalled" => "Not installed"
		][$state];

		// Running image tag
		list(, $versionOut) = $this->runCtl(["version"], false);
		$runningVersion = trim(implode("\n", $versionOut));
		$object["runningVersion"] = $runningVersion;

		// Read latest upstream release from disk cache (Never curl during get())
		$cachedLatest = $this->readCachedLatestVersion();
		$latestVersion = (string)($cachedLatest["version"] ?? "");
		$object["latestVersion"] = $latestVersion;
		$object["updateAvailable"] = $this->computeUpdateAvailable($runningVersion, $latestVersion);

		// Compose stack file path
		list(, $pathOut) = $this->runCtl(["path"], false);
		$dir = trim(implode("\n", $pathOut));
		$composeFile = (!empty($dir)) ? $dir . "/" . self::STACK_NAME . ".yml" : "";
		$object["composeFile"] = $composeFile;

		// Mask preview & count extracted from current rendered stack file
		$masks = [];
		if (!empty($composeFile) && is_readable($composeFile)) {
			$content = (string)file_get_contents($composeFile);
			if (preg_match('/tmpfs:\s*\n((?:\s+-\s+[^\n]+\n)+)/', $content, $m)) {
				foreach (explode("\n", trim($m[1])) as $line) {
					$line = trim($line);
					if (str_starts_with($line, "- ")) {
						$part = substr($line, 2);
						$colonPos = strpos($part, ":");
						$masks[] = ($colonPos !== false) ? substr($part, 0, $colonPos) : $part;
					}
				}
			}
		}
		$object["maskCount"] = count($masks);
		$object["maskPreview"] = !empty($masks) ? implode("\n", $masks) : "No paths masked";

		// Sources count
		$sources = $this->getAllSources();
		$object["sourcesCount"] = count($sources);

		return $object;
	}

	/**
	 * Asynchronously query latest upstream version from GitHub (Triggered on demand).
	 */
	public function getLatestVersion($params, $context)
	{
		$this->validateMethodContext($context, ["role" => OMV_ROLE_ADMINISTRATOR]);
		$object = $this->getSettingsObject($context);
		$channel = $this->channelOf((string)($object["version"] ?? ""));

		list($exitCode, $output) = $this->runCtl(["latest", $channel], false);
		$tag = trim(implode("\n", $output));
		if (0 === $exitCode && !empty($tag)) {
			$this->writeCachedLatestVersion($tag);
			return [
				"latestVersion" => $tag,
				"success" => true
			];
		}
		return [
			"latestVersion" => "",
			"success" => false
		];
	}

	public function set($params, $context)
	{
		$this->validateMethodContext($context, ["role" => OMV_ROLE_ADMINISTRATOR]);
		$data = array_intersect_key($params, array_flip($this->settingsFields));

		$version = trim((string)($data["version"] ?? ""));
		if ("" === $version) {
			throw new \OMV\Exception("The FileBrowser Quantum image tag must not be empty.");
		}
		if (1 === preg_match('/^v?1[\.\-]/', $version)) {
			throw new \OMV\Exception(sprintf(
				"The image tag '%s' belongs to FileBrowser v1.x, which is not supported by this plugin. Use a v2 tag (e.g. 2.0.10-beta).",
				$version
			));
		}

		$extra = trim((string)($data["extraOptions"] ?? ""));
		if (!empty($extra)) {
			foreach (preg_split('/\r?\n/', $extra) as $line) {
				$line = trim($line);
				if (empty($line) || str_starts_with($line, "#")) {
					continue;
				}
				if (!preg_match('/^[A-Za-z_][A-Za-z0-9_]*=.*$/', $line)) {
					throw new \OMV\Exception(sprintf(
						"The extra option '%s' is not in KEY=VALUE format.",
						$line
					));
				}
				$key = substr($line, 0, strpos($line, "="));
				if (in_array(strtoupper($key), self::EXTRA_OPTIONS_BLACK_LIST)) {
					throw new \OMV\Exception(sprintf(
						"The environment variable key '%s' is reserved and cannot be configured.",
						$key
					));
				}
			}
		}

		$enabling = !empty($data["enable"]);
		$dataRef = (string)($data["dataRef"] ?? "");
		$maskProtected = !empty($data["maskProtected"]);
		$sources = $this->getAllSources();

		$this->validateTopology($dataRef, $sources, $maskProtected, $enabling);

		$result = \OMV\Rpc\Rpc::call("Config", "set", [
			"id" => "conf.service.filebrowserquantum",
			"data" => $data
		], $context);

		$this->syncComposeStack($context, $this->getSettingsObject($context));

		return $result;
	}

	/**
	 * Get source mappings for DataTable.
	 */
	public function getSources($params, $context)
	{
		$this->validateMethodContext($context, ["role" => OMV_ROLE_ADMINISTRATOR]);
		$this->validateMethodParams($params, "rpc.common.getlist");

		$db = \OMV\Config\Database::getInstance();
		$result = [];
		try {
			$objs = $db->get("conf.service.filebrowserquantum.source");
			if (is_iterable($objs)) {
				foreach ($objs as $obj) {
					$entry = $obj->getAssoc();
					$ref = (string)($entry["sharedfolderref"] ?? "");
					$entry["sharedFolderName"] = $this->sharedFolderName($context, $ref);
					$entry["hostPath"] = $this->resolveSharedFolderPath($ref);
					$result[] = $entry;
				}
			}
		} catch (\Throwable $e) {
		}

		return $this->applyFilter(
			$result,
			$params["start"] ?? 0,
			$params["limit"] ?? 25,
			$params["sortfield"] ?? "path",
			$params["sortdir"] ?? "asc"
		);
	}

	/**
	 * Get a single source mapping by UUID.
	 */
	public function getSource($params, $context)
	{
		$this->validateMethodContext($context, ["role" => OMV_ROLE_ADMINISTRATOR]);
		$this->validateMethodParams($params, "rpc.common.objectuuid");
		$db = \OMV\Config\Database::getInstance();
		return $db->get("conf.service.filebrowserquantum.source", $params["uuid"])->getAssoc();
	}

	/**
	 * Create or update a source mapping.
	 */
	public function setSource($params, $context)
	{
		$this->validateMethodContext($context, ["role" => OMV_ROLE_ADMINISTRATOR]);

		$ref = trim((string)($params["sharedfolderref"] ?? ""));
		if (empty($ref)) {
			throw new \OMV\Exception("Select a shared folder.");
		}

		$sfName = $this->sharedFolderName($context, $ref);
		if (empty($sfName)) {
			throw new \OMV\Exception("The selected shared folder could not be resolved.");
		}

		$existingSources = $this->getAllSources();
		$uuid = trim((string)($params["uuid"] ?? ""));
		$isNew = empty($uuid) || $uuid === "undefined" || !\OMV\Uuid::isUuid4($uuid);
		if ($isNew) {
			unset($params["uuid"]);
		}

		$cPath = $this->normalizeContainerPath((string)($params["path"] ?? ""));
		if ("" === $cPath) {
			$cPath = $this->deriveContainerPath($sfName, $ref);
		} elseif (!$isNew) {
			// If the stored path was auto-derived from the previous shared
			// folder name, follow the folder when it is replaced.
			foreach ($existingSources as $es) {
				if (($es["uuid"] ?? "") !== $uuid) {
					continue;
				}
				$oldRef = (string)($es["sharedfolderref"] ?? "");
				if ("" !== $oldRef && $oldRef !== $ref) {
					$oldName = $this->sharedFolderName($context, $oldRef);
					if ("" !== $oldName) {
						$oldDerived = $this->normalizeContainerPath(
							$this->deriveContainerPath($oldName, $oldRef)
						);
						if ($cPath === $oldDerived) {
							$cPath = $this->deriveContainerPath($sfName, $ref);
						}
					}
				}
				break;
			}
		}
		$params["path"] = $cPath;
		$this->validateContainerPath($cPath);

		// Default the display name to the shared folder name so the
		// source list is meaningful without further user input.
		$displayName = trim((string)($params["name"] ?? ""));
		$params["name"] = ("" === $displayName) ? trim($sfName) : $displayName;

		foreach ($existingSources as $es) {
			if ($isNew || ($es["uuid"] ?? "") !== $uuid) {
				if ($this->normalizeContainerPath((string)($es["path"] ?? "")) === $cPath) {
					throw new \OMV\Exception("The container path is already used by another source.");
				}
			}
		}

		$settings = $this->getSettingsObject($context);
		$dataRef = (string)($settings["dataRef"] ?? "");
		$maskProtected = !empty($settings["maskProtected"]);

		$testSources = [];
		$found = false;
		foreach ($existingSources as $es) {
			if (!$isNew && ($es["uuid"] ?? "") === $uuid) {
				$testSources[] = $params;
				$found = true;
			} else {
				$testSources[] = $es;
			}
		}
		if (!$found) {
			$testSources[] = $params;
		}

		$this->validateTopology($dataRef, $testSources, $maskProtected, false);

		$object = new \OMV\Config\ConfigObject("conf.service.filebrowserquantum.source");
		$object->setAssoc($params, true, true);
		if ($isNew) {
			$object->setNew();
		}

		$db = \OMV\Config\Database::getInstance();
		$db->set($object);

		$this->syncComposeStack($context, $this->getSettingsObject($context));

		return $object->getAssoc();
	}

	/**
	 * Delete a source mapping and republish the stack.
	 */
	public function deleteSource($params, $context)
	{
		$this->validateMethodContext($context, ["role" => OMV_ROLE_ADMINISTRATOR]);
		$this->validateMethodParams($params, "rpc.common.objectuuid");

		$db = \OMV\Config\Database::getInstance();
		$object = $db->get("conf.service.filebrowserquantum.source", $params["uuid"]);
		$db->delete($object);

		$this->syncComposeStack($context, $this->getSettingsObject($context));
	}

	private function execCtlTask($context, string $op): string
	{
		return $this->execBgCtl([$op]);
	}

	public function start($params, $context)
	{
		$this->validateMethodContext($context, ["role" => OMV_ROLE_ADMINISTRATOR]);
		return $this->execCtlTask($context, "up");
	}

	public function stop($params, $context)
	{
		$this->validateMethodContext($context, ["role" => OMV_ROLE_ADMINISTRATOR]);
		return $this->execCtlTask($context, "down");
	}

	public function restart($params, $context)
	{
		$this->validateMethodContext($context, ["role" => OMV_ROLE_ADMINISTRATOR]);
		return $this->execCtlTask($context, "restart");
	}

	public function upgrade($params, $context)
	{
		$this->validateMethodContext($context, ["role" => OMV_ROLE_ADMINISTRATOR]);
		return $this->execCtlTask($context, "upgrade");
	}

	public function getLog($params, $context)
	{
		$this->validateMethodContext($context, ["role" => OMV_ROLE_ADMINISTRATOR]);
		return $this->execCtlTask($context, "logs");
	}

	public function getShowConfig($params, $context)
	{
		$this->validateMethodContext($context, ["role" => OMV_ROLE_ADMINISTRATOR]);
		return $this->execCtlTask($context, "showcompose");
	}

	private function execBgCtl(array $args): string
	{
		return $this->execBgProc(
			function ($bgStatusFilename, $bgOutputFilename) use ($args) {
				$cmd = new \OMV\System\Process(self::CTL, $args);
				$cmd->setRedirect2to1();
				if (0 !== ($exitStatus = $this->exec(
					$cmd, $output, $bgOutputFilename
				))) {
					throw new \OMV\ExecException($cmd, $output, $exitStatus);
				}
				return $output;
			}
		);
	}
}
