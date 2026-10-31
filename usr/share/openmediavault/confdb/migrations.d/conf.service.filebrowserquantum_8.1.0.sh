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

CONFIG="/etc/openmediavault/config.xml"

########################################################################
# Migration for openmediavault-filebrowser-quantum 8.1.0:
#
# 1. Add new parent scalar keys:
#    baseURL, appUrl, trustProxyHeaders, authMethod, puid, pgid,
#    maskProtected, extraOptions.
# 2. Ensure <sources> container node exists below <services><filebrowserquantum>.
# 3. If legacy <sourceRef> has a non-empty UUID and <sources> is empty,
#    seamlessly migrate it to a child <source> record.
########################################################################

if omv_config_exists "/config/services/filebrowserquantum"; then
	omv_config_add_key "/config/services/filebrowserquantum" "baseURL" ""
	omv_config_add_key "/config/services/filebrowserquantum" "appUrl" ""
	omv_config_add_key "/config/services/filebrowserquantum" "trustProxyHeaders" "0"
	omv_config_add_key "/config/services/filebrowserquantum" "authMethod" "password"
	omv_config_add_key "/config/services/filebrowserquantum" "puid" "1000"
	omv_config_add_key "/config/services/filebrowserquantum" "pgid" "100"
	omv_config_add_key "/config/services/filebrowserquantum" "maskProtected" "1"
	omv_config_add_key "/config/services/filebrowserquantum" "extraOptions" ""

	if ! omv_config_exists "/config/services/filebrowserquantum/sources"; then
		omv_config_add_node "/config/services/filebrowserquantum" "sources"
	fi

	# Migration of legacy sourceRef -> child <source> record
	if omv_config_exists "/config/services/filebrowserquantum/sourceRef"; then
		old_ref="$(xmlstarlet sel -t -v "/config/services/filebrowserquantum/sourceRef" "${CONFIG}" 2>/dev/null | xmlstarlet unesc 2>/dev/null || true)"
		if [ -n "${old_ref}" ]; then
			count="$(xmlstarlet sel -t -v "count(/config/services/filebrowserquantum/sources/source)" "${CONFIG}" 2>/dev/null || echo 0)"
			if [ "${count}" -eq 0 ]; then
				new_uuid="$(omv_uuid)"
				xmlstarlet ed -L \
					-s "/config/services/filebrowserquantum/sources" -t elem -n "source" -v "" \
					-s "/config/services/filebrowserquantum/sources/source[last()]" -t elem -n "uuid" -v "${new_uuid}" \
					-s "/config/services/filebrowserquantum/sources/source[last()]" -t elem -n "sharedfolderref" -v "${old_ref}" \
					-s "/config/services/filebrowserquantum/sources/source[last()]" -t elem -n "path" -v "/srv" \
					-s "/config/services/filebrowserquantum/sources/source[last()]" -t elem -n "name" -v "Primary Storage" \
					-s "/config/services/filebrowserquantum/sources/source[last()]" -t elem -n "readonly" -v "0" \
					-s "/config/services/filebrowserquantum/sources/source[last()]" -t elem -n "defaultEnabled" -v "1" \
					"${CONFIG}" 2>/dev/null || true
			fi
		fi
	fi
fi

exit 0
