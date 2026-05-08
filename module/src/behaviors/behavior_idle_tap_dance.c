/*
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_behavior_idle_tap_dance

#include <errno.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <drivers/behavior.h>
#include <zmk/behavior.h>
#include <zmk/event_manager.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/hid.h>
#include <zmk/keymap.h>
#include <zmk/keys.h>

#include <dt-bindings/zmk/keys.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

#define ZMK_BHV_IDLE_TAP_DANCE_MAX_HELD                                        \
  CONFIG_ZMK_BEHAVIOR_IDLE_TAP_DANCE_MAX_HELD
#define ZMK_BHV_IDLE_TAP_DANCE_POSITION_FREE UINT32_MAX
#define ZMK_BHV_IDLE_TAP_DANCE_BINDING_COUNT 2

struct behavior_idle_tap_dance_config {
  uint32_t tapping_term_ms;
  uint32_t require_prior_idle_ms;
  uint32_t primary_param2;
  uint32_t secondary_param2;
  struct zmk_behavior_binding behaviors[ZMK_BHV_IDLE_TAP_DANCE_BINDING_COUNT];
};

struct active_idle_tap_dance {
  int counter;
  int active_behavior_index;
  uint32_t position;
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
  uint8_t source;
#endif
  uint32_t param1;
  uint32_t param2;
  bool is_pressed;
  bool passthrough;
  bool primary_pressed;
  bool secondary_pressed;
  bool timer_cancelled;
  int64_t release_at;
  struct k_work_delayable release_timer;
  const struct behavior_idle_tap_dance_config *config;
};

static struct active_idle_tap_dance
    active_idle_tap_dances[ZMK_BHV_IDLE_TAP_DANCE_MAX_HELD] = {};

static bool have_last_non_modifier_keycode;
static int64_t last_non_modifier_keycode_at;

static struct active_idle_tap_dance *find_idle_tap_dance(uint32_t position) {
  for (int i = 0; i < ZMK_BHV_IDLE_TAP_DANCE_MAX_HELD; i++) {
    struct active_idle_tap_dance *tap_dance = &active_idle_tap_dances[i];
    if (tap_dance->position == position && !tap_dance->timer_cancelled) {
      return tap_dance;
    }
  }

  return NULL;
}

static void clear_idle_tap_dance(struct active_idle_tap_dance *tap_dance) {
  tap_dance->position = ZMK_BHV_IDLE_TAP_DANCE_POSITION_FREE;
}

static int
new_idle_tap_dance(struct zmk_behavior_binding *binding,
                   struct zmk_behavior_binding_event *event,
                   const struct behavior_idle_tap_dance_config *config,
                   struct active_idle_tap_dance **tap_dance) {
  for (int i = 0; i < ZMK_BHV_IDLE_TAP_DANCE_MAX_HELD; i++) {
    struct active_idle_tap_dance *ref_dance = &active_idle_tap_dances[i];
    if (ref_dance->position != ZMK_BHV_IDLE_TAP_DANCE_POSITION_FREE) {
      continue;
    }

    ref_dance->counter = 0;
    ref_dance->active_behavior_index = 0;
    ref_dance->position = event->position;
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
    ref_dance->source = event->source;
#endif
    ref_dance->param1 = binding->param1;
    ref_dance->param2 = binding->param2;
    ref_dance->is_pressed = true;
    ref_dance->passthrough = false;
    ref_dance->primary_pressed = false;
    ref_dance->secondary_pressed = false;
    ref_dance->timer_cancelled = false;
    ref_dance->release_at = 0;
    ref_dance->config = config;
    *tap_dance = ref_dance;
    return 0;
  }

  return -ENOMEM;
}

static bool
idle_window_open(const struct behavior_idle_tap_dance_config *config,
                 int64_t timestamp) {
  if (!have_last_non_modifier_keycode) {
    return true;
  }

  return timestamp - last_non_modifier_keycode_at >=
         config->require_prior_idle_ms;
}

static struct zmk_behavior_binding
selected_binding(struct active_idle_tap_dance *tap_dance, int behavior_index) {
  struct zmk_behavior_binding binding =
      tap_dance->config->behaviors[behavior_index];

  if (behavior_index == 0) {
    binding.param1 = tap_dance->param1;
    binding.param2 = tap_dance->config->primary_param2;
  } else {
    binding.param1 = tap_dance->param2;
    binding.param2 = tap_dance->config->secondary_param2;
  }

  return binding;
}

static int
press_idle_tap_dance_behavior(struct active_idle_tap_dance *tap_dance,
                              int behavior_index, int64_t timestamp) {
  tap_dance->active_behavior_index = behavior_index;
  if (behavior_index == 0) {
    tap_dance->primary_pressed = true;
  } else {
    tap_dance->secondary_pressed = true;
  }

  struct zmk_behavior_binding binding =
      selected_binding(tap_dance, behavior_index);
  struct zmk_behavior_binding_event event = {
      .position = tap_dance->position,
      .timestamp = timestamp,
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
      .source = tap_dance->source,
#endif
  };

  return zmk_behavior_invoke_binding(&binding, event, true);
}

static int
release_idle_tap_dance_behavior(struct active_idle_tap_dance *tap_dance,
                                int behavior_index, int64_t timestamp) {
  if (behavior_index == 0) {
    tap_dance->primary_pressed = false;
  } else {
    tap_dance->secondary_pressed = false;
  }

  struct zmk_behavior_binding binding =
      selected_binding(tap_dance, behavior_index);
  struct zmk_behavior_binding_event event = {
      .position = tap_dance->position,
      .timestamp = timestamp,
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
      .source = tap_dance->source,
#endif
  };

  return zmk_behavior_invoke_binding(&binding, event, false);
}

static int stop_timer(struct active_idle_tap_dance *tap_dance) {
  int timer_cancel_result = k_work_cancel_delayable(&tap_dance->release_timer);
  if (timer_cancel_result == -EINPROGRESS) {
    tap_dance->timer_cancelled = true;
  }

  return timer_cancel_result;
}

static void reset_timer(struct active_idle_tap_dance *tap_dance,
                        struct zmk_behavior_binding_event event) {
  tap_dance->release_at = event.timestamp + tap_dance->config->tapping_term_ms;
  int32_t ms_left = tap_dance->release_at - k_uptime_get();

  if (ms_left > 0) {
    k_work_schedule(&tap_dance->release_timer, K_MSEC(ms_left));
  }
}

static int tap_backspace(int64_t timestamp) {
  int ret = raise_zmk_keycode_state_changed_from_encoded(BSPC, true, timestamp);
  if (ret < 0) {
    return ret;
  }

  return raise_zmk_keycode_state_changed_from_encoded(BSPC, false, timestamp);
}

static int
on_idle_tap_dance_binding_pressed(struct zmk_behavior_binding *binding,
                                  struct zmk_behavior_binding_event event) {
  const struct device *dev = zmk_behavior_get_binding(binding->behavior_dev);
  const struct behavior_idle_tap_dance_config *config = dev->config;
  struct active_idle_tap_dance *tap_dance = find_idle_tap_dance(event.position);

  if (tap_dance == NULL) {
    if (new_idle_tap_dance(binding, &event, config, &tap_dance) == -ENOMEM) {
      LOG_ERR(
          "Unable to create new idle tap-dance. Insufficient active slots.");
      return ZMK_BEHAVIOR_OPAQUE;
    }

    tap_dance->counter = 1;

    if (!idle_window_open(config, event.timestamp)) {
      tap_dance->passthrough = true;
    }

    return press_idle_tap_dance_behavior(tap_dance, 0, event.timestamp);
  } else {
    stop_timer(tap_dance);
    tap_dance->is_pressed = true;

    if (tap_dance->passthrough) {
      return press_idle_tap_dance_behavior(tap_dance, 0, event.timestamp);
    }

    if (tap_dance->counter < ZMK_BHV_IDLE_TAP_DANCE_BINDING_COUNT) {
      tap_dance->counter++;
    }
  }

  if (tap_dance->counter == ZMK_BHV_IDLE_TAP_DANCE_BINDING_COUNT) {
    if (tap_dance->primary_pressed) {
      int ret = release_idle_tap_dance_behavior(tap_dance, 0, event.timestamp);
      if (ret < 0) {
        return ret;
      }
    }

    int ret = tap_backspace(event.timestamp);
    if (ret < 0) {
      return ret;
    }

    return press_idle_tap_dance_behavior(tap_dance, 1, event.timestamp);
  }

  reset_timer(tap_dance, event);
  return ZMK_BEHAVIOR_OPAQUE;
}

static int
on_idle_tap_dance_binding_released(struct zmk_behavior_binding *binding,
                                   struct zmk_behavior_binding_event event) {
  struct active_idle_tap_dance *tap_dance = find_idle_tap_dance(event.position);

  if (tap_dance == NULL) {
    return ZMK_BEHAVIOR_OPAQUE;
  }

  tap_dance->is_pressed = false;

  if (tap_dance->secondary_pressed) {
    release_idle_tap_dance_behavior(tap_dance, 1, event.timestamp);
    clear_idle_tap_dance(tap_dance);
    return ZMK_BEHAVIOR_OPAQUE;
  }

  if (tap_dance->primary_pressed) {
    release_idle_tap_dance_behavior(tap_dance, 0, event.timestamp);
  }

  if (tap_dance->passthrough) {
    clear_idle_tap_dance(tap_dance);
  } else {
    reset_timer(tap_dance, event);
  }

  return ZMK_BEHAVIOR_OPAQUE;
}

static void behavior_idle_tap_dance_timer_handler(struct k_work *item) {
  struct k_work_delayable *d_work = k_work_delayable_from_work(item);
  struct active_idle_tap_dance *tap_dance =
      CONTAINER_OF(d_work, struct active_idle_tap_dance, release_timer);

  if (tap_dance->position == ZMK_BHV_IDLE_TAP_DANCE_POSITION_FREE ||
      tap_dance->timer_cancelled) {
    return;
  }

  if (!tap_dance->is_pressed && !tap_dance->primary_pressed &&
      !tap_dance->secondary_pressed) {
    clear_idle_tap_dance(tap_dance);
  }
}

static const struct behavior_driver_api behavior_idle_tap_dance_driver_api = {
    .binding_pressed = on_idle_tap_dance_binding_pressed,
    .binding_released = on_idle_tap_dance_binding_released,
};

static int
idle_tap_dance_position_state_changed_listener(const zmk_event_t *eh) {
  struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);

  if (ev == NULL || !ev->state) {
    return ZMK_EV_EVENT_BUBBLE;
  }

  for (int i = 0; i < ZMK_BHV_IDLE_TAP_DANCE_MAX_HELD; i++) {
    struct active_idle_tap_dance *tap_dance = &active_idle_tap_dances[i];

    if (tap_dance->position == ZMK_BHV_IDLE_TAP_DANCE_POSITION_FREE ||
        tap_dance->position == ev->position || tap_dance->passthrough ||
        tap_dance->secondary_pressed) {
      continue;
    }

    stop_timer(tap_dance);
    if (tap_dance->primary_pressed) {
      tap_dance->passthrough = true;
    } else if (!tap_dance->is_pressed) {
      clear_idle_tap_dance(tap_dance);
    }

    return ZMK_EV_EVENT_BUBBLE;
  }

  return ZMK_EV_EVENT_BUBBLE;
}

static int
idle_tap_dance_keycode_state_changed_listener(const zmk_event_t *eh) {
  const struct zmk_keycode_state_changed *ev = as_zmk_keycode_state_changed(eh);

  if (ev == NULL || !ev->state || is_mod(ev->usage_page, ev->keycode)) {
    return ZMK_EV_EVENT_BUBBLE;
  }

  have_last_non_modifier_keycode = true;
  last_non_modifier_keycode_at = ev->timestamp;
  return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(behavior_idle_tap_dance,
             idle_tap_dance_position_state_changed_listener);
ZMK_SUBSCRIPTION(behavior_idle_tap_dance, zmk_position_state_changed);

ZMK_LISTENER(behavior_idle_tap_dance_keycode,
             idle_tap_dance_keycode_state_changed_listener);
ZMK_SUBSCRIPTION(behavior_idle_tap_dance_keycode, zmk_keycode_state_changed);

static int behavior_idle_tap_dance_init(const struct device *dev) {
  static bool init_first_run = true;

  if (init_first_run) {
    for (int i = 0; i < ZMK_BHV_IDLE_TAP_DANCE_MAX_HELD; i++) {
      k_work_init_delayable(&active_idle_tap_dances[i].release_timer,
                            behavior_idle_tap_dance_timer_handler);
      clear_idle_tap_dance(&active_idle_tap_dances[i]);
    }
  }

  init_first_run = false;
  return 0;
}

#define IDLE_TAP_DANCE_INST(n)                                                 \
  static const struct behavior_idle_tap_dance_config                           \
      behavior_idle_tap_dance_config_##n = {                                   \
          .tapping_term_ms = DT_INST_PROP(n, tapping_term_ms),                 \
          .require_prior_idle_ms = DT_INST_PROP(n, require_prior_idle_ms),     \
          .primary_param2 = DT_INST_PROP(n, primary_param2),                   \
          .secondary_param2 = DT_INST_PROP(n, secondary_param2),               \
          .behaviors = {{.behavior_dev = DEVICE_DT_NAME(                       \
                             DT_INST_PHANDLE_BY_IDX(n, bindings, 0))},         \
                        {.behavior_dev = DEVICE_DT_NAME(                       \
                             DT_INST_PHANDLE_BY_IDX(n, bindings, 1))}}};       \
  BEHAVIOR_DT_INST_DEFINE(n, behavior_idle_tap_dance_init, NULL, NULL,         \
                          &behavior_idle_tap_dance_config_##n, POST_KERNEL,    \
                          CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,                 \
                          &behavior_idle_tap_dance_driver_api);

DT_INST_FOREACH_STATUS_OKAY(IDLE_TAP_DANCE_INST)

#endif
