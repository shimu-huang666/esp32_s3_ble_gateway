cmake_minimum_required(VERSION 3.22)

set(IDF_TARGET "esp32s3")
set(EXTRA_COMPONENT_DIRS
    "$ENV{IDF_PATH}/examples/bluetooth/nimble/common/nimble_central_utils")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
project(esp32_s3_ble_gateway)
