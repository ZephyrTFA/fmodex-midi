/* Mock OmniMIDI.dll (KDMAPI exports only) for testing the kdmapi backend
 * without OmniMIDI: logs every call, in the log backend's "HEX..." format
 * for MIDI data, to %TEMP%\omni-mock.log. Mimics OmniMIDI 14's contract:
 * SendDirectLongData requires a prepared header. */
#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>

static FILE *f;
static int inited;

static void lg(const char *s) { if (f) { fputs(s, f); fputc('\n', f); fflush(f); } }

BOOL WINAPI IsKDMAPIAvailable(void) { lg("IsKDMAPIAvailable"); return TRUE; }
BOOL WINAPI InitializeKDMAPIStream(void)
{
    char p[MAX_PATH];
    GetTempPathA(MAX_PATH, p);
    lstrcatA(p, "omni-mock.log");
    f = fopen(p, "w");
    lg("InitializeKDMAPIStream");
    inited = 1;
    return TRUE;
}
BOOL WINAPI TerminateKDMAPIStream(void) { lg("TerminateKDMAPIStream"); inited = 0; if (f) fclose(f); f = NULL; return TRUE; }
void WINAPI ResetKDMAPIStream(void) { lg("ResetKDMAPIStream"); }
void WINAPI SendDirectData(DWORD m)
{
    char b[32];
    int st = m & 0xff, n = (st & 0xe0) == 0xc0 ? 2 : 3;
    if (!inited) { lg("ERROR SendDirectData before init"); return; }
    if (n == 2) sprintf(b, "%02X %02X", st, (unsigned)(m >> 8) & 0xff);
    else sprintf(b, "%02X %02X %02X", st, (unsigned)(m >> 8) & 0xff, (unsigned)(m >> 16) & 0xff);
    lg(b);
}
MMRESULT WINAPI PrepareLongData(MIDIHDR *h, UINT sz)
{
    if (sz < sizeof *h) return MMSYSERR_INVALPARAM;
    h->dwFlags |= MHDR_PREPARED;
    lg("PrepareLongData");
    return MMSYSERR_NOERROR;
}
MMRESULT WINAPI UnprepareLongData(MIDIHDR *h, UINT sz)
{
    h->dwFlags &= ~MHDR_PREPARED;
    lg("UnprepareLongData");
    return MMSYSERR_NOERROR;
}
MMRESULT WINAPI SendDirectLongData(MIDIHDR *h, UINT sz)
{
    if (!(h->dwFlags & MHDR_PREPARED)) { lg("ERROR long data not prepared"); return MIDIERR_UNPREPARED; }
    char b[1024];
    int n = 0;
    for (DWORD i = 0; i < h->dwBufferLength && n < 1000; i++)
        n += sprintf(b + n, i ? " %02X" : "%02X", (unsigned char)h->lpData[i]);
    lg(b);
    h->dwFlags |= MHDR_DONE;
    return MMSYSERR_NOERROR;
}
BOOL WINAPI DllMain(HINSTANCE i, DWORD r, LPVOID p) { return TRUE; }
