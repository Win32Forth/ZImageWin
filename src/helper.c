#include "helper.h"

#include <bcrypt.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wincrypt.h>
#include <winhttp.h>

static CRITICAL_SECTION g_pin_lock;
static unsigned char g_pin[32];
static int g_has_pin;
static volatile LONG g_busy;
static volatile LONG g_alive;
static int g_lock_ready;

static void zi_sha256(const unsigned char *data, size_t len, unsigned char out[32]) {
    BCRYPT_ALG_HANDLE alg = NULL;

    ZeroMemory(out, 32);
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) != 0) {
        return;
    }
    BCryptHash(alg, NULL, 0, (PUCHAR)data, (ULONG)len, out, 32);
    BCryptCloseAlgorithmProvider(alg, 0);
}

static int zi_read_tlv(const unsigned char *buf, size_t len, size_t *index, const unsigned char **full, size_t *full_len, const unsigned char **content, size_t *content_len, unsigned *tag) {
    size_t i = *index;
    size_t start;
    size_t length;
    size_t nbytes;

    if (i >= len) {
        return 0;
    }
    start = i;
    *tag = buf[i++];
    if (i >= len) {
        return 0;
    }
    if ((buf[i] & 0x80) == 0) {
        length = buf[i++];
    } else {
        nbytes = buf[i++] & 0x7f;
        if (nbytes == 0 || nbytes > 4 || i + nbytes > len) {
            return 0;
        }
        length = 0;
        while (nbytes--) {
            length = (length << 8) | buf[i++];
        }
    }
    if (length > len || i + length > len) {
        return 0;
    }
    *full = buf + start;
    *full_len = (i + length) - start;
    *content = buf + i;
    *content_len = length;
    *index = i + length;
    return 1;
}

/* SHA-256 of subjectPublicKeyInfo, the same bytes the Mac pins. */
static int zi_spki_pin(const unsigned char *der, size_t len, unsigned char out[32]) {
    size_t index = 0;
    const unsigned char *full;
    const unsigned char *content;
    size_t full_len;
    size_t content_len;
    unsigned tag;
    const unsigned char *tbs;
    size_t tbs_len;
    const unsigned char *child_full[12];
    size_t child_full_len[12];
    unsigned child_tag[12];
    int count = 0;
    int spki;

    if (!zi_read_tlv(der, len, &index, &full, &full_len, &content, &content_len, &tag) || tag != 0x30) {
        return 0;
    }
    index = 0;
    if (!zi_read_tlv(content, content_len, &index, &full, &full_len, &tbs, &tbs_len, &tag) || tag != 0x30) {
        return 0;
    }
    index = 0;
    while (index < tbs_len && count < 12) {
        const unsigned char *child;
        size_t child_len;
        if (!zi_read_tlv(tbs, tbs_len, &index, &child_full[count], &child_full_len[count], &child, &child_len, &child_tag[count])) {
            return 0;
        }
        count++;
    }
    if (count == 0) {
        return 0;
    }
    spki = child_tag[0] == 0xA0 ? 6 : 5;
    if (spki >= count || child_tag[spki] != 0x30) {
        return 0;
    }
    zi_sha256(child_full[spki], child_full_len[spki], out);
    return 1;
}

static void zi_config_path(wchar_t *path, size_t cap) {
    wchar_t *local = NULL;

    if (SUCCEEDED(SHGetKnownFolderPath(&FOLDERID_LocalAppData, 0, NULL, &local)) && local) {
        swprintf_s(path, cap, L"%s\\ZImageWin", local);
        CreateDirectoryW(path, NULL);
        swprintf_s(path, cap, L"%s\\ZImageWin\\helper.ini", local);
        CoTaskMemFree(local);
        return;
    }
    wcscpy_s(path, cap, L"helper.ini");
}

/* The bearer file is the DPAPI blob. The token is never written as text. */
static int zi_auth_path(wchar_t *path, size_t cap) {
    wchar_t *local = NULL;

    if (SUCCEEDED(SHGetKnownFolderPath(&FOLDERID_LocalAppData, 0, NULL, &local)) && local) {
        swprintf_s(path, cap, L"%s\\ZImageWin", local);
        CreateDirectoryW(path, NULL);
        swprintf_s(path, cap, L"%s\\ZImageWin\\helper.auth", local);
        CoTaskMemFree(local);
        return 1;
    }
    return 0;
}

static int zi_save_bearer(const char *token) {
    DATA_BLOB in;
    DATA_BLOB out;
    wchar_t path[MAX_PATH];
    HANDLE file;
    DWORD written = 0;
    int ok = 0;

    if (!token || !token[0] || !zi_auth_path(path, MAX_PATH)) {
        return 0;
    }
    in.pbData = (BYTE *)token;
    in.cbData = (DWORD)strlen(token);
    ZeroMemory(&out, sizeof out);
    if (!CryptProtectData(&in, L"ZImage NAS Helper", NULL, NULL, NULL, CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        return 0;
    }
    file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file != INVALID_HANDLE_VALUE) {
        if (WriteFile(file, out.pbData, out.cbData, &written, NULL) && written == out.cbData) {
            ok = 1;
        }
        CloseHandle(file);
        if (!ok) {
            DeleteFileW(path);
        }
    }
    if (out.pbData && out.cbData) {
        SecureZeroMemory(out.pbData, out.cbData);
    }
    if (out.pbData) {
        LocalFree(out.pbData);
    }
    return ok;
}

static int zi_load_bearer(char *token, size_t cap) {
    wchar_t path[MAX_PATH];
    HANDLE file;
    DWORD size;
    DWORD read = 0;
    BYTE *blob = NULL;
    DATA_BLOB in;
    DATA_BLOB out;
    int ok = 0;

    if (cap < 2) {
        return 0;
    }
    token[0] = 0;
    if (!zi_auth_path(path, MAX_PATH)) {
        return 0;
    }
    file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        return 0;
    }
    size = GetFileSize(file, NULL);
    if (size == 0 || size == INVALID_FILE_SIZE || size > 4096) {
        CloseHandle(file);
        return 0;
    }
    blob = (BYTE *)malloc(size);
    if (!blob || !ReadFile(file, blob, size, &read, NULL) || read != size) {
        free(blob);
        CloseHandle(file);
        return 0;
    }
    CloseHandle(file);
    in.pbData = blob;
    in.cbData = size;
    ZeroMemory(&out, sizeof out);
    if (CryptUnprotectData(&in, NULL, NULL, NULL, NULL, CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        if (out.cbData > 0 && (size_t)out.cbData < cap) {
            memcpy(token, out.pbData, out.cbData);
            token[out.cbData] = 0;
            ok = token[0] != 0;
        }
        if (out.pbData && out.cbData) {
            SecureZeroMemory(out.pbData, out.cbData);
        }
        if (out.pbData) {
            LocalFree(out.pbData);
        }
    }
    SecureZeroMemory(blob, size);
    free(blob);
    return ok;
}

static int zi_hex_value(wchar_t ch) {
    if (ch >= L'0' && ch <= L'9') return ch - L'0';
    if (ch >= L'a' && ch <= L'f') return ch - L'a' + 10;
    if (ch >= L'A' && ch <= L'F') return ch - L'A' + 10;
    return -1;
}

static void zi_load_pin(void) {
    wchar_t path[MAX_PATH];
    FILE *file;
    wchar_t line[128];

    g_has_pin = 0;
    ZeroMemory(g_pin, sizeof g_pin);
    zi_config_path(path, MAX_PATH);
    if (_wfopen_s(&file, path, L"rt") != 0 || !file) {
        return;
    }
    while (fgetws(line, 128, file)) {
        unsigned char pin[32];
        int i;
        if (wcsncmp(line, L"pin=", 4) != 0) {
            continue;
        }
        if (wcslen(line) < 4 + 64) {
            continue;
        }
        for (i = 0; i < 32; i++) {
            int hi = zi_hex_value(line[4 + i * 2]);
            int lo = zi_hex_value(line[4 + i * 2 + 1]);
            if (hi < 0 || lo < 0) {
                break;
            }
            pin[i] = (unsigned char)((hi << 4) | lo);
        }
        if (i == 32) {
            memcpy(g_pin, pin, 32);
            g_has_pin = 1;
        }
    }
    fclose(file);
}

static void zi_save_pin(const unsigned char pin[32]) {
    wchar_t path[MAX_PATH];
    FILE *file;
    int i;

    zi_config_path(path, MAX_PATH);
    if (_wfopen_s(&file, path, L"wt") != 0 || !file) {
        return;
    }
    fwprintf(file, L"host=%s\nport=%d\npin=", ZI_HELPER_HOST, ZI_HELPER_PORT);
    for (i = 0; i < 32; i++) {
        fwprintf(file, L"%02x", pin[i]);
    }
    fwprintf(file, L"\n");
    fclose(file);
}

void zi_helper_init(void) {
    if (!g_lock_ready) {
        InitializeCriticalSection(&g_pin_lock);
        g_lock_ready = 1;
    }
    g_alive = 1;
    zi_load_pin();
}

void zi_helper_shutdown(void) {
    g_alive = 0;
}

static const char *zi_json_value(const char *json, const char *key) {
    char pattern[64];
    const char *found;

    sprintf_s(pattern, sizeof pattern, "\"%s\"", key);
    found = strstr(json, pattern);
    if (!found) {
        return NULL;
    }
    found += strlen(pattern);
    while (*found == ' ' || *found == '\t' || *found == '\r' || *found == '\n') {
        found++;
    }
    if (*found != ':') {
        return NULL;
    }
    found++;
    while (*found == ' ' || *found == '\t' || *found == '\r' || *found == '\n') {
        found++;
    }
    return found;
}

static int zi_json_bool(const char *json, const char *key) {
    const char *value = zi_json_value(json, key);
    return value && strncmp(value, "true", 4) == 0;
}

static void zi_offline(ZiHelperNote *note, const wchar_t *text) {
    note->online = 0;
    note->library_ok = 0;
    note->safe = 0;
    wcscpy_s(note->line, 192, text);
}

static int zi_read_body(HINTERNET request, char *body, DWORD cap) {
    DWORD used = 0;

    body[0] = 0;
    for (;;) {
        DWORD got = 0;
        if (used + 1 >= cap) {
            break;
        }
        if (!WinHttpReadData(request, body + used, cap - 1 - used, &got)) {
            return 0;
        }
        if (got == 0) {
            break;
        }
        used += got;
    }
    body[used] = 0;
    return 1;
}

static int zi_pin_ok(HINTERNET request, ZiHelperNote *note) {
    PCCERT_CONTEXT cert = NULL;
    DWORD size = sizeof cert;
    unsigned char pin[32];
    int ok = 0;

    if (!WinHttpQueryOption(request, WINHTTP_OPTION_SERVER_CERT_CONTEXT, &cert, &size) || !cert) {
        zi_offline(note, L"192.168.1.147   The helper certificate could not be read.");
        return 0;
    }
    if (!zi_spki_pin(cert->pbCertEncoded, cert->cbCertEncoded, pin)) {
        CertFreeCertificateContext(cert);
        zi_offline(note, L"192.168.1.147   The helper certificate could not be read.");
        return 0;
    }
    EnterCriticalSection(&g_pin_lock);
    if (!g_has_pin) {
        memcpy(g_pin, pin, 32);
        g_has_pin = 1;
        memcpy(note->pin, pin, 32);
        note->learned_pin = 1;
        zi_save_pin(pin);
        ok = 1;
    } else if (memcmp(g_pin, pin, 32) == 0) {
        ok = 1;
    }
    LeaveCriticalSection(&g_pin_lock);
    CertFreeCertificateContext(cert);
    if (!ok) {
        zi_offline(note, L"192.168.1.147   Helper certificate changed. Pair again.");
    }
    return ok;
}

static HINTERNET zi_open(HINTERNET *session, HINTERNET *connect, const wchar_t *verb, const wchar_t *path) {
    HINTERNET request;
    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
    DWORD security = SECURITY_FLAG_IGNORE_UNKNOWN_CA | SECURITY_FLAG_IGNORE_CERT_CN_INVALID | SECURITY_FLAG_IGNORE_CERT_DATE_INVALID;

    *session = WinHttpOpen(L"ZImager", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!*session) {
        return NULL;
    }
    WinHttpSetTimeouts(*session, 4000, 4000, 4000, 8000);
    WinHttpSetOption(*session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof protocols);
    *connect = WinHttpConnect(*session, ZI_HELPER_HOST, ZI_HELPER_PORT, 0);
    if (!*connect) {
        return NULL;
    }
    request = WinHttpOpenRequest(*connect, verb, path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!request) {
        return NULL;
    }
    WinHttpSetOption(request, WINHTTP_OPTION_SECURITY_FLAGS, &security, sizeof security);
    return request;
}

static void zi_close(HINTERNET request, HINTERNET connect, HINTERNET session) {
    if (request) WinHttpCloseHandle(request);
    if (connect) WinHttpCloseHandle(connect);
    if (session) WinHttpCloseHandle(session);
}

int zi_helper_fetch(ZiHelperNote *note) {
    HINTERNET session = NULL;
    HINTERNET connect = NULL;
    HINTERNET request;
    char body[4096];
    DWORD status = 0;
    DWORD status_size = sizeof status;
    const char *state;

    ZeroMemory(note, sizeof *note);
    request = zi_open(&session, &connect, L"GET", L"/v1/health");
    if (!request || !WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, NULL, 0, 0, 0) || !WinHttpReceiveResponse(request, NULL)) {
        zi_offline(note, L"192.168.1.147   Helper offline. Open DSM Task Scheduler and confirm the boot task still runs zimage-helper.");
        zi_close(request, connect, session);
        return 0;
    }
    WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size, WINHTTP_NO_HEADER_INDEX);
    if (!zi_pin_ok(request, note)) {
        zi_close(request, connect, session);
        return 0;
    }
    if (status != 200 || !zi_read_body(request, body, sizeof body)) {
        zi_offline(note, L"192.168.1.147   Helper offline. Open DSM Task Scheduler and confirm the boot task still runs zimage-helper.");
        zi_close(request, connect, session);
        return 0;
    }
    zi_close(request, connect, session);
    state = zi_json_value(body, "status");
    note->library_ok = zi_json_bool(body, "libraryRootOK");
    note->safe = zi_json_bool(body, "manualOpsClear") && zi_json_bool(body, "reorgSafeToReboot") && zi_json_bool(body, "trimSafeToReboot");
    if (state && strncmp(state, "\"ok\"", 4) == 0 && note->library_ok) {
        note->online = 1;
        wcscpy_s(note->line, 192, note->safe
            ? L"192.168.1.147   Helper online. Safe to reboot."
            : L"192.168.1.147   Helper online. Not safe to reboot.");
        return 1;
    }
    if (state && strncmp(state, "\"library_unavailable\"", 21) == 0) {
        note->online = 1;
        wcscpy_s(note->line, 192, L"192.168.1.147   Helper online. The library folder is not available.");
        return 1;
    }
    zi_offline(note, L"192.168.1.147   Helper offline. Open DSM Task Scheduler and confirm the boot task still runs zimage-helper.");
    return 0;
}

static DWORD WINAPI zi_helper_thread(LPVOID param) {
    HWND hwnd = (HWND)param;
    ZiHelperNote *note = (ZiHelperNote *)calloc(1, sizeof *note);

    if (!note) {
        InterlockedExchange(&g_busy, 0);
        return 0;
    }
    zi_helper_fetch(note);
    if (!g_alive || !PostMessageW(hwnd, ZI_WM_HELPER, 0, (LPARAM)note)) {
        free(note);
    }
    InterlockedExchange(&g_busy, 0);
    return 0;
}

void zi_helper_refresh(HWND hwnd) {
    HANDLE thread;

    if (!hwnd || InterlockedCompareExchange(&g_busy, 1, 0) != 0) {
        return;
    }
    thread = CreateThread(NULL, 0, zi_helper_thread, hwnd, 0, NULL);
    if (thread) {
        CloseHandle(thread);
    } else {
        InterlockedExchange(&g_busy, 0);
    }
}

static int zi_read_token(const wchar_t *path, char *token, size_t cap) {
    FILE *file = NULL;
    size_t n;
    size_t end;

    if (_wfopen_s(&file, path, L"rb") != 0 || !file) {
        return 0;
    }
    n = fread(token, 1, cap - 1, file);
    fclose(file);
    token[n] = 0;
    while (n > 0 && (token[n - 1] == '\n' || token[n - 1] == '\r' || token[n - 1] == ' ')) {
        token[--n] = 0;
    }
    end = 0;
    while (token[end] == ' ' || token[end] == '\n' || token[end] == '\r') {
        end++;
    }
    if (end > 0) {
        memmove(token, token + end, strlen(token + end) + 1);
    }
    return token[0] != 0 && strlen(token) < 200;
}

int zi_helper_pair(const wchar_t *library_root, wchar_t *message, size_t cap) {
    wchar_t path[MAX_PATH];
    char token[256];
    int from_file = 0;
    HINTERNET session = NULL;
    HINTERNET connect = NULL;
    HINTERNET request = NULL;
    DWORD status = 0;
    DWORD status_size = sizeof status;
    wchar_t header[320];
    wchar_t wide[200];
    int ok = 0;

    token[0] = 0;
    swprintf_s(path, MAX_PATH, L"%s\\.zimage\\PAIRING-TOKEN", library_root);
    if (zi_read_token(path, token, sizeof token)) {
        from_file = 1;
    } else {
        swprintf_s(path, MAX_PATH, L"%s\\.zimage\\auth.token", library_root);
        if (!zi_read_token(path, token, sizeof token)) {
            wcscpy_s(message, cap, L"The helper has no pairing token in .zimage.");
            return 0;
        }
    }
    request = zi_open(&session, &connect, L"POST", L"/v1/pair/claim");
    if (!request) {
        wcscpy_s(message, cap, L"192.168.1.147   Helper offline.");
        zi_close(request, connect, session);
        SecureZeroMemory(token, sizeof token);
        return 0;
    }
    if (!from_file) {
        MultiByteToWideChar(CP_UTF8, 0, token, -1, wide, 200);
        wcscpy_s(header, 320, L"Authorization: Bearer ");
        wcscat_s(header, 320, wide);
        WinHttpAddRequestHeaders(request, header, (DWORD)-1L, WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
        SecureZeroMemory(wide, sizeof wide);
        SecureZeroMemory(header, sizeof header);
    }
    if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, token, (DWORD)strlen(token), (DWORD)strlen(token), 0) || !WinHttpReceiveResponse(request, NULL)) {
        wcscpy_s(message, cap, L"192.168.1.147   Helper offline.");
    } else {
        ZiHelperNote pin_note;
        ZeroMemory(&pin_note, sizeof pin_note);
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size, WINHTTP_NO_HEADER_INDEX);
        if (!zi_pin_ok(request, &pin_note)) {
            wcscpy_s(message, cap, L"192.168.1.147   Helper certificate changed. Pair again.");
        } else if (status == 204) {
            /* Keep the posted token. A 409 is a different token and is not saved. */
            zi_save_bearer(token);
            wcscpy_s(message, cap, L"Paired with the helper at 192.168.1.147.");
            ok = 1;
        } else if (status == 409) {
            wcscpy_s(message, cap, L"The helper is already paired. Read the saved token from the PhotoVideoAudio folder.");
        } else {
            wcscpy_s(message, cap, L"The helper refused the pairing token.");
        }
    }
    zi_close(request, connect, session);
    SecureZeroMemory(token, sizeof token);
    return ok;
}

static int zi_read_grow(HINTERNET request, char **out) {
    size_t cap = 4096;
    size_t used = 0;
    char *buf = (char *)malloc(cap);

    if (!buf) {
        return 0;
    }
    for (;;) {
        DWORD got = 0;

        if (used + 2048 + 1 > cap) {
            size_t next = cap * 2;
            char *grown;

            if (next > 4u * 1024u * 1024u) {
                break;
            }
            grown = (char *)realloc(buf, next);
            if (!grown) {
                free(buf);
                return 0;
            }
            buf = grown;
            cap = next;
        }
        if (!WinHttpReadData(request, buf + used, 2048, &got)) {
            free(buf);
            return 0;
        }
        if (got == 0) {
            break;
        }
        used += got;
    }
    if (used >= cap) {
        free(buf);
        return 0;
    }
    buf[used] = 0;
    *out = buf;
    return 1;
}

int zi_helper_call(const wchar_t *method, const wchar_t *path, const char *body, DWORD timeout_ms,
                   DWORD *status, char **out) {
    char token[256];
    wchar_t wide[200];
    wchar_t header[320];
    HINTERNET session = NULL;
    HINTERNET connect = NULL;
    HINTERNET request = NULL;
    DWORD code = 0;
    DWORD code_size = sizeof code;
    DWORD body_len = 0;
    int sent;
    ZiHelperNote pin_note;

    if (out) {
        *out = NULL;
    }
    if (!status) {
        status = &code;
    }
    *status = 0;
    token[0] = 0;
    if (!zi_load_bearer(token, sizeof token)) {
        *status = 401;
        SecureZeroMemory(token, sizeof token);
        SetLastError(ERROR_NOT_AUTHENTICATED);
        return 0;
    }
    request = zi_open(&session, &connect, method, path);
    if (request && timeout_ms != 0) {
        WinHttpSetTimeouts(session, (int)timeout_ms, (int)timeout_ms, (int)timeout_ms, (int)timeout_ms);
    }
    if (!request) {
        zi_close(request, connect, session);
        SecureZeroMemory(token, sizeof token);
        SetLastError(ERROR_WINHTTP_CANNOT_CONNECT);
        return 0;
    }
    if (MultiByteToWideChar(CP_UTF8, 0, token, -1, wide, 200) <= 0) {
        zi_close(request, connect, session);
        SecureZeroMemory(token, sizeof token);
        SecureZeroMemory(wide, sizeof wide);
        SetLastError(ERROR_INVALID_DATA);
        return 0;
    }
    wcscpy_s(header, 320, L"Authorization: Bearer ");
    wcscat_s(header, 320, wide);
    WinHttpAddRequestHeaders(request, header, (DWORD)-1L, WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
    if (body) {
        WinHttpAddRequestHeaders(request, L"Content-Type: application/json", (DWORD)-1L,
                                 WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
        body_len = (DWORD)strlen(body);
    }
    sent = body
        ? WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, (LPVOID)body, body_len, body_len, 0)
        : WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    SecureZeroMemory(wide, sizeof wide);
    SecureZeroMemory(header, sizeof header);
    SecureZeroMemory(token, sizeof token);
    if (!sent || !WinHttpReceiveResponse(request, NULL)) {
        DWORD err = GetLastError();
        zi_close(request, connect, session);
        *status = 0;
        SetLastError(err ? err : ERROR_WINHTTP_TIMEOUT);
        return 0;
    }
    WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, status, &code_size, WINHTTP_NO_HEADER_INDEX);
    ZeroMemory(&pin_note, sizeof pin_note);
    if (!zi_pin_ok(request, &pin_note)) {
        char *discard = NULL;
        if (zi_read_grow(request, &discard)) {
            free(discard);
        }
        zi_close(request, connect, session);
        *status = 0;
        SetLastError(ERROR_WINHTTP_SECURE_FAILURE);
        return 0;
    }
    if (out) {
        if (!zi_read_grow(request, out)) {
            *out = NULL;
        }
    } else {
        char *discard = NULL;
        if (zi_read_grow(request, &discard)) {
            free(discard);
        }
    }
    zi_close(request, connect, session);
    SetLastError(0);
    return 1;
}
