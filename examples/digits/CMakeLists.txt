# The following lines have to be in your project's CMakeLists in this exact
# order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# The font is loaded at run time, so it goes into the file system image of the
# storage partition. The shipped fonts/digits.bdf was made from the Spleen
# font by tools/pixelfont.py; pass -DDIGITS_BDF=/path/to/spleen-32x64.bdf to
# the build to make it again from the original.
if(DEFINED DIGITS_BDF)
    set(font_dir "${CMAKE_CURRENT_BINARY_DIR}/fonts")
    file(MAKE_DIRECTORY "${font_dir}")
    execute_process(
        COMMAND python3 "${CMAKE_CURRENT_LIST_DIR}/../../tools/pixelfont.py"
                "${DIGITS_BDF}" --ranges 0x30-0x39 --name spleen_32x64
                --comment "Spleen 32x64, digits"
                --bdf-out "${font_dir}/digits.bdf"
        RESULT_VARIABLE font_result
        OUTPUT_QUIET)
    if(NOT font_result EQUAL 0)
        message(FATAL_ERROR "Could not make the digits font from ${DIGITS_BDF}")
    endif()
else()
    set(font_dir "${CMAKE_CURRENT_LIST_DIR}/fonts")
endif()

project(example-pixelfont-digits)

# the image is built after the project() call, where the helper is registered
spiffs_create_partition_image(storage "${font_dir}" FLASH_IN_PROJECT)
