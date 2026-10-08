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
fi

exit 0
