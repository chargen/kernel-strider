set(TOP_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/include/kedr")
########################################################################

# The rules to produce handlers_${group_name}.c - handlers for the 
# exported kernel functions for a given group.
# For each function in ${functions} list, ${function}.data file 
# should contain information about the handlers of that function.
function(group_create_handlers group_name)
	set(source_file "${CMAKE_CURRENT_BINARY_DIR}/handlers_${group_name}.c")
	
	# Common name of header part of the data file
	set(header_data_file "header.data")
	set(handlers_data_file "${CMAKE_CURRENT_BINARY_DIR}/handlers.data")
	
	set(functions_data)
	foreach(func ${ARGN})
		list(APPEND functions_data "${func}.data")
	endforeach(func ${ARGN})
	
	set(data_files_abs)
	if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${header_data_file}")
		to_abs_path(data_files_abs ${header_data_file} ${functions_data})
	else ()
		to_abs_path(data_files_abs ${functions_data})
	endif (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${header_data_file}")

	if (data_files_abs)
		add_custom_command(OUTPUT ${handlers_data_file}
			COMMAND cat ${data_files_abs} > ${handlers_data_file}
			DEPENDS ${data_files_abs}
		)
	else (data_files_abs)
		add_custom_command(OUTPUT ${handlers_data_file}
			COMMAND touch ${handlers_data_file}
			DEPENDS ${data_files_abs}
		)
	endif (data_files_abs)
	
	add_custom_command(OUTPUT ${source_file}
		COMMAND ${KEDR_GEN_TOOL} 
			"${CMAKE_CURRENT_SOURCE_DIR}/templates"
			${handlers_data_file} > ${source_file}
		DEPENDS ${handlers_data_file}
	)
endfunction(group_create_handlers group_name)

# Allows to specify the source files the current directory provides for the 
# current FH group, defines the appropriate make target(s).
#
# 'sources' is a list of names of the files (note, names only, not paths) 
# in the current source or binary directory to be added to the plugin from
# the current group.
# 
# This is a macro because that allows to implicitly use the names of the 
# plugin and the group rather then require them to be set explicitly each 
# time.
macro(group_add_sources sources)
	set(gras_names ${sources} ${ARGN})
	to_abs_path(gras_src_abs ${gras_names})

	set(gras_src_val SOURCES_FH_DRD_${PLUGIN_SHORT_NAME}_${GROUP_NAME})
	set(gras_target_val TARGETS_FH_DRD_${PLUGIN_SHORT_NAME}_${GROUP_NAME})

	set(gras_sources ${${gras_src_val}})
	foreach (gras_abs ${gras_src_abs})
		list(APPEND gras_sources "${gras_abs}")
	endforeach ()

	set(${gras_src_val} ${gras_sources}
		CACHE INTERNAL
		"Sources for group '${GROUP_NAME}' of plugin '${PLUGIN_SHORT_NAME}'"
	)

	# Add a custom target for each of the files to be added. This is 
	# needed because a target can see the added custom commands only in 
	# the directory where it is defined. It can see other targets 
	# defined in any directory, however.
	set(gras_targets ${${gras_target_val}})
	foreach (gras_name ${gras_names})
		add_custom_target(
			fh_drd_${PLUGIN_SHORT_NAME}_${GROUP_NAME}_${gras_name}
			ALL
			DEPENDS ${gras_name}
		)
		list(APPEND gras_targets 
			fh_drd_${PLUGIN_SHORT_NAME}_${GROUP_NAME}_${gras_name})
	endforeach ()

	set(${gras_target_val} ${gras_targets}
		CACHE INTERNAL
		"Targets for group '${GROUP_NAME}' of plugin '${PLUGIN_SHORT_NAME}'"
	)
endmacro(group_add_sources sources)

# Adds the rules to create the FH plugin handling the listed groups of
# functions.
#
# 'plugin_name' - short name of the plugin (that is, w/o "kedr", "fh", 
#   "drd", etc.)
# 
# Call this function AFTER add_subdirectory() calls for all the groups 
# (for the appropriate variables to be set by the groups).
function(add_fh_drd_plugin plugin_name groups)
	set(plugin_full_name "kedr_fh_drd_${plugin_name}")
	set(group_targets)
	set(group_sources)
	foreach (group_name ${groups} ${ARGN})
		foreach (tgt ${TARGETS_FH_DRD_${plugin_name}_${group_name}})
			list(APPEND group_targets ${tgt})
		endforeach ()
		
		set(group_var SOURCES_FH_DRD_${plugin_name}_${group_name})
		if (DEFINED ${group_var})
			foreach(item ${${group_var}})
				list(APPEND group_sources ${item})
			endforeach()
		else ()
			message(WARNING 
		"${plugin_full_name}: group \"${group_name}\" provided no sources."
			)
		endif()
	endforeach(group_name ${groups} ${ARGN})
	
	set(func_top_dir "${CMAKE_SOURCE_DIR}/functions/")
	kbuild_include_directories("${func_top_dir}")
	kbuild_use_symbols("${CMAKE_BINARY_DIR}/core/Module.symvers")
	kbuild_add_dependencies("kedr_mem_core")

	# kbuild_add_module() does not like the sources from the directories
	# other than the current one and its subdirs, so copy fh_plugin.c to
	# the build tree explicitly.
	rule_copy_file(
		"${CMAKE_CURRENT_BINARY_DIR}/fh_plugin.c" 
		"${func_top_dir}/util/fh_plugin.c")
	
	kbuild_add_module(${plugin_full_name}
	# sources
		${group_sources}
	
	# common sources
		"fh_plugin.c"
		
	# common headers
		"${func_top_dir}/util/fh_plugin.h"
		"${TOP_INCLUDE_DIR}/kedr_mem/core_api.h"
		"${TOP_INCLUDE_DIR}/kedr_mem/functions.h"
		"${TOP_INCLUDE_DIR}/kedr_mem/local_storage.h"
		"${TOP_INCLUDE_DIR}/object_types.h"
		"${CMAKE_BINARY_DIR}/config.h"
	)
	add_dependencies(${plugin_full_name} ${group_targets})
	kedr_install_kmodule(${plugin_full_name})
	kedr_install_symvers(${plugin_full_name})
endfunction(add_fh_drd_plugin plugin_name groups)
########################################################################

# Adds the tests for those functions from ${functions_to_test} list that
# are actually available in the system (i.e. are listed in ${functions}).
# The tests check that the appropriate events are generated for the calls 
# to these functions. 
# The actual testing is performed using "test.sh" script in the tests'
# directory for the group, the name of the function being the argument of
# that script.
function(add_tests_for_group plugin_name group_name functions_to_test)
	set(test_data_file "${CMAKE_CURRENT_BINARY_DIR}/test.data")
	set(test_header_data_file "header.data")
	set(functions_triggers)
	set(test_module "test_fh_drd_${plugin_name}_${group_name}")
	set(template_dir "${CMAKE_SOURCE_DIR}/functions/tests/handlers/templates")
	
	configure_file (
		"${CMAKE_SOURCE_DIR}/functions/tests/handlers/test.sh.in"
		"${CMAKE_CURRENT_BINARY_DIR}/test.sh"
		@ONLY
	)
	
	foreach(func ${functions_to_test} ${ARGN})
		list(FIND functions ${func} func_index)
		if (NOT (func_index EQUAL -1))
			list(APPEND functions_triggers "${func}.trigger")
			
			kedr_test_add_script (functions.${plugin_name}.${func}.01
			    "test.sh" ${func}
			)
		endif()
	endforeach(func ${functions_to_test} ${ARGN})

	to_abs_path(data_files_abs ${test_header_data_file} 
		${functions_triggers})
	
	add_custom_command(OUTPUT ${test_data_file}
		COMMAND cat ${data_files_abs} > ${test_data_file}
		DEPENDS ${data_files_abs}
	)
	
	add_custom_command(OUTPUT "module.c"
		COMMAND ${KEDR_GEN_TOOL} "${template_dir}" 
			"${test_data_file}" > "module.c"
		DEPENDS "${test_data_file}"
	)

	kbuild_include_directories("${CMAKE_BINARY_DIR}")
	kbuild_add_module(${test_module}
		"module.c"
		"${CMAKE_BINARY_DIR}/config.h"
	)  
	kedr_test_add_target(${test_module})
endfunction(add_tests_for_group plugin_name group_name functions_to_test)
########################################################################

# Test-related stuff
# The directory where the tests should look for the user-mode helper 
# scripts.
set(KEDR_UMH_DIR "${CMAKE_BINARY_DIR}/core/tests")

# The core module.
set(CORE_MODULE_NAME "kedr_mem_core")
set(CORE_MODULE_DIR "${CMAKE_BINARY_DIR}/core")
########################################################################

# The operations common to different kinds of kernel modules are handled 
# there.
add_subdirectory(common)

# The operations specific to network drivers are handled there.
add_subdirectory(net)

if (KEDR_COI_INSTALL_DIR)
	# This plugin handles ordering relationships in various file- and
	# filesystem-related operations. Needs KEDR COI system.
	add_subdirectory(fs)
endif (KEDR_COI_INSTALL_DIR)

# Without KEDR COI, the full-fledged handling for cdev operations will
# not be built (it is a part of 'fs' plugin). In this case a simple 
# model of these operations can be used that is provided by 
# kedr_fh_drd_cdev plugin. It will be built anyway but cannot be used 
# simultaneously with 'fs' plugin in the same analysis session.
add_subdirectory(cdev)

# USB-related operations.
add_subdirectory(usb)
########################################################################

kedr_test_add_subdirectory(tests)
########################################################################
