idf_component_register(
    SRCS
        "i2s_pdm_example_main.c"
        "i2s_pdm_rx_stereo.c"
        "agc_processor.c"
        "manual_gain.c"

    INCLUDE_DIRS
        "."
    REQUIRES
        driver
        esp_driver_i2s
        esp_driver_gpio
        esp_timer
        usb_device_uac
)
