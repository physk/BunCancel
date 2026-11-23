include(FetchContent)
set(SMTG_ENABLE_VST3_PLUGIN_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SMTG_ENABLE_VST3_HOSTING_EXAMPLES ON CACHE BOOL "" FORCE)
set(SMTG_ENABLE_VSTGUI_SUPPORT OFF CACHE BOOL "" FORCE)
set(SMTG_CREATE_PLUGIN_LINK OFF CACHE BOOL "" FORCE)
set(SMTG_RUN_VST_VALIDATOR OFF CACHE BOOL "" FORCE)
FetchContent_Declare(vst3sdk
    GIT_REPOSITORY https://github.com/steinbergmedia/vst3sdk.git
    GIT_TAG 3cdf9ca5d1f5b1b21e0a86832aa4abe55607bd96
    GIT_SUBMODULES base cmake pluginterfaces public.sdk
    GIT_SUBMODULES_RECURSE OFF)
FetchContent_MakeAvailable(vst3sdk)
smtg_enable_vst3_sdk()
set(PROJECT_VERSION 0.1.0)
smtg_add_vst3plugin(BunCancel plugin.cpp)
target_compile_features(BunCancel PRIVATE cxx_std_20)
target_link_libraries(BunCancel PRIVATE sdk buncancel-dsp buncancel-capture)
target_include_directories(BunCancel PRIVATE "${vst3sdk_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/src/capture/windows")
# Redistribute the exact SDK notices with the binary bundle.
foreach(component IN ITEMS base cmake pluginterfaces public.sdk)
    add_custom_command(TARGET BunCancel POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:BunCancel>/../Resources/licenses/${component}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${vst3sdk_SOURCE_DIR}/${component}/LICENSE.txt"
            "$<TARGET_FILE_DIR:BunCancel>/../Resources/licenses/${component}/LICENSE.txt")
endforeach()
add_custom_command(TARGET BunCancel POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${vst3sdk_SOURCE_DIR}/LICENSE.txt"
        "$<TARGET_FILE_DIR:BunCancel>/../Resources/licenses/VST3-SDK-LICENSE.txt")
