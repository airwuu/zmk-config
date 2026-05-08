if((NOT CONFIG_ZMK_SPLIT) OR CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
  target_sources_ifdef(CONFIG_ZMK_BEHAVIOR_AUTO_SYMBOL app PRIVATE
    src/behaviors/behavior_auto_symbol.c
  )
endif()
