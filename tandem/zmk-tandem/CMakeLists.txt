if(CONFIG_TANDEM_AUTOFLASH)
  zephyr_library()
  zephyr_library_sources(src/autoflash.c)
  # ZMK's own headers, for RST_UF2.
  zephyr_library_include_directories(${APPLICATION_SOURCE_DIR}/include)
endif()
