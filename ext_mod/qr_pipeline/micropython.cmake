# MicroPython user C module: qr_pipeline

add_library(usermod_qr_pipeline INTERFACE)

target_sources(usermod_qr_pipeline INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/modqr_pipeline.c
    ${CMAKE_CURRENT_LIST_DIR}/qr_pipeline.c
)

# Include our module directory
target_include_directories(usermod_qr_pipeline INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/../micropython-camera-API/src
    ${CMAKE_CURRENT_LIST_DIR}/../quirc/quirc/lib
)

target_compile_options(usermod_qr_pipeline INTERFACE -O2)

# Link our module into the usermod aggregate target
target_link_libraries(usermod INTERFACE usermod_qr_pipeline)
