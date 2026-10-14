idf_component_register(
  SRCS "MeshRadioIdf.cpp" "ConfigStoreIdf.cpp"
  INCLUDE_DIRS "include"
  REQUIRES wcbcore esp_wifi esp_netif esp_event nvs_flash esp_timer)
