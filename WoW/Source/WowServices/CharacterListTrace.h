#ifndef WOW_CHARACTER_LIST_TRACE_H
#define WOW_CHARACTER_LIST_TRACE_H
#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <mutex>
// Temporary diagnostic build: metadata only, never payloads or credentials.
// Thread-safe and bounded to 2000 records per launch. Each write is flushed.
inline void CharacterListTrace(const char* stage, uint32_t opcode, uint32_t size,
                               uint32_t cursor, int32_t state) {
    static std::mutex lock;
    static uint32_t count = 0;
    std::lock_guard<std::mutex> guard(lock);
    if (count >= 2000) return;
    CreateDirectoryA("Logs", nullptr);
    FILE* file = nullptr;
    if (fopen_s(&file, "Logs\\CharacterListTrace.log", count ? "a" : "w") || !file) return;
    if (!count) {
        fputs("Character-list diagnostic v1; metadata only; maximum 2000 records.\n", file);
    }
    fprintf(file, "%llu stage=%s opcode=0x%04X size=%u cursor=%u state=%d\n",
            static_cast<unsigned long long>(GetTickCount64()), stage, opcode, size, cursor, state);
    ++count;
    fclose(file);
}
#endif
