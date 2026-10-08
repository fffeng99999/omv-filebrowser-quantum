# This file is part of OpenMediaVault.
#
# @license   https://www.gnu.org/licenses/gpl.html GPL Version 3
# @author    ${GITHUB_USER} <${GITHUB_USER}@users.noreply.github.com>
#
# OpenMediaVault is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# any later version.

# Make sure custom Jinja filters are registered.
{% set _ = salt['omv_utils.register_jinja_filters']() %}

include:
  - .{{ salt['pillar.get']('deploy_filebrowserquantum', 'default') }}
