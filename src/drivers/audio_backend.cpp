//  kurono os - AudioBackend default method implementations
#include "audio_backend.h"
#include "../kernel/types.h"

void AudioBackend::Describe(char* out, int max_len) const {
    if (!out || max_len <= 0) return;
    const char* n = Name();
    int i = 0;
    while (n[i] && i < max_len - 1) { out[i] = n[i]; i++; }
    out[i] = '\0';
}
