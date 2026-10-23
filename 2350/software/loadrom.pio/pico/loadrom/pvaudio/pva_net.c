// MSX PICOVERSE PROJECT
// pva_net.c - PicoVerse Audio Engine: internet radio streams over the ESP-01.
//
// The cartridge's ESP-01 runs Oduvaldo Pavan's UNAPI ESP8266 firmware. It does
// WiFi, DHCP, DNS, TCP and TLS (BearSSL) itself and talks a small binary
// protocol over the UART (the same one the Nextor UNAPI BIOS and the SymbOS
// network daemon use):
//   request   [fn] [len MSB] [len LSB] [payload]
//   response  [fn] [rc] [len MSB] [len LSB] [payload]   (payload only if rc = 0)
// Unrelated bytes may come before a response; they are skipped until fn.
//   3   NET_STATE   -> [state]                (2 = connected to the access point)
//   206 DNS_Q_NEW   [flags=0][name]  -> [ip4]
//   13  TCP_OPEN    [ip4][rport LE][lport LE][timeout LE][flags][host 0]  -> [conn]
//                   flags bit 2 = TLS (host name for SNI follows), bit 3 = check cert
//   14  TCP_CLOSE   [conn]
//   17  TCP_SEND    [conn][flags=1 push][data]
//   18  TCP_RCV     [conn][size LE]  -> [urgent u16][data]
//
// Everything here is non-blocking: pva_net_poll() moves at most one request
// forward and copies what the UART delivered, so core 1 keeps serving Nextor
// and the decoder between calls. The audio bytes go into a ring buffer that
// the decoder reads with pva_net_read().
//
// HTTP: HTTP/1.0 GET (no chunked replies) with "Icy-MetaData: 1". Redirects
// are followed, .pls / .m3u play list links are opened, ICY metadata is taken
// out of the audio and its StreamTitle kept. When the connection drops or no
// data comes for a while, the stream is opened again; the buffer keeps the
// audio going in the meantime.
//
// This work is licensed under a "Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International
// License". https://creativecommons.org/licenses/by-nc-sa/4.0/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pva_internal.h"

#define URL_MAX         255u
#define HOST_MAX        63u
#define TXQ_MAX         320u
#define SEND_CHUNK      128u
#define RCV_START       1024u
#define RCV_MIN         128u
#define LINE_MAX        255u
#define META_KEEP       255u
#define PLS_MAX         1024u
#define MAX_REDIRECTS   5u
#define RING_MIN        (8u * 1024u)
#define RING_MAX        (96u * 1024u)

#define FN_NETSTATE     3u
#define FN_TCPOPEN      13u
#define FN_TCPCLOSE     14u
#define FN_TCPSEND      17u
#define FN_TCPRCV       18u
#define FN_DNS          206u

#define T_NETSTATE_MS   2000u
#define T_DNS_MS        15000u
#define T_OPEN_MS       30000u
#define T_SEND_MS       3000u
#define T_RCV_MS        2000u
#define T_CLOSE_MS      1000u
#define T_NODATA_MS     10000u      // no audio for this long: open the stream again
#define T_HEADERS_MS    10000u
#define IDLE_POLL_MS    4u          // pause between empty TCP_RCV requests

// ---------------------------------------------------------------- ring
static uint8_t *ring;
static size_t   ring_size, ring_head, ring_tail, ring_count;

static size_t ring_free(void) { return ring_size - ring_count; }

static void ring_put(const uint8_t *p, size_t n)
{
    while (n)
    {
        size_t k = ring_size - ring_head;
        if (k > n) k = n;
        memcpy(ring + ring_head, p, k);
        ring_head = (ring_head + k) % ring_size;
        ring_count += k;
        p += k;
        n -= k;
    }
}

// A stream title is shown when the audio it belongs to is played, not when it
// arrives (that can be a whole buffer, several seconds, earlier).
static char    *pending;            // PVA_ST_TITLE_LEN + PVA_ST_ARTIST_LEN
static bool     pending_set;
static uint32_t pending_pos;        // audio byte count where it starts
static uint32_t read_total, put_total;
static void commit_title(void);

size_t pva_net_read(uint8_t *dst, size_t n)
{
    if (n > ring_count) n = ring_count;
    size_t done = 0;
    while (done < n)
    {
        size_t k = ring_size - ring_tail;
        if (k > n - done) k = n - done;
        memcpy(dst + done, ring + ring_tail, k);
        ring_tail = (ring_tail + k) % ring_size;
        done += k;
    }
    ring_count -= n;
    read_total += (uint32_t)n;
    if (pending_set && (int32_t)(read_total - pending_pos) >= 0) commit_title();
    return n;
}

size_t pva_net_buffered(void) { return ring_count; }
size_t pva_net_capacity(void) { return ring_size; }

// ---------------------------------------------------------------- state
enum
{
    S_IDLE, S_NETSTATE, S_DNS, S_OPEN, S_SEND, S_HEADERS, S_BODY, S_CLOSE, S_WAIT, S_FAILED,
    S_ABANDON       // stopped while an answer (DNS, open) was on its way: wait for it
};
enum { AFTER_NONE, AFTER_RETRY, AFTER_REDIRECT, AFTER_FAIL, AFTER_IDLE };

static pva_net_stats_t st;
static uint8_t  s = S_IDLE;
static bool     hal_open;
static char    *url;                // the URL asked for
static char    *cur;                // the URL in use (after redirects)
static char     host[HOST_MAX + 1];
static char    *path;               // URL_MAX + 1
static uint16_t port;
static bool     tls;
static uint8_t  ip[4];
static bool     have_ip;
static uint8_t  conn;
static uint8_t  after;              // what follows S_CLOSE
static uint8_t  redirects;
static uint32_t wait_until;
static uint32_t retry_delay_ms;
static uint32_t t_start;            // start of the current request phase
static uint32_t last_data_ms;
static bool     ever_streamed;
static bool     abandon_close;      // S_ABANDON: close 'conn' once the answer is in
static bool     restart_pending;    // a new URL waits until the old connection is closed

// HTTP request being sent
static char    *req;                // TXQ_MAX
static size_t   req_len, req_pos;

// response parsing (HTTP)
static char    *line;               // LINE_MAX + 1
static size_t   line_len;
static bool     status_seen;
static uint32_t metaint, meta_left_audio;
static uint32_t meta_len, meta_got;
static char    *meta;               // META_KEEP + 1
static char     station[PVA_ST_ALBUM_LEN];
static char    *title;              // PVA_ST_TITLE_LEN + PVA_ST_ARTIST_LEN
static char    *location;           // URL_MAX + 1
static bool     body_playlist;      // body is a .pls / .m3u: look for the first URL
static char    *pls;                // PLS_MAX + 1
static size_t   pls_len;
static bool     bad_format;

// ESP request / response
static uint8_t *txq;                // TXQ_MAX
static size_t   txq_len, txq_pos;
static uint8_t  want_fn;            // 0 = no request outstanding
static uint32_t want_deadline;
static uint8_t  ph;                 // 0 hunting, 1 rc, 2 len hi, 3 len lo, 4 payload
static uint8_t  rc;
static uint16_t plen, pgot;
static uint8_t  pfirst[8];          // first payload bytes (ip, conn, state, urgent)
static bool     rsp_done, rsp_timeout;
static uint16_t rcv_size;           // size asked in the current TCP_RCV
static uint32_t rcv_data;           // data bytes in the current TCP_RCV answer

// rate: bytes per 250 ms slot, 8 slots
static uint32_t rate_slot[8];
static uint32_t rate_t;
static uint8_t  rate_i;

static void http_feed(const uint8_t *p, size_t n);
static void esp_clear_all(void);
static void begin_session(void);
static void stop_session(void);

// ---------------------------------------------------------------- helpers
static bool is_digit(char c) { return c >= '0' && c <= '9'; }
static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

static bool starts_ci(const char *a, const char *pre)
{
    for (; *pre; a++, pre++)
        if (lower(*a) != lower(*pre)) return false;
    return true;
}

static bool parse_url(const char *u)
{
    const char *p;
    if (starts_ci(u, "http://")) { tls = false; p = u + 7; port = 80; }
    else if (starts_ci(u, "https://")) { tls = true; p = u + 8; port = 443; }
    else return false;
    size_t h = 0;
    while (*p && *p != '/' && *p != ':' && *p != '?')
    {
        if (h >= HOST_MAX) return false;
        host[h++] = *p++;
    }
    host[h] = 0;
    if (!h) return false;
    if (*p == ':')
    {
        p++;
        uint32_t v = 0;
        if (!is_digit(*p)) return false;
        while (is_digit(*p)) { v = v * 10u + (uint32_t)(*p - '0'); if (v > 65535u) return false; p++; }
        port = (uint16_t)v;
    }
    if (*p == '/') snprintf(path, URL_MAX + 1, "%s", p);
    else snprintf(path, URL_MAX + 1, "/%s", p);
    // an IPv4 literal needs no DNS
    int a, b, c, d;
    char tail;
    have_ip = sscanf(host, "%d.%d.%d.%d%c", &a, &b, &c, &d, &tail) == 4 &&
              a >= 0 && a < 256 && b >= 0 && b < 256 && c >= 0 && c < 256 && d >= 0 && d < 256;
    if (have_ip) { ip[0] = (uint8_t)a; ip[1] = (uint8_t)b; ip[2] = (uint8_t)c; ip[3] = (uint8_t)d; }
    return true;
}

// Give the ESP link back to the MSX side
static void release(uint8_t next_state)
{
    esp_clear_all();
    conn = 0;
    if (restart_pending && hal_open)
    {
        restart_pending = false;
        begin_session();            // keep the link: the next stream starts now
        return;
    }
    if (hal_open) pva_hal_net_close();
    hal_open = false;
    conn = 0;
    s = next_state;
}

static void set_failed(uint8_t err)
{
    st.err = err;
    pva_log("stream: failed, error %u (http %u)", (unsigned)err, (unsigned)st.http);
}

// ---------------------------------------------------------------- ESP I/O
static void esp_request(uint8_t fn, const uint8_t *a, size_t n, const uint8_t *b, size_t m, uint32_t timeout)
{
    size_t len = n + m;
    txq[0] = fn;
    txq[1] = (uint8_t)(len >> 8);
    txq[2] = (uint8_t)len;
    if (n) memcpy(txq + 3, a, n);
    if (m) memcpy(txq + 3 + n, b, m);
    txq_len = 3 + len;
    txq_pos = 0;
    want_fn = fn;
    want_deadline = pva_hal_millis() + timeout;
    ph = 0;
    rsp_done = rsp_timeout = false;
    rcv_data = 0;
}

static void payload_bytes(const uint8_t *p, size_t n)
{
    // keep the first bytes; TCP_RCV data (after the 2 urgent bytes) goes to HTTP
    while (n)
    {
        if (pgot < sizeof pfirst && !(want_fn == FN_TCPRCV && pgot >= 2))
        {
            pfirst[pgot++] = *p++;
            n--;
            continue;
        }
        if (want_fn == FN_TCPRCV)
        {
            size_t k = n;
            if (k > (size_t)(plen - pgot)) k = (size_t)(plen - pgot);
            http_feed(p, k);
            rcv_data += (uint32_t)k;
            pgot = (uint16_t)(pgot + k);
            p += k;
            n -= k;
        }
        else
        {
            size_t k = n;
            if (k > (size_t)(plen - pgot)) k = (size_t)(plen - pgot);
            pgot = (uint16_t)(pgot + k);
            p += k;
            n -= k;
        }
    }
}

// Move bytes between the UART and the request/response state.
static void esp_io(void)
{
    if (!hal_open) return;
    if (txq_pos < txq_len)
        txq_pos += pva_hal_net_write(txq + txq_pos, txq_len - txq_pos);

    uint8_t buf[256];
    for (int rounds = 0; rounds < 8; rounds++)
    {
        size_t n = pva_hal_net_read(buf, sizeof buf);
        if (!n) break;
        size_t i = 0;
        while (i < n)
        {
            if (!want_fn || rsp_done) { i = n; break; }   // nothing asked: stray bytes
            uint8_t b = buf[i];
            switch (ph)
            {
                case 0: if (b == want_fn) ph = 1; i++; break;
                case 1: rc = b; ph = 2; i++; break;
                case 2: plen = (uint16_t)(b << 8); ph = 3; i++; break;
                case 3:
                    plen = (uint16_t)(plen | b);
                    pgot = 0;
                    i++;
                    if (plen > 4096u) { ph = 0; break; }    // implausible: keep hunting
                    if (rc != 0 || plen == 0) { rsp_done = true; break; }
                    ph = 4;
                    break;
                default:
                {
                    size_t k = n - i;
                    if (k > (size_t)(plen - pgot)) k = (size_t)(plen - pgot);
                    payload_bytes(buf + i, k);
                    i += k;
                    if (pgot >= plen) rsp_done = true;
                    break;
                }
            }
        }
        if (rsp_done) break;
    }
    if (want_fn && !rsp_done && (int32_t)(pva_hal_millis() - want_deadline) >= 0)
        rsp_timeout = true;
}

// A request is finished (answer or timeout): the caller looks at rc / rsp_timeout.
static bool esp_finished(void) { return rsp_done || rsp_timeout; }
static void esp_clear(void) { want_fn = 0; rsp_done = rsp_timeout = false; }
static void esp_clear_all(void) { esp_clear(); txq_len = txq_pos = 0; }

// ---------------------------------------------------------------- HTTP parsing
static void ring_audio(const uint8_t *p, size_t n)
{
    if (!n) return;
    if (n > ring_free()) { st.overflow += (uint32_t)(n - ring_free()); n = ring_free(); }
    ring_put(p, n);
    put_total += (uint32_t)n;
    st.rx_bytes += (uint32_t)n;
    rate_slot[rate_i] += (uint32_t)n;
    last_data_ms = pva_hal_millis();
    ever_streamed = true;
}

static void parse_stream_title(void)
{
    // StreamTitle='Artist - Title';StreamUrl='...';
    const char *p = strstr(meta, "StreamTitle='");
    if (!p) return;
    p += 13;
    const char *e = strstr(p, "';");
    if (!e) e = p + strlen(p);
    if (e > p && e[-1] == '\'') e--;
    size_t n = (size_t)(e - p);
    char tmp[PVA_ST_TITLE_LEN + PVA_ST_ARTIST_LEN];
    pva_copy_ascii(tmp, sizeof tmp, p, n);
    if (pending_set) commit_title();            // the one before was not played yet: show it now
    if (!strcmp(tmp, title)) return;
    memcpy(pending, tmp, sizeof tmp);
    pending_set = true;
    pending_pos = put_total;
    if (ring_count == 0) commit_title();
}

static void commit_title(void)
{
    pending_set = false;
    if (!strcmp(pending, title)) return;
    memcpy(title, pending, PVA_ST_TITLE_LEN + PVA_ST_ARTIST_LEN);
    st.title_gen++;
    pva_log("stream title: %.40s", title);
}

static void header_line(void)
{
    line[line_len] = 0;
    while (line_len && (line[line_len - 1] == '\r' || line[line_len - 1] == ' ')) line[--line_len] = 0;
    if (!status_seen)
    {
        // "HTTP/1.1 200 OK" or "ICY 200 OK"
        status_seen = true;
        const char *sp = strchr(line, ' ');
        st.http = sp ? (uint16_t)atoi(sp + 1) : 0;
        return;
    }
    char *colon = strchr(line, ':');
    if (!colon) return;
    *colon = 0;
    char *v = colon + 1;
    while (*v == ' ' || *v == '\t') v++;
    if (starts_ci(line, "location") && !line[8]) snprintf(location, URL_MAX + 1, "%s", v);
    else if (starts_ci(line, "icy-metaint") && !line[11]) metaint = (uint32_t)atol(v);
    else if (starts_ci(line, "icy-name") && !line[8]) pva_copy_ascii(station, sizeof station, v, strlen(v));
    else if (starts_ci(line, "content-type") && !line[12])
    {
        if (starts_ci(v, "audio/x-scpls") || starts_ci(v, "audio/x-mpegurl") || starts_ci(v, "audio/mpegurl") ||
            starts_ci(v, "application/pls") || starts_ci(v, "audio/scpls"))
            body_playlist = true;
        else if (starts_ci(v, "application/vnd.apple.mpegurl") || starts_ci(v, "application/x-mpegurl") ||
                 starts_ci(v, "audio/aac") || starts_ci(v, "audio/aacp") || starts_ci(v, "audio/x-aac") ||
                 starts_ci(v, "audio/ogg") || starts_ci(v, "application/ogg") || starts_ci(v, "audio/flac") ||
                 starts_ci(v, "audio/mp4") || starts_ci(v, "video/"))
        {
            bad_format = true;
            pva_log("stream: %.40s not supported", v);
        }
    }
}

static void headers_done(void);
static uint8_t decided;

static void http_feed(const uint8_t *p, size_t n)
{
    while (n)
    {
        if (decided) return;
        if (s == S_HEADERS)
        {
            char c = (char)*p++;
            n--;
            if (c == '\n')
            {
                if (line_len == 0 || (line_len == 1 && line[0] == '\r')) { headers_done(); continue; }
                header_line();
                line_len = 0;
            }
            else if (line_len < LINE_MAX) line[line_len++] = c;
            continue;
        }
        if (s != S_BODY) return;            // closing / failed: drop
        if (body_playlist)
        {
            size_t k = n;
            if (k > PLS_MAX - pls_len) k = PLS_MAX - pls_len;
            memcpy(pls + pls_len, p, k);
            pls_len += k;
            pls[pls_len] = 0;
            p += k;
            n -= k;
            if (pls_len >= PLS_MAX) return;
            continue;
        }
        if (!metaint)
        {
            ring_audio(p, n);
            return;
        }
        if (meta_left_audio)
        {
            size_t k = n;
            if (k > meta_left_audio) k = meta_left_audio;
            ring_audio(p, k);
            meta_left_audio -= (uint32_t)k;
            p += k;
            n -= k;
            if (!meta_left_audio) { meta_len = 0xFFFFFFFFu; meta_got = 0; }
            continue;
        }
        if (meta_len == 0xFFFFFFFFu)
        {
            meta_len = (uint32_t)(*p++) * 16u;
            n--;
            meta_got = 0;
            if (!meta_len) meta_left_audio = metaint;
            continue;
        }
        size_t k = n;
        if (k > meta_len - meta_got) k = meta_len - meta_got;
        for (size_t i = 0; i < k; i++)
            if (meta_got + i < META_KEEP) meta[meta_got + i] = (char)p[i];
        meta_got += (uint32_t)k;
        p += k;
        n -= k;
        if (meta_got >= meta_len)
        {
            meta[meta_got < META_KEEP ? meta_got : META_KEEP] = 0;
            parse_stream_title();
            meta_left_audio = metaint;
        }
    }
}

// What the HTTP parser decided while a TCP_RCV answer was coming in (AFTER_xxx,
// 0 = keep going). It is carried out once that answer is complete, so no new
// request is started in the middle of a response.

static void close_then(uint8_t what)
{
    after = what;
    if (conn) esp_request(FN_TCPCLOSE, &conn, 1, NULL, 0, T_CLOSE_MS);
    else
    {
        // nothing to close: continue on the next poll
        esp_clear();
        want_fn = FN_TCPCLOSE;
        rsp_done = true;
    }
    s = S_CLOSE;
}

static void decide(uint8_t what) { if (!decided) decided = what; }

static void headers_done(void)
{
    pva_log("stream: HTTP %u%s, metaint %lu", (unsigned)st.http, body_playlist ? " (play list)" : "", (unsigned long)metaint);
    if (st.http >= 300 && st.http < 400 && location[0])
    {
        if (++redirects > MAX_REDIRECTS) { set_failed(PVA_NERR_URL); decide(AFTER_FAIL); return; }
        if (location[0] == '/')
        {
            char tmp[URL_MAX + 1];
            snprintf(tmp, sizeof tmp, "%s://%s:%u%s", tls ? "https" : "http", host, (unsigned)port, location);
            snprintf(cur, URL_MAX + 1, "%s", tmp);
        }
        else snprintf(cur, URL_MAX + 1, "%s", location);
        pva_log("stream: redirect to %.40s", cur);
        decide(AFTER_REDIRECT);
        return;
    }
    if (st.http != 200)
    {
        set_failed(PVA_NERR_HTTP);
        decide(ever_streamed ? AFTER_RETRY : AFTER_FAIL);
        return;
    }
    if (bad_format) { set_failed(PVA_NERR_FORMAT); decide(AFTER_FAIL); return; }
    s = S_BODY;
    meta_left_audio = metaint;
    meta_len = 0;
    pls_len = 0;
    last_data_ms = pva_hal_millis();
}

// First stream URL in a .pls ("File1=http://...") or .m3u body
static bool playlist_url(char *out)
{
    char *p = pls;
    while (*p)
    {
        char *e = p;
        while (*e && *e != '\n' && *e != '\r') e++;
        char c = *e;
        *e = 0;
        char *u = strstr(p, "http://");
        if (!u) u = strstr(p, "https://");
        bool ok = u && (starts_ci(p, "file") || p == u);
        if (ok) snprintf(out, URL_MAX + 1, "%s", u);
        *e = c;
        if (ok) return true;
        p = *e ? e + 1 : e;
    }
    return false;
}

static void finish_playlist(void)
{
    char u[URL_MAX + 1];
    if (playlist_url(u) && ++redirects <= MAX_REDIRECTS)
    {
        snprintf(cur, URL_MAX + 1, "%s", u);
        pva_log("stream: play list entry %.40s", cur);
        close_then(AFTER_REDIRECT);
    }
    else { set_failed(PVA_NERR_URL); close_then(AFTER_FAIL); }
}

// ---------------------------------------------------------------- sessions
static void start_open(void)
{
    uint8_t a[11];
    memcpy(a, ip, 4);
    a[4] = (uint8_t)port; a[5] = (uint8_t)(port >> 8);
    a[6] = 0xFF; a[7] = 0xFF;                   // local port: any
    a[8] = 0; a[9] = 0;                         // timeout: ESP default
    a[10] = tls ? 0x04 : 0x00;                  // TLS, certificate not checked
    esp_request(FN_TCPOPEN, a, 11, tls ? (const uint8_t *)host : NULL, tls ? strlen(host) + 1 : 0, T_OPEN_MS);
    t_start = pva_hal_millis();
    s = S_OPEN;
}

static void begin_connect(void)
{
    if (!parse_url(cur)) { set_failed(PVA_NERR_URL); close_then(AFTER_FAIL); return; }
    st.tls = tls;
    status_seen = false;
    line_len = 0;
    metaint = 0;
    location[0] = 0;
    body_playlist = false;
    bad_format = false;
    decided = 0;
    conn = 0;
    t_start = pva_hal_millis();
    if (have_ip) { start_open(); return; }
    uint8_t f = 0;
    esp_request(FN_DNS, &f, 1, (const uint8_t *)host, strlen(host), T_DNS_MS);
    s = S_DNS;
}

static void build_request(void)
{
    int n = snprintf(req, TXQ_MAX,
        "GET %s HTTP/1.0\r\n"
        "Host: %s\r\n"
        "User-Agent: PicoPlay/0.2 (MSX PicoVerse 2350)\r\n"
        "Accept: */*\r\n"
        "Icy-MetaData: 1\r\n"
        "Connection: close\r\n\r\n", path, host);
    if (n < 0 || n >= (int)TXQ_MAX - 8) n = 0;
    req_len = (size_t)n;
    req_pos = 0;
}

static void send_next_chunk(void)
{
    size_t k = req_len - req_pos;
    if (k > SEND_CHUNK) k = SEND_CHUNK;
    uint8_t a[2] = { conn, 1 };
    esp_request(FN_TCPSEND, a, 2, (const uint8_t *)req + req_pos, k, T_SEND_MS);
    req_pos += k;
}

// Ask for data; false when the buffer is full (nothing asked)
static bool request_rcv(void)
{
    bool audio = (s == S_BODY && !body_playlist);
    size_t free_b = ring_free();
    if (audio && free_b < RCV_MIN + 64u) return false;
    uint16_t n = st.rcv_max;
    if (audio && n > free_b - 64u) n = (uint16_t)(free_b - 64u);
    rcv_size = n;
    uint8_t a[3] = { conn, (uint8_t)n, (uint8_t)(n >> 8) };
    esp_request(FN_TCPRCV, a, 3, NULL, 0, T_RCV_MS);
    return true;
}

// Failure: retry if the stream had played before, else give up
static void fail_or_retry(uint8_t err)
{
    set_failed(err);
    close_then(ever_streamed ? AFTER_RETRY : AFTER_FAIL);
}

static void lost_connection(uint8_t err)
{
    st.err = err;
    pva_log("stream: connection lost (%u), reconnecting", (unsigned)err);
    close_then(AFTER_RETRY);
}

static void rate_tick(void)
{
    uint32_t now = pva_hal_millis();
    while (now - rate_t >= 250u)
    {
        rate_t += 250u;
        uint32_t sum = 0;
        for (int i = 0; i < 8; i++) sum += rate_slot[i];
        st.rate_bps = sum / 2u;
        rate_i = (uint8_t)((rate_i + 1u) & 7u);
        rate_slot[rate_i] = 0;
    }
}

static void poll_receive(uint32_t now)
{
    if (want_fn == FN_TCPRCV)
    {
        if (!esp_finished()) return;
        bool timeout = rsp_timeout;
        uint8_t r = rc;
        uint32_t got = rcv_data;
        esp_clear();
        if (decided) { close_then(decided); return; }
        if (timeout) { st.esp_errors++; if (ever_streamed) lost_connection(PVA_NERR_TIMEOUT); else fail_or_retry(PVA_NERR_TIMEOUT); return; }
        if (r != 0)
        {
            st.esp_errors++;
            if (s == S_HEADERS && got == 0 && !status_seen && st.rcv_max > RCV_MIN && rcv_size > RCV_MIN)
            {
                // the ESP may not take big reads: halve the size and ask again
                st.rcv_max = (uint16_t)(st.rcv_max / 2u);
                pva_log("stream: TCP_RCV rc %u, size now %u", (unsigned)r, (unsigned)st.rcv_max);
                wait_until = now + IDLE_POLL_MS;
                return;
            }
            if (s == S_BODY && body_playlist) { finish_playlist(); return; }
            if (s == S_HEADERS && !ever_streamed) { fail_or_retry(PVA_NERR_CLOSED); return; }
            lost_connection(PVA_NERR_CLOSED);
            return;
        }
        if (got == 0) wait_until = now + IDLE_POLL_MS;
        else
        {
            wait_until = now;
            if (s == S_BODY && !body_playlist)
            {
                uint32_t gap = now - last_data_ms;
                if (gap > 500u) st.gaps++;
            }
        }
    }
    // watchdogs
    if (s == S_HEADERS && now - t_start > T_HEADERS_MS)
    {
        if (ever_streamed) lost_connection(PVA_NERR_TIMEOUT); else fail_or_retry(PVA_NERR_TIMEOUT);
        return;
    }
    if (s == S_BODY && body_playlist && (pls_len >= PLS_MAX || now - last_data_ms > 3000u)) { finish_playlist(); return; }
    if (s == S_BODY && !body_playlist)
    {
        uint32_t gap = now - last_data_ms;
        if (gap > st.longest_gap_ms) st.longest_gap_ms = gap;
        if (gap > T_NODATA_MS) { lost_connection(PVA_NERR_TIMEOUT); return; }
    }
    if ((int32_t)(now - wait_until) >= 0 && !request_rcv())
        last_data_ms = now;         // buffer full (paused): that is not a stall
}

void pva_net_poll(void)
{
    if (s == S_IDLE || s == S_FAILED) return;
    rate_tick();
    esp_io();
    uint32_t now = pva_hal_millis();

    switch (s)
    {
        case S_NETSTATE:
            if (!esp_finished()) return;
            if (rsp_timeout || rc != 0 || pgot < 1 || pfirst[0] != 2)
            {
                bool timeout = rsp_timeout;
                pva_log("stream: WiFi not ready (%s, state %u)", timeout ? "no answer" : "answer", (unsigned)(pgot ? pfirst[0] : 255));
                esp_clear();
                if (timeout || now - t_start > 8000u) { set_failed(PVA_NERR_NOWIFI); release(S_FAILED); return; }
                wait_until = now + 1000u;       // still connecting to the access point
                after = AFTER_NONE;
                s = S_WAIT;
                return;
            }
            esp_clear();
            begin_connect();
            return;

        case S_DNS:
            if (!esp_finished()) return;
            if (rsp_timeout || rc != 0 || pgot < 4)
            {
                esp_clear();
                pva_log("stream: DNS failed for %.40s", host);
                fail_or_retry(PVA_NERR_DNS);
                return;
            }
            memcpy(ip, pfirst, 4);
            have_ip = true;
            st.dns_ms = now - t_start;
            pva_log("stream: %.32s is %u.%u.%u.%u (%lu ms)", host, ip[0], ip[1], ip[2], ip[3], (unsigned long)st.dns_ms);
            esp_clear();
            start_open();
            return;

        case S_OPEN:
            if (!esp_finished()) return;
            if (rsp_timeout || rc != 0 || pgot < 1)
            {
                pva_log("stream: %s open failed (%s %u)", tls ? "TLS" : "TCP", rsp_timeout ? "timeout" : "rc", (unsigned)rc);
                esp_clear();
                fail_or_retry(PVA_NERR_CONNECT);
                return;
            }
            conn = pfirst[0];
            st.open_ms = now - t_start;
            pva_log("stream: %s open, conn %u (%lu ms)", tls ? "TLS" : "TCP", (unsigned)conn, (unsigned long)st.open_ms);
            esp_clear();
            build_request();
            if (!req_len) { set_failed(PVA_NERR_URL); close_then(AFTER_FAIL); return; }
            send_next_chunk();
            s = S_SEND;
            return;

        case S_SEND:
            if (!esp_finished()) return;
            if (rsp_timeout || rc != 0)
            {
                esp_clear();
                if (ever_streamed) lost_connection(PVA_NERR_TIMEOUT); else fail_or_retry(PVA_NERR_TIMEOUT);
                return;
            }
            esp_clear();
            if (req_pos < req_len) { send_next_chunk(); return; }
            s = S_HEADERS;
            t_start = now;
            wait_until = now;
            request_rcv();
            return;

        case S_HEADERS:
        case S_BODY:
            poll_receive(now);
            return;

        case S_CLOSE:
            if (!esp_finished()) return;
            esp_clear();
            conn = 0;
            switch (after)
            {
                case AFTER_REDIRECT:
                    begin_connect();
                    return;
                case AFTER_RETRY:
                    st.reconnects++;
                    wait_until = now + retry_delay_ms;
                    retry_delay_ms = retry_delay_ms < 8000u ? retry_delay_ms * 2u : 8000u;
                    snprintf(cur, URL_MAX + 1, "%s", url);
                    redirects = 0;
                    s = S_WAIT;
                    return;
                case AFTER_FAIL:
                    release(S_FAILED);
                    return;
                default:
                    release(S_IDLE);
                    return;
            }

        case S_ABANDON:
            if (!esp_finished()) return;
            if (abandon_close)
            {
                abandon_close = false;
                esp_clear();
                close_then(AFTER_IDLE);
                return;
            }
            if (want_fn == FN_TCPOPEN && rsp_done && rc == 0 && pgot)
            {
                conn = pfirst[0];       // it opened after all: close it
                close_then(AFTER_IDLE);
                return;
            }
            release(S_IDLE);
            return;

        case S_WAIT:
            if ((int32_t)(now - wait_until) < 0) return;
            if (after == AFTER_RETRY) { begin_connect(); return; }
            esp_request(FN_NETSTATE, NULL, 0, NULL, 0, T_NETSTATE_MS);
            s = S_NETSTATE;
            return;

        default:
            return;
    }
}

// ---------------------------------------------------------------- public
bool pva_net_init(void)
{
    url = pva_alloc(URL_MAX + 1);
    cur = pva_alloc(URL_MAX + 1);
    path = pva_alloc(URL_MAX + 1);
    location = pva_alloc(URL_MAX + 1);
    req = pva_alloc(TXQ_MAX);
    txq = pva_alloc(TXQ_MAX);
    line = pva_alloc(LINE_MAX + 1);
    meta = pva_alloc(META_KEEP + 1);
    title = pva_alloc(PVA_ST_TITLE_LEN + PVA_ST_ARTIST_LEN);
    pending = pva_alloc(PVA_ST_TITLE_LEN + PVA_ST_ARTIST_LEN);
    pls = pva_alloc(PLS_MAX + 1);
    if (!(url && cur && path && location && req && txq && line && meta && title && pending && pls)) return false;
    size_t big = 0;
    ring = pva_hal_stream_ram(&big);
    if (ring && big >= RING_MIN) ring_size = big > RING_MAX ? RING_MAX : big;
    else
    {
        ring = NULL;
        ring_size = 0;
    }
    return true;
}

static void begin_session(void)
{
    t_start = pva_hal_millis();
    pva_log("stream: %.48s", url);
    esp_request(FN_NETSTATE, NULL, 0, NULL, 0, T_NETSTATE_MS);
    s = S_NETSTATE;
}

int pva_net_start(const char *u)
{
    if (!url || strlen(u) > URL_MAX) return PVA_E_ARGS;
    {
        // check the URL without touching the session that may still be closing
        char save_host[HOST_MAX + 1];
        memcpy(save_host, host, sizeof host);
        bool ok = parse_url(u);
        memcpy(host, save_host, sizeof host);
        if (!ok) return PVA_E_ARGS;
    }
    if (!ring) { st.err = PVA_NERR_NOWIFI; pva_log("stream: no memory for the stream buffer"); return PVA_E_NOWIFI; }
    bool busy = hal_open && s != S_IDLE && s != S_FAILED;
    if (!busy)
    {
        if (!pva_hal_net_open()) { st.err = PVA_NERR_NOWIFI; pva_log("stream: no ESP link (needs a -w image)"); return PVA_E_NOWIFI; }
        hal_open = true;
    }
    snprintf(url, URL_MAX + 1, "%s", u);
    snprintf(cur, URL_MAX + 1, "%s", u);
    memset(&st, 0, sizeof st);
    st.rcv_max = RCV_START;
    station[0] = 0;
    title[0] = 0;
    pending_set = false;
    read_total = put_total = 0;
    redirects = 0;
    retry_delay_ms = 1000u;
    ever_streamed = false;
    memset(rate_slot, 0, sizeof rate_slot);
    rate_t = pva_hal_millis();
    rate_i = 0;
    if (busy)
    {
        // the old connection is closed first (pva_net_poll), then this one starts
        restart_pending = true;
        stop_session();
        return PVA_OK;
    }
    ring_head = ring_tail = ring_count = 0;
    conn = 0;
    begin_session();
    return PVA_OK;
}

// Non-blocking: the connection is closed in the background by pva_net_poll()
// (a few milliseconds), then the ESP link goes back to the MSX side.
static void stop_session(void)
{
    ring_head = ring_tail = ring_count = 0;
    if (!hal_open) { s = S_IDLE; return; }
    switch (s)
    {
        case S_CLOSE:
            after = AFTER_IDLE;         // a close is already on its way
            return;
        case S_DNS:
        case S_OPEN:
        case S_NETSTATE:
            if (want_fn && !esp_finished())
            {
                // the answer may take a while (TLS): wait at most 2 s for it
                if (want_deadline - pva_hal_millis() > 2000u) want_deadline = pva_hal_millis() + 2000u;
                s = S_ABANDON;
                return;
            }
            break;
        default:
            break;
    }
    if (want_fn && !esp_finished() && want_fn != FN_TCPCLOSE)
    {
        // a TCP_SEND / TCP_RCV answer is on its way: it is short, take it first
        uint8_t keep = conn;
        s = S_ABANDON;
        want_deadline = pva_hal_millis() + 500u;
        conn = keep;
        abandon_close = true;
        return;
    }
    esp_clear();
    close_then(AFTER_IDLE);
}

void pva_net_stop(void)
{
    restart_pending = false;
    stop_session();
}

bool pva_net_active(void) { return s != S_IDLE && s != S_FAILED; }
bool pva_net_ended(void) { return s == S_FAILED && ring_count == 0; }

const pva_net_stats_t *pva_net_stats(void)
{
    switch (s)
    {
        case S_IDLE: st.state = PVA_NET_IDLE; break;
        case S_NETSTATE: case S_DNS: st.state = PVA_NET_LOOKUP; break;
        case S_OPEN: st.state = PVA_NET_CONNECT; break;
        case S_SEND: case S_HEADERS: st.state = PVA_NET_REQUEST; break;
        case S_BODY: st.state = body_playlist ? PVA_NET_REQUEST : PVA_NET_STREAM; break;
        case S_CLOSE: case S_WAIT: st.state = (after == AFTER_FAIL) ? PVA_NET_FAILED : PVA_NET_RETRY; break;
        default: st.state = PVA_NET_FAILED; break;
    }
    return &st;
}

const char *pva_net_station(void) { return station; }
const char *pva_net_stream_title(void) { return title ? title : ""; }
const char *pva_net_host(void) { return host; }
