/* Shared installer transport. Partial files survive Setup/process restarts.
 * No UI or installation state: Inno owns the wizard and consumes ASCII progress.
 * All remote bytes are untrusted until size AND pinned SHA-256 match. */
#define UNICODE
#define _UNICODE
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <wchar.h>
#include <wctype.h>

#define CAP 32768
static wchar_t url[CAP], cache[CAP], local[CAP], cancel_file[CAP], expected_hash[65];
static wchar_t partial[CAP], complete[CAP], validator[CAP];
static uint64_t expected_size;
static HANDLE parent;
static volatile LONG cancelled, phase;
static volatile LONG64 progress;
static PVOID volatile active_request;
static const char *error_text = "Download failed. Saved progress can be resumed by running Setup again.";
enum { CHECKING, WAITING, DOWNLOADING, RESUMING, VERIFYING, RESTARTING };
static const char *phases[] = { "CHECKING", "WAITING", "DOWNLOADING", "RESUMING", "VERIFYING", "RESTARTING" };
static int ready_local;

static int stopped(void)
{
    if (InterlockedCompareExchange(&cancelled, 0, 0)) return 1;
    if ((parent && WaitForSingleObject(parent, 0) == WAIT_OBJECT_0) ||
        (cancel_file[0] && GetFileAttributesW(cancel_file) != INVALID_FILE_ATTRIBUTES)) {
        InterlockedExchange(&cancelled, 1);
        return 1;
    }
    return 0;
}

static void status(int p, uint64_t bytes)
{
    InterlockedExchange(&phase, p);
    InterlockedExchange64(&progress, (LONG64)bytes);
}

static int valid_file(const wchar_t *path)
{
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_HASH_HANDLE hash = NULL;
    HANDLE file = INVALID_HANDLE_VALUE;
    LARGE_INTEGER size;
    unsigned char result[32], *buffer = NULL;
    wchar_t hex[65];
    DWORD got;
    uint64_t total = 0;
    int ok = 0;
    file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (file == INVALID_HANDLE_VALUE) goto done;
    if (!GetFileSizeEx(file, &size) || (uint64_t)size.QuadPart != expected_size) goto done;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) < 0 ||
        BCryptCreateHash(alg, &hash, NULL, 0, NULL, 0, 0) < 0) goto done;
    buffer = malloc(1024 * 1024);
    if (!buffer) goto done;
    status(VERIFYING, 0);
    for (;;) {
        if (stopped()) goto done;
        if (!ReadFile(file, buffer, 1024 * 1024, &got, NULL)) goto done;
        if (!got) break;
        if (BCryptHashData(hash, buffer, got, 0) < 0) goto done;
        total += got;
        InterlockedExchange64(&progress, (LONG64)total);
    }
    if (BCryptFinishHash(hash, result, sizeof result, 0) < 0) goto done;
    for (int i = 0; i < 32; ++i) swprintf_s(hex + i * 2, 65 - i * 2, L"%02x", result[i]);
    ok = total == expected_size && !_wcsicmp(hex, expected_hash);
done:
    free(buffer);
    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    return ok;
}

static void close_request(void)
{
    HINTERNET request = InterlockedExchangePointer(&active_request, NULL);
    if (request) WinHttpCloseHandle(request);
}

static int read_validator(wchar_t *value, DWORD count)
{
    HANDLE f = CreateFileW(validator, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    DWORD bytes = 0;
    value[0] = 0;
    if (f == INVALID_HANDLE_VALUE) return 0;
    int ok = ReadFile(f, value, (count - 1) * sizeof(wchar_t), &bytes, NULL);
    CloseHandle(f);
    if (!ok || bytes % sizeof(wchar_t)) return 0;
    value[bytes / sizeof(wchar_t)] = 0;
    /* Only a quoted strong ETag can be sent back as an HTTP header. */
    return value[0] == L'"' && !wcschr(value, L'\r') && !wcschr(value, L'\n');
}

static void save_validator(HINTERNET request)
{
    wchar_t value[2048];
    DWORD bytes = sizeof value;
    DeleteFileW(validator);
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_ETAG, NULL, value, &bytes, NULL) || value[0] != L'"') return;
    HANDLE f = CreateFileW(validator, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (f != INVALID_HANDLE_VALUE) {
        DWORD wrote;
        WriteFile(f, value, (DWORD)(wcslen(value) * sizeof(wchar_t)), &wrote, NULL);
        CloseHandle(f);
    }
}

/* One connection. Retry reconnects through the ORIGINAL URL, including after
 * GitHub's signed redirect expires. Return 0 success, 1 transient, 3 permanent. */
static int transfer(void)
{
    URL_COMPONENTS parts = { sizeof parts };
    wchar_t host[1024], path[CAP], extra[CAP], headers[2300], etag[2048], range[256];
    HINTERNET session = NULL, connection = NULL, request = NULL;
    HANDLE file = INVALID_HANDLE_VALUE;
    LARGE_INTEGER size, zero = {0};
    DWORD code, count, length, wrote;
    uint64_t offset, begin, end, total;
    unsigned char buffer[65536];
    int result = 1;
    parts.lpszHostName = host; parts.dwHostNameLength = 1024;
    parts.lpszUrlPath = path; parts.dwUrlPathLength = CAP;
    parts.lpszExtraInfo = extra; parts.dwExtraInfoLength = CAP;
    if (!WinHttpCrackUrl(url, 0, 0, &parts) || parts.dwUserNameLength || parts.dwPasswordLength) return 3;
    if (parts.nScheme != INTERNET_SCHEME_HTTPS &&
        !(parts.nScheme == INTERNET_SCHEME_HTTP && (!wcscmp(host,L"127.0.0.1") || !wcscmp(host,L"localhost")))) {
        error_text = "An HTTPS download URL is required."; return 3;
    }
    if (wcscat_s(path, CAP, extra)) return 3;
    session = WinHttpOpen(L"MvC2Recomp-Setup/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                          WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) goto done;
    WinHttpSetTimeouts(session, 10000, 10000, 10000, 15000);
    connection = WinHttpConnect(session, host, parts.nPort, 0);
    if (!connection) goto done;
    request = WinHttpOpenRequest(connection, L"GET", path, NULL, WINHTTP_NO_REFERER,
                                 WINHTTP_DEFAULT_ACCEPT_TYPES,
                                 parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
    if (!request) goto done;
    InterlockedExchangePointer(&active_request, request);
    DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
    WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof policy);
    file = CreateFileW(partial, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (file == INVALID_HANDLE_VALUE || !GetFileSizeEx(file, &size)) {
        error_text = "The download cache could not be written. Check free disk space and permissions.";
        result = 3; goto done;
    }
    offset = (uint64_t)size.QuadPart;
    if (offset >= expected_size) {
        if (!SetFilePointerEx(file, zero, NULL, FILE_BEGIN) || !SetEndOfFile(file)) goto done;
        offset = 0;
    }
    wcscpy_s(headers, 2300, L"Accept-Encoding: identity\r\n");
    if (offset) {
        swprintf_s(range, 256, L"Range: bytes=%llu-\r\n", (unsigned long long)offset);
        wcscat_s(headers, 2300, range);
        if (read_validator(etag, 2048)) {
            wcscat_s(headers, 2300, L"If-Range: "); wcscat_s(headers, 2300, etag); wcscat_s(headers, 2300, L"\r\n");
        }
    }
    status(offset ? RESUMING : DOWNLOADING, offset);
    if (offset) printf("EVENT Resume offset=%llu\n", (unsigned long long)offset);
    if (stopped() || !WinHttpSendRequest(request, headers, (DWORD)-1, NULL, 0, 0, 0) ||
        !WinHttpReceiveResponse(request, NULL)) goto done;
    count = sizeof code;
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, NULL, &code, &count, NULL)) goto done;
    if (code == 206) {
        count = sizeof range;
        if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_RANGE, NULL, range, &count, NULL) ||
            swscanf_s(range, L"bytes %llu-%llu/%llu", &begin, &end, &total) != 3 ||
            begin != offset || total != expected_size || end < begin || end >= total) {
            error_text = "The server returned an invalid download range. Saved data was not appended.";
            result = 3; goto done;
        }
        /* A request for the remaining payload must return the entire remainder. */
        if (end + 1 != expected_size) { result = 3; goto done; }
    } else if (code == 200) {
        /* A server ignoring Range (or an If-Range mismatch) sent a FULL file.
         * Truncate before writing; never append the full file to a prefix. */
        if (offset) {
            if (!SetFilePointerEx(file, zero, NULL, FILE_BEGIN) || !SetEndOfFile(file)) goto done;
            offset = 0;
            status(RESTARTING, 0);
        }
    } else if (code == 416 && offset) {
        if (SetFilePointerEx(file, zero, NULL, FILE_BEGIN) && SetEndOfFile(file)) DeleteFileW(validator);
        goto done;
    } else {
        error_text = "The server did not provide the texture pack. Retry to resume the saved download.";
        goto done;
    }
    count = sizeof range;
    if (WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH, NULL, range, &count, NULL) &&
        _wcstoui64(range, NULL, 10) != expected_size - offset) {
        error_text = "The server returned an unexpected texture pack size.";
        result = 3; goto done;
    }
    save_validator(request);
    size.QuadPart = (LONGLONG)offset;
    if (!SetFilePointerEx(file, size, NULL, FILE_BEGIN)) goto done;
    while (!stopped()) {
        if (!WinHttpReadData(request, buffer, sizeof buffer, &length)) goto done;
        if (!length) break;
        if (offset + length > expected_size) { result = 3; goto done; }
        if (!WriteFile(file, buffer, length, &wrote, NULL) || wrote != length) {
            error_text = "The download cache could not be written. Check free disk space.";
            result = 3; goto done;
        }
        offset += wrote;
        InterlockedExchange64(&progress, (LONG64)offset);
    }
    if (offset == expected_size && !stopped()) result = 0;
done:
    if (file != INVALID_HANDLE_VALUE) { FlushFileBuffers(file); CloseHandle(file); }
    close_request();
    if (connection) WinHttpCloseHandle(connection);
    if (session) WinHttpCloseHandle(session);
    return result;
}

static DWORD WINAPI run_download(void *unused)
{
    wchar_t mutex_name[128];
    HANDLE mutex;
    int result = 1, held = 0;
    (void)unused;
    if (local[0] && GetFileAttributesW(local) != INVALID_FILE_ATTRIBUTES) {
        if (valid_file(local)) { ready_local = 1; return 0; }
        if (stopped()) return 2;
        error_text = "The texture archive beside Setup does not match the required texture pack.";
        return 5;
    }
    if (SHCreateDirectoryExW(NULL, cache, NULL) != ERROR_SUCCESS && GetFileAttributesW(cache) == INVALID_FILE_ATTRIBUTES) {
        error_text = "The texture download cache could not be created."; return 3;
    }
    swprintf_s(mutex_name, 128, L"Local\\MvC2RecompTextureDownload_%s", expected_hash);
    mutex = CreateMutexW(NULL, FALSE, mutex_name);
    if (!mutex) return 3;
    status(WAITING, 0);
    while (!stopped()) {
        DWORD wait = WaitForSingleObject(mutex, 250);
        if (wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED) { held = 1; break; }
        if (wait == WAIT_FAILED) break;
    }
    if (!held) { CloseHandle(mutex); return stopped() ? 2 : 3; }
    if (valid_file(complete)) { result = 0; goto done; }
    if (stopped()) { result = 2; goto done; }
    DeleteFileW(complete); /* Only our hash-keyed invalid cache file. Never local input. */
    if (valid_file(partial)) { result = 0; goto promote; }
    for (int attempt = 0; attempt < 4 && !stopped(); ++attempt) {
        result = transfer();
        if (!result || result == 3) break;
        for (int tick = 0; tick < (10 << attempt) && !stopped(); ++tick) Sleep(100);
    }
    if (stopped()) { result = 2; goto done; }
    if (result) goto done;
    if (!valid_file(partial)) {
        if (stopped()) { result = 2; goto done; }
        DeleteFileW(partial); DeleteFileW(validator);
        error_text = "The downloaded texture pack failed checksum verification. Retry will start a fresh download.";
        result = 4; goto done;
    }
promote:
    if (!MoveFileExW(partial, complete, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) { result = 3; goto done; }
    DeleteFileW(validator);
done:
    ReleaseMutex(mutex); CloseHandle(mutex);
    return (DWORD)result;
}

int wmain(int argc, wchar_t **argv)
{
    DWORD parent_id = 0, code;
    for (int i = 1; i < argc; ++i) {
        if (i + 1 >= argc) return 6;
        wchar_t *value = argv[++i];
        if (!wcscmp(argv[i-1], L"--url")) { if (wcscpy_s(url, CAP, value)) return 6; }
        else if (!wcscmp(argv[i-1], L"--cache")) { if (wcscpy_s(cache, CAP, value)) return 6; }
        else if (!wcscmp(argv[i-1], L"--local")) { if (wcscpy_s(local, CAP, value)) return 6; }
        else if (!wcscmp(argv[i-1], L"--cancel")) { if (wcscpy_s(cancel_file, CAP, value)) return 6; }
        else if (!wcscmp(argv[i-1], L"--sha256")) { if (wcscpy_s(expected_hash, 65, value)) return 6; }
        else if (!wcscmp(argv[i-1], L"--bytes")) expected_size = _wcstoui64(value, NULL, 10);
        else if (!wcscmp(argv[i-1], L"--parent")) parent_id = wcstoul(value, NULL, 10);
        else return 6;
    }
    if (!url[0] || !cache[0] || !expected_size || wcslen(expected_hash) != 64) return 6;
    for (int i = 0; i < 64; ++i) if (!iswxdigit(expected_hash[i])) return 6;
    if (swprintf_s(partial, CAP, L"%s\\texture.bin.part", cache) < 0 ||
        swprintf_s(complete, CAP, L"%s\\texture.bin", cache) < 0 ||
        swprintf_s(validator, CAP, L"%s\\texture.bin.etag", cache) < 0) return 6;
    if (parent_id) {
        parent = OpenProcess(SYNCHRONIZE, FALSE, parent_id);
        if (!parent) return 2;
    }
    setvbuf(stdout, NULL, _IONBF, 0);
    HANDLE worker = CreateThread(NULL, 0, run_download, NULL, 0, NULL);
    if (!worker) return 3;
    while (WaitForSingleObject(worker, 250) == WAIT_TIMEOUT) {
        if (stopped()) close_request();
        LONG p = InterlockedCompareExchange(&phase, 0, 0);
        printf("PROGRESS %llu %llu %s\n", (unsigned long long)InterlockedCompareExchange64(&progress,0,0),
               (unsigned long long)expected_size, phases[p]);
    }
    GetExitCodeThread(worker, &code);
    CloseHandle(worker);
    if (parent) CloseHandle(parent);
    if (!code) printf("READY %s\n", ready_local ? "local" : "cache");
    else if (code == 2) printf("ERROR Download stopped. Saved progress will be resumed next time.\n");
    else printf("ERROR %s\n", error_text);
    return (int)code;
}
