idf_component_register(
  SRCS "main.cpp" "Console.cpp" "FanMcu.cpp" "FanMesh.cpp" "SagMeter.cpp"
  INCLUDE_DIRS "."
  REQUIRES wcbcore wcb_idf mcu_link maint_idf nvs_flash esp_driver_uart app_update esp_system)
