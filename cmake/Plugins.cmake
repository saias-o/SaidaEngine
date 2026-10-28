# Runtime plugins (see src/plugins/EnginePlugin.hpp and SPEC §11.5).
#
# Each plugins/<name>/plugin.cmake adds its sources to the executable that
# runs projects; the plugin registers itself and stays inert until a project
# lists it in its .saidaproj "plugins". The engine sources never name a plugin.
#
# plugin.cmake is included with two variables set:
#   SAIDA_PLUGIN_TARGET  the executable being built (editor, runtime or Web player)
#   SAIDA_PLUGIN_DIR     plugins/<name>
set(SAIDA_PLUGINS "tts" CACHE STRING
    "Runtime plugins compiled into the editor, the runtime and the Web player (;-separated)")

set(SAIDA_PLUGINS_ROOT "${CMAKE_CURRENT_LIST_DIR}/../plugins")

function(saida_add_plugins target)
    foreach(plugin IN LISTS SAIDA_PLUGINS)
        set(SAIDA_PLUGIN_TARGET ${target})
        get_filename_component(SAIDA_PLUGIN_DIR "${SAIDA_PLUGINS_ROOT}/${plugin}" ABSOLUTE)
        if(NOT EXISTS "${SAIDA_PLUGIN_DIR}/plugin.cmake")
            message(FATAL_ERROR "SAIDA_PLUGINS: '${plugin}' has no ${SAIDA_PLUGIN_DIR}/plugin.cmake")
        endif()
        include("${SAIDA_PLUGIN_DIR}/plugin.cmake")
    endforeach()
endfunction()
