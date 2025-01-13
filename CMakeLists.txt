cmake_minimum_required(VERSION 3.16.0)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# This example uses an extra component for common functions such as Wi-Fi and Ethernet connection.
list(APPEND EXTRA_COMPONENT_DIRS $ENV{IDF_PATH}/examples/common_components/protocol_examples_common)

project(esp32-embedded-proto-example)
