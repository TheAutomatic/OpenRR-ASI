// SPDX-License-Identifier: MIT
// Build once as a DLL (-DTEST_NR_DLL), once as an EXE, each with its own MinHook.
#include "rr_mochi_hook.hpp"
#include "MinHook.h"
#include <cstdio>
#include <cstdlib>
using Fn = int (*)();
static Fn original;
static int calls;
static void check(bool ok) { if (!ok) { std::puts("FAIL"); std::exit(1); } }
#ifdef TEST_NR_DLL
extern "C" __declspec(dllexport) int nr_detour() { ++calls; return original() + 10; }
extern "C" __declspec(dllexport) void install(void* entry) {
    check(MH_Initialize() == MH_OK);
    check(MH_CreateHook(entry, reinterpret_cast<void*>(&nr_detour), reinterpret_cast<void**>(&original)) == MH_OK);
    check(MH_EnableHook(entry) == MH_OK);
}
#else
static bool intercept;
static int rr_detour() { ++calls; return intercept ? 100 : original() + 100; }
int main(int argc, char**) {
    auto* entry = static_cast<unsigned char*>(VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
    check(entry != nullptr);
    // mov eax,1; ret. Separate executable buffer avoids compiler-generated thunks.
    const unsigned char code[] = {0xb8, 1, 0, 0, 0, 0xc3};
    std::memcpy(entry, code, sizeof(code));
    auto dll = LoadLibraryW(L"test_nr_hook.dll");
    check(dll != nullptr);
    auto install = reinterpret_cast<void (*)(void*)>(GetProcAddress(dll, "install"));
    auto detour = reinterpret_cast<uintptr_t>(GetProcAddress(dll, "nr_detour"));
    check(install && detour);
    check(MH_Initialize() == MH_OK);
    if (argc == 1) install(entry);
    void* target = entry;
    if (argc == 1) {
        target = openrr::pe::minhook_inner_target(entry, detour, detour + 1);
        check(target && target != entry);
        check(!openrr::pe::minhook_inner_target(entry, detour + 1, detour + 2));
        check(!openrr::pe::minhook_inner_target(nullptr, detour, detour + 1));
    }
    check(MH_CreateHook(target, reinterpret_cast<void*>(&rr_detour), reinterpret_cast<void**>(&original)) == MH_OK);
    check(MH_EnableHook(target) == MH_OK);
    if (argc != 1) install(entry);
    for (int i = 0; i < 100; ++i) check(reinterpret_cast<Fn>(entry)() == 111);
    check(calls == 100);
    // RR short-circuits the lower chain: NR must still run outside it.
    intercept = true;
    check(reinterpret_cast<Fn>(entry)() == 110);
    check(calls == 101);
    std::puts(argc == 1 ? "PASS: NR first, OpenRR inside NR trampoline" : "PASS: OpenRR first, NR outside OpenRR");
}
#endif
