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

set -e

. /usr/share/openmediavault/scripts/helper-functions

########################################################################
# Update the configuration.
# <config>
#   <services>
#     <filebrowserquantum>
#       <enable>0|1</enable>
#       <version>2.0.10-beta</version>
#       <port>8085</port>
#       <imageVariant>full|slim</imageVariant>
#       <dataRef>uuid-or-empty</dataRef>
#       <sourceRef>uuid-or-empty</sourceRef>
#       <baseURL></baseURL>
#       <appUrl></appUrl>
#       <trustProxyHeaders>0|1</trustProxyHeaders>
#       <authMethod>password|noauth|proxy</authMethod>
#       <authProxyHeader>X-Forwarded-User</authProxyHeader>
#       <puid>1000</puid>
#       <pgid>100</pgid>
#       <maskProtected>0|1</maskProtected>
#       <extraOptions></extraOptions>
#       <sources></sources>
#     </filebrowserquantum>
#   </services>
# </config>
########################################################################
if ! omv_config_exists "/config/services/filebrowserquantum"; then
	omv_config_add_node "/config/services" "filebrowserquantum"
	omv_config_add_key "/config/services/filebrowserquantum" "enable" "0"
	omv_config_add_key "/config/services/filebrowserquantum" "version" "2.0.10-beta"
	omv_config_add_key "/config/services/filebrowserquantum" "port" "8085"
	omv_config_add_key "/config/services/filebrowserquantum" "imageVariant" "full"
	omv_config_add_key "/config/services/filebrowserquantum" "dataRef" ""
	omv_config_add_key "/config/services/filebrowserquantum" "sourceRef" ""
	omv_config_add_key "/config/services/filebrowserquantum" "baseURL" ""
	omv_config_add_key "/config/services/filebrowserquantum" "appUrl" ""
	omv_config_add_key "/config/services/filebrowserquantum" "trustProxyHeaders" "0"
	omv_config_add_key "/config/services/filebrowserquantum" "authMethod" "password"
omv_config_add_key "/config/services/filebrowserquantum" "authProxyHeader" "X-Forwarded-User"
	omv_config_add_key "/config/services/filebrowserquantum" "puid" "1000"
	omv_config_add_key "/config/services/filebrowserquantum" "pgid" "100"
	omv_config_add_key "/config/services/filebrowserquantum" "maskProtected" "1"
	omv_config_add_key "/config/services/filebrowserquantum" "extraOptions" ""
fi

# The sources container node holds child source entries.
if omv_config_exists "/config/services/filebrowserquantum" && \
		! omv_config_exists "/config/services/filebrowserquantum/sources"; then
	omv_config_add_node "/config/services/filebrowserquantum" "sources"
fi

exit 0
