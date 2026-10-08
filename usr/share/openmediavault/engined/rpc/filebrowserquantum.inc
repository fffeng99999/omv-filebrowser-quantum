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
 * Settings (conf.service.filebrowserquantum) live in the OMV
 * configuration database. The FileBrowser Quantum Docker container is
 * owned by the openmediavault-compose plugin: on save the plugin
 * publishes (or updates) the stack through the Compose RPC
 * (Compose.setFile). The stack therefore follows the Compose plugin
 * conventions (<compose shared folder>/filebrowserquantum/
 * filebrowserquantum.yml + filebrowserquantum.env) and shows up in the
 * Compose web UI (Services -> Compose -> Files / Services) where it can
 * be inspected, edited, started and stopped. The plugin never renders
 * those files itself, so the Compose plugin stays the single writer.
 *
 * The stack is controlled through 'docker compose' (via the
 * /usr/sbin/omv-compose-run helper used by the Compose plugin) with the
 * /usr/sbin/omv-filebrowserquantum-ctl helper (background tasks with
 * live output). The plugin only manages the OMV-side integration
 * (registration, lifecycle, status); FileBrowser Quantum itself is never
 * modified - sources, users, shares and all other settings are managed
 * in the FileBrowser Quantum web interface.
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
     * Settings fields allowed to be persisted by set(). Everything else
     * sent by the form (read-only status fields) is stripped.
     */
    private $settingsFields = [
        "enable", "version", "port", "imageVariant",
        "dataRef", "sourceRef"
    ];

    public function getName()
    {
        return "FileBrowserQuantum";
    }

    public function initialize()
    {
        $this->registerMethod("get");
        $this->registerMethod("set");
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

    /**
     * Release channel derived from the configured version tag
     * (e.g. 2.0.10-beta -> beta, 1.5.8-stable -> stable).
     */
    private function channelOf(string $version): string
    {
        return (false !== strpos($version, "-beta")) ? "beta" : "stable";
    }

    /**
     * Docker image (repository and tag) built from the version and the
     * image variant ('full' or 'slim').
     */
    private function imageOf(string $version, string $variant): string
    {
        $tag = trim($version);
        if ("slim" === $variant) {
            $tag .= "-slim";
        }
        return self::IMAGE_REPO.":".$tag;
    }

    /**
     * Settings object enriched with read-only status information.
     */
    public function get($params, $context)
    {
        $this->validateMethodContext($context, ["role" => OMV_ROLE_ADMINISTRATOR]);

        $object = $this->getSettingsObject($context);

        // Container state. The status text is returned as the English
        // msgid; the Workbench form page runs it through the 'translate'
        // filter (request.get.transform merges the transformed keys into
        // the response, so all other fields are preserved).
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

        // Running image tag (empty while the container does not exist).
        list(, $versionOut) = $this->runCtl(["version"], false);
        $runningVersion = trim(implode("\n", $versionOut));
        $object["runningVersion"] = $runningVersion;

        // Latest upstream release tag of the configured channel.
        $channel = $this->channelOf((string)($object["version"] ?? ""));
        list(, $latestOut) = $this->runCtl(["latest", $channel], false);
        $latestVersion = trim(implode("\n", $latestOut));
        $object["latestVersion"] = $latestVersion;

        // Update is available when the running image is older than the
        // latest upstream release tag. The '-slim' suffix and a leading
        // 'v' are ignored. When either side is unknown the check reports
        // false.
        $updateAvailable = false;
        $a = preg_replace('/-slim$/', "", $runningVersion);
        $b = ltrim($latestVersion, "v");
        if (("" !== $a) && ("" !== $b)) {
            $updateAvailable = version_compare($a, $b, "<");
        }
        $object["updateAvailable"] = $updateAvailable;

        // Resolve the stack directory through the ctl helper (it lives
        // below the Compose plugin shared folder). Empty while the
        // Compose plugin is not configured.
        list(, $pathOut) = $this->runCtl(["path"], false);
        $dir = trim(implode("\n", $pathOut));
        $object["composeFile"] = ("" !== $dir)
            ? $dir."/".self::STACK_NAME.".yml" : "";

        return $object;
    }

    public function set($params, $context)
    {
        $this->validateMethodContext($context, ["role" => OMV_ROLE_ADMINISTRATOR]);
        $data = array_intersect_key($params, array_flip($this->settingsFields));

        $version = trim((string)($data["version"] ?? ""));
        if ("" === $version) {
            throw new \OMV\Exception(
                "The FileBrowser Quantum image tag must not be empty."
            );
        }
        // This plugin manages the v2 configuration format (config.yaml +
        // filebrowser.sqlite). The v1.5.x line uses a different format,
        // so a v1 tag is rejected instead of silently breaking the
        // service.
        if (1 === preg_match('/^v?1[.\-]/', $version)) {
            throw new \OMV\Exception(sprintf(
                "The image tag '%s' belongs to the FileBrowser Quantum ".
                "v1.5.x line, which is not supported by this plugin. Use ".
                "a v2 tag, e.g. 2.0.10-beta.", $version
            ));
        }

        $dataRef = trim((string)($data["dataRef"] ?? ""));
        $sourceRef = trim((string)($data["sourceRef"] ?? ""));
        if (!empty($data["enable"])) {
            if (("" === $dataRef) || ("" === $sourceRef)) {
                throw new \OMV\Exception(
                    "Select the data shared folder and the source shared ".
                    "folder before enabling the service."
                );
            }
        }

        $result = \OMV\Rpc\Rpc::call("Config", "set", [
            "id" => "conf.service.filebrowserquantum",
            "data" => $data
        ], $context);

        // Publish (create or update) the stack in the Compose plugin so
        // it follows the openmediavault-compose conventions and shows up
        // in the Compose web UI. The Compose plugin renders the stack
        // files; this plugin does not write them itself.
        $this->syncComposeStack($context, $this->getSettingsObject($context));

        return $result;
    }

    /**
     * Create or update the FileBrowser Quantum stack in the
     * openmediavault-compose plugin. The data and source folders are
     * referenced through the Compose plugin's '${{ sf:"<name>" }}'
     * placeholder, so a changed shared folder path is resolved by the
     * Compose plugin on deploy without this plugin having to re-render
     * anything.
     */
    private function syncComposeStack($context, array $config): void
    {
        $dataRef = (string)($config["dataRef"] ?? "");
        $sourceRef = (string)($config["sourceRef"] ?? "");
        if (("" === $dataRef) || ("" === $sourceRef)) {
            // Storage not configured yet - nothing to publish.
            return;
        }
        $dataName = $this->sharedFolderName($context, $dataRef);
        $sourceName = $this->sharedFolderName($context, $sourceRef);
        if (("" === $dataName) || ("" === $sourceName)) {
            throw new \OMV\Exception(
                "The selected shared folders could not be resolved."
            );
        }

        $image = $this->imageOf(
            (string)($config["version"] ?? ""),
            (string)($config["imageVariant"] ?? "full")
        );
        $port = (int)($config["port"] ?? 8085);

        $params = [
            "name" => self::STACK_NAME,
            "description" => "FileBrowser Quantum (managed by the ".
                "openmediavault-filebrowser-quantum plugin)",
            "body" => $this->buildComposeBody($dataName, $sourceName,
                $image, $port),
            "showenv" => false,
            "env" => "",
            "showoverride" => false,
            "override" => ""
        ];
        $uuid = $this->findStackUuid($context);
        if ("" !== $uuid) {
            $params["uuid"] = $uuid;
        }

        try {
            \OMV\Rpc\Rpc::call("Compose", "setFile", $params, $context);
        } catch (\Exception $e) {
            throw new \OMV\Exception(sprintf(
                "The FileBrowser Quantum stack could not be registered in ".
                "the Compose plugin: %s. Install openmediavault-compose and ".
                "set the shared folder for the compose files (Services -> ".
                "Compose -> Settings) first.", $e->getMessage()
            ));
        }
    }

    /**
     * UUID of the stack in the Compose plugin (empty when it does not
     * exist yet).
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
     * Docker Compose file body for the FileBrowser Quantum stack (the
     * Compose plugin prepends its own auto-generated header). The
     * container runs as root so that it can read and write source
     * folders regardless of their ownership, and listens on port 80
     * inside the container (mapped to the configured host port).
     */
    private function buildComposeBody(string $dataName, string $sourceName,
        string $image, int $port): string
    {
        $body = <<<'YAML'
name: filebrowserquantum

services:
  filebrowserquantum:
    container_name: filebrowserquantum
    image: %%IMAGE%%
    user: "0:0"
    volumes:
      - ${{ sf:"%%DATA%%" }}:/home/filebrowser/data
      - ${{ sf:"%%SOURCE%%" }}:/srv
    ports:
      - '%%PORT%%:80'
    restart: unless-stopped
YAML;
        $body = strtr($body, [
            "%%IMAGE%%" => $image,
            "%%DATA%%" => $dataName,
            "%%SOURCE%%" => $sourceName,
            "%%PORT%%" => (string)$port
        ]);
        return $body."\n";
    }

    /**
     * Run a stack operation as a background task (the WebUI shows a
     * task dialog with the live 'docker compose' output). This keeps
     * the HTTP request short even when images have to be pulled first.
     */
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

    /**
     * Run the ctl helper as a background task and return the task status
     * file name so the task dialog can poll the live output.
     */
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
