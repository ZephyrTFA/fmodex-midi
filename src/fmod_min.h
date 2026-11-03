/* Minimal FMOD Ex 4.32 definitions: only what the proxy needs.
 *
 * Deliberately NOT derived by including Firelight's headers (see CLAUDE.md).
 * Values match the public FMOD Ex 4.32 API and, where it matters, were checked
 * against fmodex.dll 4.32.1 itself (docs/design.md). Opaque handles are
 * void*; every C++ member function is __stdcall with `this` as stack arg 0.
 */
#ifndef FMOD_MIN_H
#define FMOD_MIN_H

typedef int FMOD_RESULT;
#define FMOD_OK                 0
#define FMOD_ERR_INTERNAL       33
#define FMOD_ERR_INVALID_HANDLE 36
#define FMOD_ERR_INVALID_PARAM  37

/* FMOD_MODE bits */
#define FMOD_LOOP_OFF           0x00000001u
#define FMOD_LOOP_NORMAL        0x00000002u
#define FMOD_LOOP_BIDI          0x00000004u
#define FMOD_CREATESTREAM       0x00000080u
#define FMOD_OPENMEMORY         0x00000800u
#define FMOD_OPENRAW            0x00001000u
#define FMOD_OPENONLY           0x00002000u
#define FMOD_ACCURATETIME       0x00004000u
#define FMOD_NONBLOCKING        0x00010000u
#define FMOD_OPENMEMORY_POINT   0x10000000u

#define FMOD_TIMEUNIT_MS        0x00000001u
#define FMOD_TIMEUNIT_PCM       0x00000002u
#define FMOD_TIMEUNIT_PCMBYTES  0x00000004u

#define FMOD_CHANNEL_FREE       (-1)
#define FMOD_CHANNEL_REUSE      (-2)

#define FMOD_SOUND_TYPE_MIDI    11

/* FMOD_CREATESOUNDEXINFO: we only touch cbsize (+0) and length (+4).
 * sizeof is 0x7c in the 4.32 layout. */
#define EXINFO_CBSIZE(p)  (*(const int *)((const char *)(p) + 0))
#define EXINFO_LENGTH(p)  (*(const unsigned *)((const char *)(p) + 4))

/* FMOD's C++ `bool` arguments occupy a 4-byte stack slot; only the low byte
 * is meaningful. Hooks take them as int and use FBOOL() to read them. */
#define FBOOL(x) (((x) & 0xff) != 0)

typedef void FMOD_SYSTEM_T;
typedef void FMOD_SOUND_T;
typedef void FMOD_CHANNEL_T;
typedef void FMOD_CHANNELGROUP_T;

typedef FMOD_RESULT (__stdcall *FMOD_CHANNEL_CALLBACK)(void *channel, int type, void *a, void *b);

#endif
