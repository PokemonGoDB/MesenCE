/* wgcore: MobileWonderGate adapter emulation. See wg.h. */
#include "wg.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* "10" in ASCII, low byte first: adapter firmware 1.0. */
#define WG_VERSION_LO '1'
#define WG_VERSION_HI '0'

/* Socket ids handed to the console. WonderGateway's tested value; trap15 used 0x30. */
#define WG_FIRST_SOCK 0

enum { RX_TYPE, RX_ZERO, RX_LEN, RX_BODY };

static void logf_(wg *g, const char *fmt, ...)
{
  char line[600];
  va_list ap;

  if (!g->ops->log)
    return;
  va_start(ap, fmt);
  vsnprintf(line, sizeof line, fmt, ap);
  va_end(ap);
  g->ops->log(g->ops->ctx, line);
}

static void hex(char *out, size_t outsz, const uint8_t *p, size_t n)
{
  size_t i, o = 0;

  out[0] = '\0';
  for (i = 0; i < n && o + 4 < outsz; i++)
    o += (size_t)snprintf(out + o, outsz - o, i ? " %02X" : "%02X", p[i]);
}

/* Printable view of socket data, for the trace. */
static void text(char *out, size_t outsz, const uint8_t *p, size_t n)
{
  size_t i, o = 0;

  for (i = 0; i < n && o + 5 < outsz; i++) {
    uint8_t c = p[i];
    if (c == '\r')      { out[o++] = '\\'; out[o++] = 'r'; }
    else if (c == '\n') { out[o++] = '\\'; out[o++] = 'n'; }
    else if (c >= 0x20 && c < 0x7F) out[o++] = (char)c;
    else o += (size_t)snprintf(out + o, outsz - o, "\\x%02X", c);
  }
  out[o] = '\0';
}

static void send_raw(wg *g, const uint8_t *p, size_t n)
{
  char h[800];

  hex(h, sizeof h, p, n);
  logf_(g, "=> %s", h);
  g->ops->tx(g->ops->ctx, p, n);
}

static void reply(wg *g, uint8_t type, uint8_t cmd, const uint8_t *param, size_t n)
{
  uint8_t f[4 + 256];

  if (n > 251)
    n = 251;
  f[0] = type;
  f[1] = 0;
  f[2] = (uint8_t)(n + 1);
  f[3] = cmd;
  if (n)
    memcpy(f + 4, param, n);
  send_raw(g, f, n + 4);
}

static void close_all(wg *g)
{
  int i;

  for (i = 0; i < WG_MAX_SOCKS; i++) {
    if (g->sock[i] >= 0)
      g->ops->sock_close(g->ops->ctx, g->sock[i]);
    g->sock[i] = -1;
  }
}

static int host_sock(wg *g, uint8_t id)
{
  int i = (int)id - WG_FIRST_SOCK;

  if (i < 0 || i >= WG_MAX_SOCKS)
    return -1;
  return g->sock[i];
}

/* ---- config ---- */

void wg_config_defaults(wg_config *cfg)
{
  static const char *const names[] = {
    "*.channel.or.jp",      /* Bandai game servers (wgb01, wgg01, *.wgg) */
    "*.mopera.ne.jp",       /* browser BPL check, mail */
    "*.sting.co.jp",        /* Wizardry Scenario 1 */
    "*.dark-eyes.com",      /* Dark Eyes: Battle Gate */
    "www.bea.hi-ho.ne.jp",  /* Pocket no Naka no Doraemon */
    "homepage2.nifty.com",  /* Doraemon (developer test URLs) */
    "hpcgi2.nifty.com",
    "www.gwp3d.com",        /* in Digimon, Doraemon, Wonder Classic; purpose unknown */
  };
  size_t i;

  memset(cfg, 0, sizeof *cfg);
  cfg->reception = 15;
  strcpy(cfg->server, "127.0.0.1");
  for (i = 0; i < sizeof names / sizeof names[0]; i++)
    strcpy(cfg->redirect[cfg->n_redirect++], names[i]);
}

static int copy_word(char *dst, size_t dstsz, const char **s)
{
  size_t n = 0;

  while (**s == ' ' || **s == '\t')
    (*s)++;
  while (**s && **s != ' ' && **s != '\t' && **s != '#' && **s != '\r' && **s != '\n') {
    if (n + 1 >= dstsz)
      return -1;
    dst[n++] = *(*s)++;
  }
  dst[n] = '\0';
  return n ? 0 : -1;
}

/* One config line: "reception 15", "server host", "redirect *.x.jp",
 * "noredirects", "portmap 80 8080". Blank lines and # comments are fine. */
int wg_config_line(wg_config *cfg, const char *line)
{
  char key[32], a[WG_NAME_LEN], b[16];
  const char *s = line;

  while (*s == ' ' || *s == '\t')
    s++;
  if (*s == '\0' || *s == '#' || *s == '\r' || *s == '\n')
    return 0;
  if (copy_word(key, sizeof key, &s))
    return -1;

  if (!strcmp(key, "noredirects")) {
    cfg->n_redirect = 0;
    return 0;
  }
  if (copy_word(a, sizeof a, &s))
    return -1;

  if (!strcmp(key, "reception")) {
    int r = atoi(a);
    if (r < 0 || r > 15)
      return -1;
    cfg->reception = (uint8_t)r;
  } else if (!strcmp(key, "server")) {
    strcpy(cfg->server, a);
  } else if (!strcmp(key, "redirect")) {
    if (cfg->n_redirect >= WG_MAX_REDIRECTS)
      return -1;
    strcpy(cfg->redirect[cfg->n_redirect++], a);
  } else if (!strcmp(key, "portmap")) {
    if (copy_word(b, sizeof b, &s) || cfg->n_portmap >= WG_MAX_PORTMAPS)
      return -1;
    cfg->portmap[cfg->n_portmap].from = (uint16_t)atoi(a);
    cfg->portmap[cfg->n_portmap].to = (uint16_t)atoi(b);
    cfg->n_portmap++;
  } else {
    return -1;
  }
  return 0;
}

static int ieq(const char *a, const char *b)
{
  while (*a && *b && tolower((unsigned char)*a) == tolower((unsigned char)*b))
    a++, b++;
  return *a == '\0' && *b == '\0';
}

int wg_name_redirected(const wg_config *cfg, const char *name)
{
  size_t nl = strlen(name);
  int i;

  for (i = 0; i < cfg->n_redirect; i++) {
    const char *p = cfg->redirect[i];
    if (p[0] == '*' && p[1] == '.') {
      size_t sl = strlen(p + 1);              /* ".example.jp" */
      if (ieq(name, p + 2))                   /* the bare domain */
        return 1;
      if (nl > sl && ieq(name + nl - sl, p + 1))
        return 1;
    } else if (ieq(name, p)) {
      return 1;
    }
  }
  return 0;
}

/* ---- commands ---- */

static void cmd_poweron(wg *g)
{
  const uint8_t ver[2] = { WG_VERSION_LO, WG_VERSION_HI };

  if (g->status == WG_STAT_OFF)
    close_all(g);
  /* A power-on during a call (the MobileWonderGate cart sends one before it
   * powers the adapter off) doesn't drop the line. */
  if (g->status != WG_STAT_PPP)
    g->status = WG_STAT_ON;
  g->awake = 1;
  reply(g, 0x01, 0x02, ver, 2);
}

static void cmd_poweroff(wg *g, const uint8_t *p, int n)
{
  if (n < 4 || p[0] != 0x55 || p[1] != 0xAA || p[2] != 0x55 || p[3] != 0xAA) {
    logf_(g, "# power-off without 55 AA 55 AA, no reply");
    return;
  }
  /* The MobileWonderGate cart ends a call by powering the adapter off. */
  if (g->status == WG_STAT_PPP)
    logf_(g, "# hang up (power-off)");
  close_all(g);
  g->status = WG_STAT_OFF;
  g->awake = 0;
  reply(g, 0x0F, 0xFF, NULL, 0);
}

static void cmd_dial(wg *g, const uint8_t *p, int n)
{
  /* params: 00 01 03 [len][digits] */
  char num[32] = "";
  int len = n > 3 ? p[3] : 0;
  /* Call-state news "connected": first byte 00, padded to 20 params.
   * SD Gundam Gashapon Senki's reader needs >= 2 params (with one, its byte
   * counter underflows: error 999); Gather Beat treats a frame shorter than
   * 24 bytes as no answer (error 99). The real adapter's reply was long. */
  static const uint8_t ok[20] = { 0 };

  if (len > 0 && 4 + len <= n && len < (int)sizeof num) {
    memcpy(num, p + 4, (size_t)len);
    num[len] = '\0';
  }
  logf_(g, "# dial '%s'", num);
  g->status = WG_STAT_PPP;
  reply(g, 0x01, 0x0B, ok, sizeof ok);
}

static void cmd_hangup(wg *g)
{
  /* Cmd 0B is call-state news: first byte 00 = connected (sent after dial),
   * nonzero = line down (a real adapter sent 0B 02 for a busy line). Sennou
   * Millennium waits for a nonzero 0B after hanging up; Raku Jongg needs 4
   * params. */
  static const uint8_t z[4] = { 1, 0, 0, 0 };

  logf_(g, "# hang up");
  close_all(g);
  g->status = WG_STAT_ON;
  reply(g, 0x01, 0x0B, z, 4);
}

static void cmd_ppp_login(wg *g, const uint8_t *p, int n)
{
  /* params: 07 ?? ?? ?? ?? 01 [ulen][user][plen][pass] */
  char user[64] = "", pass[64] = "";
  int at = 6, ul, pl;
  uint8_t ok = 1;

  ul = at < n ? p[at++] : 0;
  if (ul > 0 && at + ul <= n && ul < (int)sizeof user) {
    memcpy(user, p + at, (size_t)ul);
    user[ul] = '\0';
  }
  at += ul;
  pl = at < n ? p[at++] : 0;
  if (pl > 0 && at + pl <= n && pl < (int)sizeof pass) {
    memcpy(pass, p + at, (size_t)pl);
    pass[pl] = '\0';
  }
  logf_(g, "# PPP login user='%s' pass='%s'", user, pass);
  reply(g, 0x01, 0x10, &ok, 1);
}

static void cmd_set_dns(wg *g, const uint8_t *p, int n)
{
  uint8_t ok = 1;

  if (n >= 0x11)
    logf_(g, "# DNS %u.%u.%u.%u / %u.%u.%u.%u", p[9], p[10], p[11], p[12], p[13], p[14], p[15], p[16]);
  reply(g, 0x01, 0x11, &ok, 1);
}

static void cmd_gethost(wg *g, const uint8_t *p, int n)
{
  char name[WG_NAME_LEN * 2];
  const char *target;
  uint8_t ip[4], r[1 + sizeof name + 4];
  int len = 0, redirected;

  while (len < n && p[len] && len + 1 < (int)sizeof name)
    len++;
  memcpy(name, p, (size_t)len);
  name[len] = '\0';

  redirected = wg_name_redirected(g->cfg, name);
  target = redirected ? g->cfg->server : name;
  if (g->ops->resolve(g->ops->ctx, target, ip) != 0) {
    logf_(g, "# lookup '%s' (%s) failed", name, target);
    r[0] = 0;
    reply(g, 0x11, 0x08, r, 1);
    return;
  }
  if (redirected) {
    memcpy(g->server_ip, ip, 4);
    g->have_server_ip = 1;
  }
  logf_(g, "# lookup '%s'%s -> %u.%u.%u.%u", name, redirected ? " (redirected)" : "",
        ip[0], ip[1], ip[2], ip[3]);

  /* [1][canonical name][0][ip] -- we keep the name the game asked for. */
  r[0] = 1;
  memcpy(r + 1, name, (size_t)len + 1);
  memcpy(r + 2 + len, ip, 4);
  reply(g, 0x11, 0x08, r, (size_t)(1 + len + 1 + 4));
}

static void cmd_socknew(wg *g)
{
  uint8_t id = 0xFF;
  int i, h;

  for (i = 0; i < WG_MAX_SOCKS; i++)
    if (g->sock[i] < 0)
      break;
  if (i < WG_MAX_SOCKS && (h = g->ops->sock_open(g->ops->ctx)) >= 0) {
    g->sock[i] = h;
    id = (uint8_t)(WG_FIRST_SOCK + i);
  }
  logf_(g, "# socket new -> %02X", id);
  reply(g, 0x11, 0x01, &id, 1);
}

static void cmd_sockcon(wg *g, const uint8_t *p, int n)
{
  /* params: [sock][family hi][family lo][port lo][port hi][ip 4, network order] */
  uint8_t r[2] = { 0, 0 };
  int h = n > 0 ? host_sock(g, p[0]) : -1;
  uint16_t fam, port;
  const uint8_t *ip;
  int i;

  if (h >= 0 && n >= 9) {
    fam = (uint16_t)(p[1] << 8 | p[2]);
    port = (uint16_t)(p[3] | p[4] << 8);
    ip = p + 5;
    if (fam == 0 || fam == 2) {
      uint16_t real = port;
      /* The MobileWonderGate cart caches DNS answers across sessions and can
       * connect without a lookup, so learn the server's address here too. */
      if (!g->have_server_ip && g->cfg->server[0] &&
          g->ops->resolve(g->ops->ctx, g->cfg->server, g->server_ip) == 0)
        g->have_server_ip = 1;
      if (g->have_server_ip && !memcmp(ip, g->server_ip, 4))
        for (i = 0; i < g->cfg->n_portmap; i++)
          if (g->cfg->portmap[i].from == port)
            real = g->cfg->portmap[i].to;
      r[1] = g->ops->sock_connect(g->ops->ctx, h, ip, real) == 0;
      logf_(g, "# connect %02X to %u.%u.%u.%u:%u%s -> %s", p[0], ip[0], ip[1], ip[2], ip[3],
            port, real != port ? " (remapped)" : "", r[1] ? "ok" : "FAILED");
    } else {
      logf_(g, "# connect: unknown address family %u", fam);
    }
  }
  reply(g, 0x11, 0x03, r, 2);
}

static void cmd_sockshutdown(wg *g, const uint8_t *p, int n)
{
  uint8_t ok = 1;
  int h = n > 0 ? host_sock(g, p[0]) : -1;

  if (h >= 0)
    g->ops->sock_shutdown(g->ops->ctx, h, n > 1 ? p[1] : 2);
  logf_(g, "# shutdown %02X", n > 0 ? p[0] : 0xFF);
  reply(g, 0x11, 0x06, &ok, 1);
}

static void cmd_sockdel(wg *g, const uint8_t *p, int n)
{
  /* 2 bytes, like connect: 00 01 = ok (Sennou Millennium checks for 1;
   * WonderGateway notes Rainbow Islands fails on a 1-byte reply). */
  uint8_t r[2] = { 0, 0 };
  int i = n > 0 ? (int)p[0] - WG_FIRST_SOCK : -1;

  if (i >= 0 && i < WG_MAX_SOCKS && g->sock[i] >= 0) {
    g->ops->sock_close(g->ops->ctx, g->sock[i]);
    g->sock[i] = -1;
    r[1] = 1;
  }
  logf_(g, "# close %02X", n > 0 ? p[0] : 0xFF);
  reply(g, 0x11, 0x07, r, 2);
}

static void cmd_socksend(wg *g, const uint8_t *p, int n)
{
  uint8_t r[2] = { 0, 0xFF };
  int h = n > 0 ? host_sock(g, p[0]) : -1;
  char t[700];

  if (h >= 0) {
    int sent = n > 1 ? g->ops->sock_send(g->ops->ctx, h, p + 1, (size_t)(n - 1)) : 0;
    if (sent >= 0)
      r[1] = (uint8_t)sent;
  }
  text(t, sizeof t, p + 1, n > 1 ? (size_t)(n - 1) : 0);
  logf_(g, "# send %02X [%s]", n > 0 ? p[0] : 0xFF, t);
  reply(g, 0x11, 0x0E, r, 2);
}

static void cmd_sockrecv(wg *g, uint8_t cmd, const uint8_t *p, int n)
{
  /* 0x0F = up to len bytes; 0x0D = one line, up to and including '\n'. */
  uint8_t r[2 + 250];
  int h = n > 0 ? host_sock(g, p[0]) : -1;
  int want = n > 1 ? p[1] : 0, got = 0;
  char t[700];

  if (want > 250)
    want = 250;
  /* Nothing arrived yet on an open socket = count 0 (games poll again);
   * only a closed socket answers FF. */
  if (h < 0) {
    got = -1;
  } else if (cmd == 0x0F) {
    got = want ? g->ops->sock_recv(g->ops->ctx, h, r + 2, (size_t)want) : 0;
    if (got == WG_RECV_NODATA)
      got = 0;
    else if (want && got <= 0)
      got = -1;
  } else {
    while (got < want) {
      int n = g->ops->sock_recv(g->ops->ctx, h, r + 2 + got, 1);
      if (n == WG_RECV_NODATA)
        break;
      if (n <= 0) {
        if (got == 0)
          got = -1;
        break;
      }
      if (r[2 + got++] == '\n')
        break;
    }
  }

  r[0] = 0;
  if (got < 0) {
    r[1] = 0xFF;
    logf_(g, "# recv%s %02X: closed", cmd == 0x0D ? " line" : "", n > 0 ? p[0] : 0xFF);
    reply(g, 0x11, cmd, r, 2);
    return;
  }
  r[1] = (uint8_t)got;
  text(t, sizeof t, r + 2, (size_t)got);
  logf_(g, "# recv%s %02X [%s]", cmd == 0x0D ? " line" : "", p[0], t);
  reply(g, 0x11, cmd, r, (size_t)(2 + got));
}

static void dispatch(wg *g, uint8_t type, uint8_t cmd, const uint8_t *p, int n)
{
  char h[800];

  hex(h, sizeof h, p, (size_t)n);
  logf_(g, "<= %02X:%02X (%s)", type, cmd, h);

  switch (type << 8 | cmd) {
  case 0x0102: cmd_poweron(g); break;
  case 0x0FFF: cmd_poweroff(g, p, n); break;
  case 0x0200:
    /* Undocumented; SD Gundam Gashapon Senki sends 02:00 (03) before dialing.
     * It only waits for a type-02 frame and drains it, so echo the command. */
    reply(g, 0x02, 0x00, NULL, 0);
    break;
  case 0x0201: {
    reply(g, 0x02, 0x02, &g->status, 1);   /* reply cmd is 02 */
    break;
  }
  case 0x0100: {
    /* Answer "phone ok" even before power-on: Buffers Evolution asks first. */
    uint8_t r[2] = { 0, g->cfg->reception };
    reply(g, 0x01, 0x00, r, 2);
    break;
  }
  case 0x0108: cmd_dial(g, p, n); break;
  case 0x010A: cmd_hangup(g); break;
  case 0x0110: cmd_ppp_login(g, p, n); break;
  case 0x0111: cmd_set_dns(g, p, n); break;
  case 0x1101: cmd_socknew(g); break;
  case 0x1103: cmd_sockcon(g, p, n); break;
  case 0x1106: cmd_sockshutdown(g, p, n); break;
  case 0x1107: cmd_sockdel(g, p, n); break;
  case 0x1108: cmd_gethost(g, p, n); break;
  case 0x110D:
  case 0x110F: cmd_sockrecv(g, cmd, p, n); break;
  case 0x110E: cmd_socksend(g, p, n); break;
  default:
    logf_(g, "# UNKNOWN command %02X:%02X, no reply", type, cmd);
    break;
  }
}

/* ---- framing ---- */

void wg_init(wg *g, const wg_ops *ops, const wg_config *cfg)
{
  int i;

  memset(g, 0, sizeof *g);
  g->ops = ops;
  g->cfg = cfg;
  g->status = WG_STAT_OFF;
  for (i = 0; i < WG_MAX_SOCKS; i++)
    g->sock[i] = -1;
  g->rx_state = RX_TYPE;
}

void wg_reset(wg *g)
{
  close_all(g);
  g->status = WG_STAT_OFF;
  g->awake = 0;
  g->have_server_ip = 0;
  wg_rx_reset(g);
}

void wg_rx_reset(wg *g)
{
  if (g->rx_state != RX_TYPE)
    logf_(g, "# dropped partial frame (%d bytes)", g->rx_got);
  g->rx_state = RX_TYPE;
  g->rx_got = 0;
}

/* Frame types the console sends. Anything else before a frame is line noise:
 * sync bursts (0x00) or a game's link-cable probe (GunPey's 0x8F), which can
 * arrive with no pause before a real frame. */
static int known_type(uint8_t b)
{
  return b == 0x01 || b == 0x02 || b == 0x03 || b == 0x0F || b == 0x11;
}

void wg_rx(wg *g, uint8_t b)
{
  switch (g->rx_state) {
  case RX_TYPE:
    if (b == 0x00 && !g->awake) {
      /* Wake-up: a 00 burst to a sleeping adapter is answered with 55 x5
       * (Sennou Millennium's connect sequence: 00 x10, expect 55 x5, then power-on). */
      static const uint8_t burst[5] = { 0x55, 0x55, 0x55, 0x55, 0x55 };
      g->awake = 1;
      logf_(g, "# 00 wake burst: adapter wakes");
      send_raw(g, burst, sizeof burst);
      return;
    }
    if (!known_type(b)) {
      if (b != 0x00)
        logf_(g, "# skipped noise byte %02X", b);
      return;
    }
    g->rx_type = b;
    g->rx_state = RX_ZERO;
    return;
  case RX_ZERO:
    if (b != 0x00) {    /* not a frame after all: maybe this byte starts one */
      logf_(g, "# skipped noise byte %02X", g->rx_type);
      g->rx_state = RX_TYPE;
      wg_rx(g, b);
      return;
    }
    g->rx_state = RX_LEN;
    return;
  case RX_LEN:
    g->rx_len = b;
    g->rx_got = 0;
    if (b == 0) {
      g->rx_state = RX_TYPE;
      dispatch(g, g->rx_type, 0, NULL, 0);
      return;
    }
    g->rx_state = RX_BODY;
    return;
  case RX_BODY:
    g->rx_buf[g->rx_got++] = b;
    if (g->rx_got < g->rx_len)
      return;
    g->rx_state = RX_TYPE;
    g->rx_got = 0;
    if (g->rx_len > WG_MAX_PAYLOAD) {
      logf_(g, "# oversized frame %02X:%02X (%u bytes), no reply", g->rx_type, g->rx_buf[0], g->rx_len);
      return;
    }
    dispatch(g, g->rx_type, g->rx_buf[0], g->rx_buf + 1, g->rx_len - 1);
    return;
  }
}
