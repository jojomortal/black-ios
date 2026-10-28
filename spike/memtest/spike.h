#pragma once
#include <stddef.h>
#include <TargetConditionals.h>
#if TARGET_OS_IPHONE
#include <os/proc.h>
#endif
#ifdef __cplusplus
extern "C" {
#endif
const char* spike_run(char* out, size_t cap, int hint_mode);
#ifdef __cplusplus
}
#endif
