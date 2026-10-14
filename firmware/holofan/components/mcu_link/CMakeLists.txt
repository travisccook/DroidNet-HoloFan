idf_component_register(
  SRCS "McuUart.cpp" "McuSpi.cpp"
  INCLUDE_DIRS "include"
  REQUIRES wcbcore esp_driver_uart esp_driver_gpio esp_driver_spi esp_timer)
