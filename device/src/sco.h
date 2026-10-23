#ifndef HERA_SCO_H
#define HERA_SCO_H

/* Listens for the phone's synchronous voice channel. The accepted socket is
 * handed straight to the audio engine, which owns it from then on. */

typedef struct sco_server sco_server_t;

/* `on_conn` runs on the accept thread and takes ownership of the fd. */
sco_server_t *sco_server_create(const char *adapter_addr,
                                void (*on_conn)(void *ud, int fd), void *ud);
int  sco_server_start(sco_server_t *s);
void sco_server_stop(sco_server_t *s);   /* stops and frees */

#endif /* HERA_SCO_H */
