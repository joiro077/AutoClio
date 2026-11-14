# Ponte OBD: ESP32 classico <-Bluetooth SPP-> ELM327 ; ESP32 <-UART-> painel (ESP32-S3)
# Requer ESP-IDF 5.1 ou mais novo. Alvo: esp32 (NAO serve S3/C3/C6: precisa de Bluetooth Classic).
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(ponte_obd_bt)
