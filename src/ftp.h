#ifndef FTP_H
#define FTP_H

#include "core.h"
#include "nes.h"

#define FTP_PORT      1337
#define FTP_DATA_PORT 1338
#define FTP_DEST      "/av_contents/content_tmp/"
#define FTP_BUF_SZ    8192
#define ROM_DIR       "/av_contents/content_tmp/"

/* Create a TCP listen socket on port with SO_REUSEADDR/PORT. Returns fd or -1. */
s32 ftp_open_listen(void *G, void *socket_fn, void *bind_fn, void *listen_fn,
                    void *setsockopt_fn, void *close_fn, int port);

/* Close fd if >= 0 (safe). */
void ftp_safe_close(void *G, void *close_fn, s32 fd);

int ftp_serve(s32 srv_fd, s32 data_listen_fd,
              void *G, void *D, void *load_mod, void *mmap,
              void *kopen, void *kwrite, void *kclose, void *kmkdir,
              void *getdents, void *usleep,
              void *recvfrom, void *sendto, void *accept,
              void *getsockname,
              s32 log_fd, u8 *log_sa, s32 userId,
              struct rom_entry *roms, int max_roms);

#endif
