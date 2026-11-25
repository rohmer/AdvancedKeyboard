cmake_minimum_required(VERSION 3.24)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(AdvancedKeyboardLanguageModels LANGUAGES CXX)
endif()
get_filename_component(advanced_keyboard_root "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
if(NOT TARGET AdvancedKeyboard::ModelCodec)
    add_subdirectory("${advanced_keyboard_root}/tools/ModelCodec" "${CMAKE_CURRENT_BINARY_DIR}/model-codec")
endif()
find_package(Threads REQUIRED)
add_library(UICreatorLanguageModels STATIC Generator.cpp TrainingModel.cpp)
target_compile_features(UICreatorLanguageModels PUBLIC cxx_std_17)
target_include_directories(UICreatorLanguageModels PUBLIC "${CMAKE_CURRENT_LIST_DIR}")
target_link_libraries(UICreatorLanguageModels PUBLIC AdvancedKeyboard::ModelCodec Threads::Threads)
add_executable(UICreatorModelGen ModelGen.cpp)
target_link_libraries(UICreatorModelGen PRIVATE UICreatorLanguageModels)
if(MSVC)
    target_compile_options(UICreatorLanguageModels PRIVATE /utf-8 /EHsc)
    target_compile_options(UICreatorModelGen PRIVATE /utf-8 /EHsc)
endif()
# The unchanged Windows tools discover this ignored helper directory by walking
# repository ancestors. Configure ADVKBD_NATIVE_HELPER_OUTPUT to override it.
set(ADVKBD_NATIVE_HELPER_OUTPUT "${advanced_keyboard_root}/tools/NativeTrieGenerator/bin" CACHE PATH
    "Native helper directory (default matches managed backend discovery)")
set_target_properties(UICreatorModelGen PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${ADVKBD_NATIVE_HELPER_OUTPUT}")
foreach(configuration DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set_target_properties(UICreatorModelGen PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY_${configuration} "${ADVKBD_NATIVE_HELPER_OUTPUT}")
endforeach()
add_custom_command(TARGET UICreatorModelGen POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CMAKE_CURRENT_LIST_DIR}/wordlists"
        "$<TARGET_FILE_DIR:UICreatorModelGen>/wordlists" VERBATIM)
