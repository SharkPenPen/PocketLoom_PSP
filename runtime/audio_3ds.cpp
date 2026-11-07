// runtime/audio_3ds.cpp
#include "runtime/audio.h"

#include <3ds.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace r4 {
namespace {

constexpr int kRate = 22050;
constexpr int kChannels = 24;
constexpr int kSfxCount = static_cast<int>(Sfx::kCount);

struct Clip {
  s16* pcm = nullptr;
  size_t frames = 0;
  bool loop = false;
  bool used = false;
};

ndspWaveBuf g_wave[kChannels] = {};
Clip g_clip[kChannels];
s16* g_tone[kSfxCount] = {};
ndspWaveBuf g_tone_wave[kSfxCount] = {};
bool g_ready = false;

bool DspFirmwarePresent() {
  std::FILE* f = std::fopen("sdmc:/3ds/dspfirm.cdc", "rb");
  if (f == nullptr) return false;
  std::fclose(f);
  return true;
}

bool ReadU16(const unsigned char* d, size_t n, size_t off, unsigned* v) {
  if (off + 2 > n) return false;
  *v = static_cast<unsigned>(d[off]) | (static_cast<unsigned>(d[off + 1]) << 8);
  return true;
}
bool ReadU32(const unsigned char* d, size_t n, size_t off, unsigned* v) {
  if (off + 4 > n) return false;
  *v = static_cast<unsigned>(d[off]) | (static_cast<unsigned>(d[off + 1]) << 8) |
       (static_cast<unsigned>(d[off + 2]) << 16) |
       (static_cast<unsigned>(d[off + 3]) << 24);
  return true;
}

// WAV PCM16 -> 22050Hz stereo.
bool DecodeWav(const unsigned char* data, size_t size, std::vector<s16>* out) {
  if (size < 44) return false;
  unsigned riff = 0, wave = 0;
  if (!ReadU32(data, size, 0, &riff) || riff != 0x46464952u) return false;
  if (!ReadU32(data, size, 8, &wave) || wave != 0x45564157u) return false;
  unsigned channels = 0, rate = 0, bits = 0, data_off = 0, data_size = 0;
  size_t off = 12;
  while (off + 8 <= size) {
    unsigned id = 0, len = 0;
    if (!ReadU32(data, size, off, &id) || !ReadU32(data, size, off + 4, &len)) return false;
    const size_t body = off + 8;
    if (id == 0x20746d66u) {  // "fmt "
      if (body + 16 > size) return false;
      unsigned fmt = 0;
      ReadU16(data, size, body, &fmt);
      ReadU16(data, size, body + 2, &channels);
      ReadU32(data, size, body + 4, &rate);
      ReadU16(data, size, body + 14, &bits);
      if (fmt != 1 || bits != 16 || channels == 0 || rate == 0) return false;
    } else if (id == 0x61746164u) {  // "data"
      data_off = static_cast<unsigned>(body);
      data_size = len;
      break;
    }
    off = body + len + (len & 1u);
  }
  if (data_off == 0 || data_size == 0 || data_off + data_size > size) return false;
  const size_t src_frames = data_size / (channels * 2u);
  const size_t dst_frames = src_frames * kRate / rate;
  if (dst_frames == 0) return false;
  out->assign(dst_frames * 2, 0);
  for (size_t i = 0; i < dst_frames; ++i) {
    const size_t src = i * rate / kRate;
    int l = 0, r = 0;
    if (src < src_frames) {
      const s16* p = reinterpret_cast<const s16*>(data + data_off) + src * channels;
      l = p[0];
      r = channels > 1 ? p[1] : p[0];
    }
    (*out)[i * 2 + 0] = static_cast<s16>(l);
    (*out)[i * 2 + 1] = static_cast<s16>(r);
  }
  return true;
}

int FreeChannel() {
  for (int ch = 1; ch < kChannels; ++ch) {
    if (!g_clip[ch].used) return ch;
  }
  return -1;
}

void ReleaseChannel(int ch) {
  if (ch < 0 || ch >= kChannels) return;
  ndspChnWaveBufClear(ch);
  if (g_clip[ch].pcm != nullptr) {
    linearFree(g_clip[ch].pcm);
    g_clip[ch].pcm = nullptr;
  }
  g_clip[ch] = Clip{};
}

void BuildTone(Sfx sfx, int frequency, int ms, float volume) {
  const int idx = static_cast<int>(sfx);
  const int frames = std::max(1, kRate * ms / 1000);
  const size_t bytes = static_cast<size_t>(frames) * 4u;
  g_tone[idx] = static_cast<s16*>(linearAlloc(bytes));
  if (g_tone[idx] == nullptr) return;
  for (int i = 0; i < frames; ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(kRate);
    const float env = 1.0f - static_cast<float>(i) / static_cast<float>(frames);
    const float s = std::sin(6.28318530718f * static_cast<float>(frequency) * t) * env * volume;
    const s16 v = static_cast<s16>(s * 32767.0f);
    g_tone[idx][i * 2 + 0] = v;
    g_tone[idx][i * 2 + 1] = v;
  }
  DSP_FlushDataCache(g_tone[idx], bytes);
  g_tone_wave[idx].data_vaddr = g_tone[idx];
  g_tone_wave[idx].nsamples = static_cast<u32>(frames);
  g_tone_wave[idx].looping = false;
}

}  // namespace

bool AudioInit() {
  if (!DspFirmwarePresent()) {
    g_ready = false;
    return false;
  }
  if (ndspInit() != 0) {
    g_ready = false;
    return false;
  }
  ndspSetOutputMode(NDSP_OUTPUT_STEREO);
  for (int ch = 0; ch < kChannels; ++ch) {
    ndspChnSetInterp(ch, NDSP_INTERP_LINEAR);
    ndspChnSetRate(ch, kRate);
    ndspChnSetFormat(ch, NDSP_FORMAT_STEREO_PCM16);
    float mix[12] = {};
    mix[0] = 0.7f;
    mix[1] = 0.7f;
    ndspChnSetMix(ch, mix);
  }
  BuildTone(Sfx::kCoin, 880, 70, 0.35f);
  BuildTone(Sfx::kHurt, 180, 180, 0.45f);
  BuildTone(Sfx::kClear, 1320, 260, 0.40f);
  BuildTone(Sfx::kJump, 660, 90, 0.35f);
  BuildTone(Sfx::kTransition, 440, 120, 0.35f);
  g_ready = true;
  return true;
}

void AudioShutdown() {
  for (int ch = 0; ch < kChannels; ++ch) ReleaseChannel(ch);
  if (g_ready) ndspExit();
  for (int i = 0; i < kSfxCount; ++i) {
    if (g_tone[i] != nullptr) {
      linearFree(g_tone[i]);
      g_tone[i] = nullptr;
    }
  }
  g_ready = false;
}

bool AudioReady() { return g_ready; }

const char* AudioBackend() { return g_ready ? "ndsp" : "silent"; }

void AudioPlay(Sfx sfx) {
  if (!g_ready) return;
  const int i = static_cast<int>(sfx);
  if (i < 0 || i >= kSfxCount || g_tone[i] == nullptr) return;
  ndspChnWaveBufAdd(i, &g_tone_wave[i]);
}

void AudioPlayClip(const unsigned char* data, size_t size, bool loop) {
  if (!g_ready || data == nullptr || size == 0) return;
  std::vector<s16> pcm;
  if (!DecodeWav(data, size, &pcm)) return;
  const int ch = loop ? 0 : FreeChannel();
  if (ch < 0) return;
  ReleaseChannel(ch);
  const size_t bytes = pcm.size() * sizeof(s16);
  s16* mem = static_cast<s16*>(linearAlloc(bytes));
  if (mem == nullptr) return;
  std::memcpy(mem, pcm.data(), bytes);
  DSP_FlushDataCache(mem, bytes);
  g_clip[ch].pcm = mem;
  g_clip[ch].frames = pcm.size() / 2;
  g_clip[ch].loop = loop;
  g_clip[ch].used = true;
  g_wave[ch] = ndspWaveBuf{};
  g_wave[ch].data_vaddr = mem;
  g_wave[ch].nsamples = static_cast<u32>(g_clip[ch].frames);
  g_wave[ch].looping = loop;
  ndspChnWaveBufAdd(ch, &g_wave[ch]);
}

void AudioStop() {
  for (int ch = 0; ch < kChannels; ++ch) ReleaseChannel(ch);
}

}  // namespace r4
