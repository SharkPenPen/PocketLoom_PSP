// PocketLoom 3DS audio placeholder.
#include "runtime/audio.h"
namespace r4 {
bool AudioInit() { return false; }
void AudioShutdown() {}
bool AudioReady() { return false; }
const char* AudioBackend() { return "3ds-silent"; }
void AudioPlay(Sfx sfx) { (void)sfx; }
void AudioPlayClip(const unsigned char* data, size_t size, bool loop) {
  (void)data; (void)size; (void)loop;
}
void AudioStop() {}
}  // namespace r4
