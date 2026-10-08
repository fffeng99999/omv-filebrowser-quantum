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
# --file/--env-file arguments as the Compose web UI. The 'up' command
# also creates a minimal config.yaml (source mounted at the container
# path /srv) in the data shared folder when it does not exist yet; an
# existing file is never overwritten. So the same code path is used
# whether the container is started through Apply or through the Start
# button.
#
# The data is never touched here. When the Compose plugin is not
# configured yet, or the storage is not selected yet, the whole state is
# skipped so that applying the configuration never fails.

{% set config = salt['omv_conf.get']('conf.service.filebrowserquantum') %}
{% set compose = salt['omv_conf.get']('conf.service.compose') %}
{% if compose.sharedfolderref | length > 0 and config.dataRef | length > 0 and config.sourceRef | length > 0 %}

{% set sfpath = salt['omv_conf.get_sharedfolder_path'](compose.sharedfolderref).rstrip('/') %}
{% set stackfile = sfpath ~ '/filebrowserquantum/filebrowserquantum.yml' %}

{% if config.enable | to_bool %}

# Enabled: make sure the container is running (idempotent; it is
# recreated automatically when the rendered stack files changed).
filebrowserquantum_stack_up:
  cmd.run:
    - name: /usr/sbin/omv-filebrowserquantum-ctl up
    - onlyif: test -f '{{ stackfile }}'

{% else %}

# Disabled: remove the container but never touch the data or the config
# file. Skipped when the stack was never registered.
filebrowserquantum_stack_down:
  cmd.run:
    - name: /usr/sbin/omv-filebrowserquantum-ctl down
    - onlyif: test -f '{{ stackfile }}'

{% endif %}

{% else %}

# The Compose plugin shared folder or the plugin storage is not
# configured yet: nothing to do.
filebrowserquantum_not_configured:
  test.configurable_test_state:
    - name: filebrowserquantum_not_configured
    - changes: False
    - result: True
    - comment: "Configure the Compose plugin (Services -> Compose -> Settings) and select the data and source shared folders first."

{% endif %}
