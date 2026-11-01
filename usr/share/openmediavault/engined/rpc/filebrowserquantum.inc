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
	 * Default header used by the upstream proxy authentication.
	 */
	const DEFAULT_PROXY_HEADER = "X-Forwarded-User";

	/**
	 * Authentication methods understood by the upstream config.yaml.
	 */
	const AUTH_METHODS = ["password", "noauth", "proxy"];

	/**
	 * Settings fields allowed to be persisted by set().
	 */
	private $settingsFields = [
		"enable", "version", "port", "imageVariant",
		"dataRef", "sourceRef", "baseURL", "appUrl",
		"trustProxyHeaders", "authMethod", "authProxyHeader",
		"puid", "pgid", "maskProtected", "extraOptions"
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
		$this->registerMethod("getSharedFolderChoices");
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
	 * Normalize the reverse proxy sub path ('http.baseURL').
	 *
	 * Empty input (or '/') means "served at the root". A value that is
	 * cleared in the form really clears the stored value instead of
	 * keeping the previous prefix.
	 */
	private function normalizeBaseURL(string $value): string
	{
		$text = trim($value);
		if (("" === $text) || ("/" === $text)) {
			return "";
		}
		if (!str_starts_with($text, "/")) {
			throw new \OMV\Exception(sprintf(
				"The base URL '%s' must start with '/'.",
				$text
			));
		}
		if (1 === preg_match('/[\s?#"\'\\\\]/', $text)) {
			throw new \OMV\Exception(sprintf(
				"The base URL '%s' must not contain whitespace, quotes, '?' or '#'.",
				$text
			));
		}
		$text = rtrim((string)preg_replace('#/+#', '/', $text), "/");
		if (strlen($text) > 128) {
			throw new \OMV\Exception("The base URL must not exceed 128 characters.");
		}
		return $text;
	}

	/**
	 * Validate an optional public base URL ('http.externalUrl').
	 */
	private function validateAppUrl(string $value): string
	{
		$text = rtrim(trim($value), "/");
		if ("" === $text) {
			return "";
		}
		if (1 !== preg_match('#^https?://[^/\s]+(/[^\s]*)?$#i', $text)) {
			throw new \OMV\Exception(sprintf(
				"The application URL '%s' must start with http:// or https:// and must not contain whitespace.",
				$text
			));
		}
		return $text;
	}

	/**
	 * Origin (scheme://host[:port]) of a public URL.
	 *
	 * Upstream appends 'http.baseURL' to 'http.externalUrl' on its own, so
	 * only the origin may be written into config.yaml - otherwise every
	 * generated link would contain the sub path twice (/files/files).
	 */
	private function appUrlOrigin(string $appUrl): string
	{
		$text = trim($appUrl);
		if ("" === $text) {
			return "";
		}
		if (1 === preg_match('#^(https?://[^/\s]+)#i', $text, $matches)) {
			return $matches[1];
		}
		return "";
	}

	/**
	 * Split the public URL into the part the operator typed and the base
	 * path, so the 'Open' button never produces '/files/files' either.
	 */
	private function splitAppUrl(string $appUrl, string $baseURL): array
	{
		$url = trim($appUrl);
		$base = trim($baseURL);
		if ("" === $url) {
			return ["base" => "", "path" => $base];
		}
		if (("" !== $base) && (strlen($url) > strlen($base)) &&
				(substr($url, -strlen($base)) === $base)) {
			return [
				"base" => rtrim(substr($url, 0, -strlen($base)), "/"),
				"path" => $base
			];
		}
		return ["base" => $url, "path" => $base];
	}

	/**
	 * A pinned image tag (e.g. 2.0.10-beta) selects one immutable image;
	 * a floating tag (e.g. beta, stable) always pulls the newest one.
	 */
	private function isPinnedTag(string $version): bool
	{
		return (1 === preg_match('/^v?\d+\.\d+\.\d+/', trim($version)));
	}

	/**
	 * Tag the configured release should be advanced to, or '' when the
	 * configured tag is already current (or floating).
	 */
	private function nextTagForUpgrade(string $current, string $latest): string
	{
		$cur = ltrim(trim($current), "v");
		$lat = ltrim(trim($latest), "v");
		if (("" === $cur) || ("" === $lat)) {
			return "";
		}
		$curCmp = (string)preg_replace('/-slim$/', "", $cur);
		$latCmp = (string)preg_replace('/-slim$/', "", $lat);
		return version_compare($latCmp, $curCmp, ">") ? $lat : "";
	}

	/**
	 * Latest upstream release tag of the given channel ('' when the
	 * upstream cannot be reached).
	 */
	private function fetchLatestTag(string $channel): string
	{
		list($exitCode, $output) = $this->runCtl(["latest", $channel], false);
		$tag = trim(implode("\n", $output));
		if ((0 !== $exitCode) || ("" === $tag)) {
			return "";
		}
		$tag = ltrim($tag, "v");
		return (1 === preg_match('/^\d+\.\d+/', $tag)) ? $tag : "";
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
	 * List all shared folders known to the configuration database.
	 */
	private function listSharedFolders($context): array
	{
		$folders = \OMV\Rpc\Rpc::call("Config", "get", [
			"id" => "conf.system.sharedfolder"
		], $context);
		return is_array($folders) ? $folders : [];
	}

	/**
	 * Name of a shared folder by reference (empty when unknown).
	 */
	private function sharedFolderName($context, string $ref): string
	{
		if ("" === $ref) {
			return "";
		}
		foreach ($this->listSharedFolders($context) as $folder) {
			if (($folder["uuid"] ?? "") === $ref) {
				return (string)($folder["name"] ?? "");
			}
		}
		return "";
	}

	/**
	 * Hex digits of a UUID (used for deterministic path suffixes).
	 */
	private function uuidHex(string $uuid): string
	{
		return (string)preg_replace('/[^0-9a-f]/i', '', $uuid);
	}

	/**
	 * Shared folder index: UUID => name plus the container path derived
	 * for it.
	 *
	 * The derived path is the identity token the storage source form
	 * submits, so it must identify exactly one shared folder. Names that
	 * sanitize to the same segment (e.g. 'My Photos' and 'My_Photos' both
	 * yield '/srv/My_Photos') are disambiguated with a deterministic UUID
	 * suffix instead of falling back to first-match-wins, which could
	 * mount a folder the operator did not select.
	 */
	private function getSharedFolderIndex($context): array
	{
		$index = [];
		$bySegment = [];
		foreach ($this->listSharedFolders($context) as $folder) {
			$uuid = (string)($folder["uuid"] ?? "");
			if ("" === $uuid) {
				continue;
			}
			$name = (string)($folder["name"] ?? "");
			$derived = $this->normalizeContainerPath(
				$this->deriveContainerPath($name, $uuid)
			);
			$index[$uuid] = [
				"uuid" => $uuid,
				"name" => $name,
				"path" => $derived
			];
			$bySegment[$derived][] = $uuid;
		}

		// Disambiguate collisions deterministically (independent of the
		// order the shared folders are read in).
		$taken = [];
		foreach ($bySegment as $derived => $uuids) {
			if (count($uuids) < 2) {
				continue;
			}
			foreach ($uuids as $uuid) {
				$candidate = $derived . "-" . substr($this->uuidHex($uuid), 0, 8);
				$counter = 0;
				while (isset($taken[$candidate]) || isset($bySegment[$candidate])) {
					++$counter;
					$candidate = $derived . "-" .
						substr($this->uuidHex($uuid), 0, 8) . "-" . $counter;
				}
				$index[$uuid]["path"] = $candidate;
				$taken[$candidate] = $uuid;
			}
		}

		return $index;
	}

	/**
	 * Container path derived for the given shared folder reference
	 * (empty when the reference cannot be resolved).
	 */
	private function deriveContainerPathForRef($context, string $ref): string
	{
		if ("" === $ref) {
			return "";
		}
		$index = $this->getSharedFolderIndex($context);
		return (string)($index[$ref]["path"] ?? "");
	}

	/**
	 * Reverse mapping used by the Workbench storage source form: the
	 * 'Shared folder' select submits the container path derived for the
	 * chosen shared folder, so the 'Container path' field can be filled
	 * in from it. Resolve that value back to the shared folder UUID.
	 *
	 * A plain UUID is accepted as well (API clients). A value that maps
	 * to more than one shared folder is rejected instead of silently
	 * picking the first one.
	 */
	private function sharedFolderRefByContainerPath($context, string $cpath): string
	{
		if ("" === $cpath) {
			return "";
		}
		$normalized = $this->normalizeContainerPath($cpath);
		if ("" === $normalized) {
			return "";
		}
		$index = $this->getSharedFolderIndex($context);
		if (isset($index[$normalized])) {
			return $normalized;
		}
		$matches = [];
		foreach ($index as $uuid => $entry) {
			if ($entry["path"] === $normalized) {
				$matches[] = $uuid;
			}
		}
		if (1 === count($matches)) {
			return $matches[0];
		}
		if (count($matches) > 1) {
			throw new \OMV\Exception(sprintf(
				"The selected shared folder is ambiguous ('%s' matches %d shared folders). " .
				"Reload the page and select the shared folder again.",
				$cpath,
				count($matches)
			));
		}
		return "";
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

			// Roots that are only hidden by the optional tmpfs masking must
			// not be exposed when masking is disabled: the operator has to
			// pick a narrower source folder instead of silently publishing
			// the Compose configuration tree or the Docker data-root.
			if (!$maskProtected) {
				$sensitiveRoots = [
					"the Compose plugin shared folder (stack .env credentials)" => $composeSfHost,
					"the Docker data-root" => $dockerRootHost,
					"the RustFS object store" => $rustfsDataHost
				];
				foreach ($sensitiveRoots as $label => $rootHost) {
					if (!empty($rootHost) && $this->isStrictDescendant($rootHost, $host)) {
						throw new \OMV\Exception(sprintf(
							"The storage source '%s' contains %s and automatic masking is disabled. " .
							"Enable auto-masking or choose a narrower shared folder.",
							$host,
							$label
						));
					}
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

		// Environment: only the container runtime variables are rendered
		// here. baseURL, externalUrl and the authentication methods are
		// owned by the upstream config.yaml (see prepare_config in the
		// ctl); FILEBROWSER_BASEURL, APPLICATION_URL and FILEBROWSER_NOAUTH
		// are not supported by upstream 2.x and are not emitted any more.
		$envLines = [];
		$envLines[] = "      - PUID=" . $puid;
		$envLines[] = "      - PGID=" . $pgid;
		$envLines[] = "      - UMASK=002";

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
	 * Remove the stack from the Compose plugin (stops the container and
	 * deletes the generated stack files; the shared folders are kept).
	 *
	 * Returns true when a stack was registered and removed.
	 */
	private function unregisterStack($context): bool
	{
		$uuid = $this->findStackUuid($context);
		if ("" === $uuid) {
			return false;
		}
		try {
			\OMV\Rpc\Rpc::call("Compose", "deleteFile", [
				"uuid" => $uuid
			], $context);
		} catch (\Exception $e) {
			throw new \OMV\Exception(sprintf(
				"The FileBrowser Quantum stack could not be removed from the Compose plugin: %s. " .
				"Delete it on the Compose page (Services -> Compose -> Files).",
				$e->getMessage()
			));
		}
		return true;
	}

	/**
	 * Create or update the FileBrowser Quantum stack in the openmediavault-compose plugin.
	 *
	 * When the configuration becomes incomplete (data folder or the last
	 * storage source removed) a stack that is still registered is removed
	 * instead of being left behind: the container would otherwise keep
	 * running with the previous - now wrong - bind mounts while the plugin
	 * shows no sources at all. A first-time configuration has no stack
	 * yet, so "not configured yet" and "became incomplete" differ.
	 */
	private function syncComposeStack($context, array $config): void
	{
		$dataRef = (string)($config["dataRef"] ?? "");
		$sources = $this->getAllSources();

		if (empty($dataRef) || empty($sources)) {
			$this->unregisterStack($context);
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

		// Container health (the Salt state fails when this is unhealthy).
		list(, $healthOut) = $this->runCtl(["health"], false);
		$health = trim(implode("\n", $healthOut));
		$object["health"] = $health;
		$object["healthText"] = [
			"healthy" => "Healthy",
			"unhealthy" => "Unhealthy",
			"starting" => "Starting",
			"none" => "No health check"
		][$health] ?? "Unknown";

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

		// Sources count and the derived state of the action buttons. The
		// Workbench taskDialog does not reload the page, so the buttons are
		// driven by server-side flags (refresh with the "Refresh status"
		// button) instead of being clickable in every state.
		$sources = $this->getAllSources();
		$object["sourcesCount"] = count($sources);

		$enabled = !empty($object["enable"]);
		$running = !empty($object["running"]);
		$configured = ("" !== trim((string)($object["dataRef"] ?? ""))) &&
			(count($sources) > 0);
		$object["configured"] = $configured;
		$object["canStart"] = $configured && $enabled && !$running;
		$object["canStop"] = $running;
		$object["canRestart"] = $configured && $enabled && $running;
		$object["canUpgrade"] = $configured && $enabled;
		$object["canOpen"] = $running;

		// Link target for the "Open" button: never emit the base path twice.
		try {
			$baseURL = $this->normalizeBaseURL((string)($object["baseURL"] ?? ""));
			$appUrl = $this->validateAppUrl((string)($object["appUrl"] ?? ""));
		} catch (\Throwable $e) {
			// Legacy values that no longer validate must not break get().
			$baseURL = "";
			$appUrl = trim((string)($object["appUrl"] ?? ""));
		}
		$split = $this->splitAppUrl($appUrl, $baseURL);
		$object["openUrlBase"] = $split["base"];
		$object["openUrlPath"] = $split["path"];

		// Advisory permission check for the configured PUID/PGID.
		$object["permissionsReport"] = $this->permissionReport($object);
		$object["permissionsWarning"] =
			(false !== strpos($object["permissionsReport"], "NOT readable"));

		return $object;
	}

	/**
	 * Human readable check whether the configured PUID/PGID can read the
	 * data folder and the storage sources.
	 *
	 * This is advisory only: POSIX ACLs and root (PUID/PGID 0) also grant
	 * access, and files created later may have different owners. It exists
	 * so a permission problem shows up before the container returns HTTP
	 * 403 in the web interface.
	 */
	private function permissionReport(array $settings): string
	{
		$uid = (int)($settings["puid"] ?? 0);
		$gid = (int)($settings["pgid"] ?? 0);
		$targets = [];
		$dataRef = trim((string)($settings["dataRef"] ?? ""));
		if ("" !== $dataRef) {
			$targets[] = [
				"label" => "data",
				"path" => $this->resolveSharedFolderPath($dataRef)
			];
		}
		foreach ($this->getAllSources() as $source) {
			$targets[] = [
				"label" => (string)($source["path"] ?? ""),
				"path" => $this->resolveSharedFolderPath(
					(string)($source["sharedfolderref"] ?? "")
				)
			];
		}
		if (empty($targets)) {
			return "No data folder and no storage source configured yet.";
		}
		$lines = [];
		foreach ($targets as $target) {
			if ("" === $target["path"]) {
				$lines[] = sprintf("unknown: %s (shared folder cannot be resolved)", $target["label"]);
				continue;
			}
			if (!@is_dir($target["path"])) {
				$lines[] = sprintf("unknown: %s (not present on this host)", $target["path"]);
				continue;
			}
			if ($this->canAccess($target["path"], $uid, $gid)) {
				$lines[] = sprintf("readable: %s", $target["path"]);
			} else {
				$lines[] = sprintf(
					"NOT readable by %d:%d: %s",
					$uid,
					$gid,
					$this->describePathOwner($target["path"])
				);
			}
		}
		return implode("\n", $lines);
	}

	/**
	 * Whether a POSIX user/group can read a directory (ACLs excluded).
	 */
	private function canAccess(string $path, int $uid, int $gid): bool
	{
		if (0 === $uid) {
			return true;
		}
		$stat = @stat($path);
		if (false === $stat) {
			return false;
		}
		$mode = ((int)$stat["mode"]) & 0777;
		if ((int)$stat["uid"] === $uid) {
			return 0 !== ($mode & 0400);
		}
		if ((int)$stat["gid"] === $gid) {
			return 0 !== ($mode & 0040);
		}
		return 0 !== ($mode & 0004);
	}

	/**
	 * 'owner:group mode' of a path for the permission report.
	 */
	private function describePathOwner(string $path): string
	{
		$stat = @stat($path);
		if (false === $stat) {
			return $path;
		}
		$user = (string)$stat["uid"];
		$group = (string)$stat["gid"];
		if (function_exists("posix_getpwuid")) {
			$info = @posix_getpwuid((int)$stat["uid"]);
			if (is_array($info) && isset($info["name"])) {
				$user = $info["name"];
			}
		}
		if (function_exists("posix_getgrgid")) {
			$info = @posix_getgrgid((int)$stat["gid"]);
			if (is_array($info) && isset($info["name"])) {
				$group = $info["name"];
			}
		}
		return sprintf("%s (%s:%s %04o). Grant read access with ACL/permissions or use PUID/PGID 0 (root).",
			$path, $user, $group, ((int)$stat["mode"]) & 07777);
	}

	/**
	 * Asynchronously query latest upstream version from GitHub (Triggered on demand).
	 *
	 * A failure (no network, GitHub unreachable, unexpected response) is
	 * raised as an RPC error, so the web UI shows the real reason instead
	 * of a success notification with an empty result.
	 */
	public function getLatestVersion($params, $context)
	{
		$this->validateMethodContext($context, ["role" => OMV_ROLE_ADMINISTRATOR]);
		$object = $this->getSettingsObject($context);
		$channel = $this->channelOf((string)($object["version"] ?? ""));

		$tag = $this->fetchLatestTag($channel);
		if ("" === $tag) {
			throw new \OMV\Exception(sprintf(
				"The latest upstream release of the '%s' channel could not be determined. " .
				"Check whether the NAS can reach github.com and try again.",
				$channel
			));
		}
		$this->writeCachedLatestVersion($tag);
		return [
			"latestVersion" => $tag,
			"success" => true
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

		// URLs and the proxy header are validated before they are stored;
		// an empty base URL really clears the previous sub path.
		if (array_key_exists("baseURL", $data)) {
			$data["baseURL"] = $this->normalizeBaseURL((string)$data["baseURL"]);
		}
		if (array_key_exists("appUrl", $data)) {
			$data["appUrl"] = $this->validateAppUrl((string)$data["appUrl"]);
		}
		if (array_key_exists("authProxyHeader", $data)) {
			$header = trim((string)$data["authProxyHeader"]);
			if ("" === $header) {
				$header = self::DEFAULT_PROXY_HEADER;
			}
			if (1 !== preg_match('/^[A-Za-z0-9][A-Za-z0-9_-]{0,127}$/', $header)) {
				throw new \OMV\Exception(sprintf(
					"The proxy authentication header '%s' is not a valid HTTP header name.",
					$header
				));
			}
			$data["authProxyHeader"] = $header;
		}
		if (array_key_exists("authMethod", $data) &&
				!in_array((string)$data["authMethod"], self::AUTH_METHODS, true)) {
			throw new \OMV\Exception(sprintf(
				"The authentication method '%s' is not supported.",
				(string)$data["authMethod"]
			));
		}

		// Validate against the effective configuration (stored values plus
		// the submitted ones), so a partial API update cannot validate an
		// empty data folder it did not send.
		$stored = $this->getSettingsObject($context);
		$effective = array_merge($stored, $data);
		$enabling = !empty($effective["enable"]);
		$dataRef = (string)($effective["dataRef"] ?? "");
		$maskProtected = !empty($effective["maskProtected"]);
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
	 * Candidates for the 'Shared folder' select of the storage source
	 * form.
	 *
	 * The submitted value is the container path that is derived for the
	 * shared folder ('/srv/<shared folder name>'), i.e. exactly the value
	 * the 'Container path' field is filled in with in the Workbench form.
	 * This way the user immediately sees the mount path that will be used
	 * and can still edit it; the shared folder is resolved from this value
	 * again in setSource(). The derived path identifies exactly one shared
	 * folder: colliding names are disambiguated with a UUID suffix (see
	 * getSharedFolderIndex()), so the mounted folder is always the one the
	 * operator selected.
	 */
	public function getSharedFolderChoices($params, $context)
	{
		$this->validateMethodContext($context, ["role" => OMV_ROLE_ADMINISTRATOR]);

		$result = [];
		foreach ($this->getSharedFolderIndex($context) as $uuid => $entry) {
			$result[] = [
				"value" => $entry["path"],
				"text" => sprintf("%s (%s)", $entry["name"], $entry["path"]),
				"uuid" => $uuid,
				"name" => $entry["name"]
			];
		}

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
		$entry = $db->get("conf.service.filebrowserquantum.source", $params["uuid"])->getAssoc();
		// Additional values for the Workbench form: 'sharedfolder' is the
		// option value of the 'Shared folder' select (the container path
		// derived for the stored shared folder) and 'autoPath' keeps that
		// value so the container path follows the shared folder as long as
		// the user did not customize it.
		$derived = $this->deriveContainerPathForRef(
			$context,
			(string)($entry["sharedfolderref"] ?? "")
		);
		$entry["sharedfolder"] = $derived;
		$entry["autoPath"] = $derived;
		return $entry;
	}

	/**
	 * Create or update a source mapping.
	 */
	public function setSource($params, $context)
	{
		$this->validateMethodContext($context, ["role" => OMV_ROLE_ADMINISTRATOR]);

		// The Workbench form submits the shared folder as the container path
		// derived for it ('sharedfolder'), plain API clients keep sending
		// 'sharedfolderref' directly. Both are accepted, the form value wins.
		$selection = $this->normalizeContainerPath(
			(string)($params["sharedfolder"] ?? "")
		);
		unset($params["sharedfolder"], $params["autoPath"]);
		$ref = trim((string)($params["sharedfolderref"] ?? ""));
		if ("" !== $selection) {
			$ref = $this->sharedFolderRefByContainerPath($context, $selection);
			if ("" === $ref) {
				throw new \OMV\Exception(
					"The selected shared folder could not be resolved. Reload the page and select it again."
				);
			}
		}
		if (empty($ref)) {
			throw new \OMV\Exception("Select a shared folder.");
		}
		$params["sharedfolderref"] = $ref;

		$sfName = $this->sharedFolderName($context, $ref);
		if (empty($sfName)) {
			throw new \OMV\Exception("The selected shared folder could not be resolved.");
		}

		$existingSources = $this->getAllSources();
		$uuid = trim((string)($params["uuid"] ?? ""));
		// The Workbench form sends the OMV 'new object' sentinel UUID when
		// a source is created, so compare against it explicitly.
		$newUuid = (string)\OMV\Environment::get("OMV_CONFIGOBJECT_NEW_UUID");
		$isNew = empty($uuid) || $uuid === "undefined" ||
			!\OMV\Uuid::isUuid4($uuid) || ("" !== $newUuid && $uuid === $newUuid);
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

	/**
	 * Configuration modules whose pending changes invalidate the stack.
	 */
	const RELATED_DIRTY_MODULES = ["filebrowserquantum", "sharedfolders"];

	/**
	 * Whether a related module still has unapplied changes.
	 *
	 * Starting the container from the settings page must not run ahead of
	 * a pending Apply: the stack file and the mounts would still reflect
	 * the previous configuration.
	 */
	private function hasPendingChanges($context): bool
	{
		try {
			$info = \OMV\Rpc\Rpc::call("System", "getInformation", [], $context);
		} catch (\Throwable $e) {
			return false;
		}
		$modules = $info["dirtyModules"] ?? [];
		if (!is_array($modules)) {
			return false;
		}
		foreach ($modules as $module) {
			if (in_array((string)$module, self::RELATED_DIRTY_MODULES, true)) {
				return true;
			}
		}
		return false;
	}

	/**
	 * Fail-closed preconditions shared by start/restart/upgrade.
	 *
	 * A direct RPC call must not bypass the enabled flag, the data folder
	 * requirement or the "at least one storage source" rule.
	 */
	private function assertOperational($context): void
	{
		$settings = $this->getSettingsObject($context);
		if (empty($settings["enable"])) {
			throw new \OMV\Exception(
				"The FileBrowser Quantum service is disabled. Enable it, save and apply the configuration before starting the container."
			);
		}
		$this->validateTopology(
			(string)($settings["dataRef"] ?? ""),
			$this->getAllSources(),
			!empty($settings["maskProtected"]),
			true
		);
		if ($this->hasPendingChanges($context)) {
			throw new \OMV\Exception(
				"There are pending configuration changes (FileBrowser Quantum or shared folders). " .
				"Click 'Apply changes' first, then start or upgrade the container."
			);
		}
	}

	/**
	 * Advance a pinned image tag to the latest upstream release and return
	 * the new tag ('' when the tag is floating or already current).
	 */
	private function advanceImageTag($context): string
	{
		$settings = $this->getSettingsObject($context);
		$current = trim((string)($settings["version"] ?? ""));
		if (!$this->isPinnedTag($current)) {
			// Floating tag: 'docker compose pull' already resolves the newest
			// image of the tag, so nothing has to be rewritten.
			return "";
		}
		$latest = $this->fetchLatestTag($this->channelOf($current));
		if ("" === $latest) {
			throw new \OMV\Exception(
				"The latest upstream release could not be determined, so the pinned image tag cannot be advanced. " .
				"Check whether the NAS can reach github.com, or change the image tag manually."
			);
		}
		$next = $this->nextTagForUpgrade($current, $latest);
		if ("" === $next) {
			return "";
		}
		\OMV\Rpc\Rpc::call("Config", "set", [
			"id" => "conf.service.filebrowserquantum",
			"data" => ["version" => $next]
		], $context);
		return $next;
	}

	public function start($params, $context)
	{
		$this->validateMethodContext($context, ["role" => OMV_ROLE_ADMINISTRATOR]);
		$this->assertOperational($context);
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
		$this->assertOperational($context);
		return $this->execCtlTask($context, "restart");
	}

	/**
	 * Upgrade to the latest upstream release.
	 *
	 * The target tag is resolved and written to the configuration database
	 * first, the Compose stack is re-rendered with it, and only then the
	 * image is pulled and the container recreated - pulling the fixed tag
	 * alone never advances anything. Pull failures abort the task instead
	 * of being swallowed.
	 */
	public function upgrade($params, $context)
	{
		$this->validateMethodContext($context, ["role" => OMV_ROLE_ADMINISTRATOR]);
		$this->assertOperational($context);
		$this->advanceImageTag($context);
		$this->syncComposeStack($context, $this->getSettingsObject($context));
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
