# Copyright (c) Orbbec Inc. All Rights Reserved.
# Licensed under the MIT License.

cmake_minimum_required(VERSION 3.10)
project(ob_color)

add_executable(${PROJECT_NAME} color.cpp)

set_property(TARGET ${PROJECT_NAME} PROPERTY CXX_STANDARD 14)
target_link_libraries(${PROJECT_NAME} ob::${OB_SDK_LIB_NAME} ob::examples::utils iceoryx_posh::iceoryx_posh)

set_target_properties(${PROJECT_NAME} PROPERTIES FOLDER "examples")
if(MSVC)
    set_target_properties(${PROJECT_NAME} PROPERTIES VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
endif()

install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION bin)

# Iceoryx Subscriber
add_executable(ob_color_subscriber color_subscriber.cpp)

set_property(TARGET ob_color_subscriber PROPERTY CXX_STANDARD 14)
target_link_libraries(ob_color_subscriber iceoryx_posh::iceoryx_posh ${OpenCV_LIBS})

set_target_properties(ob_color_subscriber PROPERTIES FOLDER "examples")
install(TARGETS ob_color_subscriber RUNTIME DESTINATION bin)
