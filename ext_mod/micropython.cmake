# Copyright (c) 2024 - 2025 Kevin G. Schlosser

if(ESP_PLATFORM)
    include(${CMAKE_CURRENT_LIST_DIR}/esp32_components.cmake)
    include(${CMAKE_CURRENT_LIST_DIR}/spi3wire/micropython.cmake)

    if(DEFINED ENV{LEDSTRIP})
        include(${CMAKE_CURRENT_LIST_DIR}/led_strip/micropython.cmake)
    endif()


    if(DEFINED ENV{PY_FREERTOS})
        include(${CMAKE_CURRENT_LIST_DIR}/mpy_freertos/micropython.cmake)
    endif()

endif(ESP_PLATFORM)

include(${CMAKE_CURRENT_LIST_DIR}/lcd_bus/micropython.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/lvgl/micropython.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/lcd_utils/micropython.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/quirc/micropython.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/secp256k1/secp256k1-embedded/micropython.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/uhashlib/micropython.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/micropython-camera-API/src/micropython.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/qr_pipeline/micropython.cmake)

if(DEFINED ENV{FUSION})
    include(${CMAKE_CURRENT_LIST_DIR}/imu_fusion/micropython.cmake)
endif()
