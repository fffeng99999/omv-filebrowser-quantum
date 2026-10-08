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

namespace Engined\Module;

/**
 * FileBrowser Quantum engine module.
 *
 * Marks the module dirty whenever the settings change so that
 * 'Apply changes' re-runs the Salt state (stack lifecycle via
 * omv-compose-run), and reports the service status to the global
 * Services monitoring (dashboard).
 */
class FileBrowserQuantum extends \OMV\Engine\Module\ServiceAbstract
	implements \OMV\Engine\Notify\IListener, \OMV\Engine\Module\IServiceStatus
{
	public function getName()
	{
		return "filebrowserquantum";
	}

	public function getStatus()
	{
		$db = \OMV\Config\Database::getInstance();
		$object = $db->get("conf.service.filebrowserquantum");
		$cmd = new \OMV\System\Process(
			"/usr/sbin/omv-filebrowserquantum-ctl", ["status"]
		);
		$cmd->setRedirect2to1();
		$cmd->setQuiet(true);
		$output = [];
		$exitStatus = 0;
		$cmd->execute($output, $exitStatus);
		$running = (trim(implode("\n", $output)) === "running");
		return [
			"name" => $this->getName(),
			"title" => gettext("FileBrowser Quantum"),
			"enabled" => $object->get("enable"),
			"running" => $running
		];
	}

	public function bindListeners(\OMV\Engine\Notify\Dispatcher $dispatcher)
	{
		$dispatcher->addListener(
			OMV_NOTIFY_MODIFY,
			"org.openmediavault.conf.service.filebrowserquantum",
			[$this, "setDirty"]
		);
		// The stack references its shared folders through the Compose
		// plugin placeholder, so a modified shared folder must mark the
		// service dirty when it is one of the referenced ones.
		$dispatcher->addListener(
			OMV_NOTIFY_MODIFY,
			"org.openmediavault.conf.system.sharedfolder",
			[$this, "onSharedFolder"]
		);
	}

	public function onSharedFolder($object)
	{
		$db = \OMV\Config\Database::getInstance();
		foreach (["dataRef", "sourceRef"] as $prop) {
			if ($db->exists("conf.service.filebrowserquantum", [
				"operator" => "stringEquals",
				"arg0" => $prop,
				"arg1" => $object["uuid"]
			])) {
				$this->setDirty();
				return;
			}
		}
	}
}
