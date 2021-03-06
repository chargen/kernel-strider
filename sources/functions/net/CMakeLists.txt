# This plugin processes the operations specific to the network drivers.

set(PLUGIN_SHORT_NAME "net")

# The groups of functions to be handled.
set(FUNCTION_GROUPS
	"rtnl"           # rtnl_lock(), ...
	"netdev"         # alloc/free netdev, register/unregister
	"net_device_ops" # callbacks from struct net_device_ops
	"ethtool_ops"    # callbacks from struct ethtool_ops
	"header_ops"     # callbacks from struct header_ops
	"napi"           # NAPI-related functions
	"skb"            # alloc/free operations for SKBs
)
########################################################################

foreach(group ${FUNCTION_GROUPS})
	set(GROUP_NAME ${group})
	add_subdirectory(groups/${group})
endforeach(group ${FUNCTION_GROUPS})
########################################################################

set (source_file "module.c")
set (GROUP_LIST_FILE "${CMAKE_CURRENT_BINARY_DIR}/group.list")

# Save the list of groups to a file to be used with kedr_gen.
set (KEDR_FH_GROUP_LIST "")
foreach (gr ${FUNCTION_GROUPS})
	set (KEDR_FH_GROUP_LIST "${KEDR_FH_GROUP_LIST}\tgroup = ${gr}\n")
endforeach ()
configure_file("group.list.in" "${GROUP_LIST_FILE}")

# Generate the source file
add_custom_command(OUTPUT ${source_file}
	COMMAND ${KEDR_GEN_TOOL} 
		"${CMAKE_CURRENT_SOURCE_DIR}/templates"
		"${GROUP_LIST_FILE}" > ${source_file}
	DEPENDS ${GROUP_LIST_FILE}
)

# Add the source file to the list of sources of the plugin.
group_add_sources("${source_file}")
########################################################################

kbuild_use_symbols("${CMAKE_BINARY_DIR}/functions/common/Module.symvers")
kbuild_add_dependencies("kedr_fh_drd_common")

add_fh_drd_plugin(${PLUGIN_SHORT_NAME} ${FUNCTION_GROUPS})
########################################################################

# Tests
kedr_test_add_subdirectory(tests)
########################################################################