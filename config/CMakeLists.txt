if((NOT CONFIG_ZMK_SPLIT) OR CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
  target_sources_ifdef(CONFIG_ZMK_BEHAVIOR_IDLE_TAP_DANCE app PRIVATE
    src/behaviors/behavior_idle_tap_dance.c
  )
endif()
