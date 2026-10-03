#pragma once

#include <stddef.h>

typedef void CURL;
typedef int CURLcode;
typedef int curl_socket_t;
typedef long long curl_off_t;

struct curl_slist {
  char *data;
  struct curl_slist *next;
};

struct curl_ws_frame {
  int age;
  int flags;
  curl_off_t offset;
  curl_off_t bytesleft;
  size_t len;
  unsigned int close_code;
};

#define CURLE_OK              0
#define CURLE_AGAIN           81
#define CURL_GLOBAL_DEFAULT   3
#define CURLINFO_RESPONSE_CODE 0x200002L
#define CURLINFO_ACTIVESOCKET  0x50002CL

#define CURLOPT_WRITEDATA        10001L
#define CURLOPT_URL              10002L
#define CURLOPT_ERRORBUFFER      10010L
#define CURLOPT_WRITEFUNCTION    20011L
#define CURLOPT_POSTFIELDS       10015L
#define CURLOPT_USERAGENT        10018L
#define CURLOPT_HTTPHEADER       10023L
#define CURLOPT_CUSTOMREQUEST    10036L
#define CURLOPT_ACCEPT_ENCODING  10102L
#define CURLOPT_HEADERFUNCTION   20079L
#define CURLOPT_HEADERDATA       10029L
#define CURLOPT_POST             47L
#define CURLOPT_FOLLOWLOCATION   52L
#define CURLOPT_POSTFIELDSIZE    60L
#define CURLOPT_SSL_VERIFYPEER   64L
#define CURLOPT_SSL_VERIFYHOST   81L
#define CURLOPT_NOSIGNAL         99L
#define CURLOPT_CONNECT_ONLY     141L
#define CURLOPT_TIMEOUT_MS       155L
#define CURLOPT_CONNECTTIMEOUT_MS 156L
#define CURLOPT_HTTP_VERSION     84L
#define CURL_HTTP_VERSION_1_1    2L

#define CURLWS_TEXT    (1 << 0)
#define CURLWS_BINARY  (1 << 1)
#define CURLWS_CLOSE   (1 << 3)
#define CURLWS_PING    (1 << 4)
#define CURLWS_PONG    (1 << 6)

CURL *curl_easy_init(void);
CURLcode curl_easy_setopt(CURL *handle, long option, ...);
CURLcode curl_easy_perform(CURL *handle);
void curl_easy_cleanup(CURL *handle);
CURLcode curl_easy_getinfo(CURL *handle, long info, ...);
const char *curl_easy_strerror(CURLcode code);
CURLcode curl_global_init(long flags);
void curl_global_cleanup(void);
struct curl_slist *curl_slist_append(struct curl_slist *list, const char *string);
void curl_slist_free_all(struct curl_slist *list);
CURLcode curl_ws_recv(CURL *curl, void *buffer, size_t buflen, size_t *nread,
                      const struct curl_ws_frame **metap);
CURLcode curl_ws_send(CURL *curl, const void *buffer, size_t buflen,
                      size_t *nwritten, curl_off_t fragsize, unsigned int flags);
