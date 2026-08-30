/* Does CompareObjectHandles see a duplicate of a pipe end as the same object?
 * Both ends, since the read end of ours matches and the write end does not. */
#include <windows.h>
#include <stdio.h>

int main(void)
{
    HANDLE rd, wr, drd, dwr;
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    BOOL (WINAPI *cmp)(HANDLE, HANDLE);
    HMODULE kb = GetModuleHandleW(L"kernelbase.dll");

    cmp = (BOOL (WINAPI *)(HANDLE, HANDLE))GetProcAddress(kb, "CompareObjectHandles");
    printf("CompareObjectHandles: %p\n", (void *)cmp);
    if (!CreatePipe(&rd, &wr, &sa, 1024 * 1024)) {
        printf("CreatePipe failed\n");
        return 1;
    }
    DuplicateHandle(GetCurrentProcess(), rd, GetCurrentProcess(), &drd, 0, TRUE, DUPLICATE_SAME_ACCESS);
    DuplicateHandle(GetCurrentProcess(), wr, GetCurrentProcess(), &dwr, 0, TRUE, DUPLICATE_SAME_ACCESS);
    printf("rd %p dup %p same=%d\n", rd, drd, cmp(rd, drd));
    printf("wr %p dup %p same=%d\n", wr, dwr, cmp(wr, dwr));
    printf("rd vs wr same=%d\n", cmp(rd, wr));
    return 0;
}
