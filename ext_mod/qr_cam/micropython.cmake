add_library(usermod_qr_cam INTERFACE)

target_sources(usermod_qr_cam INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/modqr_cam.c
    ${CMAKE_CURRENT_LIST_DIR}/qr_cam_core.c
)

target_include_directories(usermod_qr_cam INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/../micropython-camera-API/src
    ${CMAKE_CURRENT_LIST_DIR}/../quirc/quirc/lib
)

target_compile_options(usermod_qr_cam INTERFACE -O2)

target_link_libraries(usermod_qr_cam INTERFACE quirc)
target_link_libraries(usermod INTERFACE usermod_qr_cam)

