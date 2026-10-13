/*
 * wgcore: MobileWonderGate adapter emulation.
 *
 * The console talks to the adapter over the EXT UART (9600 8-N-1) in small
 * frames: [type][0][len][cmd][params...], len = 1 + param count. The adapter
 * owns TCP/IP, so we map its socket commands onto a host network stack.
 *
 * Portable C99, no allocation. The platform supplies serial out, DNS and
 * TCP through wg_ops. Used by the PC host program and, later, the Wi-Fi
 * module firmware.
 *
 * Protocol knowledge: trap15's wonderfence (MIT, (C)2014 Alex Marshall),
 * the WSdev wiki, and a real-adapter capture. See docs/wondergate.md.
 */
#ifndef WG_H
#define WG_H

#include <stddef.h>
#include <stdint.h>

#define WG_MAX_SOCKS     8
#define WG_MAX_REDIRECTS 32
#define WG_MAX_PORTMAPS  8
#define WG_NAME_LEN      64
#define WG_RECV_NODATA   (-2)

/* Largest payload (cmd + params) the adapter accepts from the console. */
#define WG_MAX_PAYLOAD   145

typedef struct wg_ops {
  void *ctx;
  /* Bytes to the console. */
  void (*tx)(void *ctx, const uint8_t *buf, size_t len);
  /* Name to IPv4. 0 = ok. */
  int  (*resolve)(void *ctx, const char *name, uint8_t ip[4]);
  /* TCP. Handles are >= 0, negative = error. */
  int  (*sock_open)(void *ctx);
  int  (*sock_connect)(void *ctx, int h, const uint8_t ip[4], uint16_t port); /* 0 = ok */
  int  (*sock_send)(void *ctx, int h, const uint8_t *buf, size_t len);        /* bytes sent, < 0 = error */
  /* Bytes read (> 0); WG_RECV_NODATA = still open, nothing arrived within the
   * platform's short wait; other values <= 0 = closed. */
  int  (*sock_recv)(void *ctx, int h, uint8_t *buf, size_t len);
  void (*sock_shutdown)(void *ctx, int h, int how);
  void (*sock_close)(void *ctx, int h);
  /* One line of log text, no newline. May be NULL. */
  void (*log)(void *ctx, const char *line);
} wg_ops;

typedef struct wg_config {
  uint8_t reception;                       /* 0-15 bars reported to games */
  char    server[WG_NAME_LEN];             /* where redirected names go */
  /* Names sent to `server`. "*.example.jp" matches the domain and all subdomains. */
  char    redirect[WG_MAX_REDIRECTS][WG_NAME_LEN];
  int     n_redirect;
  /* Port remaps for connections to `server` (dev: unprivileged ports). */
  struct { uint16_t from, to; } portmap[WG_MAX_PORTMAPS];
  int     n_portmap;
} wg_config;

typedef struct wg {
  const wg_ops    *ops;
  const wg_config *cfg;

  uint8_t status;                          /* WG_STAT_* */
  int     awake;                           /* woken by a 00 burst or a power-on */
  int     sock[WG_MAX_SOCKS];              /* host handle, -1 = free */

  uint8_t server_ip[4];
  int     have_server_ip;

  /* Frame parser. */
  int     rx_state;
  uint8_t rx_type, rx_len;
  uint8_t rx_buf[256];
  int     rx_got;
} wg;

enum { WG_STAT_OFF = 0, WG_STAT_ON = 2, WG_STAT_PPP = 3 };

void wg_init(wg *g, const wg_ops *ops, const wg_config *cfg);
/* Feed one byte from the console. Replies go out through ops->tx. */
void wg_rx(wg *g, uint8_t byte);
/* Drop a half-received frame (call after ~500 ms of line silence). */
void wg_rx_reset(wg *g);
/* Console powered off or link lost: close sockets, adapter back to OFF. */
void wg_reset(wg *g);

/* Config helpers. Return 0 on success. */
void wg_config_defaults(wg_config *cfg);
int  wg_config_line(wg_config *cfg, const char *line);
int  wg_name_redirected(const wg_config *cfg, const char *name);

#endif
