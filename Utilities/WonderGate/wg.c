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

enum { RX_TYPE, RX_ZERO, RX_LEN, RX_BODY, RX_LINK };

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
    WG_LINK_HOST,           /* ours: link-cable stage uploads (Lode Runner) */
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

/* Port remap for a connection to the configured server. */
static uint16_t server_port(const wg_config *cfg, uint16_t port)
{
  int i;

  for (i = 0; i < cfg->n_portmap; i++)
    if (cfg->portmap[i].from == port)
      return cfg->portmap[i].to;
  return port;
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
  /* Cmd 0B is call-state news: 00 = connected (sent after dial), Fx = line
   * down (the games' libraries map F0 to error 007, "通信が切断されました").
   * Wonder Classic checks this reply exactly: 5 bytes in all and (byte & F0)
   * == F0, or it retries the hang-up three times (its code at 6000:0143 and
   * table A000:0272). Other games ignore the byte. WonderGateway's capture
   * (by Yuvi) shows 0B 02 for a busy line. */
  static const uint8_t z[1] = { 0xF0 };

  logf_(g, "# hang up");
  close_all(g);
  g->status = WG_STAT_ON;
  reply(g, 0x01, 0x0B, z, sizeof z);
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
  /* params: [sock][family hi][family lo][port lo][port hi][ip 4, network order]
   * Socket replies all start with the socket id they answer for: Tonpuusou and
   * Lode Runner read it, Gather Beat retries on a mismatch, Dark Eyes checks it
   * on close. */
  uint8_t r[2] = { n > 0 ? p[0] : 0, 0 };
  int h = n > 0 ? host_sock(g, p[0]) : -1;
  uint16_t fam, port;
  const uint8_t *ip;

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
        real = server_port(g->cfg, port);
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
  /* [sock][1]. Games need a reply here but ignore its contents (tested with
   * 01, 00 and 00 01), so the shape follows the other socket replies. */
  uint8_t r[2] = { n > 0 ? p[0] : 0, 1 };
  int h = n > 0 ? host_sock(g, p[0]) : -1;

  if (h >= 0)
    g->ops->sock_shutdown(g->ops->ctx, h, n > 1 ? p[1] : 2);
  logf_(g, "# shutdown %02X", n > 0 ? p[0] : 0xFF);
  reply(g, 0x11, 0x06, r, 2);
}

static void cmd_sockdel(wg *g, const uint8_t *p, int n)
{
  /* 2 bytes, like connect: 00 01 = ok (Sennou Millennium checks for 1;
   * WonderGateway notes Rainbow Islands fails on a 1-byte reply). */
  uint8_t r[2] = { n > 0 ? p[0] : 0, 0 };
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
  uint8_t r[2] = { n > 0 ? p[0] : 0, 0xFF };
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

  r[0] = n > 0 ? p[0] : 0;
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

/* ---- link-cable peer: Lode Runner for WonderSwan, DATA I/O (our addition) ----
 *
 * DATA I/O sends a map-editor slot (MAP1/MAP2) to a second console over the
 * link cable: same EXT port, same 9600 8-N-1 as the adapter. Its handshake is
 * NUL-terminated ASCII words (SEND RECV OK NG SAME) and none starts with a
 * WonderGate frame type, so a sending console is recognised by its "SEND\0"
 * between frames; no mode switch. We answer as a console in receive mode
 * ("RECV", then "OK"), the sender answers "OK" and streams the 0x200-byte slot
 * record with no further acknowledgement. We check the record and upload it to
 * our server. Protocol and ROM addresses: docs/wondergate/lode-runner.md. */

enum { LR_HANDSHAKE = 1, LR_DATA };

static const char lr_send[] = "SEND";       /* with its NUL: 5 bytes */

static void lr_say(wg *g, const char *word)
{
  logf_(g, "# Lode Runner link: say \"%s\"", word);
  send_raw(g, (const uint8_t *)word, strlen(word) + 1);
}

static void lr_stop(wg *g)
{
  g->lr_state = 0;
  g->lr_match = 0;
  g->rx_state = RX_TYPE;
  g->rx_got = 0;
}

/* zlib's CRC-32 (the game's routine at 8000:7AB4), bitwise: no table to carry. */
static uint32_t crc32_(const uint8_t *p, size_t n)
{
  uint32_t c = 0xFFFFFFFFu;
  int k;

  while (n--) {
    c ^= *p++;
    for (k = 0; k < 8; k++)
      c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
  }
  return ~c;
}

static int send_all(wg *g, int h, const uint8_t *p, size_t n)
{
  while (n) {
    int k = g->ops->sock_send(g->ops->ctx, h, p, n);
    if (k <= 0)
      return -1;
    p += k;
    n -= (size_t)k;
  }
  return 0;
}

/* POST the record to our server through the platform's normal network path. */
static void lr_upload(wg *g)
{
  int redirected = wg_name_redirected(g->cfg, WG_LINK_HOST);
  const char *target = redirected ? g->cfg->server : WG_LINK_HOST;
  uint16_t port = redirected ? server_port(g->cfg, 80) : 80;
  char head[200], t[300];
  uint8_t ip[4], resp[400];
  int h, n, got = 0, waits = 0, status = 0, body = 0;

  if (g->ops->resolve(g->ops->ctx, target, ip) != 0) {
    logf_(g, "# Lode Runner link: lookup '%s' failed, stage not uploaded", target);
    return;
  }
  if ((h = g->ops->sock_open(g->ops->ctx)) < 0) {
    logf_(g, "# Lode Runner link: no socket, stage not uploaded");
    return;
  }
  if (g->ops->sock_connect(g->ops->ctx, h, ip, port) != 0) {
    logf_(g, "# Lode Runner link: connect %u.%u.%u.%u:%u failed, stage not uploaded",
          ip[0], ip[1], ip[2], ip[3], port);
    g->ops->sock_close(g->ops->ctx, h);
    return;
  }
  n = snprintf(head, sizeof head, "POST /loderunner/stage HTTP/1.0\r\nHost: %s\r\n"
               "Content-Type: application/octet-stream\r\nContent-Length: %d\r\n\r\n",
               WG_LINK_HOST, WG_LR_RECORD);
  logf_(g, "# Lode Runner link: POST http://%s/loderunner/stage via %u.%u.%u.%u:%u, %d bytes",
        WG_LINK_HOST, ip[0], ip[1], ip[2], ip[3], port, WG_LR_RECORD);
  if (send_all(g, h, (const uint8_t *)head, (size_t)n) || send_all(g, h, g->lr_rec, WG_LR_RECORD)) {
    logf_(g, "# Lode Runner link: send failed, stage not uploaded");
    g->ops->sock_close(g->ops->ctx, h);
    return;
  }
  /* HTTP/1.0: the server closes after its reply. */
  while (got < (int)sizeof resp) {
    n = g->ops->sock_recv(g->ops->ctx, h, resp + got, sizeof resp - (size_t)got);
    if (n == WG_RECV_NODATA && ++waits < 3)
      continue;
    if (n <= 0)
      break;
    got += n;
  }
  g->ops->sock_close(g->ops->ctx, h);

  if (got > 9 && !memcmp(resp, "HTTP/", 5)) {
    for (n = 5; n < got && resp[n] != ' '; n++)
      ;
    for (n++; n < got && resp[n] >= '0' && resp[n] <= '9'; n++)
      status = status * 10 + (resp[n] - '0');
  }
  for (n = 0; n + 3 < got; n++)
    if (!memcmp(resp + n, "\r\n\r\n", 4)) {
      body = n + 4;
      break;
    }
  text(t, sizeof t, resp + body, (size_t)(body ? got - body : got));
  logf_(g, "# Lode Runner link: upload %s (HTTP %d): %s",
        status >= 200 && status < 300 ? "accepted" : "FAILED", status, t);
}

/* The whole record is in: check it the way the game's slot loader does. */
static void lr_finish(wg *g)
{
  const uint8_t *r = g->lr_rec;
  uint32_t want = (uint32_t)r[0x1FA] | (uint32_t)r[0x1FB] << 8 | (uint32_t)r[0x1FC] << 16 | (uint32_t)r[0x1FD] << 24;
  uint32_t crc = crc32_(r, 0x1FA);
  const char *why = NULL;
  int i;

  lr_stop(g);
  for (i = 0; i < 28 * 18 && !why; i++)
    if (r[i] > 0x1E)
      why = "tile value above 0x1E";
  if (r[0x1FE] != 0x4C || r[0x1FF] != 0x97)
    why = "no 4C 97 valid mark";
  else if (crc != want)
    why = "CRC-32 mismatch";
  logf_(g, "# Lode Runner link: stage received, music %u, CRC %08lX %s",
        (unsigned)(r[0x1F8] | r[0x1F9] << 8), (unsigned long)want, crc == want ? "ok" : "BAD");
  if (why) {
    logf_(g, "# Lode Runner link: record rejected (%s), not uploaded", why);
    return;
  }
  lr_upload(g);
}

/* A handshake word arrived (its NUL just did). */
static void lr_word(wg *g)
{
  const char *w = g->lr_word;

  logf_(g, "# Lode Runner link: heard \"%s\"", w);
  if (!strcmp(w, "OK")) {
    /* Both sides have each other's mode: the record follows. */
    g->lr_state = LR_DATA;
    g->lr_got = 0;
  } else if (!strcmp(w, "SEND")) {
    lr_say(g, "OK");                       /* a repeat: answer it again */
  } else if (!strcmp(w, "NG")) {
    lr_say(g, "RECV");                     /* it missed our mode: say it again */
  } else {
    logf_(g, "# Lode Runner link: unexpected, transfer abandoned");
    lr_stop(g);
  }
}

static void lr_rx(wg *g, uint8_t b)
{
  if (g->lr_state == LR_DATA) {
    g->lr_rec[g->lr_got++] = b;
    if (g->lr_got == WG_LR_RECORD)
      lr_finish(g);
    return;
  }
  if (b) {
    if (g->lr_wlen + 1 >= (int)sizeof g->lr_word) {
      logf_(g, "# Lode Runner link: not a DATA I/O word, transfer abandoned");
      lr_stop(g);
      return;
    }
    g->lr_word[g->lr_wlen++] = (char)b;
    return;
  }
  g->lr_word[g->lr_wlen] = '\0';
  g->lr_wlen = 0;
  lr_word(g);
}

/* Between frames: is this the opening "SEND\0" of a DATA I/O sender? Returns 1
 * if the byte belongs to it (none of S E N D starts a frame anyway). */
static int lr_detect(wg *g, uint8_t b)
{
  if (b != (uint8_t)lr_send[g->lr_match]) {
    g->lr_match = b == (uint8_t)lr_send[0];
    return g->lr_match;
  }
  if (++g->lr_match < (int)sizeof lr_send)
    return 1;
  logf_(g, "# Lode Runner link: a console is sending a stage (DATA I/O); answering as the receiving console");
  g->lr_match = 0;
  g->lr_state = LR_HANDSHAKE;
  g->lr_wlen = 0;
  g->lr_waited = 0;
  g->rx_state = RX_LINK;
  lr_say(g, "RECV");                       /* what a console in receive mode says */
  lr_say(g, "OK");                         /* and its answer to "SEND" */
  return 1;
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
  if (g->rx_state == RX_LINK) {
    /* After its "OK" the sender reads the slot from its EEPROM before the
     * record starts: about 130 ms, longer than a short frame gap. */
    if (g->lr_state == LR_DATA && g->lr_got == 0 && !g->lr_waited) {
      g->lr_waited = 1;
      logf_(g, "# Lode Runner link: waiting for the record (the sender reads its EEPROM first)");
      return;
    }
    logf_(g, "# Lode Runner link: line quiet, transfer dropped (%d of %d record bytes)",
          g->lr_state == LR_DATA ? g->lr_got : 0, WG_LR_RECORD);
    lr_stop(g);
    return;
  }
  g->lr_match = 0;
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
  case RX_LINK:
    lr_rx(g, b);
    return;
  case RX_TYPE:
    if (lr_detect(g, b))
      return;
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
