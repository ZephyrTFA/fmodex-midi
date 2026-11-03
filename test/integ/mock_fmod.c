/* A fake fmodex_real.dll for integration tests: implements just the exports
 * the proxy resolves, with inspectable channel state (mock_* exports). It
 * makes no sound. Built as fmodex_real.dll with mock_fmod.def. */
#include <windows.h>
#include <string.h>

typedef int R;
#define OK 0
#define ERR_INVALID_HANDLE 36

struct mch {
    int used, playing, paused, mute;
    float vol, freq;
    void *sound;
    unsigned mode;
};
static struct mch chans[64];
static int sounds[64];
static int nsounds;
static int sys_dummy, group_dummy;

/* Channel handles are 1-based indices cast to pointers, like FMOD's handles. */
static struct mch *CH(void *h)
{
    unsigned i = (unsigned)(UINT_PTR)h;
    return (i >= 1 && i <= 64 && chans[i - 1].used) ? &chans[i - 1] : NULL;
}

R __stdcall mk_System_Create(void **sys) { *sys = &sys_dummy; return OK; }

R __stdcall mk_init(void *s, int n, unsigned f, void *x) { return OK; }
R __stdcall mk_update(void *s) { return OK; }
R __stdcall mk_release(void *s) { return OK; }
R __stdcall mk_createSound(void *s, const char *d, unsigned mode, void *ex, void **snd)
{
    if (nsounds >= 64) return 1;
    *snd = &sounds[nsounds++];
    return OK;
}
R __stdcall mk_getMasterChannelGroup(void *s, void **g) { *g = &group_dummy; return OK; }
R __stdcall mk_playSound(void *s, int id, void *snd, int paused, void **ch)
{
    int i;
    if (id == -2 && CH(*ch)) {
        i = (int)(UINT_PTR)*ch - 1;
    } else {
        for (i = 0; i < 64 && chans[i].used; i++)
            ;
        if (i == 64) return 1;
    }
    chans[i] = (struct mch){ 1, 1, paused & 0xff, 0, 1.0f, 44100.0f, snd, 0 };
    *ch = (void *)(UINT_PTR)(i + 1);
    return OK;
}
R __stdcall mk_Sound_release(void *snd) { return OK; }
R __stdcall mk_Sound_getLength(void *snd, unsigned *len, unsigned unit) { *len = 1851; return OK; }
R __stdcall mk_Sound_getFormat(void *snd, int *t, int *f, int *c, int *b)
{ if (t) *t = 11; if (f) *f = 2; if (c) *c = 2; if (b) *b = 16; return OK; }

#define CHK(c) struct mch *c = CH(ch); if (!c) return ERR_INVALID_HANDLE
R __stdcall mk_stop(void *ch) { CHK(c); c->playing = 0; c->used = 0; return OK; }
R __stdcall mk_setPaused(void *ch, int p) { CHK(c); c->paused = p & 0xff; return OK; }
R __stdcall mk_setVolume(void *ch, float v) { CHK(c); c->vol = v; return OK; }
R __stdcall mk_setFrequency(void *ch, float f) { CHK(c); c->freq = f; return OK; }
R __stdcall mk_getFrequency(void *ch, float *f) { CHK(c); *f = c->freq; return OK; }
R __stdcall mk_setPan(void *ch, float p) { CHK(c); return OK; }
R __stdcall mk_setMute(void *ch, int m) { CHK(c); c->mute = m & 0xff; return OK; }
R __stdcall mk_setPriority(void *ch, int p) { CHK(c); return OK; }
R __stdcall mk_setPosition(void *ch, unsigned p, unsigned u) { CHK(c); return OK; }
R __stdcall mk_getPosition(void *ch, unsigned *p, unsigned u) { CHK(c); *p = 0; return OK; }
R __stdcall mk_setMode(void *ch, unsigned m) { CHK(c); c->mode = m; return OK; }
R __stdcall mk_setCallback(void *ch, void *cb) { CHK(c); return OK; }
R __stdcall mk_set3DAttributes(void *ch, const float *p, const float *v) { CHK(c); return OK; }
R __stdcall mk_isPlaying(void *ch, unsigned char *p) { CHK(c); *p = (unsigned char)c->playing; return OK; }

R __stdcall mk_g_setVolume(void *g, float v) { return OK; }
R __stdcall mk_g_setPitch(void *g, float p) { return OK; }
R __stdcall mk_g_setPaused(void *g, int p) { return OK; }
R __stdcall mk_g_setMute(void *g, int m) { return OK; }
R __stdcall mk_g_stop(void *g) { for (int i = 0; i < 64; i++) chans[i].used = chans[i].playing = 0; return OK; }

/* Test controls. */
int __stdcall mock_get_mute(void *ch) { struct mch *c = CH(ch); return c ? c->mute : -1; }
void __stdcall mock_end(void *ch) { struct mch *c = CH(ch); if (c) { c->playing = 0; c->used = 0; } }

BOOL WINAPI DllMain(HINSTANCE h, DWORD r, LPVOID p) { return TRUE; }
