// OhMyPebble - a digital watchface.
// Primary target: Pebble Time 2 (emery, 200x228, 64 colors).
// Also builds for basalt / chalk / diorite / flint / gabbro; layout is
// computed from the screen bounds so every resolution is handled.
#include <pebble.h>

#define ACCENT_COLOR   PBL_IF_COLOR_ELSE(GColorChromeYellow, GColorWhite)
#define DIM_COLOR      PBL_IF_COLOR_ELSE(GColorDarkGray, GColorWhite)
#define WARN_COLOR     PBL_IF_COLOR_ELSE(GColorRed, GColorWhite)
#define OK_COLOR       PBL_IF_COLOR_ELSE(GColorIslamicGreen, GColorWhite)

static Window *s_window;
static Layer *s_canvas;

static char s_time_buf[8];
static char s_date_buf[24];
static char s_steps_buf[16];
static char s_batt_buf[8];

static BatteryChargeState s_battery;
static bool s_bt_connected;
static int s_steps;
static int s_steps_goal;

static GFont s_time_font;
static GFont s_date_font;
static GFont s_small_font;

// ---------------------------------------------------------------- data

static void update_time(struct tm *t) {
  strftime(s_time_buf, sizeof(s_time_buf),
           clock_is_24h_style() ? "%H:%M" : "%I:%M", t);
  // Drop leading zero in 12h mode.
  if (!clock_is_24h_style() && s_time_buf[0] == '0') {
    memmove(s_time_buf, s_time_buf + 1, sizeof(s_time_buf) - 1);
  }
  strftime(s_date_buf, sizeof(s_date_buf), "%a %b %d", t);
}

static void update_battery(BatteryChargeState state) {
  s_battery = state;
  snprintf(s_batt_buf, sizeof(s_batt_buf), "%d%%", state.charge_percent);
}

static void update_health(void) {
#if defined(PBL_HEALTH)
  HealthServiceAccessibilityMask mask = health_service_metric_accessible(
      HealthMetricStepCount, time_start_of_today(), time(NULL));
  if (mask & HealthServiceAccessibilityMaskAvailable) {
    s_steps = (int)health_service_sum_today(HealthMetricStepCount);
  } else {
    s_steps = -1;
  }

  // Use the user's typical daily total as the "goal"; fall back to 8000.
  s_steps_goal = 8000;
  const time_t start = time_start_of_today();
  const time_t end = start + SECONDS_PER_DAY;
  HealthServiceAccessibilityMask avg_mask = health_service_metric_averaged_accessible(
      HealthMetricStepCount, start, end, HealthServiceTimeScopeDaily);
  if (avg_mask & HealthServiceAccessibilityMaskAvailable) {
    int avg = (int)health_service_sum_averaged(
        HealthMetricStepCount, start, end, HealthServiceTimeScopeDaily);
    if (avg > 0) {
      s_steps_goal = avg;
    }
  }
#else
  s_steps = -1;
  s_steps_goal = 0;
#endif
  if (s_steps >= 0) {
    snprintf(s_steps_buf, sizeof(s_steps_buf), "%d", s_steps);
  } else {
    s_steps_buf[0] = '\0';
  }
}

// ---------------------------------------------------------------- drawing

static void draw_text_centered(GContext *ctx, const char *text, GFont font,
                               GRect box, GColor color) {
  graphics_context_set_text_color(ctx, color);
  graphics_draw_text(ctx, text, font, box, GTextOverflowModeFill,
                     GTextAlignmentCenter, NULL);
}

static void draw_battery(GContext *ctx, GPoint origin) {
  const int w = 22, h = 10;
  GRect body = GRect(origin.x, origin.y, w, h);
  graphics_context_set_stroke_color(ctx, GColorWhite);
  graphics_draw_rect(ctx, body);
  graphics_context_set_fill_color(ctx, GColorWhite);
  graphics_fill_rect(ctx, GRect(origin.x + w, origin.y + 3, 2, h - 6), 0,
                     GCornerNone);

  int fill_w = (w - 4) * s_battery.charge_percent / 100;
  GColor fill = s_battery.is_charging ? OK_COLOR
               : (s_battery.charge_percent <= 20 ? WARN_COLOR : GColorWhite);
  graphics_context_set_fill_color(ctx, fill);
  graphics_fill_rect(ctx, GRect(origin.x + 2, origin.y + 2, fill_w, h - 4), 0,
                     GCornerNone);
}

static void draw_bluetooth(GContext *ctx, GPoint c) {
  // Minimal Bluetooth rune drawn with lines, ~8x12 px.
  graphics_context_set_stroke_color(ctx, s_bt_connected ? GColorWhite : WARN_COLOR);
  graphics_context_set_stroke_width(ctx, 1);
  GPoint top = GPoint(c.x, c.y - 6);
  GPoint bot = GPoint(c.x, c.y + 6);
  graphics_draw_line(ctx, top, bot);
  graphics_draw_line(ctx, top, GPoint(c.x + 4, c.y - 3));
  graphics_draw_line(ctx, GPoint(c.x + 4, c.y - 3), GPoint(c.x - 4, c.y + 3));
  graphics_draw_line(ctx, bot, GPoint(c.x + 4, c.y + 3));
  graphics_draw_line(ctx, GPoint(c.x + 4, c.y + 3), GPoint(c.x - 4, c.y - 3));
  if (!s_bt_connected) {
    graphics_draw_line(ctx, GPoint(c.x - 6, c.y - 6), GPoint(c.x + 6, c.y + 6));
  }
}

static void canvas_update_proc(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);
  // Keep content away from the edge on round displays.
  GRect area = grect_inset(bounds, GEdgeInsets(PBL_IF_ROUND_ELSE(bounds.size.h / 8, 6)));
  const int w = area.size.w;
  const int cx = bounds.size.w / 2;
  const bool large = bounds.size.w >= 200;   // emery / gabbro

  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);

  // --- status row: bluetooth + battery
  const int status_y = area.origin.y + 4;
  draw_bluetooth(ctx, GPoint(cx - 30, status_y + 6));
  draw_battery(ctx, GPoint(cx - 14, status_y + 1));
  graphics_context_set_text_color(ctx, DIM_COLOR);
  graphics_draw_text(ctx, s_batt_buf, s_small_font,
                     GRect(cx + 14, status_y - 4, 40, 20),
                     GTextOverflowModeFill, GTextAlignmentLeft, NULL);

  // --- time (vertically centered)
  const int time_h = large ? 56 : 48;
  const int time_y = bounds.size.h / 2 - time_h / 2 - (large ? 14 : PBL_IF_ROUND_ELSE(20, 12));
  draw_text_centered(ctx, s_time_buf, s_time_font,
                     GRect(area.origin.x, time_y, w, time_h + 6), GColorWhite);

  // accent divider
  const int div_y = time_y + time_h + 6;
  const int div_w = w * 2 / 3;
  graphics_context_set_fill_color(ctx, ACCENT_COLOR);
  graphics_fill_rect(ctx, GRect(cx - div_w / 2, div_y, div_w, 3), 1, GCornersAll);

  // --- date
  draw_text_centered(ctx, s_date_buf, s_date_font,
                     GRect(area.origin.x, div_y + 4, w, 30), ACCENT_COLOR);

  // --- steps + progress bar
  if (s_steps >= 0) {
    const int bar_w = w * 2 / 3;
    const int bar_h = 4;
    const int bar_y = area.origin.y + area.size.h - bar_h - 4;
    const int text_y = bar_y - 22;

    draw_text_centered(ctx, s_steps_buf, s_small_font,
                       GRect(area.origin.x, text_y, w, 20), GColorWhite);

    graphics_context_set_fill_color(ctx, DIM_COLOR);
    graphics_fill_rect(ctx, GRect(cx - bar_w / 2, bar_y, bar_w, bar_h), 2, GCornersAll);
    if (s_steps_goal > 0) {
      int pct = s_steps * 100 / s_steps_goal;
      if (pct > 100) pct = 100;
      int fw = bar_w * pct / 100;
      if (fw > 0) {
        graphics_context_set_fill_color(ctx, pct >= 100 ? OK_COLOR : ACCENT_COLOR);
        graphics_fill_rect(ctx, GRect(cx - bar_w / 2, bar_y, fw, bar_h), 2, GCornersAll);
      }
    }
  }
}

// ---------------------------------------------------------------- events

static void tick_handler(struct tm *tick_time, TimeUnits units_changed) {
  update_time(tick_time);
  if (units_changed & DAY_UNIT) {
    update_health();
  }
  layer_mark_dirty(s_canvas);
}

static void battery_handler(BatteryChargeState state) {
  update_battery(state);
  layer_mark_dirty(s_canvas);
}

static void bt_handler(bool connected) {
  if (s_bt_connected && !connected) {
    vibes_double_pulse();
  }
  s_bt_connected = connected;
  layer_mark_dirty(s_canvas);
}

#if defined(PBL_HEALTH)
static void health_handler(HealthEventType event, void *context) {
  if (event == HealthEventMovementUpdate || event == HealthEventSignificantUpdate) {
    update_health();
    layer_mark_dirty(s_canvas);
  }
}
#endif

// ---------------------------------------------------------------- window

static void window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);
  const bool large = bounds.size.w >= 200;

  s_time_font = fonts_get_system_font(large ? FONT_KEY_ROBOTO_BOLD_SUBSET_49
                                            : FONT_KEY_LECO_42_NUMBERS);
  s_date_font = fonts_get_system_font(large ? FONT_KEY_GOTHIC_24_BOLD
                                            : FONT_KEY_GOTHIC_18_BOLD);
  s_small_font = fonts_get_system_font(FONT_KEY_GOTHIC_18);

  s_canvas = layer_create(bounds);
  layer_set_update_proc(s_canvas, canvas_update_proc);
  layer_add_child(root, s_canvas);
}

static void window_unload(Window *window) {
  layer_destroy(s_canvas);
}

static void init(void) {
  s_window = window_create();
  window_set_background_color(s_window, GColorBlack);
  window_set_window_handlers(s_window, (WindowHandlers) {
    .load = window_load,
    .unload = window_unload,
  });
  window_stack_push(s_window, true);

  time_t now = time(NULL);
  update_time(localtime(&now));
  update_battery(battery_state_service_peek());
  s_bt_connected = connection_service_peek_pebble_app_connection();
  update_health();

  tick_timer_service_subscribe(MINUTE_UNIT, tick_handler);
  battery_state_service_subscribe(battery_handler);
  connection_service_subscribe((ConnectionHandlers) {
    .pebble_app_connection_handler = bt_handler,
  });
#if defined(PBL_HEALTH)
  health_service_events_subscribe(health_handler, NULL);
#endif
}

static void deinit(void) {
  tick_timer_service_unsubscribe();
  battery_state_service_unsubscribe();
  connection_service_unsubscribe();
#if defined(PBL_HEALTH)
  health_service_events_unsubscribe();
#endif
  window_destroy(s_window);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}
