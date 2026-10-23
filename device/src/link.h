#ifndef HERA_LINK_H
#define HERA_LINK_H

#include <stddef.h>
#include <stdint.h>

#include "proto.h"

/* Transport layer: the two channels of PROTOCOL.md §4 and the session of §5.
 *
 * A dedicated thread owns all sockets. It polls the control listener, up to
 * two control connections (the current one and a pending one, §5.7), the
 * audio socket and a wake-up eventfd, and reassembles the control byte
 * stream into frames (§4.1.1).
 *
 * Control uses TCP and audio uses UDP (§4). There are no output queues:
 * control frames are written directly to the socket, and an audio datagram
 * the socket does not accept is dropped (§6.6). */

typedef struct link link_t;

/* All callbacks run on the link thread. They must not block. */
typedef struct {
    void *ud;
    /* HELLO accepted and READY sent: the client is ready for the state
     * snapshot (§5.4). `resumed` is set when an existing session continues
     * (resume or takeover). */
    void (*on_up)(void *ud, int resumed);
    /* The session ended. `expired` is set when the resume window ran out,
     * and clear when the client said BYE or the session failed. */
    void (*on_down)(void *ud, int expired);
    /* The control channel dropped but the session is still resumable (§5.4):
     * the call stays up and the device feeds silence to the phone. */
    void (*on_suspend)(void *ud);
    /* A CMD frame arrived; the link has NOT acked it yet. */
    void (*on_cmd)(void *ud, const hera_tlv_t *cmd);
    /* An audio block from the client, already validated. */
    void (*on_audio)(void *ud, const hera_audio_t *block);
    /* The client's `ts` jumped by `missing` blocks (§6.4). */
    void (*on_audio_gap)(void *ud, unsigned missing);
    /* The negotiated audio format changed (§6.2). */
    void (*on_format)(void *ud, uint8_t fmt, unsigned frame_ms);
    /* CSTATS from the client: diagnostics only (§10.2). */
    void (*on_cstats)(void *ud, const hera_cstats_t *cs);
    /* Once a second while a client is up: time to send STATS. */
    void (*on_tick)(void *ud);
} link_cb_t;

link_t *link_create(const link_cb_t *cb, const char *ident);

/* Start listening. `token` is the 16-byte session token (§11.4), used only as
 * the HELLO HMAC key. Returns 0 on success. */
int  link_start(link_t *l, int ctrl_port, int audio_port,
                const uint8_t token[HERA_TOKEN_LEN]);
/* Stop the thread and close the sockets; link_* calls remain safe. */
void link_stop(link_t *l);
/* Free the link; nothing may use it afterwards. */
void link_destroy(link_t *l);

/* True once HELLO has been exchanged and READY sent. */
int  link_is_up(link_t *l);
/* True while a dropped session is still inside its resume window (§5.4). */
int  link_is_suspended(link_t *l);
/* The negotiated audio format, 0 when there is no session (§6.2). */
uint8_t link_format(link_t *l);

/* The negotiated block duration in ms (§6.2). Fixed for the session once
 * HELLO has been processed. */
unsigned link_frame_ms(link_t *l);
/* The epoch stamped on outgoing blocks (§6.3). */
uint8_t link_epoch(link_t *l);

/* Tell the link the SCO sample rate, so format selection can prefer the format
 * that needs no conversion (§6.2). Call it before AUDIO_OPEN. */
void link_set_sco_rate(link_t *l, unsigned rate);

/* Bump the epoch: a new audio session starts, `ts` restarts from 0 (§6.3). */
void link_new_audio_epoch(link_t *l);

/* Apply CMD SET_FORMAT. Returns 0 if the format was accepted, or -1 and
 * leaves the current format unchanged (§9). */
int  link_set_format(link_t *l, uint8_t fmt);

/* ----- output (thread-safe) -----
 * Control writes may wait briefly for socket space; audio never waits and is
 * dropped instead (§6.6). */
void link_send_event(link_t *l, uint8_t id, uint8_t a1, uint16_t a2,
                     const char *str);
void link_send_ack(link_t *l, uint8_t id, uint8_t a1);
void link_send_stats(link_t *l, const uint8_t stats[HERA_STATS_LEN]);
void link_send_log(link_t *l, const char *text);

/* Send one audio block to the client. `samples` must hold
 * fmt_block_bytes(link_format(l), link_frame_ms(l)) bytes. */
void link_send_audio(link_t *l, const uint8_t *samples, uint32_t ts);

/* Send BYE on the current session (§5.6). Dropped if no session is up. */
void link_say_bye(link_t *l, uint8_t reason, const char *text);

#endif /* HERA_LINK_H */
