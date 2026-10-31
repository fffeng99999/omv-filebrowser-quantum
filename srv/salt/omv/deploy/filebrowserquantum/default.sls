# This file is part of OpenMediaVault.
#
# @license   https://www.gnu.org/licenses/gpl.html GPL Version 3
# @author    ${GITHUB_USER} <${GITHUB_USER}@users.noreply.github.com>
#
# OpenMediaVault is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# any later version.

# Manages the lifecycle of the FileBrowser Quantum Docker container.
#
# The stack files themselves are rendered by the openmediavault-compose
# plugin (the settings page registers the stack through the Compose
# RPC); this state only makes sure the container is running when the
# service is enabled and stops it when it is disabled.
#
# Both branches go through /usr/sbin/omv-filebrowserquantum-ctl, which
# wraps the Compose plugin helper 'omv-compose-run' with the same
# --file/--env-file arguments as the Compose web UI.

{% set config = salt['omv_conf.get']('conf.service.filebrowserquantum') %}
{% set compose = salt['omv_conf.get']('conf.service.compose') %}
{% set stackfile = salt['cmd.run']('/usr/sbin/omv-filebrowserquantum-ctl path', python_shell=False) ~ '/filebrowserquantum.yml' %}

# Precondition check: Compose shared folder and data folder must be configured
{% if compose.sharedfolderref | length > 0 and config.dataRef | length > 0 %}

{% if config.enable | to_bool %}
filebrowserquantum_stack_up:
  cmd.run:
    - name: /usr/sbin/omv-filebrowserquantum-ctl up
    - onlyif: test -f '{{ stackfile }}'

filebrowserquantum_health_check:
  cmd.run:
    - name: /usr/sbin/omv-filebrowserquantum-ctl status
    - require:
      - cmd: filebrowserquantum_stack_up
{% else %}
filebrowserquantum_stack_down:
  cmd.run:
    - name: /usr/sbin/omv-filebrowserquantum-ctl down
    - onlyif: test -f '{{ stackfile }}'
{% endif %}

{% else %}
# Fallback branch: Compose or Data folder not configured or unassigned
filebrowserquantum_not_configured:
  test.configurable_test_state:
    - name: "FileBrowser Quantum skipped: Compose shared folder or Data folder not configured."
    - result: True
    - changes: False

# Clean up running container if storage was unassigned while container was running
filebrowserquantum_cleanup_orphans:
  cmd.run:
    - name: /usr/sbin/omv-filebrowserquantum-ctl down
    - onlyif: docker ps --filter name=^filebrowserquantum$ --filter status=running -q | grep -q .
{% endif %}
