set(IDF_TARGET esp32s3)

set(SDKCONFIG_DEFAULTS
    boards/sdkconfig.base
    boards/sdkconfig.usb
    boards/sdkconfig.ble
    boards/sdkconfig.spiram_sx
    # Ensure our board-specific overrides come last
    boards/ESP32_S3_TOUCH_LCD_2/sdkconfig.board
)

# Use this board's custom partition table CSV
set(MICROPY_BOARD_PARTITION_TABLE_FILENAME partitions.csv)
