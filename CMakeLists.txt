# SPDX-License-Identifier: Apache-2.0
# Top-level ESP-IDF project.
#
# COMPONENTS is an explicit allow-list: this firmware has no Wi-Fi, no Bluetooth, no
# networking and no TLS, so the wireless stacks and their binary blobs are excluded from
# the build entirely. That is both smaller and immune to a half-cloned IDF submodule
# (esp_wifi/lib) failing to provide libcore.a. Add a component here when the code that
# needs it lands (bt for BLE MIDI, esp_driver_usb_serial_jtag / tinyusb for USB MIDI).
cmake_minimum_required(VERSION 3.16)

set(COMPONENTS
    main
    fmengine
    driver
    esp_driver_i2s
    esp_driver_i2c
    esp_driver_gpio
    esp_timer
    esp_common
    esp_system
    esp_hw_support
    esp_rom
    log
    freertos
    hal
    newlib
    soc
    heap
    spi_flash
    bootloader_support
    partition_table
    app_update
    efuse
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(cardputer_fm1)
