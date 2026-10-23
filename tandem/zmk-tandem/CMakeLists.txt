if(CONFIG_TANDEM_AUTOFLASH)
  zephyr_library()
  zephyr_library_sources(src/autoflash.c)
  # ZMK's own headers, for RST_UF2.
  zephyr_library_include_directories(${APPLICATION_SOURCE_DIR}/include)

  # Embed the main keymap file, so the app can read the layout back from the
  # keyboard. The per-half .keymap files only include it, so look for the one
  # that defines the keymap node.
  set(tandem_keymap "")
  file(GLOB tandem_candidates "${ZMK_CONFIG}/*.keymap")
  foreach(candidate ${tandem_candidates})
    file(READ ${candidate} text)
    string(REGEX MATCH "keymap[ \t\r\n]*{" found "${text}")
    if(found)
      set(tandem_keymap ${candidate})
    endif()
  endforeach()

  set(tandem_inc ${CMAKE_CURRENT_BINARY_DIR}/tandem_keymap.inc)
  if(tandem_keymap)
    file(READ ${tandem_keymap} hex HEX)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
    file(WRITE ${tandem_inc} "${bytes}0x00\n")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${tandem_keymap})
  else()
    file(WRITE ${tandem_inc} "0x00\n")
  endif()
  zephyr_library_include_directories(${CMAKE_CURRENT_BINARY_DIR})
endif()
