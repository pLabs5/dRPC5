#include "gw/gw_common.h"
static size_t gw_append(char *out, size_t cap, size_t off, int *trunc,
                        const char *fmt, ...) {
  va_list ap;
  int n;

  if (off >= cap) {
    *trunc = 1;
    return off;
  }
  va_start(ap, fmt);
  n = vsnprintf(out + off, cap - off, fmt, ap);
  va_end(ap);
  if (n < 0) {
    *trunc = 1;
    return off;
  }
  if ((size_t)n >= cap - off) {
    *trunc = 1;
    return cap - 1;
  }
  return off + (size_t)n;
}

int gateway_build_presence(char *out, size_t cap, const gw_activity *act,
                           const char *status, char *published,
                           size_t pubcap) {
  char esc[512];
  const char *name = act && act->name ? act->name : "";
  size_t off = 0;
  int trunc = 0;

  if (cap == 0) return -1;
  if (published && pubcap) published[0] = 0;

  off = gw_append(out, cap, off, &trunc,
                  "{\"op\":3,\"d\":{\"since\":0,\"activities\":[");
  if (act && act->has_game) {
    json_copy(esc, sizeof(esc), name);
    off = gw_append(out, cap, off, &trunc, "{\"name\":\"%s\",\"type\":%d,",
                    esc, act->type);
    {
      char aid[32] = "";
      cfg_get("app_id", aid, sizeof(aid));
      if (aid[0])
        off = gw_append(out, cap, off, &trunc, "\"application_id\":\"%s\",", aid);
    }
    {
      char plat[32] = "";
      cfg_get("platform", plat, sizeof(plat));
      json_copy(esc, sizeof(esc), plat[0] ? plat : "ps5");
      off = gw_append(out, cap, off, &trunc, "\"platform\":\"%s\",", esc);
    }
    if (act->details && act->details[0]) {
      json_copy(esc, sizeof(esc), act->details);
      off = gw_append(out, cap, off, &trunc, "\"details\":\"%s\",", esc);
    }
    if (act->state && act->state[0]) {
      json_copy(esc, sizeof(esc), act->state);
      off = gw_append(out, cap, off, &trunc, "\"state\":\"%s\",", esc);
    }
    if ((act->start_epoch > 0 || act->end_epoch > 0) && g_time_known) {
      long long real_now = (long long)time(NULL) + g_time_off;
      long long st = act->start_epoch > 0 ? act->start_epoch + g_time_off : 0;
      long long en = act->end_epoch > 0 ? act->end_epoch + g_time_off : 0;

      if (st > real_now + 60) st = 0;
      if (en > 0 && en < st) en = 0;
      if (st > 0 || en > 0) {
        off = gw_append(out, cap, off, &trunc, "\"timestamps\":{");
        if (st > 0) {
          off = gw_append(out, cap, off, &trunc, "\"start\":%lld",
                          st * 1000);
          if (en > 0) off = gw_append(out, cap, off, &trunc, ",");
        }
        if (en > 0)
          off = gw_append(out, cap, off, &trunc, "\"end\":%lld", en * 1000);
        off = gw_append(out, cap, off, &trunc, "},");
      }
    }
    off = gw_append(out, cap, off, &trunc, "\"assets\":{");
    {
      int wrote = 0;
      if (act->large_key && act->large_key[0]) {
        json_copy(esc, sizeof(esc), act->large_key);
        off = gw_append(out, cap, off, &trunc, "\"large_image\":\"%s\",", esc);
        wrote = 1;
      }
      if (act->large_text && act->large_text[0]) {
        json_copy(esc, sizeof(esc), act->large_text);
        off = gw_append(out, cap, off, &trunc, "\"large_text\":\"%s\",", esc);
        wrote = 1;
      }
      if (act->small_key && act->small_key[0]) {
        json_copy(esc, sizeof(esc), act->small_key);
        off = gw_append(out, cap, off, &trunc, "\"small_image\":\"%s\",", esc);
        wrote = 1;
      }
      if (act->small_text && act->small_text[0]) {
        json_copy(esc, sizeof(esc), act->small_text);
        off = gw_append(out, cap, off, &trunc, "\"small_text\":\"%s\",", esc);
        wrote = 1;
      }
      if (wrote && off > 0 && out[off - 1] == ',') {
        off--;
        out[off] = 0;
      }
    }
    off = gw_append(out, cap, off, &trunc, "},\"flags\":0}]");
    if (published && pubcap) snprintf(published, pubcap, "%s", name);
  } else {
    off = gw_append(out, cap, off, &trunc, "]");
  }
  off = gw_append(out, cap, off, &trunc,
                  ",\"status\":\"%s\",\"afk\":false,\"flags\":0}}",
                  status && status[0] ? status : "online");

  if (trunc) {
    if (published && pubcap) published[0] = 0;
    snprintf(out, cap,
             "{\"op\":3,\"d\":{\"since\":0,\"activities\":[],"
             "\"status\":\"%s\",\"afk\":false,\"flags\":0}}",
             status && status[0] ? status : "online");
  }
  return 0;
}

void reload_config(char *status, size_t status_cap, char *mode,
                          size_t mode_cap, char *name, size_t name_cap,
                          char *details, size_t details_cap, char *state,
                          size_t state_cap, char *large_key,
                          size_t large_key_cap, char *large_text,
                          size_t large_text_cap, char *small_key,
                          size_t small_key_cap, char *small_text,
                          size_t small_text_cap) {
  cfg_get("status", status, status_cap);
  cfg_get("mode", mode, mode_cap);
  cfg_get("name", name, name_cap);
  cfg_get("details", details, details_cap);
  cfg_get("state", state, state_cap);
  cfg_get("asset_key", large_key, large_key_cap);
  cfg_get("asset_text", large_text, large_text_cap);
  cfg_get("asset_small_key", small_key, small_key_cap);
  cfg_get("asset_small_text", small_text, small_text_cap);
}

void detect_activity(gw_activity *act, int src_game, int show_platform,
                            int show_artwork, const char *manual_name,
                            const char *manual_details,
                            const char *manual_state,
                            const char *large_key, const char *large_text,
                            const char *small_key, const char *small_text) {
  static char gname[192], gdetails[192], gart[512];
  ps5_app_t app;

  memset(act, 0, sizeof(*act));
  act->type = 0;

  if (presence_foreground(&app) == 0 && app.title_id[0] && src_game) {
    snprintf(gname, sizeof(gname), "%s", app.name);
    snprintf(gdetails, sizeof(gdetails), "%s",
             show_platform ? "PlayStation 5" : app.title_id);
    act->has_game = 1;
    act->name = gname;
    act->start_epoch = app.start_epoch;
    act->details = gdetails;
    act->state = "";
    if (show_artwork) {
      if (custom_art(app.title_id, gart, sizeof(gart)) == 0) {
        act->large_key = gart;
        if (strncmp(gart, "mp:", 3) != 0)
          psn_art_refresh_async(app.title_id, app.concept_id, app.content_id);
      } else if (large_key && large_key[0]) {
        act->large_key = large_key;
      } else if (psn_art_cached(app.title_id, gart, sizeof(gart)) == 0) {
        act->large_key = gart;
        if (strncmp(gart, "mp:", 3) != 0)
          psn_art_refresh_async(app.title_id, app.concept_id, app.content_id);
      } else {
        psn_art_refresh_async(app.title_id, app.concept_id, app.content_id);
      }
      act->large_text =
          large_text && large_text[0] ? large_text : gname;
    }
    act->small_key = small_key;
    act->small_text = small_text;
    return;
  }

  if (manual_name && manual_name[0]) {
    act->has_game = 1;
    act->name = manual_name;
    act->details = manual_details;
    act->state = manual_state;
    act->large_key = large_key;
    act->large_text = large_text;
    act->small_key = small_key;
    act->small_text = small_text;
  }
}
