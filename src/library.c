/* Events, Reorganize, Review, Duplicates, Clean, Set date, and Remove from library.
   The NAS helper performs the file moves. A confirmation sits in front of each one. */
#include "library.h"
#include "helper.h"
#include "grid.h"
#include "editor.h"

#include <shobjidl.h>
#include <winhttp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ZI_LIB_CAP 200

enum {
    ZI_JOB_EVENTS = 1,
    ZI_JOB_RENAME,
    ZI_JOB_REORG_STATUS,
    ZI_JOB_REORG_PLAN,
    ZI_JOB_REORG_START,
    ZI_JOB_REORG_PAUSE,
    ZI_JOB_REORG_RESUME,
    ZI_JOB_REVIEW,
    ZI_JOB_DUP_LIST,
    ZI_JOB_DUP_SCAN,
    ZI_JOB_DUP_POLL,
    ZI_JOB_TRIM_STATUS,
    ZI_JOB_TRIM_START,
    ZI_JOB_TRIM_PAUSE,
    ZI_JOB_TRIM_CONTINUE,
    ZI_JOB_CLEAN,
    ZI_JOB_DATE,
    ZI_JOB_REMOVE,
    ZI_JOB_RESTORE,
    ZI_JOB_PIN,
    ZI_JOB_PURGE,
    ZI_JOB_PURGE_ALL,
    ZI_JOB_PASS
};

enum {
    ZI_HIT_NONE = 0,
    ZI_HIT_INERT,
    ZI_HIT_RENAME,
    ZI_HIT_EVENT,
    ZI_HIT_PLAN,
    ZI_HIT_START,
    ZI_HIT_PAUSE,
    ZI_HIT_RESUME,
    ZI_HIT_CHOOSE,
    ZI_HIT_DATE,
    ZI_HIT_FIND,
    ZI_HIT_TRIM_START,
    ZI_HIT_TRIM_PAUSE,
    ZI_HIT_TRIM_CONTINUE,
    ZI_HIT_RESTORE,
    ZI_HIT_PIN,
    ZI_HIT_DELETE,
    ZI_HIT_DELETE_ALL
};

enum {
    ZI_PEND_NONE = 0,
    ZI_PEND_PLAN,
    ZI_PEND_START,
    ZI_PEND_PAUSE,
    ZI_PEND_RESUME
};

typedef struct ZiEvent {
    char uid[64];
    wchar_t name[180];
} ZiEvent;

typedef struct ZiLibItem {
    char uid[64];
    wchar_t path[MAX_PATH];
    wchar_t name[ZI_NAME];
} ZiLibItem;

typedef struct ZiLibJob {
    int kind;
    HWND hwnd;
    wchar_t library_root[MAX_PATH];
    wchar_t text[512];
    char uid[64];
    int pinned;
    int found;
    int prefer_needs;
    int count;
    ZiLibItem *items;
} ZiLibJob;

typedef struct ZiLibNote {
    int kind;
    int done;
    int reload;
    DWORD status;
    int moved;
    int stayed;
    int removed;
    int kept;
    wchar_t text[512];
    ZiFile *files;
    int file_count;
    int have_reorg;
    int reorg_safe;
    int reorg_pause;
    int move_n;
    int placed_n;
    int park_n;
    int leave_n;
    long long cache_bytes;
    wchar_t run_state[48];
    wchar_t phase[280];
    char recovery[64];
    int have_trim;
    int trim_safe;
    int trim_pause;
    int eligible;
    int trim_moved;
    int waiting;
    wchar_t trim_state[48];
    int have_scan;
    int scan_done;
    int scan_total;
    int scan_groups;
    wchar_t scan_state[32];
    wchar_t scan_phase[32];
    ZiEvent *events;
    int event_count;
    int clear_pending;
    /* Toolbar pass. 0 keeps the previous line, 1 applies text, 2 clears it. */
    int pass_code;
    LONG pass_serial;
} ZiLibNote;

typedef struct ZiHit {
    int id;
    int extra;
    int enabled;
    RECT rc;
    const wchar_t *label;
} ZiHit;

static volatile LONG g_alive = 1;
static volatile LONG g_job_busy;
static volatile LONG g_pass_busy;
static volatile LONG g_pass_serial;
static int g_modal;
static HWND g_event_edit;
static HWND g_prefix_edit;
static HWND g_date_edit;
static HFONT g_edit_font;

static ZiEvent g_events[ZI_LIB_CAP];
static int g_event_count;
static int g_event_scroll;
static RECT g_event_list;
static int g_event_row_h;

static int g_reorg_known;
static int g_reorg_safe;
static int g_reorg_pause;
static int g_move_n;
static int g_placed_n;
static int g_park_n;
static int g_leave_n;
static long long g_cache_bytes;
static wchar_t g_run_state[48];
static wchar_t g_phase[280];
static char g_recovery[64];
static wchar_t g_reorg_notice[512];
static int g_pending;
static wchar_t g_pending_prefix[280];

static int g_trim_known;
static int g_trim_safe;
static int g_trim_pause;
static int g_eligible;
static int g_trim_moved;
static int g_waiting;
static wchar_t g_trim_state[48];
static wchar_t g_dup_notice[512];
static int g_scan_running;
static wchar_t g_scan_line[180];

static ZiHit g_hits[32];
static int g_hit_count;

static HWND g_delete_edit;
static int g_delete_done;
static int g_delete_ok;
static wchar_t g_delete_text[64];

static void zi_note_free(ZiLibNote *note);
static void zi_start(ZiApp *app, ZiLibJob *job);
static int zi_job_busy(void);
static void zi_copy_notice(wchar_t *dest, size_t cap, const wchar_t *text);

static int zi_job_busy(void) {
    return g_job_busy != 0;
}

static void zi_copy_notice(wchar_t *dest, size_t cap, const wchar_t *text) {
    if (!dest || cap == 0) {
        return;
    }
    if (!text) {
        dest[0] = L'\0';
        return;
    }
    wcsncpy_s(dest, cap, text, _TRUNCATE);
}

static void zi_app_notice(ZiApp *app, const wchar_t *text) {
    zi_copy_notice(app->notice, 192, text);
}

static void zi_note_free(ZiLibNote *note) {
    int i;

    if (!note) {
        return;
    }
    if (note->files) {
        for (i = 0; i < note->file_count; i++) {
            if (note->files[i].thumb) {
                DeleteObject(note->files[i].thumb);
            }
        }
        free(note->files);
    }
    free(note->events);
    free(note);
}

static void zi_post(HWND hwnd, ZiLibNote *note) {
    int done;

    if (!note) {
        return;
    }
    done = note->done;
    if (!g_alive || !hwnd || !PostMessageW(hwnd, ZI_WM_LIBRARY, 0, (LPARAM)note)) {
        zi_note_free(note);
        if (done || !g_alive) {
            InterlockedExchange(&g_job_busy, 0);
        }
    }
}

static const char *zi_find_key(const char *json, const char *end, const char *key) {
    const char *p;
    int depth = 0;
    size_t klen;

    if (!json || !key) {
        return NULL;
    }
    if (!end) {
        end = json + strlen(json);
    }
    klen = strlen(key);
    p = json;
    while (p < end) {
        if (*p == '"') {
            const char *name = p + 1;
            const char *q = name;
            int esc = 0;

            while (q < end) {
                if (esc) {
                    esc = 0;
                    q++;
                    continue;
                }
                if (*q == '\\') {
                    esc = 1;
                    q++;
                    continue;
                }
                if (*q == '"') {
                    break;
                }
                q++;
            }
            if (q >= end) {
                return NULL;
            }
            if (depth == 1 && (size_t)(q - name) == klen && memcmp(name, key, klen) == 0) {
                const char *v = q + 1;

                while (v < end && (*v == ' ' || *v == '\t' || *v == '\n' || *v == '\r')) {
                    v++;
                }
                if (v < end && *v == ':') {
                    v++;
                    while (v < end && (*v == ' ' || *v == '\t' || *v == '\n' || *v == '\r')) {
                        v++;
                    }
                    return v;
                }
            }
            p = q + 1;
            continue;
        }
        if (*p == '{' || *p == '[') {
            depth++;
        } else if (*p == '}' || *p == ']') {
            depth--;
            if (depth < 0) {
                return NULL;
            }
        }
        p++;
    }
    return NULL;
}

static int zi_slice_value(const char *p, const char *end, const char **out_end) {
    if (!p || p >= end) {
        return 0;
    }
    if (*p == '"' ) {
        int esc = 0;
        p++;
        while (p < end) {
            if (esc) {
                esc = 0;
                p++;
                continue;
            }
            if (*p == '\\') {
                esc = 1;
                p++;
                continue;
            }
            if (*p == '"') {
                *out_end = p + 1;
                return 1;
            }
            p++;
        }
        return 0;
    }
    if (*p == '{' || *p == '[') {
        char open = *p;
        char close = (open == '{') ? '}' : ']';
        int depth = 0;
        int in_str = 0;
        int esc = 0;

        while (p < end) {
            if (in_str) {
                if (esc) {
                    esc = 0;
                } else if (*p == '\\') {
                    esc = 1;
                } else if (*p == '"') {
                    in_str = 0;
                }
                p++;
                continue;
            }
            if (*p == '"') {
                in_str = 1;
            } else if (*p == open) {
                depth++;
            } else if (*p == close) {
                depth--;
                if (depth == 0) {
                    *out_end = p + 1;
                    return 1;
                }
            }
            p++;
        }
        return 0;
    }
    while (p < end && *p != ',' && *p != '}' && *p != ']' && *p != ' ' && *p != '\n' && *p != '\r' && *p != '\t') {
        p++;
    }
    *out_end = p;
    return 1;
}

static unsigned zi_hex4(const char *s) {
    unsigned value = 0;
    int i;

    for (i = 0; i < 4; i++) {
        char c = s[i];
        unsigned digit;

        if (c >= '0' && c <= '9') {
            digit = (unsigned)(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            digit = (unsigned)(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            digit = (unsigned)(c - 'A' + 10);
        } else {
            return 0;
        }
        value = (value << 4) | digit;
    }
    return value;
}

static void zi_utf8_put(char *dest, size_t cap, size_t *n, unsigned cp) {
    if (cp < 0x80) {
        if (*n + 1 < cap) {
            dest[(*n)++] = (char)cp;
        }
        return;
    }
    if (cp < 0x800) {
        if (*n + 2 < cap) {
            dest[(*n)++] = (char)(0xC0 | (cp >> 6));
            dest[(*n)++] = (char)(0x80 | (cp & 0x3F));
        }
        return;
    }
    if (*n + 3 < cap) {
        dest[(*n)++] = (char)(0xE0 | (cp >> 12));
        dest[(*n)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        dest[(*n)++] = (char)(0x80 | (cp & 0x3F));
    }
}

static void zi_unescape(const char *s, const char *end, char *dest, size_t cap) {
    size_t n = 0;

    if (cap == 0) {
        return;
    }
    while (s < end && n + 1 < cap) {
        if (*s == '\\' && s + 1 < end) {
            s++;
            if (*s == 'u' && s + 4 < end) {
                zi_utf8_put(dest, cap, &n, zi_hex4(s + 1));
                s += 5;
                continue;
            }
            if (*s == 'n') {
                dest[n++] = '\n';
            } else if (*s == 'r') {
                dest[n++] = '\r';
            } else if (*s == 't') {
                dest[n++] = '\t';
            } else {
                dest[n++] = *s;
            }
            s++;
            continue;
        }
        dest[n++] = *s++;
    }
    dest[n] = 0;
}

static int zi_json_string(const char *json, const char *end, const char *key, char *dest, size_t cap) {
    const char *v = zi_find_key(json, end, key);
    const char *stop = NULL;

    dest[0] = 0;
    if (!v || *v != '"') {
        return 0;
    }
    if (!zi_slice_value(v, end ? end : v + strlen(v), &stop) || stop <= v + 1) {
        return 0;
    }
    zi_unescape(v + 1, stop - 1, dest, cap);
    return dest[0] != 0;
}

static int zi_json_bool(const char *json, const char *end, const char *key) {
    const char *v = zi_find_key(json, end, key);
    return v && strncmp(v, "true", 4) == 0;
}

static int zi_json_int(const char *json, const char *end, const char *key, int *out) {
    const char *v = zi_find_key(json, end, key);
    char *stop = NULL;
    long value;

    if (!v) {
        return 0;
    }
    value = strtol(v, &stop, 10);
    if (stop == v) {
        return 0;
    }
    *out = (int)value;
    return 1;
}

static int zi_json_i64(const char *json, const char *end, const char *key, long long *out) {
    const char *v = zi_find_key(json, end, key);
    char *stop = NULL;
    long long value;

    if (!v) {
        return 0;
    }
    value = strtoll(v, &stop, 10);
    if (stop == v) {
        return 0;
    }
    *out = value;
    return 1;
}

static int zi_json_error(const char *json, char *dest, size_t cap) {
    if (!json) {
        dest[0] = 0;
        return 0;
    }
    return zi_json_string(json, NULL, "error", dest, cap);
}

static int zi_each_object(const char *array, const char *end, int (*visit)(const char *, const char *, void *), void *ctx) {
    const char *p;
    int depth;
    int in_str;
    int esc;

    if (!array || *array != '[') {
        return 0;
    }
    if (!end) {
        end = array + strlen(array);
    }
    p = array + 1;
    while (p < end) {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == ',')) {
            p++;
        }
        if (p >= end || *p == ']') {
            break;
        }
        if (*p != '{') {
            return 0;
        }
        {
            const char *start = p;
            const char *stop = NULL;

            depth = 0;
            in_str = 0;
            esc = 0;
            while (p < end) {
                if (in_str) {
                    if (esc) {
                        esc = 0;
                    } else if (*p == '\\') {
                        esc = 1;
                    } else if (*p == '"') {
                        in_str = 0;
                    }
                    p++;
                    continue;
                }
                if (*p == '"') {
                    in_str = 1;
                } else if (*p == '{' || *p == '[') {
                    depth++;
                } else if (*p == '}' || *p == ']') {
                    depth--;
                    if (depth == 0) {
                        stop = p + 1;
                        p++;
                        break;
                    }
                }
                p++;
            }
            if (!stop) {
                return 0;
            }
            if (!visit(start, stop, ctx)) {
                return 1;
            }
        }
    }
    return 1;
}

static int zi_uid_ok(const char *uid) {
    size_t n = 0;

    if (!uid || !uid[0]) {
        return 0;
    }
    while (uid[n]) {
        char c = uid[n];

        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) {
            return 0;
        }
        n++;
        if (n >= 64) {
            return 0;
        }
    }
    return 1;
}

static int zi_escape(const char *in, char *out, size_t cap) {
    size_t n = 0;

    if (!in) {
        in = "";
    }
    while (*in) {
        unsigned char c = (unsigned char)*in++;
        char hex[8];

        if (c == '\\' || c == '"') {
            if (n + 2 >= cap) {
                return 0;
            }
            out[n++] = '\\';
            out[n++] = (char)c;
            continue;
        }
        if (c < 0x20) {
            sprintf_s(hex, sizeof hex, "\\u%04X", c);
            if (n + 6 >= cap) {
                return 0;
            }
            memcpy(out + n, hex, 6);
            n += 6;
            continue;
        }
        if (n + 1 >= cap) {
            return 0;
        }
        out[n++] = (char)c;
    }
    if (n >= cap) {
        return 0;
    }
    out[n] = 0;
    return 1;
}

static int zi_url_encode(const char *in, char *out, size_t cap) {
    static const char hex[] = "0123456789ABCDEF";
    size_t n = 0;

    while (*in) {
        unsigned char c = (unsigned char)*in++;
        int plain = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')
            || c == '/' || c == '-' || c == '_' || c == '.' || c == '~';

        if (plain) {
            if (n + 1 >= cap) {
                return 0;
            }
            out[n++] = (char)c;
        } else {
            if (n + 3 >= cap) {
                return 0;
            }
            out[n++] = '%';
            out[n++] = hex[c >> 4];
            out[n++] = hex[c & 15];
        }
    }
    if (n >= cap) {
        return 0;
    }
    out[n] = 0;
    return 1;
}

static int zi_wide_utf8(const wchar_t *text, char *dest, size_t cap) {
    if (!text || !dest || cap == 0) {
        return 0;
    }
    if (WideCharToMultiByte(CP_UTF8, 0, text, -1, dest, (int)cap, NULL, NULL) <= 0) {
        dest[0] = 0;
        return 0;
    }
    return 1;
}

static void zi_utf8_wide(const char *text, wchar_t *dest, size_t cap) {
    if (!dest || cap == 0) {
        return;
    }
    dest[0] = L'\0';
    if (!text || !text[0]) {
        return;
    }
    if (MultiByteToWideChar(CP_UTF8, 0, text, -1, dest, (int)cap) <= 0) {
        dest[0] = L'\0';
    }
}

static int zi_call(const wchar_t *method, const wchar_t *path, const char *body, DWORD timeout, DWORD *status, char **out) {
    DWORD code = 0;
    int ok = zi_helper_call(method, path, body, timeout, &code, out);

    if (status) {
        *status = code;
    }
    return ok;
}

static int zi_fault_kind(DWORD status) {
    DWORD err = GetLastError();

    if (status == 401 || err == ERROR_NOT_AUTHENTICATED) {
        return 3;
    }
    if (err == ERROR_WINHTTP_SECURE_FAILURE) {
        return 2;
    }
    if (err == ERROR_WINHTTP_TIMEOUT) {
        return 1;
    }
    return 4;
}

static void zi_status_fault(int fault, wchar_t *dest, size_t cap) {
    const wchar_t *text = L"The status check failed. The lines above are the last answer. The next check is in 2 seconds.";

    if (fault == 1) {
        text = L"The status check waited 10 seconds and got no answer. The lines above are the last answer. The next check is in 2 seconds.";
    } else if (fault == 2) {
        text = L"The helper certificate changed, so the status check failed. Pair again.";
    } else if (fault == 3) {
        text = L"The helper refused the token, so the status check failed. Pair again.";
    } else if (fault == 4) {
        text = L"The helper did not answer the status check. The lines above are the last answer. The next check is in 2 seconds.";
    }
    zi_copy_notice(dest, cap, text);
}

static void zi_action_fault(int fault, const wchar_t *action, wchar_t *dest, size_t cap) {
    if (fault == 3) {
        swprintf_s(dest, cap, L"The helper refused the token, so %s did not run. Pair again.", action);
        return;
    }
    if (fault == 2) {
        swprintf_s(dest, cap, L"The helper certificate changed, so %s did not run. Pair again.", action);
        return;
    }
    swprintf_s(dest, cap,
               L"The helper did not answer, so %s was not confirmed. Nothing new was copied. The panel checks every 2 seconds.",
               action);
}

static void zi_plan_failure(const char *code, wchar_t *dest, size_t cap) {
    wchar_t wide[80];

    if (!code || !code[0]) {
        zi_copy_notice(dest, cap, L"The plan was refused and the helper did not say why. Nothing was copied.");
        return;
    }
    if (strcmp(code, "prefix") == 0) {
        zi_copy_notice(dest, cap, L"The plan prefix must be Photos or a folder inside it.");
        return;
    }
    if (strcmp(code, "busy") == 0) {
        zi_copy_notice(dest, cap, L"The plan was refused because reorganize is already running.");
        return;
    }
    if (strcmp(code, "catalog") == 0) {
        zi_copy_notice(dest, cap, L"The plan was not saved because the catalog could not be updated.");
        return;
    }
    if (strcmp(code, "library_unavailable") == 0) {
        zi_copy_notice(dest, cap, L"The plan was not saved because the helper has no catalog open.");
        return;
    }
    if (strcmp(code, "method") == 0) {
        zi_copy_notice(dest, cap, L"The plan was refused because the helper did not accept the request.");
        return;
    }
    if (strcmp(code, "trim") == 0) {
        zi_copy_notice(dest, cap, L"The plan was refused because Trim is still running.");
        return;
    }
    zi_utf8_wide(code, wide, 80);
    swprintf_s(dest, cap, L"The plan was refused (%s). Nothing was copied.", wide);
}

static void zi_action_failure(const char *code, const wchar_t *fallback, wchar_t *dest, size_t cap) {
    wchar_t wide[80];
    wchar_t temp[512];

    if (!code) {
        code = "";
    }
    if (strcmp(code, "trim") == 0) {
        zi_copy_notice(dest, cap, L"Refused because Trim is still running. Reorganize waits until Trim is idle.");
        return;
    }
    if (strcmp(code, "plan") == 0) {
        zi_copy_notice(dest, cap, L"Refused because no Photos folder is saved. Press Plan first.");
        return;
    }
    if (strcmp(code, "busy") == 0) {
        zi_copy_notice(dest, cap, L"Refused because reorganize is already running.");
        return;
    }
    if (strcmp(code, "catalog") == 0) {
        zi_copy_notice(dest, cap, L"Refused because the catalog could not be updated.");
        return;
    }
    if (strcmp(code, "library_unavailable") == 0) {
        zi_copy_notice(dest, cap, L"Refused because the helper has no catalog open.");
        return;
    }
    if (strcmp(code, "prefix") == 0) {
        zi_copy_notice(dest, cap, L"The plan prefix must be Photos or a folder inside it.");
        return;
    }
    if (strcmp(code, "method") == 0) {
        zi_copy_notice(dest, cap, L"Refused because the helper did not accept the request.");
        return;
    }
    zi_utf8_wide(code, wide, 80);
    if (swprintf_s(temp, 512, L"%s The helper said %s.", fallback, wide) < 0) {
        zi_copy_notice(dest, cap, fallback);
        return;
    }
    zi_copy_notice(dest, cap, temp);
}

static void zi_take_reorg(const char *body, ZiLibNote *note) {
    char recovery[64];
    char state[48];
    char phase[240];

    if (!body) {
        return;
    }
    note->have_reorg = 1;
    note->reorg_safe = zi_json_bool(body, NULL, "reorgSafeToReboot");
    note->reorg_pause = zi_json_bool(body, NULL, "pauseRequested");
    zi_json_int(body, NULL, "move", &note->move_n);
    zi_json_int(body, NULL, "alreadyPlaced", &note->placed_n);
    zi_json_int(body, NULL, "park", &note->park_n);
    zi_json_int(body, NULL, "leave", &note->leave_n);
    zi_json_i64(body, NULL, "cacheBytes", &note->cache_bytes);
    state[0] = 0;
    phase[0] = 0;
    recovery[0] = 0;
    zi_json_string(body, NULL, "runState", state, sizeof state);
    zi_json_string(body, NULL, "phase", phase, sizeof phase);
    zi_json_string(body, NULL, "lastRecovery", recovery, sizeof recovery);
    zi_utf8_wide(state, note->run_state, 48);
    zi_utf8_wide(phase, note->phase, 280);
    strncpy_s(note->recovery, 64, recovery, _TRUNCATE);
}

static void zi_take_trim(const char *body, ZiLibNote *note) {
    char state[48];

    if (!body) {
        return;
    }
    note->have_trim = 1;
    note->trim_safe = zi_json_bool(body, NULL, "safeToReboot");
    note->trim_pause = zi_json_bool(body, NULL, "pauseRequested");
    zi_json_int(body, NULL, "eligible", &note->eligible);
    zi_json_int(body, NULL, "moved", &note->trim_moved);
    zi_json_int(body, NULL, "waiting", &note->waiting);
    state[0] = 0;
    zi_json_string(body, NULL, "runState", state, sizeof state);
    zi_utf8_wide(state, note->trim_state, 48);
}

static void zi_take_scan(const char *body, ZiLibNote *note) {
    char state[32];
    char phase[32];

    if (!body) {
        return;
    }
    note->have_scan = 1;
    zi_json_int(body, NULL, "done", &note->scan_done);
    zi_json_int(body, NULL, "total", &note->scan_total);
    zi_json_int(body, NULL, "groups", &note->scan_groups);
    state[0] = 0;
    phase[0] = 0;
    zi_json_string(body, NULL, "runState", state, sizeof state);
    zi_json_string(body, NULL, "phase", phase, sizeof phase);
    zi_utf8_wide(state, note->scan_state, 32);
    zi_utf8_wide(phase, note->scan_phase, 32);
}

static int zi_focus(const char *prefix) {
    char body[160];
    char escaped[80];
    DWORD status = 0;
    char *out = NULL;

    if (!zi_escape(prefix, escaped, sizeof escaped)) {
        return 0;
    }
    sprintf_s(body, sizeof body, "{\"prefix\":\"%s\"}", escaped);
    zi_call(L"POST", L"/v1/focus", body, 5000, &status, &out);
    free(out);
    return 1;
}

static int zi_video_name(const char *rel) {
    const char *dot = rel ? strrchr(rel, '.') : NULL;
    if (!dot || !dot[1]) {
        return 0;
    }
    dot++;
    return _stricmp(dot, "mov") == 0 || _stricmp(dot, "mp4") == 0 || _stricmp(dot, "m4v") == 0
        || _stricmp(dot, "avi") == 0 || _stricmp(dot, "mkv") == 0 || _stricmp(dot, "mpg") == 0
        || _stricmp(dot, "wmv") == 0 || _stricmp(dot, "webm") == 0;
}

static void zi_base_name(const char *rel, wchar_t *dest, size_t cap) {
    const char *slash = rel ? strrchr(rel, '/') : NULL;
    const char *base = slash ? slash + 1 : rel;

    zi_utf8_wide(base ? base : "", dest, cap);
    if (!dest[0]) {
        wcscpy_s(dest, cap, L"Photograph");
    }
}

static void zi_unc_path(const wchar_t *root, const char *rel, wchar_t *dest, size_t cap) {
    wchar_t wide[MAX_PATH];
    wchar_t *p;

    dest[0] = L'\0';
    if (!root || !root[0] || !rel || !rel[0]) {
        return;
    }
    if (MultiByteToWideChar(CP_UTF8, 0, rel, -1, wide, MAX_PATH) <= 0) {
        return;
    }
    for (p = wide; *p; p++) {
        if (*p == L'/') {
            *p = L'\\';
        }
    }
    if (swprintf_s(dest, cap, L"%s\\%s", root, wide) < 0) {
        dest[0] = L'\0';
    }
}

static int zi_parked_rel(const char *rel, const char *placement) {
    if (placement && (strcmp(placement, "removed") == 0 || strcmp(placement, "purged") == 0 || strcmp(placement, "duplicates") == 0)) {
        return 1;
    }
    if (!rel) {
        return 0;
    }
    if (strcmp(rel, "Photos/_Removed") == 0 || strncmp(rel, "Photos/_Removed/", 16) == 0) {
        return 1;
    }
    if (strcmp(rel, "Photos/_Duplicates") == 0 || strncmp(rel, "Photos/_Duplicates/", 19) == 0) {
        return 1;
    }
    return 0;
}

static void zi_fill_file(ZiFile *file, const wchar_t *root, const char *uid, const char *rel, const char *media,
                         const char *placement, int keeper, int pinned, const wchar_t *caption) {
    ZeroMemory(file, sizeof *file);
    if (zi_uid_ok(uid)) {
        strncpy_s(file->uid, 64, uid, _TRUNCATE);
    }
    file->pinned = pinned;
    file->keeper = keeper;
    file->parked = zi_parked_rel(rel, placement);
    file->video = (media && _stricmp(media, "video") == 0) || zi_video_name(rel);
    if (caption && caption[0]) {
        wcsncpy_s(file->name, ZI_NAME, caption, _TRUNCATE);
    } else {
        zi_base_name(rel, file->name, ZI_NAME);
    }
    if (keeper) {
        wchar_t marked[ZI_NAME];

        if (swprintf_s(marked, ZI_NAME, L"Keeper \u00B7 %s", file->name) >= 0) {
            wcscpy_s(file->name, ZI_NAME, marked);
        }
    }
    zi_unc_path(root, rel, file->path, MAX_PATH);
}

typedef struct ZiRowBuild {
    const wchar_t *root;
    const char *keeper_uid;
    ZiFile *files;
    int count;
    int capped;
} ZiRowBuild;

static int zi_visit_asset(const char *start, const char *end, void *ctx) {
    ZiRowBuild *build = (ZiRowBuild *)ctx;
    char uid[64];
    char rel[700];
    char media[32];
    char placement[32];
    int keeper;
    int pinned;
    wchar_t caption[ZI_NAME];

    if (build->count >= ZI_MAX_FILES) {
        build->capped = 1;
        return 0;
    }
    uid[0] = rel[0] = media[0] = placement[0] = 0;
    zi_json_string(start, end, "uid", uid, sizeof uid);
    zi_json_string(start, end, "relPath", rel, sizeof rel);
    zi_json_string(start, end, "mediaClass", media, sizeof media);
    zi_json_string(start, end, "placement", placement, sizeof placement);
    keeper = zi_json_bool(start, end, "isKeeper");
    if (!keeper && build->keeper_uid && uid[0] && strcmp(uid, build->keeper_uid) == 0) {
        keeper = 1;
    }
    if (uid[0]) {
        int seen;

        for (seen = 0; seen < build->count; seen++) {
            if (strcmp(build->files[seen].uid, uid) == 0) {
                if (keeper) {
                    build->files[seen].keeper = 1;
                }
                return 1;
            }
        }
    }
    pinned = zi_json_bool(start, end, "pinned");
    caption[0] = L'\0';
    zi_fill_file(&build->files[build->count], build->root, uid, rel, media, placement, keeper, pinned, caption);
    build->count++;
    return 1;
}

static int zi_visit_group(const char *start, const char *end, void *ctx) {
    ZiRowBuild *build = (ZiRowBuild *)ctx;
    char keeper_uid[64];
    const char *members;
    const char *keeper;

    if (build->count >= ZI_MAX_FILES) {
        build->capped = 1;
        return 0;
    }
    keeper_uid[0] = 0;
    keeper = zi_find_key(start, end, "keeper");
    if (keeper && *keeper == '{') {
        const char *keeper_end = NULL;
        if (zi_slice_value(keeper, end, &keeper_end)) {
            zi_json_string(keeper, keeper_end, "uid", keeper_uid, sizeof keeper_uid);
            build->keeper_uid = keeper_uid;
            zi_visit_asset(keeper, keeper_end, build);
            build->keeper_uid = NULL;
        }
    }
    members = zi_find_key(start, end, "members");
    if (members && *members == '[') {
        build->keeper_uid = keeper_uid;
        zi_each_object(members, end, zi_visit_asset, build);
        build->keeper_uid = NULL;
    }
    return build->count < ZI_MAX_FILES;
}

static ZiFile *zi_rows_from(const char *body, const wchar_t *root, int groups, int *count) {
    ZiRowBuild build;
    const char *array;

    *count = 0;
    build.root = root;
    build.keeper_uid = NULL;
    build.count = 0;
    build.capped = 0;
    build.files = (ZiFile *)calloc(ZI_MAX_FILES, sizeof(ZiFile));
    if (!build.files || !body) {
        free(build.files);
        return NULL;
    }
    array = zi_find_key(body, NULL, groups ? "groups" : "rows");
    if (!array || *array != '[') {
        free(build.files);
        return NULL;
    }
    if (groups) {
        zi_each_object(array, NULL, zi_visit_group, &build);
    } else {
        zi_each_object(array, NULL, zi_visit_asset, &build);
    }
    *count = build.count;
    if (build.count == 0) {
        free(build.files);
        return NULL;
    }
    return build.files;
}

typedef struct ZiMatch {
    const char *rel;
    char uid[64];
    int found;
} ZiMatch;

static int zi_visit_match(const char *start, const char *end, void *ctx) {
    ZiMatch *match = (ZiMatch *)ctx;
    char rel[700];
    char uid[64];

    rel[0] = uid[0] = 0;
    zi_json_string(start, end, "relPath", rel, sizeof rel);
    if (strcmp(rel, match->rel) != 0) {
        return 1;
    }
    zi_json_string(start, end, "uid", uid, sizeof uid);
    if (!zi_uid_ok(uid)) {
        return 1;
    }
    strncpy_s(match->uid, 64, uid, _TRUNCATE);
    match->found = 1;
    return 0;
}

static int zi_lookup_uid(const wchar_t *root, const wchar_t *path, char *uid, size_t cap) {
    char rel[1024];
    char encoded[1400];
    char query[1600];
    wchar_t wide[1700];
    char *body = NULL;
    DWORD status = 0;
    const char *rows;
    ZiMatch match;
    size_t root_len;
    const wchar_t *tail;
    wchar_t folded[MAX_PATH];
    size_t i;
    size_t j;

    uid[0] = 0;
    if (!root || !root[0] || !path) {
        return 0;
    }
    root_len = wcslen(root);
    while (root_len > 0 && (root[root_len - 1] == L'\\' || root[root_len - 1] == L'/')) {
        root_len--;
    }
    if (_wcsnicmp(path, root, root_len) != 0) {
        return 0;
    }
    tail = path + root_len;
    if (*tail == L'\\' || *tail == L'/') {
        tail++;
    }
    j = 0;
    for (i = 0; tail[i] && j + 1 < MAX_PATH; i++) {
        folded[j++] = (tail[i] == L'\\') ? L'/' : tail[i];
    }
    folded[j] = L'\0';
    if (!folded[0] || !zi_wide_utf8(folded, rel, sizeof rel)) {
        return 0;
    }
    if (!zi_url_encode(rel, encoded, sizeof encoded)) {
        return 0;
    }
    sprintf_s(query, sizeof query, "/v1/assets?q=%s&limit=200", encoded);
    if (MultiByteToWideChar(CP_UTF8, 0, query, -1, wide, 1700) <= 0) {
        return 0;
    }
    if (!zi_call(L"GET", wide, NULL, 10000, &status, &body) || status != 200 || !body) {
        free(body);
        return 0;
    }
    rows = zi_find_key(body, NULL, "rows");
    ZeroMemory(&match, sizeof match);
    match.rel = rel;
    if (rows) {
        zi_each_object(rows, NULL, zi_visit_match, &match);
    }
    free(body);
    if (!match.found || cap < 2) {
        return 0;
    }
    strncpy_s(uid, cap, match.uid, _TRUNCATE);
    return 1;
}

static int zi_asset_post(const char *uid, const char *suffix, const char *body, DWORD timeout, DWORD *status, char **out) {
    char path[180];
    wchar_t wide[180];

    if (!zi_uid_ok(uid)) {
        if (status) {
            *status = 0;
        }
        return 0;
    }
    sprintf_s(path, sizeof path, "/v1/assets/%s%s", uid, suffix);
    if (MultiByteToWideChar(CP_UTF8, 0, path, -1, wide, 180) <= 0) {
        return 0;
    }
    return zi_call(L"POST", wide, body, timeout, status, out);
}

static void zi_commas(int value, wchar_t *dest, size_t cap) {
    wchar_t raw[32];
    wchar_t out[48];
    int n;
    int i;
    int j = 0;

    if (value < 0) {
        value = 0;
    }
    swprintf_s(raw, 32, L"%d", value);
    n = (int)wcslen(raw);
    for (i = 0; i < n && j + 2 < 48; i++) {
        if (i > 0 && (n - i) % 3 == 0) {
            out[j++] = L',';
        }
        out[j++] = raw[i];
    }
    out[j] = L'\0';
    wcscpy_s(dest, cap, out);
}

static void zi_find_text(int groups, int list_count, int recorded, wchar_t *dest, size_t cap) {
    wchar_t a[32];
    wchar_t b[32];
    wchar_t temp[512];

    if (recorded > list_count && list_count >= 0) {
        zi_commas(recorded, a, 32);
        zi_commas(list_count, b, 32);
        swprintf_s(temp, 512, L"Found %s matching groups. This list shows the first %s.", a, b);
        zi_copy_notice(dest, cap, temp);
        return;
    }
    if (groups > 0) {
        zi_commas(groups, a, 32);
        swprintf_s(temp, 512, L"Found %s matching groups. Start gathers the extra copies into Photos/_Duplicates.", a);
        zi_copy_notice(dest, cap, temp);
        return;
    }
    if (g_eligible > 0) {
        zi_commas(g_eligible, a, 32);
        swprintf_s(temp, 512, L"No copies are in this list yet. %s groups are ready. Start gathers the extra copies into Photos/_Duplicates.", a);
        zi_copy_notice(dest, cap, temp);
        return;
    }
    if (g_waiting > 0) {
        zi_commas(g_waiting, a, 32);
        swprintf_s(temp, 512, L"No group is ready to gather. %s groups are still waiting for a sharpness score.", a);
        zi_copy_notice(dest, cap, temp);
        return;
    }
    zi_copy_notice(dest, cap, L"No matching groups are recorded yet. Matches are added as previews are made.");
}

static void zi_scan_line(const ZiLibNote *note, wchar_t *dest, size_t cap) {
    wchar_t done[32];
    wchar_t total[32];
    char phase[32];

    dest[0] = L'\0';
    if (!note->have_scan || wcscmp(note->scan_state, L"running") != 0) {
        return;
    }
    WideCharToMultiByte(CP_UTF8, 0, note->scan_phase, -1, phase, 32, NULL, NULL);
    if (note->scan_total <= 0) {
        if (strcmp(phase, "comparing") == 0) {
            zi_copy_notice(dest, cap, L"Preparing to compare photos.");
        } else if (strcmp(phase, "saving") == 0) {
            zi_copy_notice(dest, cap, L"Saving groups.");
        } else {
            zi_copy_notice(dest, cap, L"Checking previews for pictures that still need a fingerprint.");
        }
        return;
    }
    zi_commas(note->scan_done, done, 32);
    zi_commas(note->scan_total, total, 32);
    if (strcmp(phase, "comparing") == 0) {
        swprintf_s(dest, cap, L"Comparing photos, %s of %s.", done, total);
    } else if (strcmp(phase, "saving") == 0) {
        swprintf_s(dest, cap, L"Saving groups, %s of %s.", done, total);
    } else {
        swprintf_s(dest, cap, L"Hashing previews, %s of %s.", done, total);
    }
}

static int zi_load_events(const char *body, ZiLibNote *note) {
    const char *rows = body ? zi_find_key(body, NULL, "rows") : NULL;
    ZiEvent *events;
    int count = 0;

    if (!rows || *rows != '[') {
        return 0;
    }
    events = (ZiEvent *)calloc(ZI_LIB_CAP, sizeof(ZiEvent));
    if (!events) {
        return 0;
    }
    {
        const char *p = rows + 1;
        const char *end = rows + strlen(rows);
        while (p < end && count < ZI_LIB_CAP) {
            const char *stop = NULL;
            char uid[64];
            char name[240];

            while (p < end && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t' || *p == ',')) {
                p++;
            }
            if (p >= end || *p == ']') {
                break;
            }
            if (*p != '{' || !zi_slice_value(p, end, &stop)) {
                break;
            }
            uid[0] = name[0] = 0;
            zi_json_string(p, stop, "uid", uid, sizeof uid);
            zi_json_string(p, stop, "name", name, sizeof name);
            if (zi_uid_ok(uid)) {
                strncpy_s(events[count].uid, 64, uid, _TRUNCATE);
                zi_utf8_wide(name, events[count].name, 180);
                count++;
            }
            p = stop;
        }
    }
    note->events = events;
    note->event_count = count;
    return 1;
}

static void zi_attach_dup(ZiLibNote *note, const wchar_t *root, int found) {
    char *body = NULL;
    DWORD status = 0;
    wchar_t path[64];

    wcscpy_s(path, 64, found ? L"/v1/duplicates?limit=200&found=1" : L"/v1/duplicates?limit=200");
    if (!zi_call(L"GET", path, NULL, 10000, &status, &body) || status != 200) {
        free(body);
        return;
    }
    note->files = zi_rows_from(body, root, 1, &note->file_count);
    note->reload = 1;
    free(body);
}

static void zi_finish_scan(ZiLibNote *note, const wchar_t *root, int poll) {
    if (wcscmp(note->scan_state, L"completed") == 0) {
        int recorded = note->scan_groups;

        zi_attach_dup(note, root, 1);
        zi_find_text(recorded > 0 ? recorded : note->file_count, note->file_count, recorded, note->text, 512);
        return;
    }
    if (wcscmp(note->scan_state, L"failed") == 0) {
        wchar_t a[32];
        wchar_t b[32];

        if (note->scan_total > 0) {
            zi_commas(note->scan_done, a, 32);
            zi_commas(note->scan_total, b, 32);
            swprintf_s(note->text, 512, L"The scan stopped at %s of %s.", a, b);
        } else {
            zi_copy_notice(note->text, 512, L"The scan stopped before it finished.");
        }
        return;
    }
    if (poll && wcscmp(note->scan_state, L"running") != 0) {
        zi_copy_notice(note->text, 512, L"The scan ended before it finished.");
    }
}

static void zi_progress(HWND hwnd, int kind, const wchar_t *text) {
    ZiLibNote *note = (ZiLibNote *)calloc(1, sizeof *note);

    if (!note) {
        return;
    }
    note->kind = kind;
    note->done = 0;
    zi_copy_notice(note->text, 512, text);
    zi_post(hwnd, note);
}

static int zi_batch(ZiLibJob *job, const char *suffix, const char *body, const wchar_t *moving, ZiLibNote *note) {
    int i;
    int moved = 0;
    int stayed = 0;

    for (i = 0; i < job->count; i++) {
        char uid[64];
        char *reply = NULL;
        DWORD status = 0;
        char err[64];
        wchar_t line[192];
        int posted;

        if (!g_alive) {
            stayed += job->count - i;
            break;
        }
        swprintf_s(line, 192, moving, i + 1, job->count);
        zi_progress(job->hwnd, job->kind, line);
        uid[0] = 0;
        if (zi_uid_ok(job->items[i].uid)) {
            strncpy_s(uid, 64, job->items[i].uid, _TRUNCATE);
        } else if (!zi_lookup_uid(job->library_root, job->items[i].path, uid, sizeof uid)) {
            stayed++;
            continue;
        }
        posted = zi_asset_post(uid, suffix, body, 10000, &status, &reply);
        err[0] = 0;
        if (reply) {
            zi_json_error(reply, err, sizeof err);
        }
        free(reply);
        if (!posted) {
            stayed += job->count - i;
            break;
        }
        if (status == 200 && !err[0]) {
            moved++;
        } else {
            stayed++;
        }
    }
    note->moved = moved;
    note->stayed = stayed;
    return 1;
}

static void zi_finish_date(ZiLibNote *note, int total) {
    wchar_t temp[256];

    if (note->stayed == 0) {
        if (note->moved == 1) {
            zi_copy_notice(note->text, 512, L"Moved to its date folder.");
        } else {
            swprintf_s(temp, 256, L"Moved %d files to their date folders.", note->moved);
            zi_copy_notice(note->text, 512, temp);
        }
        return;
    }
    if (note->moved == 0) {
        zi_copy_notice(note->text, 512, total == 1 ? L"The date was not saved." : L"The dates were not saved.");
        return;
    }
    swprintf_s(temp, 256, L"%d moved to their date folders. %d stayed.", note->moved, note->stayed);
    zi_copy_notice(note->text, 512, temp);
}

static void zi_finish_remove(ZiLibNote *note, int total) {
    wchar_t temp[256];

    if (note->stayed == 0) {
        if (note->moved == 1) {
            zi_copy_notice(note->text, 512, L"Moved to Photos/_Removed.");
        } else {
            swprintf_s(temp, 256, L"Moved %d files to Photos/_Removed.", note->moved);
            zi_copy_notice(note->text, 512, temp);
        }
        return;
    }
    if (note->moved == 0) {
        zi_copy_notice(note->text, 512, total == 1 ? L"The file stayed in the library." : L"The files stayed in the library.");
        return;
    }
    swprintf_s(temp, 256, L"%d moved to Photos/_Removed. %d stayed in the library.", note->moved, note->stayed);
    zi_copy_notice(note->text, 512, temp);
}

static void zi_clean_text(int removed, int kept, wchar_t *dest, size_t cap) {
    wchar_t temp[256];

    if (removed == 0 && kept == 0) {
        zi_copy_notice(dest, cap, L"This folder has no leftover records.");
        return;
    }
    if (kept == 0) {
        if (removed == 1) {
            zi_copy_notice(dest, cap, L"Removed 1 leftover record.");
        } else {
            swprintf_s(temp, 256, L"Removed %d leftover records.", removed);
            zi_copy_notice(dest, cap, temp);
        }
        return;
    }
    if (removed == 0) {
        if (kept == 1) {
            zi_copy_notice(dest, cap, L"1 record was left in place.");
        } else {
            swprintf_s(temp, 256, L"%d records were left in place.", kept);
            zi_copy_notice(dest, cap, temp);
        }
        return;
    }
    if (removed == 1 && kept == 1) {
        zi_copy_notice(dest, cap, L"Removed 1 leftover record. 1 record was left in place.");
        return;
    }
    if (removed == 1) {
        swprintf_s(temp, 256, L"Removed 1 leftover record. %d records were left in place.", kept);
    } else if (kept == 1) {
        swprintf_s(temp, 256, L"Removed %d leftover records. 1 record was left in place.", removed);
    } else {
        swprintf_s(temp, 256, L"Removed %d leftover records. %d records were left in place.", removed, kept);
    }
    zi_copy_notice(dest, cap, temp);
}

static int zi_leap(int year) {
    return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

static int zi_calendar_day(int year, int month, int day) {
    static const int days[13] = { 0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };

    if (year < 1 || month < 1 || month > 12 || day < 1) {
        return 0;
    }
    if (month == 2 && zi_leap(year)) {
        return day <= 29;
    }
    return day <= days[month];
}

static int zi_digits(const wchar_t *text, int n) {
    int i;

    for (i = 0; i < n; i++) {
        if (text[i] < L'0' || text[i] > L'9') {
            return 0;
        }
    }
    return 1;
}

/* Four digits stay a year. A real day becomes midnight UTC. A real timestamp is sent as typed. */
static int zi_capture_request(const wchar_t *text, char *dest, size_t cap) {
    wchar_t buf[80];
    int year;
    int month;
    int day;
    int hour;
    int minute;
    int second;
    const wchar_t *t;

    dest[0] = 0;
    if (!text) {
        return 0;
    }
    wcsncpy_s(buf, 80, text, _TRUNCATE);
    while (buf[0] == L' ' || buf[0] == L'\t') {
        memmove(buf, buf + 1, (wcslen(buf) + 1) * sizeof(wchar_t));
    }
    {
        size_t n = wcslen(buf);
        while (n > 0 && (buf[n - 1] == L' ' || buf[n - 1] == L'\t')) {
            buf[--n] = L'\0';
        }
    }
    if (wcslen(buf) == 4 && zi_digits(buf, 4)) {
        return zi_wide_utf8(buf, dest, cap);
    }
    if (wcslen(buf) == 10 && buf[4] == L'-' && buf[7] == L'-' && zi_digits(buf, 4) && zi_digits(buf + 5, 2) && zi_digits(buf + 8, 2)) {
        year = _wtoi(buf);
        month = _wtoi(buf + 5);
        day = _wtoi(buf + 8);
        if (!zi_calendar_day(year, month, day)) {
            return 0;
        }
        if (swprintf_s(buf, 80, L"%04d-%02d-%02dT00:00:00Z", year, month, day) < 0) {
            return 0;
        }
        return zi_wide_utf8(buf, dest, cap);
    }
    t = wcschr(buf, L'T');
    if (!t || (t - buf) != 10 || buf[4] != L'-' || buf[7] != L'-') {
        return 0;
    }
    if (!zi_digits(buf, 4) || !zi_digits(buf + 5, 2) || !zi_digits(buf + 8, 2)) {
        return 0;
    }
    year = _wtoi(buf);
    month = _wtoi(buf + 5);
    day = _wtoi(buf + 8);
    if (!zi_calendar_day(year, month, day) || wcslen(t + 1) < 8 || t[3] != L':' || t[6] != L':') {
        return 0;
    }
    if (!zi_digits(t + 1, 2) || !zi_digits(t + 4, 2) || !zi_digits(t + 7, 2)) {
        return 0;
    }
    hour = _wtoi(t + 1);
    minute = _wtoi(t + 4);
    second = _wtoi(t + 7);
    if (hour > 23 || minute > 59 || second > 60) {
        return 0;
    }
    return zi_wide_utf8(buf, dest, cap);
}

static void zi_reorg_follow(const wchar_t *path, const char *body, DWORD timeout, const wchar_t *idle, int plan, ZiLibNote *note) {
    char *reply = NULL;
    DWORD status = 0;
    char err[64];
    int ok = zi_call(L"POST", path, body, timeout, &status, &reply);

    err[0] = 0;
    if (!ok) {
        int fault = zi_fault_kind(status);
        if (fault == 1 && plan) {
            zi_copy_notice(note->text, 512, L"The plan request waited 10 seconds. The status line says whether the count is running. Nothing has been copied.");
        } else if (fault == 1 && wcscmp(path, L"/v1/reorg/start") == 0) {
            zi_copy_notice(note->text, 512, L"The Mac stopped waiting after 10 seconds. Start may still be reaching the helper. This panel checks every 2 seconds.");
        } else if (fault == 1 && wcscmp(path, L"/v1/reorg/pause") == 0) {
            zi_copy_notice(note->text, 512, L"The Mac stopped waiting after 10 seconds. Pause may still be set. This panel checks every 2 seconds.");
        } else if (fault == 1 && wcscmp(path, L"/v1/reorg/resume") == 0) {
            zi_copy_notice(note->text, 512, L"The Mac stopped waiting after 10 seconds. Resume may still be reaching the helper. This panel checks every 2 seconds.");
        } else if (plan) {
            zi_action_fault(fault, L"the plan", note->text, 512);
        } else {
            zi_action_fault(fault, idle, note->text, 512);
        }
    } else if (reply && zi_json_error(reply, err, sizeof err)) {
        if (plan) {
            zi_plan_failure(err, note->text, 512);
        } else {
            zi_action_failure(err, idle, note->text, 512);
        }
    } else if (status == 200 && reply && zi_find_key(reply, NULL, "runState")) {
        zi_take_reorg(reply, note);
    } else if (status == 200 && plan && reply) {
        note->text[0] = L'\0';
    } else if (status == 200) {
        zi_copy_notice(note->text, 512, idle);
    } else {
        if (plan) {
            zi_plan_failure(err, note->text, 512);
        } else {
            zi_action_failure(err[0] ? err : "method", idle, note->text, 512);
        }
    }
    free(reply);
    if (plan || note->text[0]) {
        char *status_body = NULL;
        DWORD status_code = 0;
        if (zi_call(L"GET", L"/v1/reorg/status", NULL, 10000, &status_code, &status_body) && status_code == 200) {
            zi_take_reorg(status_body, note);
        }
        free(status_body);
    }
}

static DWORD WINAPI zi_library_thread(LPVOID param) {
    ZiLibJob *job = (ZiLibJob *)param;
    ZiLibNote *note = (ZiLibNote *)calloc(1, sizeof *note);
    char *body = NULL;
    DWORD status = 0;

    if (!job || !note) {
        free(note);
        if (job) {
            free(job->items);
            free(job);
        }
        InterlockedExchange(&g_job_busy, 0);
        return 0;
    }
    note->kind = job->kind;
    note->done = 1;
    switch (job->kind) {
    case ZI_JOB_EVENTS:
        if (!zi_call(L"GET", L"/v1/events", NULL, 10000, &status, &body) || status != 200) {
            note->text[0] = L'\0';
        } else {
            zi_load_events(body, note);
        }
        break;
    case ZI_JOB_RENAME: {
        char escaped[800];
        char payload[900];
        char utf[400];
        char uid_path[120];
        wchar_t wide_path[140];
        char err[64];

        if (!zi_wide_utf8(job->text, utf, sizeof utf) || !zi_escape(utf, escaped, sizeof escaped) || !zi_uid_ok(job->uid)) {
            zi_copy_notice(note->text, 512, L"The name was not saved.");
            break;
        }
        sprintf_s(payload, sizeof payload, "{\"name\":\"%s\"}", escaped);
        sprintf_s(uid_path, sizeof uid_path, "/v1/events/%s", job->uid);
        MultiByteToWideChar(CP_UTF8, 0, uid_path, -1, wide_path, 140);
        err[0] = 0;
        if (!zi_call(L"POST", wide_path, payload, 10000, &status, &body) || status != 200) {
            zi_copy_notice(note->text, 512, L"The name was not saved.");
        } else if (body && zi_json_error(body, err, sizeof err)) {
            zi_copy_notice(note->text, 512, L"The name was not saved.");
        } else {
            zi_load_events(NULL, note);
            note->events = (ZiEvent *)calloc(1, sizeof(ZiEvent));
            if (note->events) {
                char name[240];
                name[0] = 0;
                strncpy_s(note->events[0].uid, 64, job->uid, _TRUNCATE);
                if (!body || !zi_json_string(body, NULL, "name", name, sizeof name)) {
                    zi_utf8_wide(utf, note->events[0].name, 180);
                } else {
                    zi_utf8_wide(name, note->events[0].name, 180);
                }
                note->event_count = 1;
            }
        }
        break;
    }
    case ZI_JOB_REORG_STATUS:
        if (!zi_call(L"GET", L"/v1/reorg/status", NULL, 10000, &status, &body) || status != 200 || !body) {
            zi_status_fault(zi_fault_kind(status), note->text, 512);
        } else {
            zi_take_reorg(body, note);
        }
        break;
    case ZI_JOB_REORG_PLAN: {
        char utf[400];
        char escaped[800];
        char payload[900];

        if (!zi_wide_utf8(job->text, utf, sizeof utf) || !zi_escape(utf, escaped, sizeof escaped)) {
            zi_plan_failure("prefix", note->text, 512);
            break;
        }
        sprintf_s(payload, sizeof payload, "{\"prefix\":\"%s\"}", escaped);
        zi_reorg_follow(L"/v1/reorg/plan", payload, 10000, L"The plan was refused and the helper did not say why. Nothing was copied.", 1, note);
        break;
    }
    case ZI_JOB_REORG_START:
        zi_reorg_follow(L"/v1/reorg/start", NULL, 10000, L"Start was not confirmed. The helper sent no status.", 0, note);
        break;
    case ZI_JOB_REORG_PAUSE:
        zi_reorg_follow(L"/v1/reorg/pause", NULL, 10000, L"Pause was not confirmed. The helper sent no status.", 0, note);
        break;
    case ZI_JOB_REORG_RESUME:
        zi_reorg_follow(L"/v1/reorg/resume", NULL, 10000, L"Resume was not confirmed. The helper sent no status.", 0, note);
        break;
    case ZI_JOB_REVIEW:
        zi_focus("Photos/_Review");
        if (!zi_call(L"GET", L"/v1/assets?review=1&limit=200", NULL, 10000, &status, &body) || status != 200) {
            zi_copy_notice(note->text, 512, L"No photographs are parked for review.");
        } else {
            note->files = zi_rows_from(body, job->library_root, 0, &note->file_count);
            if (!note->files || note->file_count == 0) {
                zi_copy_notice(note->text, 512, L"No photographs are parked for review.");
            }
        }
        break;
    case ZI_JOB_DUP_LIST:
        zi_focus("Photos/_Duplicates");
        zi_attach_dup(note, job->library_root, job->found);
        if (job->found) {
            zi_find_text(note->file_count, note->file_count, 0, note->text, 512);
        } else if (note->file_count == 0) {
            zi_copy_notice(note->text, 512, L"This list shows copies gathered into Photos/_Duplicates. Find duplicates looks for matches. Start gathers the extra copies into this list.");
        } else {
            zi_copy_notice(note->text, 512, L"Gathered copies are listed here.");
        }
        break;
    case ZI_JOB_DUP_SCAN:
    case ZI_JOB_DUP_POLL: {
        int poll = job->kind == ZI_JOB_DUP_POLL;
        const wchar_t *method = poll ? L"GET" : L"POST";

        if (!zi_call(method, L"/v1/duplicates/scan", NULL, 10000, &status, &body)) {
            if (!poll && status == 401) {
                zi_copy_notice(note->text, 512, L"The helper refused the token, so the status check failed. Pair again.");
            } else {
                zi_copy_notice(note->text, 512, L"Find duplicates got no answer from the helper.");
            }
            break;
        }
        if (poll) {
            if (status != 200 || !body) {
                zi_copy_notice(note->text, 512, L"Find duplicates got no answer from the helper.");
                break;
            }
            zi_take_scan(body, note);
            zi_finish_scan(note, job->library_root, 1);
            break;
        }
        if (status == 404) {
            free(body);
            body = NULL;
            zi_attach_dup(note, job->library_root, 1);
            if (note->file_count == 0) {
                zi_copy_notice(note->text, 512, L"This helper cannot start a new scan. It records matches only while it is starting up.");
            } else {
                zi_find_text(note->file_count, note->file_count, 0, note->text, 512);
            }
            break;
        }
        if (status == 409) {
            zi_copy_notice(note->text, 512, L"Scan waits until Reorganize and Gather are idle.");
            break;
        }
        if (status == 401) {
            zi_copy_notice(note->text, 512, L"The helper refused the token, so the status check failed. Pair again.");
            break;
        }
        if (status != 200 || !body) {
            zi_copy_notice(note->text, 512, L"Find duplicates got no answer from the helper.");
            break;
        }
        zi_take_scan(body, note);
        zi_finish_scan(note, job->library_root, 0);
        break;
    }
    case ZI_JOB_TRIM_STATUS:
        if (zi_call(L"GET", L"/v1/trim/status", NULL, 10000, &status, &body) && status == 200 && body) {
            zi_take_trim(body, note);
        }
        break;
    case ZI_JOB_TRIM_START:
    case ZI_JOB_TRIM_PAUSE:
    case ZI_JOB_TRIM_CONTINUE: {
        const wchar_t *path = L"/v1/trim/start";
        const wchar_t *idle = L"Trim did not start.";
        char err[64];

        if (job->kind == ZI_JOB_TRIM_PAUSE) {
            path = L"/v1/trim/pause";
            idle = L"Trim did not pause.";
        } else if (job->kind == ZI_JOB_TRIM_CONTINUE) {
            path = L"/v1/trim/continue";
            idle = L"Trim did not continue.";
        }
        err[0] = 0;
        if (!zi_call(L"POST", path, NULL, 10000, &status, &body)) {
            zi_copy_notice(note->text, 512, idle);
        } else if (body && zi_json_error(body, err, sizeof err) && strcmp(err, "busy") == 0) {
            zi_copy_notice(note->text, 512, L"Trim waits until Reorganize is idle.");
        } else if (status == 200 && body && zi_find_key(body, NULL, "runState")) {
            zi_take_trim(body, note);
        } else {
            zi_copy_notice(note->text, 512, idle);
        }
        break;
    }
    case ZI_JOB_CLEAN: {
        char utf[400];
        char escaped[800];
        char payload[900];
        char err[64];
        int removed = 0;
        int kept = 0;

        err[0] = 0;
        if (!zi_wide_utf8(job->text, utf, sizeof utf) || !zi_escape(utf, escaped, sizeof escaped)) {
            zi_copy_notice(note->text, 512, L"The display folder must be Photos or a folder inside it.");
            break;
        }
        sprintf_s(payload, sizeof payload, "{\"prefix\":\"%s\"}", escaped);
        if (!zi_call(L"POST", L"/v1/folders/clean", payload, 300000, &status, &body)) {
            zi_copy_notice(note->text, 512, L"Folder cleanup did not run.");
        } else if (body && zi_json_error(body, err, sizeof err) && strcmp(err, "busy") == 0) {
            zi_copy_notice(note->text, 512, L"Folder cleanup waits until the helper is idle.");
        } else if (status == 200 && body) {
            zi_json_int(body, NULL, "removed", &removed);
            zi_json_int(body, NULL, "kept", &kept);
            note->removed = removed;
            note->kept = kept;
            zi_clean_text(removed, kept, note->text, 512);
            note->reload = removed > 0;
        } else {
            zi_copy_notice(note->text, 512, L"Folder cleanup did not run.");
        }
        break;
    }
    case ZI_JOB_DATE: {
        char wire[80];
        char escaped[120];
        char payload[180];

        zi_focus("Photos");
        if (!zi_capture_request(job->text, wire, sizeof wire) || !zi_escape(wire, escaped, sizeof escaped)) {
            zi_copy_notice(note->text, 512, L"The date was not saved. Use YYYY-MM-DD or a four-digit year.");
            break;
        }
        sprintf_s(payload, sizeof payload, "{\"captureTime\":\"%s\"}", escaped);
        zi_batch(job, "/place", payload, L"Moving %d of %d to its date folder.", note);
        zi_finish_date(note, job->count);
        note->reload = 1;
        break;
    }
    case ZI_JOB_REMOVE:
        zi_batch(job, "/to-removed", NULL, L"Moving %d of %d to Photos/_Removed.", note);
        zi_finish_remove(note, job->count);
        note->reload = 1;
        break;
    case ZI_JOB_RESTORE:
    case ZI_JOB_PIN:
    case ZI_JOB_PURGE: {
        char payload[400];
        char *reply = NULL;
        char err[64];
        const char *suffix = "/restore";
        const char *json = NULL;
        int ok;

        err[0] = 0;
        if (job->kind == ZI_JOB_PIN) {
            suffix = "/pin";
            sprintf_s(payload, sizeof payload, "{\"pinned\":%s}", job->pinned ? "true" : "false");
            json = payload;
        } else if (job->kind == ZI_JOB_PURGE) {
            char utf[300];
            char escaped[600];
            suffix = "/purge";
            if (!zi_wide_utf8(job->text, utf, sizeof utf) || !zi_escape(utf, escaped, sizeof escaped)) {
                zi_copy_notice(note->text, 512, L"The file was not deleted.");
                break;
            }
            sprintf_s(payload, sizeof payload, "{\"confirmFilename\":\"%s\"}", escaped);
            json = payload;
        }
        ok = zi_asset_post(job->uid, suffix, json, 10000, &status, &reply);
        if (reply) {
            zi_json_error(reply, err, sizeof err);
        }
        free(reply);
        if (!ok || status != 200 || err[0]) {
            if (job->kind == ZI_JOB_RESTORE) {
                zi_copy_notice(note->text, 512, L"The file was not restored.");
            } else if (job->kind == ZI_JOB_PIN) {
                zi_copy_notice(note->text, 512, L"The pin was not saved.");
            } else {
                zi_copy_notice(note->text, 512, L"The file was not deleted.");
            }
        } else {
            if (job->kind == ZI_JOB_RESTORE) {
                zi_copy_notice(note->text, 512, L"Restored and pinned.");
            }
            zi_attach_dup(note, job->library_root, 0);
            note->reload = 1;
        }
        break;
    }
    case ZI_JOB_PURGE_ALL: {
        char *reply = NULL;
        char err[64];

        err[0] = 0;
        if (wcscmp(job->text, L"DELETE") != 0) {
            zi_copy_notice(note->text, 512, L"Type DELETE to delete every duplicate.");
            break;
        }
        if (!zi_call(L"POST", L"/v1/duplicates/purge-all", "{\"confirm\":\"DELETE\"}", 10000, &status, &reply)) {
            zi_copy_notice(note->text, 512, L"The duplicates were not deleted.");
        } else if (reply && zi_json_error(reply, err, sizeof err)) {
            zi_copy_notice(note->text, 512, L"The duplicates were not deleted.");
        } else if (status != 200) {
            zi_copy_notice(note->text, 512, L"The duplicates were not deleted.");
        } else {
            zi_attach_dup(note, job->library_root, 0);
            note->reload = 1;
        }
        free(reply);
        break;
    }
    default:
        break;
    }
    free(body);
    free(job->items);
    zi_post(job->hwnd, note);
    free(job);
    return 0;
}

static void zi_start(ZiApp *app, ZiLibJob *job) {
    HANDLE thread;

    if (!app || !job || !app->hwnd) {
        if (job) {
            free(job->items);
            free(job);
        }
        return;
    }
    if (InterlockedCompareExchange(&g_job_busy, 1, 0) != 0) {
        free(job->items);
        free(job);
        return;
    }
    job->hwnd = app->hwnd;
    wcsncpy_s(job->library_root, MAX_PATH, app->library_root, _TRUNCATE);
    thread = CreateThread(NULL, 0, zi_library_thread, job, 0, NULL);
    if (thread) {
        CloseHandle(thread);
        return;
    }
    free(job->items);
    free(job);
    InterlockedExchange(&g_job_busy, 0);
}

static ZiLibJob *zi_job(int kind) {
    ZiLibJob *job = (ZiLibJob *)calloc(1, sizeof *job);

    if (job) {
        job->kind = kind;
    }
    return job;
}

static int zi_confirm(HWND hwnd, const wchar_t *text) {
    int answer;

    g_modal = 1;
    answer = MessageBoxW(hwnd, text, L"ZImager", MB_OKCANCEL | MB_ICONWARNING);
    g_modal = 0;
    return answer == IDOK;
}

static void zi_trim_text(wchar_t *text) {
    size_t n;
    wchar_t *start = text;

    if (!text) {
        return;
    }
    while (*start == L' ' || *start == L'\t' || *start == L'\r' || *start == L'\n') {
        start++;
    }
    if (start != text) {
        memmove(text, start, (wcslen(start) + 1) * sizeof(wchar_t));
    }
    n = wcslen(text);
    while (n > 0 && (text[n - 1] == L' ' || text[n - 1] == L'\t' || text[n - 1] == L'\r' || text[n - 1] == L'\n')) {
        text[--n] = L'\0';
    }
}

static int zi_folder_prefix(const wchar_t *typed, wchar_t *dest, size_t cap) {
    wchar_t buf[512];
    wchar_t *parts[40];
    int count = 0;
    wchar_t *cursor;
    int i;
    size_t used = 0;

    dest[0] = L'\0';
    if (!typed) {
        wcscpy_s(dest, cap, L"Photos");
        return 1;
    }
    wcsncpy_s(buf, 512, typed, _TRUNCATE);
    zi_trim_text(buf);
    cursor = buf;
    while (*cursor == L'/' || *cursor == L'\\') {
        cursor++;
    }
    if (!cursor[0]) {
        wcscpy_s(dest, cap, L"Photos");
        return 1;
    }
    parts[count++] = cursor;
    for (; *cursor && count < 40; cursor++) {
        if (*cursor == L'/' || *cursor == L'\\') {
            *cursor = L'\0';
            if (cursor[1]) {
                parts[count++] = cursor + 1;
            }
        }
    }
    for (i = 0; i < count; i++) {
        if (!parts[i][0] || wcscmp(parts[i], L".") == 0 || wcscmp(parts[i], L"..") == 0) {
            wcsncpy_s(dest, cap, typed, _TRUNCATE);
            zi_trim_text(dest);
            return 0;
        }
    }
    dest[0] = L'\0';
    for (i = 0; i < count; i++) {
        size_t len = wcslen(parts[i]);
        if (used + len + 2 >= cap) {
            return 0;
        }
        if (i) {
            dest[used++] = L'/';
        }
        wcscpy_s(dest + used, cap - used, parts[i]);
        used += len;
    }
    return 1;
}

static int zi_inside_photos(const wchar_t *prefix) {
    const wchar_t *p;

    if (!prefix || !prefix[0]) {
        return 0;
    }
    if (wcscmp(prefix, L"Photos") == 0) {
        return 1;
    }
    if (wcsncmp(prefix, L"Photos/", 7) != 0) {
        return 0;
    }
    p = prefix + 7;
    if (!*p) {
        return 0;
    }
    while (*p) {
        const wchar_t *slash = wcschr(p, L'/');
        size_t n = slash ? (size_t)(slash - p) : wcslen(p);
        if (n == 0 || (n == 1 && p[0] == L'.') || (n == 2 && p[0] == L'.' && p[1] == L'.')) {
            return 0;
        }
        if (!slash) {
            break;
        }
        p = slash + 1;
    }
    return 1;
}

static int zi_saved_folder(const wchar_t *phase, wchar_t *dest, size_t cap) {
    if (!phase || (wcscmp(phase, L"Photos") != 0 && wcsncmp(phase, L"Photos/", 7) != 0)) {
        return 0;
    }
    if (wcscmp(phase, L"Photos/_Duplicates") == 0 || wcsncmp(phase, L"Photos/_Duplicates/", 19) == 0) {
        return 0;
    }
    if (wcscmp(phase, L"Photos/_Removed") == 0 || wcsncmp(phase, L"Photos/_Removed/", 16) == 0) {
        return 0;
    }
    wcscpy_s(dest, cap, phase);
    return 1;
}

static int zi_prefix_from_scan(const wchar_t *scan, const wchar_t *folder, wchar_t *dest, size_t cap) {
    size_t n;
    const wchar_t *tail;
    wchar_t buf[512];

    if (!scan || !scan[0] || !folder) {
        return 0;
    }
    n = wcslen(scan);
    while (n > 0 && (scan[n - 1] == L'\\' || scan[n - 1] == L'/')) {
        n--;
    }
    if (_wcsnicmp(folder, scan, n) != 0) {
        return 0;
    }
    if (folder[n] != L'\0' && folder[n] != L'\\' && folder[n] != L'/') {
        return 0;
    }
    tail = folder + n;
    if (*tail == L'\\' || *tail == L'/') {
        tail++;
    }
    if (!tail[0]) {
        wcscpy_s(dest, cap, L"Photos");
        return 1;
    }
    if (swprintf_s(buf, 512, L"Photos/%s", tail) < 0) {
        return 0;
    }
    {
        wchar_t *p;
        for (p = buf; *p; p++) {
            if (*p == L'\\') {
                *p = L'/';
            }
        }
    }
    if (!zi_inside_photos(buf)) {
        return 0;
    }
    wcscpy_s(dest, cap, buf);
    return 1;
}

static int zi_current_prefix(ZiApp *app, wchar_t *dest, size_t cap) {
    wchar_t folder[MAX_PATH];

    if (!zi_current_folder(app, folder, MAX_PATH)) {
        return 0;
    }
    return zi_prefix_from_scan(app->library_scan, folder, dest, cap);
}

static int zi_parked_file(const ZiFile *file) {
    const wchar_t *p;

    if (!file) {
        return 1;
    }
    if (file->parked) {
        return 1;
    }
    p = file->path;
    while (p && *p) {
        if (_wcsnicmp(p, L"\\Photos\\_Removed", 16) == 0 || _wcsnicmp(p, L"/Photos/_Removed", 16) == 0) {
            return 1;
        }
        if (_wcsnicmp(p, L"\\Photos\\_Duplicates", 19) == 0 || _wcsnicmp(p, L"/Photos/_Duplicates", 19) == 0) {
            return 1;
        }
        p++;
    }
    return 0;
}

static void zi_item_from_file(ZiLibItem *item, const ZiFile *file) {
    const wchar_t *base;

    ZeroMemory(item, sizeof *item);
    if (zi_uid_ok(file->uid)) {
        strncpy_s(item->uid, 64, file->uid, _TRUNCATE);
    }
    wcsncpy_s(item->path, MAX_PATH, file->path, _TRUNCATE);
    base = wcsrchr(file->path, L'\\');
    base = (base && base[1]) ? base + 1 : file->name;
    wcsncpy_s(item->name, ZI_NAME, base, _TRUNCATE);
}

static int zi_collect_marks(const ZiApp *app, int clicked, int marks_only, ZiLibItem **out) {
    ZiLibItem *items = (ZiLibItem *)calloc(ZI_MAX_FILES, sizeof(ZiLibItem));
    int n = 0;
    int i;
    int use_marks = 0;

    if (!items) {
        return 0;
    }
    if (marks_only) {
        use_marks = clicked >= 0 && clicked < app->file_count && app->file_mark[clicked];
    } else {
        for (i = 0; i < app->file_count; i++) {
            if (app->file_mark[i]) {
                use_marks = 1;
                break;
            }
        }
    }
    if (use_marks) {
        for (i = 0; i < app->file_count; i++) {
            if (!app->file_mark[i] || zi_parked_file(&app->files[i])) {
                continue;
            }
            zi_item_from_file(&items[n], &app->files[i]);
            n++;
        }
    } else if (!marks_only && app->file_selected >= 0 && app->file_selected < app->file_count && !zi_parked_file(&app->files[app->file_selected])) {
        zi_item_from_file(&items[n], &app->files[app->file_selected]);
        n++;
    } else if (marks_only && clicked >= 0 && clicked < app->file_count && !zi_parked_file(&app->files[clicked])) {
        zi_item_from_file(&items[n], &app->files[clicked]);
        n++;
    }
    if (n == 0) {
        free(items);
        return 0;
    }
    *out = items;
    return n;
}

static void zi_show_edits(ZiApp *app) {
    int events = app->events_open;
    int reorg = app->reorg_open;
    int date = zi_library_date_visible(app);

    if (g_event_edit) {
        ShowWindow(g_event_edit, events ? SW_SHOW : SW_HIDE);
    }
    if (g_prefix_edit) {
        ShowWindow(g_prefix_edit, reorg ? SW_SHOW : SW_HIDE);
    }
    if (g_date_edit) {
        ShowWindow(g_date_edit, date ? SW_SHOW : SW_HIDE);
    }
}

static void zi_add_hit(int id, int extra, int enabled, RECT rc, const wchar_t *label) {
    if (g_hit_count >= 32) {
        return;
    }
    g_hits[g_hit_count].id = id;
    g_hits[g_hit_count].extra = extra;
    g_hits[g_hit_count].enabled = enabled;
    g_hits[g_hit_count].rc = rc;
    g_hits[g_hit_count].label = label;
    g_hit_count++;
}

static RECT zi_box(int x, int y, int w, int h) {
    RECT rc;
    rc.left = x;
    rc.top = y;
    rc.right = x + w;
    rc.bottom = y + h;
    return rc;
}

static void zi_place_edit(HWND edit, HFONT font, RECT rc) {
    if (!edit) {
        return;
    }
    if (font) {
        SendMessageW(edit, WM_SETFONT, (WPARAM)font, TRUE);
    }
    MoveWindow(edit, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, TRUE);
}

static const wchar_t *zi_recovery_line(const char *code) {
    if (!code || !code[0] || strcmp(code, "none") == 0) {
        return NULL;
    }
    if (strcmp(code, "planned_kept") == 0) {
        return L"Last recovery: a file had not started copying, so it was left in place.";
    }
    if (strcmp(code, "scratch_reset") == 0) {
        return L"Last recovery: an unfinished temporary copy was discarded.";
    }
    if (strcmp(code, "catalog_updated") == 0) {
        return L"Last recovery: the catalog was updated after the new file was in place.";
    }
    if (strcmp(code, "source_detached") == 0) {
        return L"Last recovery: the original was set aside after the new file checked out.";
    }
    if (strcmp(code, "backup_unlinked") == 0) {
        return L"Last recovery: the temporary original was removed after the new file checked out.";
    }
    if (strcmp(code, "source_restored") == 0) {
        return L"Last recovery: the original was put back.";
    }
    if (strcmp(code, "failed") == 0) {
        return L"Last recovery: the open file was marked failed.";
    }
    return NULL;
}

static void zi_paint_button(ZiApp *app, HDC hdc, const ZiHit *hit) {
    HBRUSH fill = hit->enabled ? app->button_brush : app->panel;
    HPEN pen = app->muted_pen;
    HGDIOBJ old_brush = SelectObject(hdc, fill);
    HGDIOBJ old_pen = SelectObject(hdc, pen);
    RECT text = hit->rc;
    int radius = zi_px(app, 8);

    RoundRect(hdc, hit->rc.left, hit->rc.top, hit->rc.right, hit->rc.bottom, radius, radius);
    SelectObject(hdc, old_pen);
    SelectObject(hdc, old_brush);
    if (hit->label) {
        HFONT old = (HFONT)SelectObject(hdc, app->font);
        InflateRect(&text, -zi_px(app, 4), 0);
        SetTextColor(hdc, hit->enabled ? ZI_INK : ZI_MUTED);
        SetBkMode(hdc, TRANSPARENT);
        DrawTextW(hdc, hit->label, -1, &text, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        SelectObject(hdc, old);
    }
}

static void zi_paint_wrap(ZiApp *app, HDC hdc, RECT rc, const wchar_t *text, COLORREF color) {
    HFONT old = (HFONT)SelectObject(hdc, app->font);

    SetTextColor(hdc, color);
    SetBkMode(hdc, TRANSPARENT);
    DrawTextW(hdc, text, -1, &rc, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
    SelectObject(hdc, old);
}

static COLORREF zi_lamp_color(int known, int safe) {
    if (!known) {
        return ZI_MUTED;
    }
    if (safe) {
        return RGB(31, 115, 56);
    }
    return RGB(184, 107, 20);
}

static const wchar_t *zi_lamp_text(int known, int safe) {
    if (!known) {
        return L"Waiting for the helper.";
    }
    return safe ? L"Safe to reboot" : L"Not safe to reboot";
}

static const wchar_t *zi_trim_activity(const wchar_t *state) {
    if (!state) {
        return NULL;
    }
    if (wcscmp(state, L"running") == 0 || wcscmp(state, L"pause_requested") == 0 || wcscmp(state, L"recovering") == 0) {
        return L"Gathering copies.";
    }
    if (wcscmp(state, L"paused") == 0) {
        return L"Gathering is paused.";
    }
    if (wcscmp(state, L"blocked") == 0) {
        return L"Gathering stopped. The helper could not copy the next file.";
    }
    return NULL;
}

static int zi_json_object(const char *body) {
    if (!body) {
        return 0;
    }
    while (*body == ' ' || *body == '\t' || *body == '\n' || *body == '\r') {
        body++;
    }
    return *body == '{';
}

/* Thousands separators, same places as DuplicateCopy.count. */
static void zi_group_count(int value, wchar_t *dest, size_t cap) {
    wchar_t digits[16];
    size_t n;
    size_t i;
    size_t o = 0;

    if (!dest || cap == 0) {
        return;
    }
    dest[0] = L'\0';
    swprintf_s(digits, 16, L"%d", value);
    n = wcslen(digits);
    for (i = 0; i < n; i++) {
        if (i > 0 && (n - i) % 3 == 0) {
            if (o + 2 >= cap) {
                break;
            }
            dest[o++] = L',';
        }
        if (o + 1 >= cap) {
            break;
        }
        dest[o++] = digits[i];
    }
    dest[o] = L'\0';
}

static const wchar_t *zi_pass_name(const char *phase) {
    if (strcmp(phase, "cache") == 0) {
        return L"Checking the preview cache";
    }
    if (strcmp(phase, "previews") == 0) {
        return L"Making previews";
    }
    if (strcmp(phase, "relink") == 0) {
        return L"Checking files already in the catalog";
    }
    if (strcmp(phase, "presence") == 0) {
        return L"Recording which files are present";
    }
    if (strcmp(phase, "match") == 0) {
        return L"Matching files to the catalog";
    }
    if (strcmp(phase, "inventory") == 0) {
        return L"Looking through Photos";
    }
    if (strcmp(phase, "dating") == 0) {
        return L"Reading dates";
    }
    if (strcmp(phase, "hashing") == 0) {
        return L"Hashing previews";
    }
    if (strcmp(phase, "neighbors") == 0) {
        return L"Matching nearby dates";
    }
    if (strcmp(phase, "events") == 0) {
        return L"Naming events";
    }
    if (strcmp(phase, "scoring") == 0) {
        return L"Scoring sharpness";
    }
    if (strcmp(phase, "comparing") == 0) {
        return L"Comparing photos";
    }
    if (strcmp(phase, "saving") == 0) {
        return L"Saving groups";
    }
    if (strcmp(phase, "planning") == 0) {
        return L"Planning folders";
    }
    if (strcmp(phase, "folder") == 0) {
        return L"Making previews for the folder on screen";
    }
    return L"Working";
}

static int zi_pass_estimate(const char *phase) {
    return strcmp(phase, "inventory") == 0 || strcmp(phase, "relink") == 0 || strcmp(phase, "cache") == 0;
}

/* DuplicateCopy.passLine. An empty phase leaves dest empty. */
static void zi_format_pass(const char *phase, int done, int total, wchar_t *dest, size_t cap) {
    const wchar_t *name;
    wchar_t done_text[32];
    wchar_t total_text[32];
    const wchar_t *of_word;

    if (!dest || cap == 0) {
        return;
    }
    dest[0] = L'\0';
    if (!phase || !phase[0]) {
        return;
    }
    name = zi_pass_name(phase);
    if (total > 0) {
        zi_group_count(done, done_text, 32);
        zi_group_count(total, total_text, 32);
        of_word = zi_pass_estimate(phase) ? L"of about" : L"of";
        swprintf_s(dest, cap, L"%s, %s %s %s.", name, done_text, of_word, total_text);
        return;
    }
    if (done > 0) {
        zi_group_count(done, done_text, 32);
        swprintf_s(dest, cap, L"%s, %s looked at.", name, done_text);
        return;
    }
    swprintf_s(dest, cap, L"%s.", name);
}

typedef struct ZiPassStart {
    HWND hwnd;
    LONG serial;
} ZiPassStart;

static DWORD WINAPI zi_pass_thread(LPVOID param) {
    ZiPassStart *start = (ZiPassStart *)param;
    HWND hwnd;
    LONG serial;
    ZiLibNote *note;
    char *body = NULL;
    DWORD status = 0;
    char phase[64];
    int done_n = 0;
    int total_n = 0;

    if (!start) {
        InterlockedExchange(&g_pass_busy, 0);
        return 0;
    }
    hwnd = start->hwnd;
    serial = start->serial;
    free(start);
    note = (ZiLibNote *)calloc(1, sizeof *note);
    if (!note) {
        InterlockedExchange(&g_pass_busy, 0);
        return 0;
    }
    note->kind = ZI_JOB_PASS;
    note->pass_serial = serial;
    phase[0] = 0;
    if (g_alive && zi_call(L"GET", L"/v1/duplicates/scan", NULL, 10000, &status, &body)) {
        if (status == 404) {
            note->pass_code = 2;
        } else if (status == 200 && zi_json_object(body)) {
            zi_json_string(body, NULL, "passPhase", phase, sizeof phase);
            zi_json_int(body, NULL, "passDone", &done_n);
            zi_json_int(body, NULL, "passTotal", &total_n);
            zi_format_pass(phase, done_n, total_n, note->text, 512);
            note->pass_code = 1;
        }
    }
    free(body);
    if (!g_alive || !hwnd || !PostMessageW(hwnd, ZI_WM_LIBRARY, 0, (LPARAM)note)) {
        zi_note_free(note);
    }
    InterlockedExchange(&g_pass_busy, 0);
    return 0;
}

void zi_library_refresh_pass(ZiApp *app) {
    ZiPassStart *start;
    HANDLE thread;

    if (!app || !app->hwnd || !app->source_server || g_modal || !g_alive) {
        return;
    }
    if (InterlockedCompareExchange(&g_pass_busy, 1, 0) != 0) {
        return;
    }
    start = (ZiPassStart *)calloc(1, sizeof *start);
    if (!start) {
        InterlockedExchange(&g_pass_busy, 0);
        return;
    }
    start->hwnd = app->hwnd;
    start->serial = InterlockedIncrement(&g_pass_serial);
    thread = CreateThread(NULL, 0, zi_pass_thread, start, 0, NULL);
    if (thread) {
        CloseHandle(thread);
        return;
    }
    free(start);
    InterlockedExchange(&g_pass_busy, 0);
}

void zi_library_clear_pass(ZiApp *app) {
    InterlockedIncrement(&g_pass_serial);
    if (app) {
        app->pass_line[0] = L'\0';
    }
}

void zi_library_startup(HWND hwnd) {
    HINSTANCE instance = GetModuleHandleW(NULL);
    WNDCLASSW wc;
    ZiApp *app = (ZiApp *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    g_alive = 1;
    if (app) {
        app->event_selected = -1;
    }
    g_event_edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL, 0, 0, 0, 0, hwnd, (HMENU)4101, instance, NULL);
    g_prefix_edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL, 0, 0, 0, 0, hwnd, (HMENU)4102, instance, NULL);
    g_date_edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL, 0, 0, 0, 0, hwnd, (HMENU)4103, instance, NULL);
    ZeroMemory(&wc, sizeof wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = instance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"ZImagerDeleteAll";
    RegisterClassW(&wc);
    SetTimer(hwnd, 2, 2000, NULL);
}

void zi_library_shutdown(void) {
    g_alive = 0;
}

int zi_library_date_visible(const ZiApp *app) {
    if (!app->source_server || !zi_showing_folder(app) || app->events_open || app->reorg_open) {
        return 0;
    }
    return app->library_section == 0 || app->library_section == 1;
}

int zi_library_clean_ready(const ZiApp *app) {
    if (!app->source_server || app->library_section != 0 || app->events_open || app->reorg_open) {
        return 0;
    }
    if (app->date_busy || app->removal_busy || app->clean_busy || zi_job_busy()) {
        return 0;
    }
    return 1;
}

int zi_library_can_remove(const ZiApp *app, int index) {
    ZiLibItem *items = NULL;
    int count;

    if (!app->source_server || app->library_section != 0 || app->events_open || app->reorg_open) {
        return 0;
    }
    if (app->date_busy || app->removal_busy || app->clean_busy || zi_job_busy()) {
        return 0;
    }
    count = zi_collect_marks(app, index, 1, &items);
    free(items);
    return count > 0;
}

void zi_library_show_photos(ZiApp *app) {
    app->events_open = 0;
    app->reorg_open = 0;
    app->library_section = 0;
    app->grid_note[0] = L'\0';
    zi_grid_clear(app);
}

void zi_library_open_section(ZiApp *app, int section) {
    ZiLibJob *job;

    app->notice[0] = L'\0';
    app->events_open = 0;
    app->reorg_open = 0;
    app->server_folder = 1;
    app->library_section = section;
    app->grid_note[0] = L'\0';
    zi_grid_clear(app);
    if (section == 1) {
        zi_copy_notice(app->grid_note, 320, L"Waiting for the helper.");
        job = zi_job(ZI_JOB_REVIEW);
        if (job) {
            zi_start(app, job);
        }
    } else if (section == 2) {
        zi_copy_notice(app->grid_note, 320, L"Waiting for the helper.");
        job = zi_job(ZI_JOB_DUP_LIST);
        if (job) {
            zi_start(app, job);
        }
        job = zi_job(ZI_JOB_TRIM_STATUS);
        if (job && !zi_job_busy()) {
            zi_start(app, job);
        } else {
            free(job);
        }
    }
}

void zi_library_toggle_events(ZiApp *app) {
    ZiLibJob *job;

    if (app->events_open) {
        zi_library_show_photos(app);
        app->server_folder = 1;
        return;
    }
    app->reorg_open = 0;
    app->events_open = 1;
    app->server_folder = 1;
    app->notice[0] = L'\0';
    job = zi_job(ZI_JOB_EVENTS);
    if (job) {
        zi_start(app, job);
    }
}

void zi_library_toggle_reorg(ZiApp *app) {
    ZiLibJob *job;
    wchar_t prefix[512];

    if (app->reorg_open) {
        zi_library_show_photos(app);
        app->server_folder = 1;
        return;
    }
    app->events_open = 0;
    app->reorg_open = 1;
    app->server_folder = 1;
    app->notice[0] = L'\0';
    if (g_prefix_edit && GetWindowTextLengthW(g_prefix_edit) == 0 && zi_current_prefix(app, prefix, 512)) {
        SetWindowTextW(g_prefix_edit, prefix);
    }
    job = zi_job(ZI_JOB_REORG_STATUS);
    if (job) {
        zi_start(app, job);
    }
}

void zi_library_layout(ZiApp *app) {
    int busy = zi_job_busy();
    int y;
    int h = zi_px(app, 26);
    int gap = zi_px(app, 8);
    int pad = zi_px(app, 12);
    RECT area;
    RECT field;

    g_hit_count = 0;
    g_event_list.left = g_event_list.right = 0;
    if (g_edit_font != app->font) {
        g_edit_font = app->font;
    }
    if (app->events_open) {
        int row = zi_px(app, 28);
        int list_bottom;

        area = app->regions.canvas;
        InflateRect(&area, -pad, -pad);
        g_event_row_h = row;
        g_event_list = area;
        g_event_list.top += zi_px(app, 36);
        list_bottom = area.bottom - zi_px(app, 78);
        if (list_bottom < g_event_list.top) {
            list_bottom = g_event_list.top;
        }
        g_event_list.bottom = list_bottom;
        field = zi_box(area.left, area.bottom - h - gap - h, area.right - area.left, h);
        zi_place_edit(g_event_edit, app->font, field);
        zi_add_hit(ZI_HIT_RENAME, 0, !busy && app->event_selected >= 0 && app->event_selected < g_event_count,
                   zi_box(area.left, area.bottom - h, zi_px(app, 110), h), L"Rename");
    }
    if (app->reorg_open) {
        int x;
        area = app->regions.canvas;
        InflateRect(&area, -pad, -pad);
        y = area.bottom - h;
        x = area.left;
        field = zi_box(area.left + zi_px(app, 64), y - h - gap, area.right - area.left - zi_px(app, 64), h);
        zi_place_edit(g_prefix_edit, app->font, field);
        zi_add_hit(ZI_HIT_PLAN, 0, !busy, zi_box(x, y, zi_px(app, 72), h), L"Plan");
        x += zi_px(app, 80);
        zi_add_hit(ZI_HIT_START, 0, !busy, zi_box(x, y, zi_px(app, 72), h), L"Start");
        x += zi_px(app, 80);
        zi_add_hit(ZI_HIT_PAUSE, 0, !busy, zi_box(x, y, zi_px(app, 72), h), L"Pause");
        x += zi_px(app, 80);
        zi_add_hit(ZI_HIT_RESUME, 0, !busy, zi_box(x, y, zi_px(app, 84), h), L"Resume");
        x += zi_px(app, 92);
        zi_add_hit(ZI_HIT_CHOOSE, 0, !busy, zi_box(x, y, zi_px(app, 130), h), L"Choose Folder");
    }
    if (zi_library_date_visible(app)) {
        int movable = 0;
        ZiLibItem *items = NULL;
        int count = zi_collect_marks(app, -1, 0, &items);
        RECT column = app->regions.right;

        free(items);
        movable = count > 0;
        field = zi_box(column.left + pad, column.bottom - pad - h - gap - h, column.right - column.left - pad * 2, h);
        zi_place_edit(g_date_edit, app->font, field);
        zi_add_hit(ZI_HIT_DATE, 0, movable && !busy && !app->date_busy && !app->removal_busy && !app->clean_busy,
                   zi_box(column.left + pad, column.bottom - pad - h, column.right - column.left - pad * 2, h), L"Set date");
    }
    if (app->source_server && zi_showing_folder(app) && app->library_section == 2 && !app->events_open && !app->reorg_open) {
        const wchar_t *pin_label = L"Pin";
        int selected = app->file_selected >= 0 && app->file_selected < app->file_count;
        RECT column = app->regions.right;
        int width;

        if (selected && app->files[app->file_selected].pinned) {
            pin_label = L"Clear pin";
        }
        y = column.top + pad + zi_px(app, 28);
        width = column.right - column.left - pad * 2;
        zi_add_hit(ZI_HIT_FIND, 0, !busy, zi_box(column.left + pad, y, width, h), L"Find duplicates");
        y += h + gap + zi_px(app, 24);
        zi_add_hit(ZI_HIT_TRIM_START, 0, !busy, zi_box(column.left + pad, y, width, h), L"Start");
        y += h + gap;
        zi_add_hit(ZI_HIT_TRIM_PAUSE, 0, !busy, zi_box(column.left + pad, y, width, h), L"Pause");
        y += h + gap;
        zi_add_hit(ZI_HIT_TRIM_CONTINUE, 0, !busy, zi_box(column.left + pad, y, width, h), L"Continue");
        y += h + gap + zi_px(app, 18);
        zi_add_hit(ZI_HIT_RESTORE, 0, !busy && selected, zi_box(column.left + pad, y, width, h), L"Restore");
        y += h + gap;
        zi_add_hit(ZI_HIT_PIN, 0, !busy && selected, zi_box(column.left + pad, y, width, h), pin_label);
        y += h + gap;
        zi_add_hit(ZI_HIT_DELETE, 0, !busy && selected, zi_box(column.left + pad, y, width, h), L"Delete");
        y += h + gap;
        zi_add_hit(ZI_HIT_DELETE_ALL, 0, !busy && app->file_count > 0, zi_box(column.left + pad, y, width, h), L"Delete all duplicates");
    }
    zi_show_edits(app);
}

void zi_library_paint_center(ZiApp *app, HDC hdc) {
    RECT area = app->regions.canvas;
    RECT title;
    RECT body;
    int i;
    int pad = zi_px(app, 12);
    HFONT old;

    FillRect(hdc, &app->regions.canvas, app->panel);
    InflateRect(&area, -pad, -pad);
    title = area;
    title.bottom = title.top + zi_px(app, 32);
    old = (HFONT)SelectObject(hdc, app->font_header);
    SetTextColor(hdc, ZI_INK);
    SetBkMode(hdc, TRANSPARENT);
    DrawTextW(hdc, app->events_open ? L"Events" : L"Reorganize", -1, &title, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(hdc, old);
    if (app->events_open) {
        int row = g_event_row_h > 0 ? g_event_row_h : zi_px(app, 28);
        int first;

        if (g_event_count == 0) {
            body = g_event_list;
            zi_paint_wrap(app, hdc, body, L"No events are stored. An event is created from photos that already have a capture date.", ZI_MUTED);
        }
        first = g_event_scroll / row;
        if (first < 0) {
            first = 0;
        }
        for (i = first; i < g_event_count; i++) {
            RECT rc = g_event_list;
            rc.top = g_event_list.top + (i * row) - g_event_scroll;
            rc.bottom = rc.top + row;
            if (rc.top >= g_event_list.bottom) {
                break;
            }
            if (i == app->event_selected) {
                FillRect(hdc, &rc, app->list_on);
            }
            rc.left += zi_px(app, 8);
            old = (HFONT)SelectObject(hdc, app->font);
            SetTextColor(hdc, ZI_INK);
            DrawTextW(hdc, g_events[i].name, -1, &rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            SelectObject(hdc, old);
        }
        body = area;
        body.top = area.bottom - zi_px(app, 78);
        body.bottom = body.top + zi_px(app, 22);
        old = (HFONT)SelectObject(hdc, app->font);
        SetTextColor(hdc, ZI_INK);
        DrawTextW(hdc, L"Event name", -1, &body, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(hdc, old);
        if (app->notice[0]) {
            body = area;
            body.bottom = g_event_list.top;
            zi_paint_wrap(app, hdc, body, app->notice, ZI_INK);
        }
    } else if (app->reorg_open) {
        const wchar_t *lamp = zi_lamp_text(g_reorg_known, g_reorg_safe);
        const wchar_t *recovery = zi_recovery_line(g_recovery);
        wchar_t counts[256];
        wchar_t cache[128];
        wchar_t custom[160];
        int y = title.bottom + zi_px(app, 8);

        body = area;
        body.top = y;
        body.bottom = y + zi_px(app, 24);
        old = (HFONT)SelectObject(hdc, app->font);
        SetTextColor(hdc, zi_lamp_color(g_reorg_known, g_reorg_safe));
        DrawTextW(hdc, lamp, -1, &body, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        y = body.bottom + zi_px(app, 4);
        SetTextColor(hdc, ZI_INK);
        if (g_run_state[0]) {
            body.top = y;
            body.bottom = y + zi_px(app, 22);
            DrawTextW(hdc, g_run_state, -1, &body, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            y = body.bottom;
        }
        if (g_phase[0]) {
            body.top = y;
            body.bottom = y + zi_px(app, 22);
            DrawTextW(hdc, g_phase, -1, &body, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            y = body.bottom + zi_px(app, 6);
        }
        swprintf_s(counts, 256, L"Move %d    Already placed %d    Park %d    Leave %d", g_move_n, g_placed_n, g_park_n, g_leave_n);
        body.top = y;
        body.bottom = y + zi_px(app, 22);
        DrawTextW(hdc, counts, -1, &body, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        y = body.bottom + zi_px(app, 4);
        body.top = y;
        body.bottom = y + zi_px(app, 48);
        DrawTextW(hdc, L"Move is copied. Already placed is already in the planned folder. Park goes to Review. Leave stays where it is.",
                  -1, &body, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
        y = body.bottom + zi_px(app, 4);
        swprintf_s(cache, 128, L"Cache %lld bytes", g_cache_bytes);
        body.top = y;
        body.bottom = y + zi_px(app, 22);
        DrawTextW(hdc, cache, -1, &body, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        y = body.bottom;
        if (!recovery && g_recovery[0] && strcmp(g_recovery, "none") != 0) {
            wchar_t wide[80];
            zi_utf8_wide(g_recovery, wide, 80);
            swprintf_s(custom, 160, L"Last recovery: %s.", wide);
            recovery = custom;
        }
        if (recovery) {
            body.top = y;
            body.bottom = y + zi_px(app, 40);
            DrawTextW(hdc, recovery, -1, &body, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
            y = body.bottom;
        }
        if (g_reorg_notice[0]) {
            body.top = y + zi_px(app, 8);
            body.bottom = area.bottom - zi_px(app, 78);
            DrawTextW(hdc, g_reorg_notice, -1, &body, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
        }
        body.top = area.bottom - zi_px(app, 78);
        body.bottom = body.top + zi_px(app, 26);
        DrawTextW(hdc, L"Prefix", -1, &body, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(hdc, old);
    }
    for (i = 0; i < g_hit_count; i++) {
        if (g_hits[i].rc.left >= app->regions.canvas.left) {
            zi_paint_button(app, hdc, &g_hits[i]);
        }
    }
}

void zi_library_paint_side(ZiApp *app, HDC hdc) {
    int i;
    RECT column = app->regions.right;
    int pad = zi_px(app, 12);
    HFONT old;

    if (app->source_server && zi_showing_folder(app) && app->library_section == 2 && !app->events_open && !app->reorg_open) {
        RECT text = column;
        const wchar_t *activity;

        InflateRect(&text, -pad, -pad);
        text.bottom = text.top + zi_px(app, 24);
        old = (HFONT)SelectObject(hdc, app->font);
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, zi_lamp_color(g_trim_known, g_trim_safe));
        DrawTextW(hdc, zi_lamp_text(g_trim_known, g_trim_safe), -1, &text, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        text.top = text.bottom;
        text.bottom = text.top + zi_px(app, 40);
        SetTextColor(hdc, ZI_INK);
        if (g_scan_line[0]) {
            DrawTextW(hdc, g_scan_line, -1, &text, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
        }
        text.top += zi_px(app, 36);
        text.bottom = text.top + zi_px(app, 22);
        DrawTextW(hdc, L"Gather duplicates", -1, &text, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        activity = zi_trim_activity(g_trim_state);
        if (activity) {
            text.top = column.top + pad + zi_px(app, 28) + zi_px(app, 26) + zi_px(app, 8) + zi_px(app, 24) + (zi_px(app, 26) + zi_px(app, 8)) * 3;
            text.bottom = text.top + zi_px(app, 36);
            DrawTextW(hdc, activity, -1, &text, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
        }
        if (g_trim_known) {
            wchar_t counts[160];
            swprintf_s(counts, 160, L"Eligible %d   Moved %d   Waiting %d", g_eligible, g_trim_moved, g_waiting);
            text.top = column.bottom - zi_px(app, 28);
            text.bottom = column.bottom - zi_px(app, 6);
            DrawTextW(hdc, counts, -1, &text, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        }
        if (g_dup_notice[0]) {
            text.left = column.left + pad;
            text.right = column.right - pad;
            text.top = column.top + zi_px(app, 250);
            text.bottom = column.bottom - zi_px(app, 32);
            DrawTextW(hdc, g_dup_notice, -1, &text, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
        }
        SelectObject(hdc, old);
    } else if (zi_library_date_visible(app)) {
        RECT text = column;
        text.left += pad;
        text.right -= pad;
        text.bottom = column.bottom - zi_px(app, 8);
        text.top = text.bottom - zi_px(app, 110);
        if (app->notice[0]) {
            zi_paint_wrap(app, hdc, text, app->notice, ZI_INK);
        }
    }
    for (i = 0; i < g_hit_count; i++) {
        if (g_hits[i].rc.left >= app->regions.right.left) {
            zi_paint_button(app, hdc, &g_hits[i]);
        }
    }
}

int zi_library_hittest(const ZiApp *app, int x, int y, int *extra) {
    POINT pt;
    int i;

    if (extra) {
        *extra = 0;
    }
    pt.x = x;
    pt.y = y;
    if (app->events_open && PtInRect(&g_event_list, pt) && g_event_row_h > 0) {
        int index = (y - g_event_list.top + g_event_scroll) / g_event_row_h;
        if (index >= 0 && index < g_event_count) {
            if (extra) {
                *extra = index;
            }
            return ZI_HIT_EVENT;
        }
        return ZI_HIT_INERT;
    }
    for (i = g_hit_count - 1; i >= 0; i--) {
        if (!PtInRect(&g_hits[i].rc, pt)) {
            continue;
        }
        if (extra) {
            *extra = g_hits[i].extra;
        }
        return g_hits[i].enabled ? g_hits[i].id : ZI_HIT_INERT;
    }
    return ZI_HIT_NONE;
}

void zi_library_wheel(ZiApp *app, int delta) {
    int row = g_event_row_h > 0 ? g_event_row_h : zi_px(app, 28);
    int view = g_event_list.bottom - g_event_list.top;
    int max_scroll = g_event_count * row - view;

    if (max_scroll < 0) {
        max_scroll = 0;
    }
    g_event_scroll += (delta > 0 ? -row : row) * 3;
    if (g_event_scroll < 0) {
        g_event_scroll = 0;
    }
    if (g_event_scroll > max_scroll) {
        g_event_scroll = max_scroll;
    }
    if (app->hwnd) {
        InvalidateRect(app->hwnd, NULL, FALSE);
    }
}

static void zi_read_edit(HWND edit, wchar_t *dest, size_t cap) {
    dest[0] = L'\0';
    if (!edit) {
        return;
    }
    GetWindowTextW(edit, dest, (int)cap);
    zi_trim_text(dest);
}

static void zi_choose_prefix(ZiApp *app) {
    IFileOpenDialog *dlg = NULL;
    IShellItem *item = NULL;
    IShellItem *start = NULL;
    PWSTR path = NULL;
    DWORD options = 0;
    HRESULT hr;
    wchar_t photos[MAX_PATH];
    wchar_t prefix[512];

    if (!app->library_root[0]) {
        zi_app_notice(app, L"Sign in to \\\\192.168.1.147\\BackupData, then choose the PhotoVideoAudio folder.");
        if (app->hwnd) {
            InvalidateRect(app->hwnd, NULL, FALSE);
        }
        return;
    }
    hr = CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, &IID_IFileOpenDialog, (void **)&dlg);
    if (FAILED(hr) || !dlg) {
        return;
    }
    dlg->lpVtbl->GetOptions(dlg, &options);
    dlg->lpVtbl->SetOptions(dlg, options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dlg->lpVtbl->SetTitle(dlg, L"Choose a folder inside Photos.");
    if (app->library_scan[0]) {
        wcscpy_s(photos, MAX_PATH, app->library_scan);
    } else {
        swprintf_s(photos, MAX_PATH, L"%s\\Photos", app->library_root);
    }
    if (SUCCEEDED(SHCreateItemFromParsingName(photos, NULL, &IID_IShellItem, (void **)&start)) && start) {
        dlg->lpVtbl->SetFolder(dlg, start);
        start->lpVtbl->Release(start);
    }
    hr = dlg->lpVtbl->Show(dlg, app->hwnd);
    if (SUCCEEDED(hr)) {
        hr = dlg->lpVtbl->GetResult(dlg, &item);
    }
    if (SUCCEEDED(hr) && item) {
        hr = item->lpVtbl->GetDisplayName(item, SIGDN_FILESYSPATH, &path);
    }
    if (SUCCEEDED(hr) && path) {
        if (!zi_prefix_from_scan(app->library_scan[0] ? app->library_scan : photos, path, prefix, 512)) {
            zi_copy_notice(g_reorg_notice, 512, L"The plan prefix must be Photos or a folder inside it.");
        } else if (g_prefix_edit) {
            SetWindowTextW(g_prefix_edit, prefix);
            g_reorg_notice[0] = L'\0';
        }
    }
    if (path) {
        CoTaskMemFree(path);
    }
    if (item) {
        item->lpVtbl->Release(item);
    }
    dlg->lpVtbl->Release(dlg);
    if (app->hwnd) {
        InvalidateRect(app->hwnd, NULL, FALSE);
    }
}

static LRESULT CALLBACK zi_delete_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    (void)lparam;
    switch (msg) {
    case WM_COMMAND:
        if (LOWORD(wparam) == IDOK) {
            GetWindowTextW(g_delete_edit, g_delete_text, 64);
            g_delete_ok = 1;
            g_delete_done = 1;
            DestroyWindow(hwnd);
            return 0;
        }
        if (LOWORD(wparam) == IDCANCEL) {
            g_delete_ok = 0;
            g_delete_done = 1;
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    case WM_CLOSE:
        g_delete_ok = 0;
        g_delete_done = 1;
        DestroyWindow(hwnd);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

static int zi_ask_delete(HWND owner, wchar_t *dest, size_t cap) {
    HWND dialog;
    HWND label;
    HWND ok;
    HWND cancel;
    RECT rc;
    MSG msg;
    int width = 460;
    int height = 180;
    HINSTANCE instance = GetModuleHandleW(NULL);

    dest[0] = L'\0';
    g_delete_text[0] = L'\0';
    g_delete_done = 0;
    g_delete_ok = 0;
    GetWindowRect(owner, &rc);
    dialog = CreateWindowExW(WS_EX_DLGMODALFRAME, L"ZImagerDeleteAll", L"ZImager",
                             WS_POPUP | WS_CAPTION | WS_SYSMENU,
                             (rc.left + rc.right - width) / 2, (rc.top + rc.bottom - height) / 2,
                             width, height, owner, NULL, instance, NULL);
    if (!dialog) {
        return 0;
    }
    SetWindowLongPtrW(dialog, GWLP_WNDPROC, (LONG_PTR)zi_delete_proc);
    label = CreateWindowExW(0, L"STATIC", L"Type DELETE to permanently delete every file in the duplicates store.",
                            WS_CHILD | WS_VISIBLE, 16, 16, width - 40, 40, dialog, NULL, instance, NULL);
    g_delete_edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                    16, 64, width - 40, 24, dialog, (HMENU)4104, instance, NULL);
    ok = CreateWindowExW(0, L"BUTTON", L"Delete", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                         width - 190, 104, 80, 28, dialog, (HMENU)IDOK, instance, NULL);
    cancel = CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE,
                             width - 100, 104, 80, 28, dialog, (HMENU)IDCANCEL, instance, NULL);
    if (g_edit_font) {
        SendMessageW(label, WM_SETFONT, (WPARAM)g_edit_font, TRUE);
        SendMessageW(g_delete_edit, WM_SETFONT, (WPARAM)g_edit_font, TRUE);
        SendMessageW(ok, WM_SETFONT, (WPARAM)g_edit_font, TRUE);
        SendMessageW(cancel, WM_SETFONT, (WPARAM)g_edit_font, TRUE);
    }
    EnableWindow(owner, FALSE);
    ShowWindow(dialog, SW_SHOW);
    UpdateWindow(dialog);
    SetFocus(g_delete_edit);
    g_modal = 1;
    while (!g_delete_done && GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (!IsDialogMessageW(dialog, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    if (!g_delete_done) {
        PostQuitMessage(0);
    }
    g_modal = 0;
    EnableWindow(owner, TRUE);
    SetForegroundWindow(owner);
    g_delete_edit = NULL;
    if (g_delete_ok) {
        wcsncpy_s(dest, cap, g_delete_text, _TRUNCATE);
    }
    return g_delete_ok;
}

static void zi_absorb(void) {
    if (g_pending == ZI_PEND_PLAN) {
        if (wcscmp(g_run_state, L"planning") == 0) {
            if (wcscmp(g_reorg_notice, L"The plan request waited 10 seconds. The status line says whether the count is running. Nothing has been copied.") == 0) {
                g_reorg_notice[0] = L'\0';
            }
        } else if (wcscmp(g_run_state, L"plan_ready") == 0 && wcscmp(g_phase, g_pending_prefix) == 0) {
            g_pending = ZI_PEND_NONE;
            if (wcscmp(g_reorg_notice, L"The plan request waited 10 seconds. The status line says whether the count is running. Nothing has been copied.") == 0) {
                g_reorg_notice[0] = L'\0';
            }
        } else if (wcscmp(g_run_state, L"idle") == 0) {
            g_pending = ZI_PEND_NONE;
            if (wcscmp(g_reorg_notice, L"The plan request waited 10 seconds. The status line says whether the count is running. Nothing has been copied.") == 0) {
                zi_copy_notice(g_reorg_notice, 512, L"The helper stopped planning and returned to idle. The plan was not saved. Press Plan to try again.");
            }
        } else {
            g_pending = ZI_PEND_NONE;
        }
        return;
    }
    if (g_pending == ZI_PEND_START) {
        if (wcscmp(g_run_state, L"running") == 0 || wcscmp(g_run_state, L"completed") == 0 || wcscmp(g_run_state, L"blocked") == 0
            || wcscmp(g_run_state, L"paused") == 0 || wcscmp(g_run_state, L"pause_requested") == 0 || wcscmp(g_run_state, L"recovering") == 0) {
            g_pending = ZI_PEND_NONE;
        }
        return;
    }
    if (g_pending == ZI_PEND_PAUSE) {
        if (g_reorg_pause || wcscmp(g_run_state, L"paused") == 0 || wcscmp(g_run_state, L"pause_requested") == 0) {
            g_pending = ZI_PEND_NONE;
        }
        return;
    }
    if (g_pending == ZI_PEND_RESUME) {
        if (wcscmp(g_run_state, L"running") == 0 || wcscmp(g_run_state, L"completed") == 0 || wcscmp(g_run_state, L"blocked") == 0) {
            g_pending = ZI_PEND_NONE;
        }
    }
}

static void zi_apply_reorg(ZiLibNote *note) {
    if (note->have_reorg) {
        g_reorg_known = 1;
        g_reorg_safe = note->reorg_safe;
        g_reorg_pause = note->reorg_pause;
        g_move_n = note->move_n;
        g_placed_n = note->placed_n;
        g_park_n = note->park_n;
        g_leave_n = note->leave_n;
        g_cache_bytes = note->cache_bytes;
        wcscpy_s(g_run_state, 48, note->run_state);
        wcscpy_s(g_phase, 280, note->phase);
        strncpy_s(g_recovery, 64, note->recovery, _TRUNCATE);
    }
    if (note->text[0]) {
        zi_copy_notice(g_reorg_notice, 512, note->text);
    } else if (note->have_reorg && note->kind != ZI_JOB_REORG_STATUS) {
        g_reorg_notice[0] = L'\0';
    }
    zi_absorb();
}

static int zi_note_stale(const ZiApp *app, const ZiLibNote *note) {
    switch (note->kind) {
    case ZI_JOB_EVENTS:
    case ZI_JOB_RENAME:
        return !app->events_open;
    case ZI_JOB_REORG_STATUS:
    case ZI_JOB_REORG_PLAN:
    case ZI_JOB_REORG_START:
    case ZI_JOB_REORG_PAUSE:
    case ZI_JOB_REORG_RESUME:
        return !app->reorg_open;
    case ZI_JOB_REVIEW:
        return app->library_section != 1 || app->events_open || app->reorg_open;
    case ZI_JOB_DUP_LIST:
    case ZI_JOB_DUP_SCAN:
    case ZI_JOB_DUP_POLL:
    case ZI_JOB_TRIM_STATUS:
    case ZI_JOB_TRIM_START:
    case ZI_JOB_TRIM_PAUSE:
    case ZI_JOB_TRIM_CONTINUE:
    case ZI_JOB_RESTORE:
    case ZI_JOB_PIN:
    case ZI_JOB_PURGE:
    case ZI_JOB_PURGE_ALL:
        return app->library_section != 2 || app->events_open || app->reorg_open;
    default:
        return 0;
    }
}

void zi_library_on_note(ZiApp *app, void *payload) {
    ZiLibNote *note = (ZiLibNote *)payload;
    wchar_t previous[48];

    if (!note) {
        return;
    }
    /* This read does not own g_job_busy. Applying it must not clear another job. */
    if (note->kind == ZI_JOB_PASS) {
        if (g_alive && app && app->source_server && note->pass_serial == InterlockedOr(&g_pass_serial, 0)) {
            if (note->pass_code == 2) {
                app->pass_line[0] = L'\0';
            } else if (note->pass_code == 1) {
                zi_copy_notice(app->pass_line, 192, note->text);
            }
        }
        zi_note_free(note);
        if (g_alive && app && app->hwnd) {
            InvalidateRect(app->hwnd, NULL, FALSE);
        }
        return;
    }
    if (!g_alive || !app) {
        zi_note_free(note);
        InterlockedExchange(&g_job_busy, 0);
        return;
    }
    if (note->done) {
        InterlockedExchange(&g_job_busy, 0);
        app->date_busy = 0;
        app->removal_busy = 0;
        app->clean_busy = 0;
        app->dup_busy = 0;
    }
    if (!note->done) {
        if (!zi_note_stale(app, note) && note->text[0]) {
            if (note->kind == ZI_JOB_DATE || note->kind == ZI_JOB_REMOVE || note->kind == ZI_JOB_CLEAN) {
                zi_app_notice(app, note->text);
            }
        }
        zi_note_free(note);
        if (app->hwnd) {
            InvalidateRect(app->hwnd, NULL, FALSE);
        }
        return;
    }
    if (zi_note_stale(app, note)) {
        zi_note_free(note);
        if (app->hwnd) {
            InvalidateRect(app->hwnd, NULL, FALSE);
        }
        return;
    }
    switch (note->kind) {
    case ZI_JOB_EVENTS:
        g_event_count = 0;
        if (note->events && note->event_count > 0) {
            int n = note->event_count;
            if (n > ZI_LIB_CAP) {
                n = ZI_LIB_CAP;
            }
            memcpy(g_events, note->events, (size_t)n * sizeof(ZiEvent));
            g_event_count = n;
        }
        if (app->event_selected >= g_event_count) {
            app->event_selected = -1;
        }
        break;
    case ZI_JOB_RENAME:
        if (note->text[0]) {
            zi_app_notice(app, note->text);
        } else if (note->events && note->event_count == 1) {
            int i;
            for (i = 0; i < g_event_count; i++) {
                if (strcmp(g_events[i].uid, note->events[0].uid) == 0) {
                    wcscpy_s(g_events[i].name, 180, note->events[0].name);
                    break;
                }
            }
        }
        break;
    case ZI_JOB_REORG_STATUS:
    case ZI_JOB_REORG_PLAN:
    case ZI_JOB_REORG_START:
    case ZI_JOB_REORG_PAUSE:
    case ZI_JOB_REORG_RESUME:
        zi_apply_reorg(note);
        break;
    case ZI_JOB_REVIEW:
        zi_copy_notice(app->grid_note, 320, note->text);
        zi_grid_install(app, note->files, note->file_count);
        note->files = NULL;
        zi_editor_sync_details(app);
        break;
    case ZI_JOB_DUP_LIST:
    case ZI_JOB_RESTORE:
    case ZI_JOB_PIN:
    case ZI_JOB_PURGE:
    case ZI_JOB_PURGE_ALL:
        if (note->files || note->reload || note->kind == ZI_JOB_DUP_LIST) {
            zi_grid_install(app, note->files, note->file_count);
            note->files = NULL;
            if (note->file_count == 0 && !note->text[0]) {
                zi_copy_notice(app->grid_note, 320, L"This list shows copies gathered into Photos/_Duplicates. Find duplicates looks for matches. Start gathers the extra copies into this list.");
            } else {
                app->grid_note[0] = L'\0';
            }
            zi_editor_sync_details(app);
        }
        if (note->text[0]) {
            zi_copy_notice(g_dup_notice, 512, note->text);
            if (note->file_count == 0) {
                zi_copy_notice(app->grid_note, 320, note->text);
            }
        }
        break;
    case ZI_JOB_DUP_SCAN:
    case ZI_JOB_DUP_POLL:
        if (note->have_scan) {
            g_scan_running = wcscmp(note->scan_state, L"running") == 0;
            zi_scan_line(note, g_scan_line, 180);
        } else {
            g_scan_running = 0;
            g_scan_line[0] = L'\0';
        }
        if (note->reload) {
            zi_grid_install(app, note->files, note->file_count);
            note->files = NULL;
            zi_editor_sync_details(app);
        }
        if (note->text[0]) {
            zi_copy_notice(g_dup_notice, 512, note->text);
            if (app->file_count == 0) {
                zi_copy_notice(app->grid_note, 320, note->text);
            } else {
                app->grid_note[0] = L'\0';
            }
        }
        break;
    case ZI_JOB_TRIM_STATUS:
    case ZI_JOB_TRIM_START:
    case ZI_JOB_TRIM_PAUSE:
    case ZI_JOB_TRIM_CONTINUE:
        wcscpy_s(previous, 48, g_trim_state);
        if (note->have_trim) {
            g_trim_known = 1;
            g_trim_safe = note->trim_safe;
            g_trim_pause = note->trim_pause;
            g_eligible = note->eligible;
            g_trim_moved = note->trim_moved;
            g_waiting = note->waiting;
            wcscpy_s(g_trim_state, 48, note->trim_state);
        }
        if (note->text[0]) {
            zi_copy_notice(g_dup_notice, 512, note->text);
        } else if (note->have_trim && note->kind != ZI_JOB_TRIM_STATUS) {
            g_dup_notice[0] = L'\0';
        }
        if (note->kind == ZI_JOB_TRIM_STATUS && g_trim_known) {
            int was_running = wcscmp(previous, L"running") == 0 || wcscmp(previous, L"pause_requested") == 0 || wcscmp(previous, L"recovering") == 0;
            int now_running = wcscmp(g_trim_state, L"running") == 0 || wcscmp(g_trim_state, L"pause_requested") == 0 || wcscmp(g_trim_state, L"recovering") == 0;
            if (was_running && !now_running && !zi_job_busy()) {
                ZiLibJob *job = zi_job(ZI_JOB_DUP_LIST);
                if (job) {
                    zi_start(app, job);
                }
            }
        }
        break;
    case ZI_JOB_CLEAN:
        zi_app_notice(app, note->text);
        if (note->reload) {
            zi_grid_clear(app);
            zi_grid_load(app);
        }
        break;
    case ZI_JOB_DATE:
    case ZI_JOB_REMOVE:
        zi_app_notice(app, note->text);
        if (note->reload && app->library_section == 0 && !app->events_open && !app->reorg_open) {
            zi_grid_clear(app);
            zi_grid_load(app);
        } else if (note->reload && app->library_section == 1) {
            ZiLibJob *job = zi_job(ZI_JOB_REVIEW);
            if (job && !zi_job_busy()) {
                zi_start(app, job);
            } else {
                free(job);
            }
        }
        break;
    default:
        break;
    }
    zi_note_free(note);
    if (app->hwnd) {
        InvalidateRect(app->hwnd, NULL, FALSE);
    }
}

void zi_library_on_timer(ZiApp *app) {
    ZiLibJob *job;

    if (!app || g_modal || zi_job_busy() || !g_alive) {
        return;
    }
    if (app->reorg_open) {
        job = zi_job(ZI_JOB_REORG_STATUS);
        if (job) {
            zi_start(app, job);
        }
        return;
    }
    if (app->source_server && zi_showing_folder(app) && app->library_section == 2 && !app->events_open) {
        if (g_scan_running) {
            job = zi_job(ZI_JOB_DUP_POLL);
            if (job) {
                zi_start(app, job);
            }
            return;
        }
        job = zi_job(ZI_JOB_TRIM_STATUS);
        if (job) {
            zi_start(app, job);
        }
    }
}

static void zi_click_plan(ZiApp *app) {
    wchar_t typed[512];
    wchar_t prefix[512];
    ZiLibJob *job;

    zi_read_edit(g_prefix_edit, typed, 512);
    zi_folder_prefix(typed, prefix, 512);
    if (g_prefix_edit) {
        SetWindowTextW(g_prefix_edit, prefix);
    }
    if (!zi_inside_photos(prefix)) {
        zi_copy_notice(g_reorg_notice, 512, L"The plan prefix must be Photos or a folder inside it.");
        InvalidateRect(app->hwnd, NULL, FALSE);
        return;
    }
    g_pending = ZI_PEND_PLAN;
    wcscpy_s(g_pending_prefix, 280, prefix);
    g_reorg_notice[0] = L'\0';
    job = zi_job(ZI_JOB_REORG_PLAN);
    if (!job) {
        return;
    }
    wcscpy_s(job->text, 512, prefix);
    zi_start(app, job);
}

static void zi_click_start(ZiApp *app) {
    wchar_t typed_raw[512];
    wchar_t typed[512];
    wchar_t saved[280];
    ZiLibJob *job;

    if (g_pending == ZI_PEND_PLAN) {
        zi_copy_notice(g_reorg_notice, 512, wcscmp(g_run_state, L"planning") == 0
            ? L"Start was not sent. The helper is counting files. Wait until the status line says the plan is ready."
            : L"Start was not sent. The plan request is still open. Wait until the status line says the plan is ready.");
        InvalidateRect(app->hwnd, NULL, FALSE);
        return;
    }
    zi_read_edit(g_prefix_edit, typed_raw, 512);
    zi_folder_prefix(typed_raw, typed, 512);
    if (!zi_saved_folder(g_phase, saved, 280) || wcscmp(saved, typed) != 0) {
        wchar_t temp[512];
        if (g_reorg_known && zi_saved_folder(g_phase, saved, 280)) {
            swprintf_s(temp, 512, L"Start was not sent. The field is %s and the saved plan is %s. Press Plan for %s.", typed, saved, typed);
        } else {
            swprintf_s(temp, 512, L"Start was not sent. %s has no plan yet. Press Plan. Start only copies a folder after Plan saves it.", typed);
        }
        zi_copy_notice(g_reorg_notice, 512, temp);
        InvalidateRect(app->hwnd, NULL, FALSE);
        return;
    }
    if (!zi_confirm(app->hwnd, L"Start reorganize for this folder? Files will be copied into the planned folders.")) {
        return;
    }
    if (zi_job_busy()) {
        return;
    }
    g_pending = ZI_PEND_START;
    g_reorg_notice[0] = L'\0';
    job = zi_job(ZI_JOB_REORG_START);
    if (job) {
        zi_start(app, job);
    }
}

static void zi_click_pause(ZiApp *app) {
    ZiLibJob *job;

    g_pending = ZI_PEND_PAUSE;
    g_reorg_notice[0] = L'\0';
    job = zi_job(ZI_JOB_REORG_PAUSE);
    if (job) {
        zi_start(app, job);
    }
}

static void zi_click_resume(ZiApp *app) {
    ZiLibJob *job;

    g_pending = ZI_PEND_RESUME;
    g_reorg_notice[0] = L'\0';
    job = zi_job(ZI_JOB_REORG_RESUME);
    if (job) {
        zi_start(app, job);
    }
}

static void zi_click_rename(ZiApp *app) {
    wchar_t name[180];
    ZiLibJob *job;

    if (app->event_selected < 0 || app->event_selected >= g_event_count) {
        return;
    }
    zi_read_edit(g_event_edit, name, 180);
    if (!name[0]) {
        return;
    }
    job = zi_job(ZI_JOB_RENAME);
    if (!job) {
        return;
    }
    strncpy_s(job->uid, 64, g_events[app->event_selected].uid, _TRUNCATE);
    wcscpy_s(job->text, 512, name);
    zi_start(app, job);
}

static void zi_click_date(ZiApp *app) {
    wchar_t typed[80];
    char wire[80];
    ZiLibItem *items = NULL;
    int count;
    ZiLibJob *job;

    zi_read_edit(g_date_edit, typed, 80);
    if (!zi_capture_request(typed, wire, sizeof wire)) {
        zi_app_notice(app, L"The date was not saved. Use YYYY-MM-DD or a four-digit year.");
        InvalidateRect(app->hwnd, NULL, FALSE);
        return;
    }
    count = zi_collect_marks(app, -1, 0, &items);
    if (count <= 0) {
        zi_app_notice(app, L"The date was not saved.");
        InvalidateRect(app->hwnd, NULL, FALSE);
        return;
    }
    if (!zi_confirm(app->hwnd, L"Move the selected files into their date folders?")) {
        free(items);
        return;
    }
    if (zi_job_busy()) {
        free(items);
        return;
    }
    job = zi_job(ZI_JOB_DATE);
    if (!job) {
        free(items);
        return;
    }
    job->items = items;
    job->count = count;
    wcscpy_s(job->text, 512, typed);
    app->date_busy = 1;
    zi_app_notice(app, L"Moving 1 of 1 to its date folder.");
    zi_start(app, job);
    if (!zi_job_busy()) {
        app->date_busy = 0;
    }
}

void zi_library_clean(ZiApp *app) {
    wchar_t prefix[512];
    ZiLibJob *job;

    if (!zi_library_clean_ready(app)) {
        return;
    }
    if (!zi_current_prefix(app, prefix, 512)) {
        zi_app_notice(app, L"The display folder must be Photos or a folder inside it.");
        InvalidateRect(app->hwnd, NULL, FALSE);
        return;
    }
    job = zi_job(ZI_JOB_CLEAN);
    if (!job) {
        return;
    }
    wcscpy_s(job->text, 512, prefix);
    app->clean_busy = 1;
    zi_app_notice(app, L"Cleaning leftover records.");
    zi_start(app, job);
    if (!zi_job_busy()) {
        app->clean_busy = 0;
    }
}

void zi_library_remove(ZiApp *app, int index) {
    ZiLibItem *items = NULL;
    int count;
    ZiLibJob *job;

    if (!zi_library_can_remove(app, index)) {
        return;
    }
    count = zi_collect_marks(app, index, 1, &items);
    if (count <= 0) {
        return;
    }
    if (!zi_confirm(app->hwnd, L"Move the selected files to Photos/_Removed?")) {
        free(items);
        return;
    }
    if (zi_job_busy()) {
        free(items);
        return;
    }
    job = zi_job(ZI_JOB_REMOVE);
    if (!job) {
        free(items);
        return;
    }
    job->items = items;
    job->count = count;
    app->removal_busy = 1;
    zi_start(app, job);
    if (!zi_job_busy()) {
        app->removal_busy = 0;
    }
}

static ZiFile *zi_selected_dup(ZiApp *app) {
    if (app->file_selected < 0 || app->file_selected >= app->file_count) {
        return NULL;
    }
    return &app->files[app->file_selected];
}

static void zi_click_trim(ZiApp *app, int kind, const wchar_t *confirm) {
    ZiLibJob *job;

    if (confirm && !zi_confirm(app->hwnd, confirm)) {
        return;
    }
    if (zi_job_busy()) {
        return;
    }
    job = zi_job(kind);
    if (!job) {
        return;
    }
    app->dup_busy = 1;
    zi_start(app, job);
    if (!zi_job_busy()) {
        app->dup_busy = 0;
    }
}

static void zi_click_find(ZiApp *app) {
    ZiLibJob *job;

    if (zi_job_busy()) {
        return;
    }
    g_scan_running = 1;
    job = zi_job(ZI_JOB_DUP_SCAN);
    if (!job) {
        g_scan_running = 0;
        return;
    }
    app->dup_busy = 1;
    zi_start(app, job);
    if (!zi_job_busy()) {
        app->dup_busy = 0;
        g_scan_running = 0;
    }
}

static void zi_click_restore(ZiApp *app) {
    ZiFile *file = zi_selected_dup(app);
    ZiLibJob *job;

    if (!file || !zi_uid_ok(file->uid)) {
        zi_copy_notice(g_dup_notice, 512, L"The file was not restored.");
        InvalidateRect(app->hwnd, NULL, FALSE);
        return;
    }
    if (!zi_confirm(app->hwnd, L"Restore this file and pin it?")) {
        return;
    }
    if (zi_job_busy()) {
        return;
    }
    job = zi_job(ZI_JOB_RESTORE);
    if (!job) {
        return;
    }
    strncpy_s(job->uid, 64, file->uid, _TRUNCATE);
    app->dup_busy = 1;
    zi_start(app, job);
    if (!zi_job_busy()) {
        app->dup_busy = 0;
    }
}

static void zi_click_pin(ZiApp *app) {
    ZiFile *file = zi_selected_dup(app);
    ZiLibJob *job;

    if (!file || !zi_uid_ok(file->uid)) {
        zi_copy_notice(g_dup_notice, 512, L"The pin was not saved.");
        InvalidateRect(app->hwnd, NULL, FALSE);
        return;
    }
    if (zi_job_busy()) {
        return;
    }
    job = zi_job(ZI_JOB_PIN);
    if (!job) {
        return;
    }
    strncpy_s(job->uid, 64, file->uid, _TRUNCATE);
    job->pinned = !file->pinned;
    app->dup_busy = 1;
    zi_start(app, job);
    if (!zi_job_busy()) {
        app->dup_busy = 0;
    }
}

static void zi_click_delete_one(ZiApp *app) {
    ZiFile *file = zi_selected_dup(app);
    const wchar_t *base;
    ZiLibJob *job;

    if (!file || !zi_uid_ok(file->uid)) {
        zi_copy_notice(g_dup_notice, 512, L"The file was not deleted.");
        InvalidateRect(app->hwnd, NULL, FALSE);
        return;
    }
    if (!zi_confirm(app->hwnd, L"Delete this duplicate from the store? This cannot be undone.")) {
        return;
    }
    if (zi_job_busy()) {
        return;
    }
    base = wcsrchr(file->path, L'\\');
    base = (base && base[1]) ? base + 1 : file->name;
    job = zi_job(ZI_JOB_PURGE);
    if (!job) {
        return;
    }
    strncpy_s(job->uid, 64, file->uid, _TRUNCATE);
    wcsncpy_s(job->text, 512, base, _TRUNCATE);
    app->dup_busy = 1;
    zi_start(app, job);
    if (!zi_job_busy()) {
        app->dup_busy = 0;
    }
}

static void zi_click_delete_all(ZiApp *app) {
    wchar_t typed[64];
    ZiLibJob *job;

    if (!zi_ask_delete(app->hwnd, typed, 64)) {
        return;
    }
    if (wcscmp(typed, L"DELETE") != 0) {
        zi_copy_notice(g_dup_notice, 512, L"Type DELETE to delete every duplicate.");
        InvalidateRect(app->hwnd, NULL, FALSE);
        return;
    }
    if (zi_job_busy()) {
        return;
    }
    job = zi_job(ZI_JOB_PURGE_ALL);
    if (!job) {
        return;
    }
    wcscpy_s(job->text, 512, L"DELETE");
    app->dup_busy = 1;
    zi_start(app, job);
    if (!zi_job_busy()) {
        app->dup_busy = 0;
    }
}

void zi_library_click(ZiApp *app, int id, int extra) {
    if (id == ZI_HIT_NONE || id == ZI_HIT_INERT || !app) {
        return;
    }
    switch (id) {
    case ZI_HIT_EVENT:
        app->event_selected = extra;
        if (extra >= 0 && extra < g_event_count && g_event_edit) {
            SetWindowTextW(g_event_edit, g_events[extra].name);
        }
        InvalidateRect(app->hwnd, NULL, FALSE);
        break;
    case ZI_HIT_RENAME:
        zi_click_rename(app);
        break;
    case ZI_HIT_PLAN:
        zi_click_plan(app);
        break;
    case ZI_HIT_START:
        zi_click_start(app);
        break;
    case ZI_HIT_PAUSE:
        zi_click_pause(app);
        break;
    case ZI_HIT_RESUME:
        zi_click_resume(app);
        break;
    case ZI_HIT_CHOOSE:
        zi_choose_prefix(app);
        break;
    case ZI_HIT_DATE:
        zi_click_date(app);
        break;
    case ZI_HIT_FIND:
        zi_click_find(app);
        break;
    case ZI_HIT_TRIM_START:
        zi_click_trim(app, ZI_JOB_TRIM_START, L"Move the extra copies into Photos/_Duplicates? One keeper stays in its date folder.");
        break;
    case ZI_HIT_TRIM_PAUSE:
        zi_click_trim(app, ZI_JOB_TRIM_PAUSE, NULL);
        break;
    case ZI_HIT_TRIM_CONTINUE:
        zi_click_trim(app, ZI_JOB_TRIM_CONTINUE, L"Resume gathering duplicates into Photos/_Duplicates?");
        break;
    case ZI_HIT_RESTORE:
        zi_click_restore(app);
        break;
    case ZI_HIT_PIN:
        zi_click_pin(app);
        break;
    case ZI_HIT_DELETE:
        zi_click_delete_one(app);
        break;
    case ZI_HIT_DELETE_ALL:
        zi_click_delete_all(app);
        break;
    default:
        break;
    }
}
