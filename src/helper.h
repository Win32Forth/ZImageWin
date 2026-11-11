/* Talks to zimage-helper on the DS423. Health is open. The token stays off the screen. */
#ifndef ZIMAGEWIN_HELPER_H
#define ZIMAGEWIN_HELPER_H

#include <windows.h>

#define ZI_HELPER_HOST L"192.168.1.147"
#define ZI_HELPER_PORT 8742
#define ZI_WM_HELPER (WM_APP + 1)

typedef struct ZiHelperNote {
    int online;
    int library_ok;
    int safe;
    int learned_pin;
    unsigned char pin[32];
    wchar_t line[192];
} ZiHelperNote;

void zi_helper_init(void);
void zi_helper_shutdown(void);
void zi_helper_refresh(HWND hwnd);
/* Blocking health check. Used by the refresh thread and by the build probe. */
int zi_helper_fetch(ZiHelperNote *note);
/* Reads PAIRING-TOKEN or auth.token under the library root and claims it. The message has no token. */
int zi_helper_pair(const wchar_t *library_root, wchar_t *message, size_t cap);

/* Authorized helper call. A missing saved token sets status 401 and does not use the network.
   out receives a malloc'd body when it is not NULL. The caller frees it. */
int zi_helper_call(const wchar_t *method, const wchar_t *path, const char *body, DWORD timeout_ms,
                   DWORD *status, char **out);

#endif
