asm(".global curl_global_init\n"
    ".type curl_global_init @function\n"
    "curl_global_init:\n");

asm(".global curl_global_cleanup\n"
    ".type curl_global_cleanup @function\n"
    "curl_global_cleanup:\n");

asm(".global curl_easy_init\n"
    ".type curl_easy_init @function\n"
    "curl_easy_init:\n");

asm(".global curl_easy_setopt\n"
    ".type curl_easy_setopt @function\n"
    "curl_easy_setopt:\n");

asm(".global curl_easy_perform\n"
    ".type curl_easy_perform @function\n"
    "curl_easy_perform:\n");

asm(".global curl_easy_cleanup\n"
    ".type curl_easy_cleanup @function\n"
    "curl_easy_cleanup:\n");

asm(".global curl_easy_getinfo\n"
    ".type curl_easy_getinfo @function\n"
    "curl_easy_getinfo:\n");

asm(".global curl_easy_strerror\n"
    ".type curl_easy_strerror @function\n"
    "curl_easy_strerror:\n");

asm(".global curl_slist_append\n"
    ".type curl_slist_append @function\n"
    "curl_slist_append:\n");

asm(".global curl_slist_free_all\n"
    ".type curl_slist_free_all @function\n"
    "curl_slist_free_all:\n");

asm(".global curl_ws_recv\n"
    ".type curl_ws_recv @function\n"
    "curl_ws_recv:\n");

asm(".global curl_ws_send\n"
    ".type curl_ws_send @function\n"
    "curl_ws_send:\n");
