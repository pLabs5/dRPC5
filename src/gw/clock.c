#include "gw_common.h"
long long g_time_off;
int g_time_known;
long long g_time_synced_ms;

static size_t date_hdr_cb(char *buf, size_t size, size_t nmemb, void *ud) {
  char *out = (char *)ud;
  size_t len = size * nmemb;

  if (len > 5 && !strncasecmp(buf, "date:", 5) && !out[0]) {
    size_t n = len - 5;

    while (n && (buf[5 + n - 1] == '\r' || buf[5 + n - 1] == '\n')) n--;
    if (n > 63) n = 63;
    memcpy(out, buf + 5, n);
    out[n] = 0;
  }
  return len;
}

static long long rfc1123_epoch(const char *s) {
  static const char kMon[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
  char mon[4];
  const char *p;
  long long era, yoe, doy, doe, days;
  int d = 0, y = 0, h = 0, mi = 0, se = 0, mo;

  if (sscanf(s, "%*3s, %d %3s %d %d:%d:%d", &d, mon, &y, &h, &mi, &se) != 6)
    return -1;
  p = strstr(kMon, mon);
  if (!p || (p - kMon) % 3) return -1;
  mo = (int)((p - kMon) / 3) + 1;
  if (d < 1 || d > 31 || y < 1970 || mo < 1 || mo > 12) return -1;
  if (h < 0 || h > 23 || mi < 0 || mi > 59 || se < 0 || se > 60) return -1;

  y -= mo <= 2;
  era = (y >= 0 ? y : y - 399) / 400;
  yoe = y - era * 400;
  doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  days = era * 146097 + doe - 719468;
  return days * 86400 + h * 3600 + mi * 60 + se;
}

void sync_time_offset(void) {
  CURL *h;
  struct curl_slist *pins;
  CURLcode rc;
  char date[64];
  long long ep, off;

  date[0] = 0;
  pins = psn_pinned_hosts();
  h = curl_easy_init();
  if (!h) {
    curl_slist_free_all(pins);
    return;
  }
  curl_easy_setopt(h, CURLOPT_URL, "https://discord.com/api/v10/gateway");
  curl_easy_setopt(h, CURLOPT_NOBODY, 1L);
  curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(h, CURLOPT_HEADERFUNCTION, date_hdr_cb);
  curl_easy_setopt(h, CURLOPT_HEADERDATA, date);
curl_easy_setopt(h, CURLOPT_RESOLVE, pins);
    curl_setup(h, 10000L, 15000L);

  rc = curl_easy_perform(h);
  curl_easy_cleanup(h);
  curl_slist_free_all(pins);
  g_time_synced_ms = now_ms();
  if (rc != CURLE_OK) return;
  ep = rfc1123_epoch(date);
  if (ep <= 0) return;
  off = ep - (long long)time(NULL);
  if (!g_time_known || off != g_time_off)
    dlogf("drpc5: clock offset %llds\n", off);
  g_time_off = off;
  g_time_known = 1;
}