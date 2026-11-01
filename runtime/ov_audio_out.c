// nn::audio AudioOut on SDL3: XCX streams PCM through AudioOut next to the audio renderer (XCX used only the
// renderer). Each open AudioOut is an SDL audio stream of 16-bit samples at 48 kHz with the channel count the game
// asked for. AppendAudioOutBuffer queues a buffer's data on the stream; a buffer is released once the device has
// consumed every byte up to its end, which GetReleasedAudioOutBuffer reports in append order. Without an audio device
// the data is consumed at the real-time rate, so the game's pacing stays the same.
//
// Guest layouts (SDK 19.3.2): AudioOutBuffer {next, data, capacity, size} at +0, +8, +0x10, +0x18, filled by the SDK's
// own SetAudioOutBufferInfo; AudioOutParameter {int32 sample rate, uint16 channel count, uint16 0xCAFE}, 0 meaning the
// default. The AudioOut object itself is only a key here.
#include <SDL3/SDL.h>
#include <stdint.h>
#include <string.h>

#include "host.h"
#include "platform/platform.h"

#define OUT_MAX 8
#define QUEUE_MAX 64
#define RATE 48000
#define FMT_PCM16 2
enum { STATE_STARTED = 0, STATE_STOPPED = 1 };

typedef struct { uint64_t buf, end; } Queued;   // end: total bytes appended once this buffer's data is in
typedef struct {
    uint64_t guest, event;   // event: the SystemEvent of the overloads that take one, signalled on release; 0 if none
    uint32_t channels, state;
    float volume;
    SDL_AudioStream *stream;
    uint64_t appended, consumed_at_stop, start_ns;   // byte counts; start_ns: the device-less clock
    Queued q[QUEUE_MAX];
    int nq;
} Out;

static Out outs[OUT_MAX];
static PlatMutex lock = PLAT_MUTEX_INIT;
static int sdl_ready = -1;

static uint64_t now_ns(void) { return plat_now_ns(); }

static Out *find(uint64_t guest) {
    for (int i = 0; i < OUT_MAX; i++)
        if (outs[i].guest == guest) return &outs[i];
    return NULL;
}

static uint32_t frame_bytes(const Out *o) { return 2 * o->channels; }

// Bytes the device has taken so far. Under lock.
static uint64_t consumed(const Out *o) {
    if (o->state != STATE_STARTED) return o->consumed_at_stop;
    if (o->stream) {
        int queued = SDL_GetAudioStreamQueued(o->stream);
        return queued < 0 || (uint64_t)queued > o->appended ? o->consumed_at_stop : o->appended - (uint64_t)queued;
    }
    uint64_t played = o->consumed_at_stop + (now_ns() - o->start_ns) * RATE / 1000000000 * frame_bytes(o);
    return played < o->appended ? played : o->appended;
}

// The SDL device pulls on its own thread: a release there wakes a game waiting on the buffer event. SDL holds the
// stream's lock around this callback, and the functions below hold `lock` while they call into the stream
// (consumed), so waiting for `lock` here deadlocks with them: the game's audio thread then never returns from
// GetReleasedAudioOutBuffer, keeps its virtual core, and the threads bound to that core stop (XCX after New Game).
// A busy `lock` skips this pull; the next one, a few milliseconds later, signals the release.
static void SDLCALL device_pull(void *user, SDL_AudioStream *s, int additional, int total) {
    (void)s, (void)additional, (void)total;
    Out *o = user;
    uint64_t ev = 0;
    if (!plat_mutex_trylock_shared(&lock)) return;
    if (o->guest && o->event && o->nq && consumed(o) >= o->q[0].end) ev = o->event;
    plat_mutex_unlock_shared(&lock);
    if (ev) kern_signal_system_event(ev);
}

static uint32_t open_out(Cpu *c, uint64_t guest, uint64_t event, const uint8_t *param) {
    int32_t rate = 0;
    uint16_t channels = 0;
    if (param) memcpy(&rate, param, 4), memcpy(&channels, param + 4, 2);
    if (rate && rate != RATE) rt_log("audout: the game asked for %d Hz; the output runs at %d Hz", rate, RATE);
    if (!channels) channels = 2;
    if (channels != 1 && channels != 2 && channels != 6) rt_fatal("audout: %u channels", channels);
    if (event) kern_bind_system_event(c, event, 1);
    plat_mutex_lock(&lock);
    Out *o = find(guest);
    if (!o) o = find(0);
    if (!o) rt_fatal("audout: more than %d AudioOuts open", OUT_MAX);
    if (sdl_ready < 0) sdl_ready = SDL_InitSubSystem(SDL_INIT_AUDIO) ? 1 : 0;
    *o = (Out){.guest = guest, .event = event, .channels = channels, .state = STATE_STOPPED, .volume = 1.0f};
    if (sdl_ready) {
        SDL_AudioSpec spec = {SDL_AUDIO_S16LE, channels, RATE};
        o->stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
        if (o->stream) SDL_SetAudioStreamGetCallback(o->stream, device_pull, o);
    }
    plat_mutex_unlock(&lock);
    rt_log("audout: opened %#llx, %u channels at %d Hz, %s", (unsigned long long)guest, channels, RATE,
           o->stream ? "on the default device" : "without a device (timed)");
    return 0;
}

// Result OpenDefaultAudioOut(AudioOut *, const AudioOutParameter &)
static void ov_open_default(Cpu *c) { ret_x(c, open_out(c, arg_x(c, 0), 0, arg_p(c, 1))); }
// Result OpenDefaultAudioOut(AudioOut *, os::SystemEvent *, const AudioOutParameter &)
static void ov_open_default_event(Cpu *c) { ret_x(c, open_out(c, arg_x(c, 0), arg_x(c, 1), arg_p(c, 2))); }
// Result OpenAudioOut(AudioOut *, const char *name, const AudioOutParameter &): every name is the default device
static void ov_open_named(Cpu *c) { ret_x(c, open_out(c, arg_x(c, 0), 0, arg_p(c, 2))); }
// Result OpenAudioOut(AudioOut *, os::SystemEvent *, const char *name, const AudioOutParameter &)
static void ov_open_named_event(Cpu *c) { ret_x(c, open_out(c, arg_x(c, 0), arg_x(c, 1), arg_p(c, 3))); }

static Out *need(Cpu *c, const char *fn) {
    Out *o = find(arg_x(c, 0));
    if (!o || !arg_x(c, 0)) rt_fatal("nn::audio::%s(%#llx): not an open AudioOut", fn, (unsigned long long)arg_x(c, 0));
    return o;
}

// void CloseAudioOut(AudioOut *)
static void ov_close(Cpu *c) {
    plat_mutex_lock(&lock);
    Out *o = need(c, "CloseAudioOut");
    SDL_AudioStream *s = o->stream;
    memset(o, 0, sizeof *o);
    plat_mutex_unlock(&lock);
    if (s) SDL_DestroyAudioStream(s);
}

// Result StartAudioOut(AudioOut *)
static void ov_start(Cpu *c) {
    plat_mutex_lock(&lock);
    Out *o = need(c, "StartAudioOut");
    if (o->state != STATE_STARTED) {
        o->state = STATE_STARTED;
        o->start_ns = now_ns();
        if (o->stream) SDL_ResumeAudioStreamDevice(o->stream);
    }
    plat_mutex_unlock(&lock);
    ret_x(c, 0);
}

// void StopAudioOut(AudioOut *)
static void ov_stop(Cpu *c) {
    plat_mutex_lock(&lock);
    Out *o = need(c, "StopAudioOut");
    if (o->state == STATE_STARTED) {
        o->consumed_at_stop = consumed(o);
        o->state = STATE_STOPPED;
        if (o->stream) SDL_PauseAudioStreamDevice(o->stream);
    }
    plat_mutex_unlock(&lock);
}

// bool AppendAudioOutBuffer(AudioOut *, AudioOutBuffer *)
static void ov_append(Cpu *c) {
    uint64_t buf = arg_x(c, 1);
    if (!buf) rt_fatal("nn::audio::AppendAudioOutBuffer: null buffer");
    const void *data = (const void *)(uintptr_t)LD64(buf + 8);
    uint64_t size = LD64(buf + 0x18);
    plat_mutex_lock(&lock);
    Out *o = need(c, "AppendAudioOutBuffer");
    int ok = o->nq < QUEUE_MAX;
    if (ok) {
        size -= size % frame_bytes(o);
        if (o->stream && size) {
            if (o->volume != 1.0f) SDL_SetAudioStreamGain(o->stream, o->volume);
            SDL_PutAudioStreamData(o->stream, data, (int)size);
        }
        o->appended += size;
        o->q[o->nq++] = (Queued){buf, o->appended};
    }
    plat_mutex_unlock(&lock);
    ret_x(c, (uint64_t)ok);
}

// AudioOutBuffer *GetReleasedAudioOutBuffer(AudioOut *): the oldest buffer the device has finished, else NULL
static void ov_released(Cpu *c) {
    uint64_t buf = 0;
    plat_mutex_lock(&lock);
    Out *o = need(c, "GetReleasedAudioOutBuffer");
    if (o->nq && consumed(o) >= o->q[0].end) {
        buf = o->q[0].buf;
        memmove(o->q, o->q + 1, (size_t)--o->nq * sizeof *o->q);
    }
    plat_mutex_unlock(&lock);
    ret_x(c, buf);
}

// bool ContainsAudioOutBuffer(const AudioOut *, const AudioOutBuffer *)
static void ov_contains(Cpu *c) {
    int found = 0;
    plat_mutex_lock_shared(&lock);
    Out *o = need(c, "ContainsAudioOutBuffer");
    for (int i = 0; i < o->nq && !found; i++) found = o->q[i].buf == arg_x(c, 1);
    plat_mutex_unlock_shared(&lock);
    ret_x(c, (uint64_t)found);
}

// bool FlushAudioOutBuffers(AudioOut *): drops the data not played yet; the buffers are released at once
static void ov_flush(Cpu *c) {
    plat_mutex_lock(&lock);
    Out *o = need(c, "FlushAudioOutBuffers");
    if (o->stream) SDL_ClearAudioStream(o->stream);
    o->consumed_at_stop = o->appended;
    o->start_ns = now_ns();
    for (int i = 0; i < o->nq; i++) o->q[i].end = 0;
    plat_mutex_unlock(&lock);
    ret_x(c, 1);
}

static void ov_state(Cpu *c) {
    plat_mutex_lock_shared(&lock);
    uint32_t s = need(c, "GetAudioOutState")->state;
    plat_mutex_unlock_shared(&lock);
    ret_x(c, s);
}
static void ov_channels(Cpu *c) {
    plat_mutex_lock_shared(&lock);
    uint32_t n = need(c, "GetAudioOutChannelCount")->channels;
    plat_mutex_unlock_shared(&lock);
    ret_x(c, n);
}
static void ov_rate(Cpu *c) { need(c, "GetAudioOutSampleRate"), ret_x(c, RATE); }
static void ov_format(Cpu *c) { need(c, "GetAudioOutSampleFormat"), ret_x(c, FMT_PCM16); }
static void ov_buffer_count(Cpu *c) {
    plat_mutex_lock_shared(&lock);
    int n = need(c, "GetAudioOutBufferCount")->nq;
    plat_mutex_unlock_shared(&lock);
    ret_x(c, (uint64_t)n);
}
static void ov_played(Cpu *c) {
    plat_mutex_lock_shared(&lock);
    Out *o = need(c, "GetAudioOutPlayedSampleCount");
    uint64_t n = consumed(o) / frame_bytes(o);
    plat_mutex_unlock_shared(&lock);
    ret_x(c, n);
}
static void ov_get_volume(Cpu *c) {
    plat_mutex_lock_shared(&lock);
    float v = need(c, "GetAudioOutVolume")->volume;
    plat_mutex_unlock_shared(&lock);
    ret_s(c, v);
}
static void ov_set_volume(Cpu *c) {
    plat_mutex_lock(&lock);
    need(c, "SetAudioOutVolume")->volume = arg_s(c, 0);
    plat_mutex_unlock(&lock);
}

// ---- the output device as the game sees it: one stereo TV output that never changes
#define DEVICE_NAME "AudioTvOutput"
// Result GetActiveAudioDeviceName(AudioDeviceName *out): 256 bytes
static void ov_device_name(Cpu *c) {
    char *out = arg_p(c, 0);
    if (out) memset(out, 0, 256), memcpy(out, DEVICE_NAME, sizeof DEVICE_NAME);
    ret_x(c, 0);
}
static void ov_device_channels(Cpu *c) { ret_x(c, 2); }
// Result AcquireAudioDeviceNotificationForOutput(os::SystemEvent *): an event that never fires
static void ov_device_notification(Cpu *c) {
    kern_bind_system_event(c, arg_x(c, 0), 1);
    ret_x(c, 0);
}

HOST_OVERRIDE("_ZN2nn5audio19OpenDefaultAudioOutEPNS0_8AudioOutERKNS0_17AudioOutParameterE", ov_open_default);
HOST_OVERRIDE("_ZN2nn5audio19OpenDefaultAudioOutEPNS0_8AudioOutEPNS_2os11SystemEventERKNS0_17AudioOutParameterE",
              ov_open_default_event);
HOST_OVERRIDE("_ZN2nn5audio12OpenAudioOutEPNS0_8AudioOutEPKcRKNS0_17AudioOutParameterE", ov_open_named);
HOST_OVERRIDE("_ZN2nn5audio12OpenAudioOutEPNS0_8AudioOutEPNS_2os11SystemEventEPKcRKNS0_17AudioOutParameterE",
              ov_open_named_event);
HOST_OVERRIDE("_ZN2nn5audio13CloseAudioOutEPNS0_8AudioOutE", ov_close);
HOST_OVERRIDE("_ZN2nn5audio13StartAudioOutEPNS0_8AudioOutE", ov_start);
HOST_OVERRIDE("_ZN2nn5audio12StopAudioOutEPNS0_8AudioOutE", ov_stop);
HOST_OVERRIDE("_ZN2nn5audio20AppendAudioOutBufferEPNS0_8AudioOutEPNS0_14AudioOutBufferE", ov_append);
HOST_OVERRIDE("_ZN2nn5audio25GetReleasedAudioOutBufferEPNS0_8AudioOutE", ov_released);
HOST_OVERRIDE("_ZN2nn5audio22ContainsAudioOutBufferEPKNS0_8AudioOutEPKNS0_14AudioOutBufferE", ov_contains);
HOST_OVERRIDE("_ZN2nn5audio20FlushAudioOutBuffersEPNS0_8AudioOutE", ov_flush);
HOST_OVERRIDE("_ZN2nn5audio16GetAudioOutStateEPKNS0_8AudioOutE", ov_state);
HOST_OVERRIDE("_ZN2nn5audio23GetAudioOutChannelCountEPKNS0_8AudioOutE", ov_channels);
HOST_OVERRIDE("_ZN2nn5audio21GetAudioOutSampleRateEPKNS0_8AudioOutE", ov_rate);
HOST_OVERRIDE("_ZN2nn5audio23GetAudioOutSampleFormatEPKNS0_8AudioOutE", ov_format);
HOST_OVERRIDE("_ZN2nn5audio22GetAudioOutBufferCountEPKNS0_8AudioOutE", ov_buffer_count);
HOST_OVERRIDE("_ZN2nn5audio28GetAudioOutPlayedSampleCountEPKNS0_8AudioOutE", ov_played);
HOST_OVERRIDE("_ZN2nn5audio17GetAudioOutVolumeEPKNS0_8AudioOutE", ov_get_volume);
HOST_OVERRIDE("_ZN2nn5audio17SetAudioOutVolumeEPNS0_8AudioOutEf", ov_set_volume);
HOST_OVERRIDE("_ZN2nn5audio24GetActiveAudioDeviceNameEPNS0_15AudioDeviceNameE", ov_device_name);
HOST_OVERRIDE("_ZN2nn5audio30GetActiveAudioOutputDeviceNameEPNS0_15AudioDeviceNameE", ov_device_name);
HOST_OVERRIDE("_ZN2nn5audio41GetActiveAudioDeviceChannelCountForOutputEv", ov_device_channels);
HOST_OVERRIDE("_ZN2nn5audio39AcquireAudioDeviceNotificationForOutputEPNS_2os11SystemEventE", ov_device_notification);
