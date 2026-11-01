# This file is part of OpenMediaVault.
#
# @license   https://www.gnu.org/licenses/gpl.html GPL Version 3
# @author    ${GITHUB_USER} <${GITHUB_USER}@users.noreply.github.com>
#
# OpenMediaVault is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# any later version.
#
# Manages the lifecycle of the FileBrowser Quantum Docker container.
#
# The stack files themselves are rendered by the openmediavault-compose
# plugin (the settings page registers the stack through the Compose
# RPC); this state makes sure the container is running when the service
# is enabled and stops it when it is disabled.
#
# Both branches go through /usr/sbin/omv-filebrowserquantum-ctl, which
# runs the fail-closed checks (enabled, data folder, at least one storage
# source) before it touches the stack and wraps the Compose plugin helper
# 'omv-compose-run' with the same --file/--env-file arguments as the
# Compose web UI.
#
# The health check is not a no-op any more: 'omv-filebrowserquantum-ctl
# verify' exits non-zero when the container is not running or Docker
# reports it as unhealthy, so a failing deployment makes 'Apply changes'
# fail instead of silently reporting success.

{% set config = salt['omv_conf.get']('conf.service.filebrowserquantum') %}
{% set compose = salt['omv_conf.get']('conf.service.compose') %}
{% set compose_sf = compose.sharedfolderref | default('') %}
{% set data_ref = config.dataRef | default('') %}
{% set stackfile = salt['cmd.run']('/usr/sbin/omv-filebrowserquantum-ctl path', python_shell=False) ~ '/filebrowserquantum.yml' %}

# Precondition: the Compose shared folder and the data folder must be
# configured; the storage sources themselves are checked by the ctl, which
# fails closed with an explicit message when none is configured.
{% if compose_sf | length > 0 and data_ref | length > 0 %}

{% if config.enable | to_bool %}
filebrowserquantum_stack_up:
  cmd.run:
    - name: /usr/sbin/omv-filebrowserquantum-ctl up
    - onlyif: test -f '{{ stackfile }}'

filebrowserquantum_health_check:
  cmd.run:
    - name: /usr/sbin/omv-filebrowserquantum-ctl verify
    - onlyif: test -f '{{ stackfile }}'
    - require:
      - cmd: filebrowserquantum_stack_up
{% else %}
filebrowserquantum_stack_down:
  cmd.run:
    - name: /usr/sbin/omv-filebrowserquantum-ctl down
    - onlyif: test -f '{{ stackfile }}'
{% endif %}

{% else %}
# Fallback branch: the Compose shared folder or the data folder is not
# configured. The state never fails here (a first-time installation is not
# an error), but a container left over from an earlier configuration is
# stopped so it cannot keep exposing the old mounts.
filebrowserquantum_not_configured:
  test.configurable_test_state:
    - name: "FileBrowser Quantum skipped: Compose shared folder or data folder not configured."
    - result: True
    - changes: False

filebrowserquantum_cleanup_orphans:
  cmd.run:
    - name: /usr/sbin/omv-filebrowserquantum-ctl down
    - onlyif: docker ps --filter name=^filebrowserquantum$ --filter status=running -q | grep -q .
{% endif %}
