cmake_minimum_required(VERSION 3.24)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(AdvancedKeyboardParity LANGUAGES CXX)
endif()
get_filename_component(advanced_keyboard_root "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
if(TARGET AdvancedKeyboard::Prediction)
    set(prediction_target AdvancedKeyboard::Prediction)
else()
    add_library(advanced_keyboard_parity_prediction STATIC
        "${advanced_keyboard_root}/src/PredictionModel.cpp"
        "${advanced_keyboard_root}/src/PredictionPrefix.cpp"
        "${advanced_keyboard_root}/src/Utf8Fold.cpp")
    target_compile_features(advanced_keyboard_parity_prediction PUBLIC cxx_std_17)
    target_compile_definitions(advanced_keyboard_parity_prediction PRIVATE
        ESPOS_KEYBOARD_HEADLESS_PREDICTION=1 LVSERIAL_USE_ADVANCED_KEYBOARD_PREDICTION=1)
    target_include_directories(advanced_keyboard_parity_prediction PUBLIC
        "${advanced_keyboard_root}/src" "${advanced_keyboard_root}/include")
    if(MSVC)
        target_compile_options(advanced_keyboard_parity_prediction PRIVATE /utf-8 /EHsc)
    endif()
    set(prediction_target advanced_keyboard_parity_prediction)
endif()
add_executable(host_parity host_parity.cpp)
target_compile_features(host_parity PRIVATE cxx_std_17)
target_include_directories(host_parity PRIVATE
    "${advanced_keyboard_root}/integrations/espos/include")
target_link_libraries(host_parity PRIVATE ${prediction_target})
if(MSVC)
    target_compile_options(host_parity PRIVATE /utf-8 /EHsc)
endif()

set(ADVKBD_PARITY_ASSEMBLY "" CACHE FILEPATH "Built managed KeyboardParity.dll for optional replay smoke test")
if(ADVKBD_PARITY_ASSEMBLY)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    enable_testing()
    add_test(NAME AdvancedKeyboardManagedNativeParity COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_LIST_DIR}/smoke_test.py" --managed "${ADVKBD_PARITY_ASSEMBLY}" --native "$<TARGET_FILE:host_parity>")
    set_tests_properties(AdvancedKeyboardManagedNativeParity PROPERTIES TIMEOUT 90 LABELS "tools;managed;parity")
endif()
