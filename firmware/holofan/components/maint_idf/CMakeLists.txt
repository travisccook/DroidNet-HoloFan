idf_component_register(
  SRCS "MaintIdf.cpp" "UploadIdf.cpp"
  INCLUDE_DIRS "include"
  REQUIRES wcbcore esp_wifi esp_netif esp_event esp_http_server app_update esp_partition esp_timer mdns heap lwip)
