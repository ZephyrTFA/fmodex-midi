/* The sequencer: one thread that owns the backend and every voice. Every
 * engine_* call only queues a command and returns; none of them touches
 * FMOD or the backend directly. Voice ids are chosen by the caller. */
#ifndef ENGINE_H
#define ENGINE_H

#include <stdint.h>
#include "smf.h"
#include "config.h"

/* A refcounted parsed song, shared between the Sound* registry and voices. */
struct song_ref {
    volatile long refs;
    struct smf_song *song;
};

struct song_ref *song_ref_new(struct smf_song *song);   /* refs = 1 */
void song_ref_get(struct song_ref *s);
void song_ref_put(struct song_ref *s);

enum { ENGINE_IDLE, ENGINE_STARTING, ENGINE_UP, ENGINE_FAILED };

/* Start the sequencer thread, which opens the backend itself (that can take
 * 100+ ms). Returns -1 if MIDI output is disabled or the thread can't start;
 * otherwise commands queue up until the backend is open. If opening fails,
 * engine_status() becomes ENGINE_FAILED and queued commands are dropped. */
int engine_start(const struct config *cfg);
/* Stop all voices, close the backend, join the thread. */
void engine_shutdown(void);
int engine_status(void);

/* New voice in state Prepared; takes its own reference on `song`. */
void engine_voice_new(int id, struct song_ref *song, int loop, double gain, double ratio);
/* paused=0 on a Prepared voice starts it (most recent start owns the device). */
void engine_voice_paused(int id, int paused);
void engine_voice_seek(int id, int64_t pos_us);
void engine_voice_gain(int id, double gain);
void engine_voice_ratio(int id, double ratio);
void engine_voice_loop(int id, int loop);
/* Notes off and forget the voice. */
void engine_voice_free(int id);
/* Text marker into the event log (log backend only). */
void engine_mark(const char *text);

/* 1 once the voice's song has played to its end (not looping) or it was cut
 * off by a newer voice; for midiplay. */
int engine_voice_done(int id);

#endif
