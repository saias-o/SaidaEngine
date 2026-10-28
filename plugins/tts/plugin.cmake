# Text-to-speech runtime plugin: SAPI on Windows, speechSynthesis on the Web.
target_sources(${SAIDA_PLUGIN_TARGET} PRIVATE "${SAIDA_PLUGIN_DIR}/src/TtsPlugin.cpp")
if(WIN32)
    target_link_libraries(${SAIDA_PLUGIN_TARGET} PRIVATE sapi ole32)
endif()
