cmake_minimum_required(VERSION 3.24)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(AdvancedKeyboardNativeTests LANGUAGES C CXX)
endif()
get_filename_component(advanced_keyboard_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(keyboard_core "${advanced_keyboard_root}/src")
set(keyboard_headers "${advanced_keyboard_root}/include")
set(model_codec "${advanced_keyboard_root}/tools/ModelCodec")
if(NOT EXISTS "${keyboard_core}/PredictionModel.cpp" OR NOT EXISTS "${model_codec}/RadixModelBuilder.cpp")
    message(FATAL_ERROR "Configure tests from a complete standalone AdvancedKeyboard extraction")
endif()
find_package(Threads REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
enable_testing()

# Reuse the headless component when the root build provides it. A direct
# `cmake -S tests` build uses only the same local sources, never another checkout.
if(TARGET AdvancedKeyboard::Prediction)
    set(prediction_target AdvancedKeyboard::Prediction)
else()
    add_library(advanced_keyboard_test_prediction STATIC
        "${keyboard_core}/PredictionModel.cpp" "${keyboard_core}/PredictionPrefix.cpp"
        "${keyboard_core}/Utf8Fold.cpp" "${keyboard_core}/PredictionStorage.cpp")
    target_compile_features(advanced_keyboard_test_prediction PUBLIC cxx_std_17)
    target_include_directories(advanced_keyboard_test_prediction PUBLIC "${keyboard_core}" "${keyboard_headers}")
    target_compile_definitions(advanced_keyboard_test_prediction PRIVATE
        ESPOS_KEYBOARD_HEADLESS_PREDICTION=1 LVSERIAL_USE_ADVANCED_KEYBOARD_PREDICTION=1)
    target_link_libraries(advanced_keyboard_test_prediction PUBLIC Threads::Threads)
    set(prediction_target advanced_keyboard_test_prediction)
endif()
if(NOT TARGET AdvancedKeyboard::ModelCodec)
    add_subdirectory("${model_codec}" "${CMAKE_CURRENT_BINARY_DIR}/model-codec")
endif()

function(configure_keyboard_test target)
    target_compile_features(${target} PRIVATE cxx_std_17)
    target_include_directories(${target} PRIVATE "${keyboard_core}" "${keyboard_headers}" "${model_codec}")
    target_compile_definitions(${target} PRIVATE ESPOS_KEYBOARD_HEADLESS_PREDICTION=1
        LVSERIAL_USE_ADVANCED_KEYBOARD_PREDICTION=1)
    if(MSVC)
        target_compile_options(${target} PRIVATE /utf-8 /EHsc)
    endif()
endfunction()

add_library(advanced_keyboard_test_facade STATIC "${keyboard_core}/Prediction.cpp")
configure_keyboard_test(advanced_keyboard_test_facade)
target_include_directories(advanced_keyboard_test_facade PUBLIC "${CMAKE_CURRENT_LIST_DIR}/native/stubs" "${keyboard_headers}")
target_compile_definitions(advanced_keyboard_test_facade PUBLIC LVSERIAL_USE_ADVANCED_KEYBOARD_BUILTIN_TRIE=1)
target_link_libraries(advanced_keyboard_test_facade PUBLIC ${prediction_target})

add_executable(advanced_keyboard_prediction_tests native/prediction/KeyboardPrediction.cpp)
configure_keyboard_test(advanced_keyboard_prediction_tests)
target_link_libraries(advanced_keyboard_prediction_tests PRIVATE advanced_keyboard_test_facade)
add_test(NAME AdvancedKeyboardPrediction COMMAND advanced_keyboard_prediction_tests)

add_executable(advanced_keyboard_c_consumer native/prediction/Consumer.c)
set_target_properties(advanced_keyboard_c_consumer PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES LINKER_LANGUAGE CXX)
target_link_libraries(advanced_keyboard_c_consumer PRIVATE advanced_keyboard_test_facade)
add_test(NAME AdvancedKeyboardPublicC COMMAND advanced_keyboard_c_consumer)
add_test(NAME AdvancedKeyboardBundledModels COMMAND "${Python3_EXECUTABLE}" -m unittest discover
    -s "${CMAKE_CURRENT_LIST_DIR}/python" -p test_bundled_models.py -v)
set_tests_properties(AdvancedKeyboardBundledModels PROPERTIES TIMEOUT 90 LABELS "prediction;bundled;radix")
add_test(NAME AdvancedKeyboardBundledMigration COMMAND "${Python3_EXECUTABLE}" -m unittest discover
    -s "${advanced_keyboard_root}/tools/RadixModel" -p test_rebuild_bundled_models.py -v)
set_tests_properties(AdvancedKeyboardBundledMigration PROPERTIES TIMEOUT 90 LABELS "prediction;bundled;radix;tools")

add_executable(radix_prediction native/radix/Main.cpp)
configure_keyboard_test(radix_prediction)
target_link_libraries(radix_prediction PRIVATE ${prediction_target} AdvancedKeyboard::ModelCodec)
add_executable(radix_cursor_cache native/radix/CursorCache.cpp)
configure_keyboard_test(radix_cursor_cache)
target_link_libraries(radix_cursor_cache PRIVATE ${prediction_target} AdvancedKeyboard::ModelCodec)
add_test(NAME AdvancedKeyboardRadixCodec COMMAND radix_prediction)
add_test(NAME AdvancedKeyboardRadixCursorCache COMMAND radix_cursor_cache)
add_test(NAME AdvancedKeyboardRadixIndexParity COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_LIST_DIR}/native/radix/VerifyIndex.py" "$<TARGET_FILE:radix_prediction>")
set_tests_properties(AdvancedKeyboardRadixCursorCache PROPERTIES TIMEOUT 90 LABELS "prediction;radix;cache")

option(ADVKBD_TEST_GENERATOR "Build native trainer and generator golden/parity checks" ON)
if(ADVKBD_TEST_GENERATOR)
    if(NOT TARGET UICreatorLanguageModels)
        add_subdirectory("${advanced_keyboard_root}/tools/NativeTrieGenerator" "${CMAKE_CURRENT_BINARY_DIR}/native-generator")
    endif()
    add_executable(advanced_keyboard_training_tests native/language-models/Training.cpp)
    configure_keyboard_test(advanced_keyboard_training_tests)
    target_link_libraries(advanced_keyboard_training_tests PRIVATE UICreatorLanguageModels)
    add_test(NAME AdvancedKeyboardModifiedKneserNey COMMAND advanced_keyboard_training_tests "${CMAKE_CURRENT_BINARY_DIR}/training-scratch")
    foreach(kind legacy optimized)
        if(kind STREQUAL "legacy")
            set(test_source Generator.cpp)
        else()
            set(test_source Optimized.cpp)
        endif()
        add_executable(advanced_keyboard_${kind}_generator native/language-models/${test_source})
        configure_keyboard_test(advanced_keyboard_${kind}_generator)
        target_include_directories(advanced_keyboard_${kind}_generator PRIVATE "${CMAKE_CURRENT_LIST_DIR}/third_party")
        target_link_libraries(advanced_keyboard_${kind}_generator PRIVATE UICreatorLanguageModels advanced_keyboard_test_facade)
    endforeach()
    add_test(NAME AdvancedKeyboardLegacyGenerator COMMAND advanced_keyboard_legacy_generator
        "${CMAKE_CURRENT_LIST_DIR}/fixtures/language-models" "${CMAKE_CURRENT_BINARY_DIR}/legacy-output"
        "${advanced_keyboard_root}/tools/NativeTrieGenerator/source/wordlists")
    add_test(NAME AdvancedKeyboardOptimizedGenerator COMMAND advanced_keyboard_optimized_generator
        "${CMAKE_CURRENT_BINARY_DIR}/optimized-output")
    set_tests_properties(AdvancedKeyboardModifiedKneserNey AdvancedKeyboardLegacyGenerator AdvancedKeyboardOptimizedGenerator
        PROPERTIES TIMEOUT 180 LABELS "training;generator;parity")
endif()

option(ADVKBD_TEST_PSRAM "Exercise ESP32/Arduino allocation and lifetime with a host fake allocator" ON)
if(ADVKBD_TEST_PSRAM)
    foreach(platform ESP32 ArduinoESP32)
        add_executable(advanced_keyboard_${platform}_psram native/esp32/Main.cpp
            "${keyboard_core}/Prediction.cpp" "${keyboard_core}/PredictionStorage.cpp"
            "${keyboard_core}/PredictionModel.cpp" "${keyboard_core}/PredictionPrefix.cpp" "${keyboard_core}/Utf8Fold.cpp")
        configure_keyboard_test(advanced_keyboard_${platform}_psram)
        target_include_directories(advanced_keyboard_${platform}_psram PRIVATE "${CMAKE_CURRENT_LIST_DIR}/native/stubs")
        target_compile_definitions(advanced_keyboard_${platform}_psram PRIVATE LVSERIAL_USE_ADVANCED_KEYBOARD_BUILTIN_TRIE=1)
        if(platform STREQUAL "ESP32")
            target_compile_definitions(advanced_keyboard_${platform}_psram PRIVATE ESP_PLATFORM=1)
        else()
            target_compile_definitions(advanced_keyboard_${platform}_psram PRIVATE ARDUINO_ARCH_ESP32=1)
        endif()
        target_link_libraries(advanced_keyboard_${platform}_psram PRIVATE Threads::Threads)
        add_test(NAME AdvancedKeyboard${platform}Psram COMMAND advanced_keyboard_${platform}_psram)
        set_tests_properties(AdvancedKeyboard${platform}Psram PROPERTIES TIMEOUT 60 LABELS "prediction;psram;host-fake")
    endforeach()
endif()

add_test(NAME AdvancedKeyboardBenchmarkTools COMMAND "${Python3_EXECUTABLE}" -m unittest discover
    -s "${advanced_keyboard_root}/tools/benchmarks" -p test_benchmark.py -v)
set_tests_properties(AdvancedKeyboardBenchmarkTools PROPERTIES TIMEOUT 60 LABELS "benchmark;unit")

add_test(NAME AdvancedKeyboardPortableTools COMMAND "${Python3_EXECUTABLE}" -m unittest discover
    -s "${CMAKE_CURRENT_LIST_DIR}/python" -p test_legacy_tools.py -v)
set_tests_properties(AdvancedKeyboardPortableTools PROPERTIES TIMEOUT 60 LABELS "tools;legacy;partition")
