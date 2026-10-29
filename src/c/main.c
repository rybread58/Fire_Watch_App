#include <pebble.h>

// Set to 1 to fast-forward through the day (20 min per second) and cycle
// through every weather type each simulated day. Handy in the emulator.
#define DEMO_MODE 0

// Pebble 8-bit colour (2 bits per channel) from a 0xRRGGBB literal, usable in static tables.
#define C8(hex) ((uint8_t)(0xC0 | ((((hex) >> 22) & 3) << 4) | ((((hex) >> 14) & 3) << 2) | (((hex) >> 6) & 3)))

#define PERSIST_WEATHER 1
#define STALE_SECS (6 * 60 * 60)
#define DEFAULT_SUNRISE (6 * 60 + 45)
#define DEFAULT_SUNSET (18 * 60 + 30)
#define TWILIGHT 45  // minutes either side of sunrise/sunset that count as dawn/dusk

// ── Layout (Pebble Time 2, 200 x 228) ─────────────────────────────────────────
#define INFO_Y 131   // top of the stats panel over the dark forest
#define TIME_W 128   // left column: time + date
#define STAT_X 130   // right column: temp / steps / heart / battery
#define STAT_ROW 20

typedef enum { TOD_NIGHT, TOD_DAWN, TOD_DAY, TOD_DUSK } TOD;

// Must match COND in src/pkjs/index.js
typedef enum { W_CLEAR, W_PARTLY, W_DRIZZLE, W_RAIN, W_SNOW, W_STORM, W_COUNT } Cond;

typedef struct {
  int16_t temp;
  uint8_t cond;
  int16_t sunrise;  // minutes after local midnight
  int16_t sunset;
  int32_t fetched;  // epoch seconds, 0 = never
} Weather;

// Spots over the night scenes where the UFO can hover; it moves between them hourly.
typedef struct { int16_t x, y; } UfoSpot;
static const UfoSpot UFO_SPOTS[] = {
  { 98, 50 },   // over the valley
  { 172, 30 },  // above the right-hand ridge
  { 136, 34 },  // right over the lookout
};
#define UFO_STORM_SPOT 1  // the storm scene has lightning through the middle

static Window *s_window;
static Layer *s_scene_layer;
static Layer *s_info_layer;
static GBitmap *s_scene_bmp;
static uint32_t s_scene_res;
static GBitmap *s_icon_steps, *s_icon_heart, *s_icon_temp;
static GFont s_time_font, s_date_font, s_stat_font;

static Weather s_wx = { .cond = W_CLEAR, .sunrise = DEFAULT_SUNRISE, .sunset = DEFAULT_SUNSET };
static struct tm s_tm;
static int s_steps;
static int s_hr;
static BatteryChargeState s_battery;
#if DEMO_MODE
static int s_demo_min;
static int s_demo_cond;
#endif

static inline GColor col(uint8_t argb) { return (GColor){ .argb = argb }; }

static void set_color(GContext *ctx, uint8_t c) {
  graphics_context_set_fill_color(ctx, col(c));
  graphics_context_set_stroke_color(ctx, col(c));
}

static bool weather_fresh(void) {
  return s_wx.fetched != 0 && (int32_t)time(NULL) - s_wx.fetched < STALE_SECS;
}

static Cond current_cond(void) {
#if DEMO_MODE
  return (Cond)s_demo_cond;
#else
  return weather_fresh() && s_wx.cond < W_COUNT ? (Cond)s_wx.cond : W_CLEAR;
#endif
}

static int minute_of_day(void) { return s_tm.tm_hour * 60 + s_tm.tm_min; }

static TOD current_tod(void) {
  int m = minute_of_day();
  if (m < s_wx.sunrise - TWILIGHT || m >= s_wx.sunset + TWILIGHT) return TOD_NIGHT;
  if (m < s_wx.sunrise + TWILIGHT) return TOD_DAWN;
  if (m < s_wx.sunset - TWILIGHT) return TOD_DAY;
  return TOD_DUSK;
}

static uint32_t pick_scene(TOD tod, Cond cond) {
  bool wet = cond == W_DRIZZLE || cond == W_RAIN || cond == W_STORM;
  switch (tod) {
    case TOD_DAWN:
      if (cond == W_SNOW) return RESOURCE_ID_SCENE_DUSK_SNOW;
      return wet ? RESOURCE_ID_SCENE_DAWN_RAIN : RESOURCE_ID_SCENE_DAWN_CLEAR;
    case TOD_DUSK:
      if (cond == W_SNOW) return RESOURCE_ID_SCENE_DUSK_SNOW;
      return wet ? RESOURCE_ID_SCENE_DUSK_RAIN : RESOURCE_ID_SCENE_DUSK_CLEAR;
    case TOD_NIGHT:
      switch (cond) {
        case W_PARTLY: return RESOURCE_ID_SCENE_NIGHT_CLOUD;
        case W_DRIZZLE:
        case W_RAIN: return RESOURCE_ID_SCENE_NIGHT_RAIN;
        case W_SNOW: return RESOURCE_ID_SCENE_NIGHT_SNOW;
        case W_STORM: return RESOURCE_ID_SCENE_NIGHT_STORM;
        default: return RESOURCE_ID_SCENE_NIGHT_CLEAR;
      }
    default:
      switch (cond) {
        case W_PARTLY: return RESOURCE_ID_SCENE_DAY_PARTLY;
        case W_DRIZZLE:
        case W_RAIN: return RESOURCE_ID_SCENE_DAY_RAIN;
        case W_SNOW: return RESOURCE_ID_SCENE_DAY_SNOW;
        case W_STORM: return RESOURCE_ID_SCENE_DAY_STORM;
        default: return RESOURCE_ID_SCENE_DAY_CLEAR;
      }
  }
}

// ── UFO ───────────────────────────────────────────────────────────────────────

// Saucer drawn in the lookout's own palette: navy silhouette, black edges and a
// grey sill, with three small green lights along the rim.
static void draw_ufo(GContext *ctx, int cx, int cy) {
  static const char *SPRITE[] = {
    "....dbbbd....",
    "...bbbbbbb...",
    ".bbbbbbbbbbb.",
    "dbbgbbgbbgbbd",  // rim lights (cy)
    "fffffffffffff",
    "..bbbbbbbbb..",
    "....ddddd....",
  };
  for (int r = 0; r < (int)ARRAY_LENGTH(SPRITE); r++) {
    for (int c = 0; SPRITE[r][c]; c++) {
      uint8_t color;
      switch (SPRITE[r][c]) {
        case 'b': color = C8(0x000055); break;
        case 'd': color = C8(0x000000); break;
        case 'g': color = C8(0x55AA55); break;
        case 'f': color = C8(0x555555); break;
        default: continue;
      }
      set_color(ctx, color);
      graphics_draw_pixel(ctx, GPoint(cx - 6 + c, cy - 3 + r));
    }
  }
}

static void scene_update(Layer *layer, GContext *ctx) {
  graphics_context_set_antialiased(ctx, false);
  TOD tod = current_tod();
  Cond cond = current_cond();

  uint32_t res = pick_scene(tod, cond);
  if (res != s_scene_res || !s_scene_bmp) {
    if (s_scene_bmp) gbitmap_destroy(s_scene_bmp);
    s_scene_bmp = gbitmap_create_with_resource(res);
    s_scene_res = res;
  }
  if (s_scene_bmp) graphics_draw_bitmap_in_rect(ctx, s_scene_bmp, GRect(0, 0, 200, 228));

  if (tod == TOD_NIGHT) {
    int m = minute_of_day();
    const UfoSpot *spot = &UFO_SPOTS[cond == W_STORM ? UFO_STORM_SPOT : s_tm.tm_hour % ARRAY_LENGTH(UFO_SPOTS)];
    static const int8_t BOB[4] = { 0, -1, -2, -1 };
    int uy = spot->y + BOB[m % 4];
    draw_ufo(ctx, spot->x, uy);
  }
}

// ── Stats panel ───────────────────────────────────────────────────────────────

static void draw_battery_icon(GContext *ctx, int x, int y) {
  uint8_t c = s_battery.charge_percent <= 20 && !s_battery.is_charging ? C8(0xFF5555) : C8(0xFFFFFF);
  set_color(ctx, c);
  graphics_draw_rect(ctx, GRect(x, y + 3, 14, 9));
  graphics_fill_rect(ctx, GRect(x + 14, y + 5, 2, 5), 0, GCornerNone);
  int fill = s_battery.charge_percent * 10 / 100;
  if (fill > 0) graphics_fill_rect(ctx, GRect(x + 2, y + 5, fill, 5), 0, GCornerNone);
  if (s_battery.is_charging) {  // little bolt
    set_color(ctx, C8(0xFFFF00));
    graphics_draw_line(ctx, GPoint(x + 8, y + 4), GPoint(x + 5, y + 8));
    graphics_draw_line(ctx, GPoint(x + 5, y + 8), GPoint(x + 9, y + 7));
    graphics_draw_line(ctx, GPoint(x + 9, y + 7), GPoint(x + 6, y + 11));
  }
}

static void draw_stat(GContext *ctx, int row, GBitmap *icon, GSize icon_size, const char *text) {
  int y = 10 + row * STAT_ROW;
  if (icon) {
    graphics_context_set_compositing_mode(ctx, GCompOpSet);
    graphics_draw_bitmap_in_rect(ctx, icon, GRect(STAT_X + (18 - icon_size.w) / 2, y + 1, icon_size.w, icon_size.h));
    graphics_context_set_compositing_mode(ctx, GCompOpAssign);
  }
  graphics_draw_text(ctx, text, s_stat_font, GRect(STAT_X + 22, y, 200 - STAT_X - 24, STAT_ROW),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
}

static void info_update(Layer *layer, GContext *ctx) {
  char buf[24];
  graphics_context_set_text_color(ctx, GColorWhite);

  // time + date
  strftime(buf, sizeof(buf), clock_is_24h_style() ? "%H:%M" : "%I:%M", &s_tm);
  char *t = (!clock_is_24h_style() && buf[0] == '0') ? buf + 1 : buf;
  graphics_draw_text(ctx, t, s_time_font, GRect(0, 22, TIME_W, 30),
                     GTextOverflowModeFill, GTextAlignmentCenter, NULL);
  strftime(buf, sizeof(buf), "%a %d %b", &s_tm);
  for (char *c = buf; *c; c++) {
    if (*c >= 'a' && *c <= 'z') *c -= 32;
  }
  graphics_draw_text(ctx, buf, s_date_font, GRect(0, 52, TIME_W, 18),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);

  // divider
  set_color(ctx, C8(0x555555));
  graphics_draw_line(ctx, GPoint(TIME_W, 12), GPoint(TIME_W, 84));

  // stats
  if (weather_fresh()) snprintf(buf, sizeof(buf), "%d°", s_wx.temp);
  else snprintf(buf, sizeof(buf), "--°");
  draw_stat(ctx, 0, s_icon_temp, GSize(8, 16), buf);

  if (s_steps >= 1000) snprintf(buf, sizeof(buf), "%d,%03d", s_steps / 1000, s_steps % 1000);
  else snprintf(buf, sizeof(buf), "%d", s_steps);
  draw_stat(ctx, 1, s_icon_steps, GSize(12, 16), buf);

  if (s_hr > 0) snprintf(buf, sizeof(buf), "%d", s_hr);
  else snprintf(buf, sizeof(buf), "--");
  draw_stat(ctx, 2, s_icon_heart, GSize(16, 14), buf);

  draw_battery_icon(ctx, STAT_X + 1, 10 + 3 * STAT_ROW);
  snprintf(buf, sizeof(buf), "%d%%", s_battery.charge_percent);
  draw_stat(ctx, 3, NULL, GSize(0, 0), buf);
}

// ── Services ──────────────────────────────────────────────────────────────────

static void redraw(void) {
  if (s_scene_layer) layer_mark_dirty(s_scene_layer);
  if (s_info_layer) layer_mark_dirty(s_info_layer);
}

static void update_health(void) {
  s_steps = (int)health_service_sum_today(HealthMetricStepCount);
  time_t now = time(NULL);
  if (health_service_metric_accessible(HealthMetricHeartRateBPM, now, now) & HealthServiceAccessibilityMaskAvailable) {
    HealthValue hr = health_service_peek_current_value(HealthMetricHeartRateBPM);
    if (hr > 0) s_hr = (int)hr;
  }
}

static void health_handler(HealthEventType event, void *context) {
  if (event == HealthEventMovementUpdate || event == HealthEventHeartRateUpdate || event == HealthEventSignificantUpdate) {
    update_health();
    if (s_info_layer) layer_mark_dirty(s_info_layer);
  }
}

static void battery_handler(BatteryChargeState state) {
  s_battery = state;
  if (s_info_layer) layer_mark_dirty(s_info_layer);
}

static void __attribute__((unused)) request_weather(void) {
  DictionaryIterator *it;
  if (app_message_outbox_begin(&it) != APP_MSG_OK) return;
  dict_write_uint8(it, MESSAGE_KEY_REQUEST, 1);
  app_message_outbox_send();
}

static void tick_handler(struct tm *t, TimeUnits changed) {
  s_tm = *t;
#if DEMO_MODE
  s_demo_min = (s_demo_min + 20) % 1440;
  if (s_demo_min < 20) s_demo_cond = (s_demo_cond + 1) % W_COUNT;
  s_tm.tm_hour = s_demo_min / 60;
  s_tm.tm_min = s_demo_min % 60;
#else
  if (t->tm_min % 30 == 0) request_weather();
#endif
  redraw();
}

static void inbox_received(DictionaryIterator *it, void *context) {
  Tuple *t;
  if ((t = dict_find(it, MESSAGE_KEY_TEMP))) s_wx.temp = t->value->int32;
  if ((t = dict_find(it, MESSAGE_KEY_COND))) s_wx.cond = t->value->int32;
  if ((t = dict_find(it, MESSAGE_KEY_SUNRISE))) s_wx.sunrise = t->value->int32;
  if ((t = dict_find(it, MESSAGE_KEY_SUNSET))) s_wx.sunset = t->value->int32;
  s_wx.fetched = (int32_t)time(NULL);
  persist_write_data(PERSIST_WEATHER, &s_wx, sizeof(s_wx));
  redraw();
}

static void window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect b = layer_get_bounds(root);

  s_time_font = fonts_get_system_font(FONT_KEY_LECO_26_BOLD_NUMBERS_AM_PM);
  s_date_font = fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD);
  s_stat_font = fonts_get_system_font(FONT_KEY_GOTHIC_14);
  s_icon_steps = gbitmap_create_with_resource(RESOURCE_ID_ICON_STEPS);
  s_icon_heart = gbitmap_create_with_resource(RESOURCE_ID_ICON_HEART);
  s_icon_temp = gbitmap_create_with_resource(RESOURCE_ID_ICON_TEMP);

  s_scene_layer = layer_create(b);
  layer_set_update_proc(s_scene_layer, scene_update);
  layer_add_child(root, s_scene_layer);

  s_info_layer = layer_create(GRect(0, INFO_Y, b.size.w, b.size.h - INFO_Y));
  layer_set_update_proc(s_info_layer, info_update);
  layer_add_child(root, s_info_layer);
}

static void window_unload(Window *window) {
  layer_destroy(s_info_layer);
  layer_destroy(s_scene_layer);
  s_info_layer = s_scene_layer = NULL;
  if (s_scene_bmp) gbitmap_destroy(s_scene_bmp);
  s_scene_bmp = NULL;
  gbitmap_destroy(s_icon_steps);
  gbitmap_destroy(s_icon_heart);
  gbitmap_destroy(s_icon_temp);
}

static void init(void) {
  if (persist_exists(PERSIST_WEATHER)) persist_read_data(PERSIST_WEATHER, &s_wx, sizeof(s_wx));
  if (s_wx.sunrise <= 0 || s_wx.sunset <= s_wx.sunrise) {
    s_wx.sunrise = DEFAULT_SUNRISE;
    s_wx.sunset = DEFAULT_SUNSET;
  }
  time_t now = time(NULL);
  s_tm = *localtime(&now);
  s_battery = battery_state_service_peek();
  update_health();

  s_window = window_create();
  window_set_background_color(s_window, GColorBlack);
  window_set_window_handlers(s_window, (WindowHandlers){ .load = window_load, .unload = window_unload });
  window_stack_push(s_window, true);

  app_message_register_inbox_received(inbox_received);
  app_message_open(128, 64);

  tick_timer_service_subscribe(DEMO_MODE ? SECOND_UNIT : MINUTE_UNIT, tick_handler);
  battery_state_service_subscribe(battery_handler);
  health_service_events_subscribe(health_handler, NULL);
}

static void deinit(void) {
  health_service_events_unsubscribe();
  battery_state_service_unsubscribe();
  tick_timer_service_unsubscribe();
  window_destroy(s_window);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}
