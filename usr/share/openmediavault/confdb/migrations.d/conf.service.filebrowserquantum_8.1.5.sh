#!/usr/bin/env dash
#
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
# OpenMediaVault is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.

set -e

. /usr/share/openmediavault/scripts/helper-functions

########################################################################
# Migration for openmediavault-filebrowserquantum 8.1.5:
#
# 1. Add the proxy authentication header key. 8.1.4 wrote
#    FILEBROWSER_NOAUTH / APPLICATION_URL / FILEBROWSER_BASEURL into the
#    container environment, none of which upstream 2.x reads; the
#    authentication method, base URL and external URL now live in
#    config.yaml and switchable proxy authentication needs the header
#    name.
# 2. Storage sources always store the full container path. A legacy empty
#    value is left empty on purpose: setSource() derives the path from
#    the shared folder when it saves the next time.
########################################################################

if omv_config_exists "/config/services/filebrowserquantum"; then
	omv_config_add_key "/config/services/filebrowserquantum" \
		"authProxyHeader" "X-Forwarded-User"
fi

exit 0
