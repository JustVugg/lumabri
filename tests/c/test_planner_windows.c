/* Bounded metadata inspection of a >4 GiB sparse checkpoint, in a Unicode
 * directory. Only the tiny header/layout are stored or read by this test. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>
#include <assert.h>
#include <fcntl.h>
#include <io.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
static uint64_t received;
static int observed_read(int fd, void *data, unsigned size) {
    int n = _read(fd, data, size);
    if (n > 0) received += (unsigned)n;
    return n;
}
#define _read observed_read
#include "lumabri_planner.h"
#undef _read

static int visited;
static int visit(const LmbPlanTensor *t, void *unused) {
    (void)unused;
    if (!strcmp(t->name, "weights")) assert(t->bytes == UINT64_C(4294971392));
    else {
        assert(!strcmp(t->name, "layout"));
        assert(t->meta_i64[0] == 17 && t->meta_i64[1] == 29);
    }
    visited++; return 0;
}
static void utf8(const wchar_t *wide, char *text, int cap) {
    assert(WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, -1,
                              text, cap, NULL, NULL));
}
int main(void) {
    wchar_t temp[MAX_PATH], root[MAX_PATH], file[MAX_PATH];
    DWORD n = GetTempPathW(MAX_PATH, temp); assert(n && n < MAX_PATH);
    assert(swprintf(root, MAX_PATH, L"%lslumabri-planner-%lu-%llu-\u00e9-\u65e5\u672c",
        temp, GetCurrentProcessId(), (unsigned long long)GetTickCount64()) > 0);
    assert(CreateDirectoryW(root, NULL));
    assert(swprintf(file, MAX_PATH, L"%ls/model-\u65e5.safetensors", root) > 0);
    HANDLE h = CreateFileW(file, GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    assert(h != INVALID_HANDLE_VALUE);
    DWORD got;
    assert(DeviceIoControl(h, FSCTL_SET_SPARSE, NULL, 0, NULL, 0, &got, NULL));
    const char *header = "{\"weights\":{\"dtype\":\"U8\",\"shape\":[4294971392],"
        "\"data_offsets\":[0,4294971392]},\"layout\":{\"dtype\":\"I64\",\"shape\":[2],"
        "\"data_offsets\":[4294971392,4294971408]}}";
    uint64_t size = strlen(header);
    unsigned char prefix[8], layout[16] = {17,0,0,0,0,0,0,0,29};
    for (unsigned i=0; i<8; i++) prefix[i] = (unsigned char)(size >> (8*i));
    assert(WriteFile(h, prefix, 8, &got, NULL) && got == 8);
    assert(WriteFile(h, header, (DWORD)size, &got, NULL) && got == size);
    LARGE_INTEGER at = {.QuadPart = 8+(LONGLONG)size+4294971392LL};
    assert(SetFilePointerEx(h, at, NULL, FILE_BEGIN));
    assert(WriteFile(h, layout, 16, &got, NULL) && got == 16);
    CloseHandle(h);
    char root8[1024], file8[1024]; utf8(root, root8, sizeof root8); utf8(file, file8, sizeof file8);
    assert(lmb_plan_optional_directory(root8) == 1);
    assert(lmb_plan_optional_directory(file8) == -1);
    assert(!lmb_plan_tensors(root8, visit, NULL));
    assert(visited == 2 && received == 8+size+16);
    int fd = lmb_plan_open(file8); assert(fd >= 0);
    DWORD flags; assert(GetHandleInformation((HANDLE)_get_osfhandle(fd), &flags));
    assert(!(flags & HANDLE_FLAG_INHERIT));
    uint64_t actual; assert(!lmb_plan_file_size(fd, &actual) && actual == 8+size+4294971408LL);
    assert(lmb_plan_read_at(fd, prefix, 1, UINT64_MAX) == -1);
    assert(lmb_plan_read_at(fd, prefix, 1, actual) == -1);
    close(fd);
    uint64_t budget = size-1;
    assert(lmb_plan_tensor_file_id(file8, &budget, visit, NULL, 0) == -1);
    assert(DeleteFileW(file));
    assert(swprintf(file, MAX_PATH, L"%ls/config.json", root) > 0);
    FILE *out = _wfopen(file, L"wb"); assert(out);
    assert(fputs("{\"model_type\":\"olmoe\"}", out) >= 0); assert(!fclose(out));
    uint64_t json_size; char *json = lmb_v41_read_json(root8, "config.json", 128, &json_size);
    assert(json && json_size == strlen(json) && strstr(json, "olmoe")); free(json);
    utf8(file, file8, sizeof file8);
    FILE *in = lmb_plan_fopen(file8); assert(in && fgetc(in) == '{'); fclose(in);
    assert(DeleteFileW(file)); assert(RemoveDirectoryW(root));
    puts("WINDOWS PLANNER PASS: Unicode paths, 64-bit extents, bounded header/layout reads, EOF and no inherited descriptor");
    return 0;
}
