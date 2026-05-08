/*
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_behavior_auto_symbol

#include <errno.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/logging/log.h>

#include <drivers/behavior.h>
#include <zmk/behavior.h>
#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/keymap.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

#define ZMK_BHV_AUTO_SYMBOL_MAX_HELD CONFIG_ZMK_BEHAVIOR_AUTO_SYMBOL_MAX_HELD
#define ZMK_BHV_AUTO_SYMBOL_POSITION_FREE UINT32_MAX
#define ZMK_BHV_AUTO_SYMBOL_BINDING_COUNT 2
#define ZMK_BHV_AUTO_SYMBOL_NO_INTERRUPT_LAYER UINT8_MAX

struct behavior_auto_symbol_config {
  uint32_t tapping_term_ms;
  uint32_t interrupt_layer;
  struct zmk_behavior_binding behaviors[ZMK_BHV_AUTO_SYMBOL_BINDING_COUNT];
};

struct active_auto_symbol {
  uint32_t position;
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
  uint8_t source;
#endif
  uint32_t param1;
  uint32_t param2;
  int64_t pressed_at;
  bool interrupted;
  bool primary_pressed;
  bool interrupt_layer_active;
  const struct behavior_auto_symbol_config *config;
};

static struct active_auto_symbol
    active_auto_symbols[ZMK_BHV_AUTO_SYMBOL_MAX_HELD] = {};

static bool has_interrupt_layer(const struct active_auto_symbol *auto_symbol) {
  return auto_symbol->config->interrupt_layer !=
         ZMK_BHV_AUTO_SYMBOL_NO_INTERRUPT_LAYER;
}

static struct active_auto_symbol *find_auto_symbol(uint32_t position) {
  for (int i = 0; i < ZMK_BHV_AUTO_SYMBOL_MAX_HELD; i++) {
    struct active_auto_symbol *auto_symbol = &active_auto_symbols[i];
    if (auto_symbol->position == position) {
      return auto_symbol;
    }
  }

  return NULL;
}

static void clear_auto_symbol(struct active_auto_symbol *auto_symbol) {
  auto_symbol->position = ZMK_BHV_AUTO_SYMBOL_POSITION_FREE;
}

static int new_auto_symbol(struct zmk_behavior_binding *binding,
                           struct zmk_behavior_binding_event *event,
                           const struct behavior_auto_symbol_config *config,
                           struct active_auto_symbol **auto_symbol) {
  for (int i = 0; i < ZMK_BHV_AUTO_SYMBOL_MAX_HELD; i++) {
    struct active_auto_symbol *ref = &active_auto_symbols[i];
    if (ref->position != ZMK_BHV_AUTO_SYMBOL_POSITION_FREE) {
      continue;
    }

    ref->position = event->position;
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
    ref->source = event->source;
#endif
    ref->param1 = binding->param1;
    ref->param2 = binding->param2;
    ref->pressed_at = event->timestamp;
    ref->interrupted = false;
    ref->primary_pressed = false;
    ref->interrupt_layer_active = false;
    ref->config = config;
    *auto_symbol = ref;
    return 0;
  }

  return -ENOMEM;
}

static struct zmk_behavior_binding
selected_binding(struct active_auto_symbol *auto_symbol, int behavior_index) {
  struct zmk_behavior_binding binding =
      auto_symbol->config->behaviors[behavior_index];

  binding.param1 = behavior_index == 0 ? auto_symbol->param1 : auto_symbol->param2;
  binding.param2 = 0;

  return binding;
}

static int press_auto_symbol_behavior(struct active_auto_symbol *auto_symbol,
                                      int behavior_index, int64_t timestamp) {
  struct zmk_behavior_binding binding =
      selected_binding(auto_symbol, behavior_index);
  struct zmk_behavior_binding_event event = {
      .position = auto_symbol->position,
      .timestamp = timestamp,
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
      .source = auto_symbol->source,
#endif
  };

  return zmk_behavior_invoke_binding(&binding, event, true);
}

static int release_auto_symbol_behavior(struct active_auto_symbol *auto_symbol,
                                        int behavior_index, int64_t timestamp) {
  struct zmk_behavior_binding binding =
      selected_binding(auto_symbol, behavior_index);
  struct zmk_behavior_binding_event event = {
      .position = auto_symbol->position,
      .timestamp = timestamp,
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
      .source = auto_symbol->source,
#endif
  };

  return zmk_behavior_invoke_binding(&binding, event, false);
}

static int tap_auto_symbol_behavior(struct active_auto_symbol *auto_symbol,
                                    int behavior_index, int64_t timestamp) {
  int ret = press_auto_symbol_behavior(auto_symbol, behavior_index, timestamp);
  if (ret < 0) {
    return ret;
  }

  return release_auto_symbol_behavior(auto_symbol, behavior_index, timestamp);
}

static int press_primary_on_interrupt(struct active_auto_symbol *auto_symbol,
                                      int64_t timestamp) {
  int ret = press_auto_symbol_behavior(auto_symbol, 0, timestamp);
  if (ret < 0) {
    LOG_WRN("Failed to press interrupted auto-symbol primary (%d)", ret);
    return ret;
  }

  auto_symbol->primary_pressed = true;
  return 0;
}

static int
on_auto_symbol_binding_pressed(struct zmk_behavior_binding *binding,
                               struct zmk_behavior_binding_event event) {
  const struct device *dev = zmk_behavior_get_binding(binding->behavior_dev);
  const struct behavior_auto_symbol_config *config = dev->config;
  struct active_auto_symbol *auto_symbol;

  if (new_auto_symbol(binding, &event, config, &auto_symbol) == -ENOMEM) {
    LOG_ERR("Unable to create new auto-symbol. Insufficient active slots.");
  }

  return ZMK_BEHAVIOR_OPAQUE;
}

static int
on_auto_symbol_binding_released(struct zmk_behavior_binding *binding,
                                struct zmk_behavior_binding_event event) {
  struct active_auto_symbol *auto_symbol = find_auto_symbol(event.position);

  if (auto_symbol == NULL) {
    return ZMK_BEHAVIOR_OPAQUE;
  }

  if (auto_symbol->interrupt_layer_active) {
    zmk_keymap_layer_deactivate(auto_symbol->config->interrupt_layer);
    clear_auto_symbol(auto_symbol);
    return ZMK_BEHAVIOR_OPAQUE;
  }

  if (auto_symbol->primary_pressed) {
    release_auto_symbol_behavior(auto_symbol, 0, event.timestamp);
    clear_auto_symbol(auto_symbol);
    return ZMK_BEHAVIOR_OPAQUE;
  }

  if (auto_symbol->interrupted) {
    clear_auto_symbol(auto_symbol);
    return ZMK_BEHAVIOR_OPAQUE;
  }

  int behavior_index =
      event.timestamp - auto_symbol->pressed_at >=
              auto_symbol->config->tapping_term_ms
          ? 1
          : 0;

  tap_auto_symbol_behavior(auto_symbol, behavior_index, event.timestamp);
  clear_auto_symbol(auto_symbol);
  return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api behavior_auto_symbol_driver_api = {
    .binding_pressed = on_auto_symbol_binding_pressed,
    .binding_released = on_auto_symbol_binding_released,
};

static int auto_symbol_position_state_changed_listener(const zmk_event_t *eh) {
  struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);

  if (ev == NULL || !ev->state) {
    return ZMK_EV_EVENT_BUBBLE;
  }

  for (int i = 0; i < ZMK_BHV_AUTO_SYMBOL_MAX_HELD; i++) {
    struct active_auto_symbol *auto_symbol = &active_auto_symbols[i];

    if (auto_symbol->position == ZMK_BHV_AUTO_SYMBOL_POSITION_FREE ||
        auto_symbol->position == ev->position || auto_symbol->interrupted) {
      continue;
    }

    auto_symbol->interrupted = true;
    if (has_interrupt_layer(auto_symbol)) {
      if (ev->timestamp - auto_symbol->pressed_at >=
          auto_symbol->config->tapping_term_ms) {
        zmk_keymap_layer_activate(auto_symbol->config->interrupt_layer);
        auto_symbol->interrupt_layer_active = true;
      } else {
        press_primary_on_interrupt(auto_symbol, ev->timestamp);
      }
    } else {
      press_primary_on_interrupt(auto_symbol, ev->timestamp);
    }
  }

  return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(behavior_auto_symbol, auto_symbol_position_state_changed_listener);
ZMK_SUBSCRIPTION(behavior_auto_symbol, zmk_position_state_changed);

static int behavior_auto_symbol_init(const struct device *dev) {
  static bool init_first_run = true;

  if (init_first_run) {
    for (int i = 0; i < ZMK_BHV_AUTO_SYMBOL_MAX_HELD; i++) {
      clear_auto_symbol(&active_auto_symbols[i]);
    }
  }

  init_first_run = false;
  return 0;
}

#define AUTO_SYMBOL_INST(n)                                                   \
  static const struct behavior_auto_symbol_config                             \
      behavior_auto_symbol_config_##n = {                                     \
          .tapping_term_ms = DT_INST_PROP(n, tapping_term_ms),                \
          .interrupt_layer = DT_INST_PROP(n, interrupt_layer),                \
          .behaviors = {{.behavior_dev = DEVICE_DT_NAME(                     \
                             DT_INST_PHANDLE_BY_IDX(n, bindings, 0))},       \
                        {.behavior_dev = DEVICE_DT_NAME(                     \
                             DT_INST_PHANDLE_BY_IDX(n, bindings, 1))}}};     \
  BEHAVIOR_DT_INST_DEFINE(n, behavior_auto_symbol_init, NULL, NULL,          \
                          &behavior_auto_symbol_config_##n, POST_KERNEL,     \
                          CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,                \
                          &behavior_auto_symbol_driver_api);

DT_INST_FOREACH_STATUS_OKAY(AUTO_SYMBOL_INST)

#endif
