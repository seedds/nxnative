// Nextendo Network Apple platform integration (macOS and iOS):
// - OAuth 2.0 + PKCE browser sign-in (ASWebAuthenticationSession + loopback callback)
// - NSURLSession HTTP client for /api/oauth/token, /api/nex-token, /api/friends, /api/presence, /api/bcat
// - Optional local RSA-SHA256 signing for BAAS id_token (Security.framework)
// - TLS wrapper for nn::ssl over host TCP sockets (SecureTransport) with SNI recovery and peer allowlist
#include <TargetConditionals.h>
#if TARGET_OS_IPHONE
#import <UIKit/UIKit.h>
#else
#import <Cocoa/Cocoa.h>
#endif
#import <AuthenticationServices/AuthenticationServices.h>
#import <Foundation/Foundation.h>
#import <Security/Security.h>
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#import <Security/SecureTransport.h>
#pragma clang diagnostic pop

#include "dm.h"
#include "nextendo_client.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

// ---------- Presentation Anchor for ASWebAuthenticationSession ----------
@interface DmNextendoAuthAnchor : NSObject <ASWebAuthenticationPresentationContextProviding>
@end

@implementation DmNextendoAuthAnchor
- (ASPresentationAnchor)presentationAnchorForWebAuthenticationSession:(ASWebAuthenticationSession *)session {
    (void)session;
#if TARGET_OS_IPHONE
    for (UIScene *scene in UIApplication.sharedApplication.connectedScenes) {
        if (![scene isKindOfClass:[UIWindowScene class]]) continue;
        UIWindowScene *ws = (UIWindowScene *)scene;
        for (UIWindow *w in ws.windows) {
            if (w.isKeyWindow) return w;
        }
        if (ws.windows.firstObject) return ws.windows.firstObject;
    }
    return [UIWindow new];
#else
    return NSApp.keyWindow ?: NSApp.windows.firstObject ?: [NSWindow new];
#endif
}
@end

// ---------- HTTP Helper ----------
static NSMutableURLRequest *nx_make_request(const dm_nextendo_config *cfg, NSString *urlStr, NSString *method, const char *bearer) {
    NSURL *url = [NSURL URLWithString:urlStr];
    if (!url) return nil;
    NSMutableURLRequest *req = [NSMutableURLRequest requestWithURL:url cachePolicy:NSURLRequestReloadIgnoringLocalCacheData timeoutInterval:15.0];
    req.HTTPMethod = method;
    [req setValue:@"nxnative" forHTTPHeaderField:@"User-Agent"];
    if (cfg && cfg->client_id[0]) [req setValue:@(cfg->client_id) forHTTPHeaderField:@"X-Nextendo-Client-Id"];
    if (bearer && *bearer) [req setValue:[NSString stringWithFormat:@"Bearer %s", bearer] forHTTPHeaderField:@"Authorization"];
    return req;
}

static NSData *nx_sync_http(NSURLRequest *req, NSInteger *statusOut) {
    if (!req) return nil;
    __block NSData *respData = nil;
    __block NSInteger status = 0;
    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
    NSURLSessionDataTask *task = [[NSURLSession sharedSession] dataTaskWithRequest:req completionHandler:^(NSData *data, NSURLResponse *resp, NSError *err) {
        if (!err && [resp isKindOfClass:[NSHTTPURLResponse class]]) {
            status = ((NSHTTPURLResponse *)resp).statusCode;
            respData = data;
        }
        dispatch_semaphore_signal(sem);
    }];
    [task resume];
    dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 16LL * NSEC_PER_SEC));
    if (statusOut) *statusOut = status;
    return respData;
}

static NSString *nx_form_escape(NSString *s) {
    NSMutableCharacterSet *allowed = [NSMutableCharacterSet alphanumericCharacterSet];
    [allowed addCharactersInString:@"-._~"];
    return [s stringByAddingPercentEncodingWithAllowedCharacters:allowed] ?: @"";
}

// ---------- OAuth 2.0 + PKCE Browser Sign-In ----------
static int nx_loopback_listen(uint16_t *port_out) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in sa = {0};
    sa.sin_len = sizeof sa;
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = 0;
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) < 0 || listen(fd, 4) < 0) {
        close(fd);
        return -1;
    }
    socklen_t slen = sizeof sa;
    if (getsockname(fd, (struct sockaddr *)&sa, &slen) < 0) {
        close(fd);
        return -1;
    }
    *port_out = ntohs(sa.sin_port);
    return fd;
}

static int nx_loopback_wait_code(int lfd, const char *expected_state, char *code_out, size_t code_cap, int timeout_ms) {
    struct pollfd pfd = {.fd = lfd, .events = POLLIN, .revents = 0};
    int pr = poll(&pfd, 1, timeout_ms);
    if (pr <= 0 || !(pfd.revents & POLLIN)) return -1;
    int cfd = accept(lfd, NULL, NULL);
    if (cfd < 0) return -1;
    char req[4096];
    ssize_t n = recv(cfd, req, sizeof req - 1, 0);
    int ok = -1;
    if (n > 0) {
        req[n] = '\0';
        char *sp1 = strchr(req, ' ');
        char *sp2 = sp1 ? strchr(sp1 + 1, ' ') : NULL;
        if (sp1 && sp2) {
            *sp2 = '\0';
            char *q = strchr(sp1 + 1, '?');
            if (q && dm_nextendo_parse_callback_query(q + 1, expected_state, code_out, code_cap) == 0) ok = 0;
        }
    }
    const char *resp = ok == 0
        ? "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nConnection: close\r\n\r\n"
          "<!doctype html><html><body><h3>Signed in to Nextendo. You may close this window.</h3></body></html>"
        : "HTTP/1.1 400 Bad Request\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\nSign-in cancelled or failed.";
    (void)send(cfd, resp, strlen(resp), 0);
    close(cfd);
    return ok;
}

int dm_nextendo_platform_sign_in(const dm_nextendo_config *cfg, char *nex_out, size_t nex_cap,
                                 char *access_out, size_t access_cap, char *refresh_out, size_t refresh_cap) {
    if (!cfg || !cfg->api_url[0] || !cfg->client_id[0]) return -1;
    dm_nextendo_pkce pkce;
    if (dm_nextendo_pkce_generate(&pkce) != 0) return -1;

    uint16_t port = 0;
    int lfd = nx_loopback_listen(&port);
    if (lfd < 0) return -1;

    char redirect_uri[128], auth_url[1536];
    snprintf(redirect_uri, sizeof redirect_uri, "http://127.0.0.1:%u/callback", (unsigned)port);
    if (dm_nextendo_build_authorize_url(&pkce, redirect_uri, auth_url, sizeof auth_url) != 0) {
        close(lfd);
        return -1;
    }

    NSURL *nsUrl = [NSURL URLWithString:@(auth_url)];
    if (!nsUrl) {
        close(lfd);
        return -1;
    }

    __block ASWebAuthenticationSession *session = nil;
    __block DmNextendoAuthAnchor *anchor = nil;
    dispatch_async(dispatch_get_main_queue(), ^{
        anchor = [DmNextendoAuthAnchor new];
        session = [[ASWebAuthenticationSession alloc] initWithURL:nsUrl callbackURLScheme:@"nxnative" completionHandler:^(NSURL *cb, NSError *err) {
            (void)cb; (void)err;
        }];
        session.presentationContextProvider = anchor;
        if (![session start]) {
#if !TARGET_OS_IPHONE
            [[NSWorkspace sharedWorkspace] openURL:nsUrl];
#endif
        }
    });

    char code[512] = "";
    int got = nx_loopback_wait_code(lfd, pkce.state, code, sizeof code, 120000);
    close(lfd);
    dispatch_async(dispatch_get_main_queue(), ^{
        [session cancel];
        session = nil;
        anchor = nil;
    });
    if (got != 0) return -1;

    // Exchange authorization code for tokens.
    NSString *tokenUrl = [NSString stringWithFormat:@"%s/api/oauth/token", cfg->api_url];
    NSMutableURLRequest *req = nx_make_request(cfg, tokenUrl, @"POST", NULL);
    [req setValue:@"application/x-www-form-urlencoded" forHTTPHeaderField:@"Content-Type"];
    NSString *body = [NSString stringWithFormat:@"grant_type=authorization_code&client_id=%@&code=%@&code_verifier=%@&redirect_uri=%@",
                      nx_form_escape(@(cfg->client_id)), nx_form_escape(@(code)),
                      nx_form_escape(@(pkce.verifier)), nx_form_escape(@(redirect_uri))];
    req.HTTPBody = [body dataUsingEncoding:NSUTF8StringEncoding];
    NSInteger status = 0;
    NSData *data = nx_sync_http(req, &status);
    if (!data || status < 200 || status >= 300) return -1;

    NSDictionary *json = [NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
    if (![json isKindOfClass:[NSDictionary class]]) return -1;
    NSString *acc = [json[@"access_token"] isKindOfClass:[NSString class]] ? json[@"access_token"] : nil;
    NSString *ref = [json[@"refresh_token"] isKindOfClass:[NSString class]] ? json[@"refresh_token"] : nil;
    NSString *nex = [json[@"nex_token"] isKindOfClass:[NSString class]] ? json[@"nex_token"] : nil;

    if (acc && access_out && access_cap) strlcpy(access_out, acc.UTF8String, access_cap);
    if (ref && refresh_out && refresh_cap) strlcpy(refresh_out, ref.UTF8String, refresh_cap);

    // If /api/oauth/token returned an OAuth access_token without nex_token, fetch /api/nex-token.
    if (!nex && acc) {
        NSString *nexUrl = [NSString stringWithFormat:@"%s/api/nex-token", cfg->api_url];
        NSMutableURLRequest *nreq = nx_make_request(cfg, nexUrl, @"GET", acc.UTF8String);
        NSInteger nstatus = 0;
        NSData *ndata = nx_sync_http(nreq, &nstatus);
        if (ndata && nstatus >= 200 && nstatus < 300) {
            NSDictionary *njson = [NSJSONSerialization JSONObjectWithData:ndata options:0 error:nil];
            if ([njson isKindOfClass:[NSDictionary class]] && [njson[@"nex_token"] isKindOfClass:[NSString class]])
                nex = njson[@"nex_token"];
        }
    }
    if (!nex || !nex_out || !nex_cap) return -1;
    strlcpy(nex_out, nex.UTF8String, nex_cap);
    return 0;
}

// ---------- Friends, Presence & BCAT HTTP ----------
int dm_nextendo_platform_fetch_friends(const dm_nextendo_config *cfg, dm_nextendo_friend *out, int max) {
    if (!cfg || !cfg->api_url[0] || !out || max <= 0) return -1;
    const char *bearer = cfg->access_token[0] ? cfg->access_token : cfg->nex_token;
    if (!bearer[0]) return -1;
    NSString *url = [NSString stringWithFormat:@"%s/api/friends", cfg->api_url];
    NSInteger status = 0;
    NSData *data = nx_sync_http(nx_make_request(cfg, url, @"GET", bearer), &status);
    if (!data || status < 200 || status >= 300) return -1;
    NSDictionary *root = [NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
    NSArray *arr = [root isKindOfClass:[NSDictionary class]] && [root[@"friends"] isKindOfClass:[NSArray class]] ? root[@"friends"] : nil;
    if (!arr) return 0;

    int n = 0;
    for (id item in arr) {
        if (n >= max || ![item isKindOfClass:[NSDictionary class]]) break;
        NSDictionary *d = item;
        dm_nextendo_friend f;
        memset(&f, 0, sizeof f);
        if ([d[@"pid"] respondsToSelector:@selector(unsignedLongLongValue)]) f.pid = [d[@"pid"] unsignedLongLongValue];
        NSString *name = [d[@"name"] isKindOfClass:[NSString class]] ? d[@"name"]
                       : [d[@"username"] isKindOfClass:[NSString class]] ? d[@"username"] : @"Player";
        strlcpy(f.name, name.UTF8String ?: "Player", sizeof f.name);
        f.favorite = [d[@"favorite"] respondsToSelector:@selector(boolValue)] ? [d[@"favorite"] boolValue] : 0;

        NSDictionary *pres = [d[@"presence"] isKindOfClass:[NSDictionary class]] ? d[@"presence"] : d;
        if ([pres[@"status"] respondsToSelector:@selector(unsignedIntValue)]) f.online_status = [pres[@"status"] unsignedIntValue];
        else if ([pres[@"online_status"] respondsToSelector:@selector(unsignedIntValue)]) f.online_status = [pres[@"online_status"] unsignedIntValue];

        if ([pres[@"app_id"] isKindOfClass:[NSString class]]) {
            f.app_id = strtoull(((NSString *)pres[@"app_id"]).UTF8String, NULL, 16);
        }
        if ([pres[@"app_field"] isKindOfClass:[NSString class]]) {
            int flen = dm_nextendo_base64url_decode(((NSString *)pres[@"app_field"]).UTF8String, f.app_field, sizeof f.app_field);
            if (flen > 0) f.app_field_len = (size_t)flen;
        }
        if (f.pid) out[n++] = f;
    }
    return n;
}

int dm_nextendo_platform_post_presence(const dm_nextendo_config *cfg, int status_val, const char *app_id,
                                       const char *app_field_b64, const char *detail) {
    if (!cfg || !cfg->api_url[0]) return -1;
    const char *bearer = cfg->access_token[0] ? cfg->access_token : cfg->nex_token;
    if (!bearer[0]) return -1;
    NSString *url = [NSString stringWithFormat:@"%s/api/presence", cfg->api_url];
    NSMutableURLRequest *req = nx_make_request(cfg, url, @"POST", bearer);
    [req setValue:@"application/json" forHTTPHeaderField:@"Content-Type"];
    NSDictionary *payload = @{
        @"status": @(status_val),
        @"app_id": @(app_id ?: ""),
        @"app_field": @(app_field_b64 ?: ""),
        @"app_detail": @(detail ?: ""),
    };
    req.HTTPBody = [NSJSONSerialization dataWithJSONObject:payload options:0 error:nil];
    req.timeoutInterval = 8.0;
    static dispatch_queue_t s_pres_q;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ s_pres_q = dispatch_queue_create("nxnative.nextendo.presence", DISPATCH_QUEUE_SERIAL); });
    dispatch_async(s_pres_q, ^{
        NSInteger httpStatus = 0;
        (void)nx_sync_http(req, &httpStatus);
    });
    return 0;
}

int dm_nextendo_platform_sync_bcat(const dm_nextendo_config *cfg, const char *title_id, const char *dest_dir) {
    if (!cfg || !cfg->api_url[0] || !title_id || !*title_id || !dest_dir || !*dest_dir) return -1;
    const char *bearer = cfg->access_token[0] ? cfg->access_token : cfg->nex_token;
    NSString *url = [NSString stringWithFormat:@"%s/api/bcat/%s", cfg->api_url, title_id];
    NSInteger status = 0;
    NSData *data = nx_sync_http(nx_make_request(cfg, url, @"GET", bearer[0] ? bearer : NULL), &status);
    if (!data || status < 200 || status >= 300 || data.length == 0) return -1;
    NSString *dir = @(dest_dir);
    [NSFileManager.defaultManager createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
    NSString *file = [dir stringByAppendingPathComponent:[NSString stringWithFormat:@"%s.bcat", title_id]];
    return [data writeToFile:file atomically:YES] ? 0 : -1;
}

// Unwrap PKCS#8 PrivateKeyInfo (SEQUENCE { INTEGER 0, AlgorithmIdentifier, OCTET STRING <pkcs1> })
// into raw PKCS#1 RSAPrivateKey DER for SecKeyCreateWithData.
static NSData *nx_unwrap_pkcs8_rsa(NSData *der) {
    const uint8_t *b = der.bytes;
    size_t n = der.length, i = 0;
    if (n < 32 || b[i++] != 0x30) return der;
    // Skip outer SEQUENCE length
    if (b[i] & 0x80) i += 1 + (b[i] & 0x7f); else i++;
    if (i + 3 >= n || b[i] != 0x02 || b[i + 1] != 0x01 || b[i + 2] != 0x00) return der;
    i += 3;
    // Skip AlgorithmIdentifier SEQUENCE
    if (i + 2 >= n || b[i] != 0x30) return der;
    i++;
    size_t alg_len = b[i++];
    if (alg_len & 0x80) {
        size_t nb = alg_len & 0x7f;
        if (i + nb >= n) return der;
        alg_len = 0;
        for (size_t k = 0; k < nb; k++) alg_len = (alg_len << 8) | b[i++];
    }
    i += alg_len;
    // Expect OCTET STRING containing PKCS#1 RSAPrivateKey
    if (i + 2 >= n || b[i++] != 0x04) return der;
    size_t oct_len = b[i++];
    if (oct_len & 0x80) {
        size_t nb = oct_len & 0x7f;
        if (i + nb > n) return der;
        oct_len = 0;
        for (size_t k = 0; k < nb; k++) oct_len = (oct_len << 8) | b[i++];
    }
    if (i + oct_len > n) return der;
    return [NSData dataWithBytes:b + i length:oct_len];
}

// ---------- Optional RS256 BAAS Signing ----------
int dm_nextendo_platform_sign_rs256(const char *pem_path, const uint8_t *msg, size_t msg_len,
                                    uint8_t *sig_out, size_t *sig_len) {
    if (!pem_path || !*pem_path || !msg || !sig_out || !sig_len) return -1;
    NSString *pem = [NSString stringWithContentsOfFile:@(pem_path) encoding:NSUTF8StringEncoding error:nil];
    if (!pem) return -1;
    BOOL is_pkcs8 = [pem containsString:@"BEGIN PRIVATE KEY"];
    NSMutableString *b64 = [NSMutableString string];
    for (NSString *line in [pem componentsSeparatedByCharactersInSet:NSCharacterSet.newlineCharacterSet]) {
        NSString *t = [line stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
        if (!t.length || [t hasPrefix:@"-----"]) continue;
        [b64 appendString:t];
    }
    NSData *der = [[NSData alloc] initWithBase64EncodedString:b64 options:0];
    if (!der.length) return -1;
    if (is_pkcs8) der = nx_unwrap_pkcs8_rsa(der);
    NSDictionary *attrs = @{
        (__bridge id)kSecAttrKeyType: (__bridge id)kSecAttrKeyTypeRSA,
        (__bridge id)kSecAttrKeyClass: (__bridge id)kSecAttrKeyClassPrivate,
    };
    SecKeyRef key = SecKeyCreateWithData((__bridge CFDataRef)der, (__bridge CFDictionaryRef)attrs, NULL);
    if (!key) return -1;
    NSData *msgData = [NSData dataWithBytes:msg length:msg_len];
    CFDataRef sig = SecKeyCreateSignature(key, kSecKeyAlgorithmRSASignatureMessagePKCS1v15SHA256, (__bridge CFDataRef)msgData, NULL);
    CFRelease(key);
    if (!sig) return -1;
    size_t n = (size_t)CFDataGetLength(sig);
    int rc = -1;
    if (n <= *sig_len) {
        memcpy(sig_out, CFDataGetBytePtr(sig), n);
        *sig_len = n;
        rc = 0;
    }
    CFRelease(sig);
    return rc;
}

// ---------- TLS Transport for nn::ssl (SecureTransport) ----------
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

struct dm_ssl_conn {
    SSLContextRef ctx;
    int fd;
    int nonblock;
    int handshake_done;
    uint32_t last_err;
    char host[256];
    char alpn[64];
    uint8_t peek_buf[256];
    size_t peek_len;
};

static OSStatus nx_ssl_io_read(SSLConnectionRef ref, void *data, size_t *len) {
    const struct dm_ssl_conn *c = (const struct dm_ssl_conn *)ref;
    size_t want = *len, got = 0;
    while (got < want) {
        ssize_t r = recv(c->fd, (uint8_t *)data + got, want - got, 0);
        if (r > 0) {
            got += (size_t)r;
            if (c->nonblock) break;
        } else if (r == 0) {
            *len = got;
            return got ? noErr : errSSLClosedGraceful;
        } else if (errno == EINTR) {
            continue;
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            *len = got;
            return errSSLWouldBlock;
        } else {
            *len = got;
            return errSSLClosedAbort;
        }
    }
    *len = got;
    return noErr;
}

static OSStatus nx_ssl_io_write(SSLConnectionRef ref, const void *data, size_t *len) {
    const struct dm_ssl_conn *c = (const struct dm_ssl_conn *)ref;
    size_t want = *len, sent = 0;
    while (sent < want) {
        ssize_t w = send(c->fd, (const uint8_t *)data + sent, want - sent, 0);
        if (w > 0) {
            sent += (size_t)w;
        } else if (w < 0 && errno == EINTR) {
            continue;
        } else if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            *len = sent;
            return errSSLWouldBlock;
        } else {
            *len = sent;
            return errSSLClosedAbort;
        }
    }
    *len = sent;
    return noErr;
}

dm_ssl_conn *dm_nextendo_ssl_create(void) {
    dm_ssl_conn *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    c->fd = -1;
    c->ctx = SSLCreateContext(kCFAllocatorDefault, kSSLClientSide, kSSLStreamType);
    if (!c->ctx) {
        free(c);
        return NULL;
    }
    SSLSetIOFuncs(c->ctx, nx_ssl_io_read, nx_ssl_io_write);
    SSLSetConnection(c->ctx, c);
    SSLSetProtocolVersionMin(c->ctx, kTLSProtocol12);
    // Custom peer validation: Nextendo servers use self-signed certs for Nintendo hostnames,
    // so we verify the peer IP against the resolved Nextendo server/NAT allowlist on handshake.
    SSLSetSessionOption(c->ctx, kSSLSessionOptionBreakOnServerAuth, true);
    return c;
}

void dm_nextendo_ssl_destroy(dm_ssl_conn *c) {
    if (!c) return;
    if (c->ctx) {
        if (c->handshake_done && c->fd >= 0) SSLClose(c->ctx);
        CFRelease(c->ctx);
    }
    if (c->fd >= 0) close(c->fd);
    free(c);
}

int dm_nextendo_ssl_set_fd(dm_ssl_conn *c, int host_fd) {
    if (!c || host_fd < 0) return -1;
    int dupfd = dup(host_fd);
    if (dupfd < 0) return -1;
    if (c->fd >= 0) close(c->fd);
    c->fd = dupfd;
    return 0;
}

int dm_nextendo_ssl_set_hostname(dm_ssl_conn *c, const char *host) {
    if (!c || !host) return -1;
    strlcpy(c->host, host, sizeof c->host);
    return 0;
}

int dm_nextendo_ssl_set_alpn(dm_ssl_conn *c, const char *proto) {
    if (!c || !proto) return -1;
    strlcpy(c->alpn, proto, sizeof c->alpn);
    return 0;
}

int dm_nextendo_ssl_set_io_mode(dm_ssl_conn *c, int nonblock) {
    if (!c) return -1;
    c->nonblock = !!nonblock;
    if (c->fd >= 0) {
        int fl = fcntl(c->fd, F_GETFL, 0);
        if (fl >= 0) fcntl(c->fd, F_SETFL, nonblock ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK));
    }
    return 0;
}

int dm_nextendo_ssl_handshake(dm_ssl_conn *c) {
    if (!c || !c->ctx || c->fd < 0) return -1;
    struct sockaddr_in peer = {0};
    socklen_t plen = sizeof peer;
    if (getpeername(c->fd, (struct sockaddr *)&peer, &plen) != 0 || peer.sin_family != AF_INET) {
        c->last_err = 0x2A2u;
        return -1;
    }
    uint32_t peer_ip = ntohl(peer.sin_addr.s_addr);
    if (!dm_nextendo_is_allowed_tls_peer(peer_ip)) {
        LOG("nextendo ssl: refused TLS handshake to non-Nextendo peer %s", inet_ntoa(peer.sin_addr));
        c->last_err = 0x2A2u;
        return -1;
    }

    // Recover SNI from DNS redirect table if host is empty or a literal IPv4 address.
    struct in_addr tmp;
    if (!c->host[0] || inet_pton(AF_INET, c->host, &tmp) == 1) {
        if (dm_nextendo_lookup_sni(peer_ip, c->host, sizeof c->host) != 0)
            strlcpy(c->host, "nintendo.net", sizeof c->host);
    }
    SSLSetPeerDomainName(c->ctx, c->host, strlen(c->host));

    for (;;) {
        OSStatus st = SSLHandshake(c->ctx);
        if (st == noErr) {
            c->handshake_done = 1;
            c->last_err = 0;
            return 0;
        }
        if (st == errSSLServerAuthCompleted) {
            // Peer IP was already verified against the Nextendo allowlist above; continue handshake.
            continue;
        }
        if (st == errSSLWouldBlock) {
            c->last_err = 0x19901u;
            return -2;
        }
        c->last_err = 0x2A2u;
        return -1;
    }
}

int dm_nextendo_ssl_read(dm_ssl_conn *c, void *buf, size_t len, int peek) {
    if (!c || !c->ctx || !buf || !len) return -1;
    if (c->peek_len > 0) {
        size_t take = len < c->peek_len ? len : c->peek_len;
        memcpy(buf, c->peek_buf, take);
        if (!peek) {
            memmove(c->peek_buf, c->peek_buf + take, c->peek_len - take);
            c->peek_len -= take;
        }
        return (int)take;
    }
    size_t got = 0;
    size_t req = peek && len > sizeof c->peek_buf ? sizeof c->peek_buf : len;
    OSStatus st = SSLRead(c->ctx, peek ? (void *)c->peek_buf : buf, req, &got);
    if (got > 0) {
        if (peek) {
            c->peek_len = got;
            memcpy(buf, c->peek_buf, got);
        }
        c->last_err = 0;
        return (int)got;
    }
    if (st == errSSLWouldBlock) { c->last_err = 0x19901u; return -2; }
    if (st == errSSLClosedGraceful) { c->last_err = 0; return 0; }
    c->last_err = 0x2A2u;
    return -1;
}

int dm_nextendo_ssl_write(dm_ssl_conn *c, const void *buf, size_t len) {
    if (!c || !c->ctx || !buf || !len) return -1;
    size_t sent = 0;
    OSStatus st = SSLWrite(c->ctx, buf, len, &sent);
    if (sent > 0) { c->last_err = 0; return (int)sent; }
    if (st == errSSLWouldBlock) { c->last_err = 0x19901u; return -2; }
    c->last_err = 0x2A2u;
    return -1;
}

int dm_nextendo_ssl_pending(dm_ssl_conn *c) {
    if (!c || !c->ctx) return 0;
    size_t buffered = 0;
    SSLGetBufferedReadSize(c->ctx, &buffered);
    return (int)(buffered + c->peek_len);
}

int dm_nextendo_ssl_poll(dm_ssl_conn *c, int want_read, int want_write, int timeout_ms) {
    if (!c || c->fd < 0) return -1;
    int rev = 0;
    if (want_read && dm_nextendo_ssl_pending(c) > 0) rev |= 1;
    struct pollfd pfd = {.fd = c->fd, .events = (short)((want_read ? POLLIN : 0) | (want_write ? POLLOUT : 0)), .revents = 0};
    int r = poll(&pfd, 1, rev ? 0 : timeout_ms);
    if (r < 0) return -1;
    if (pfd.revents & POLLIN) rev |= 1;
    if (pfd.revents & POLLOUT) rev |= 2;
    if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) rev |= 4;
    return rev;
}

uint32_t dm_nextendo_ssl_last_error(dm_ssl_conn *c) {
    return c ? c->last_err : 0x2A2u;
}

#pragma clang diagnostic pop

// ---------- System Root CA Certificates for nn::ssl::BuiltInManager (NPLN OpenSSL/gRPC) ----------

typedef struct {
    uint32_t cert_id;
    uint32_t status;
    uint64_t cert_size;
    uint8_t *cert_data;
} dm_ssl_builtin_cert_info;

static NSArray<NSData *> *nx_load_system_anchors(void) {
    static NSArray<NSData *> *cached = nil;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        NSMutableArray<NSData *> *list = [NSMutableArray array];
#if !TARGET_OS_IPHONE
        CFArrayRef anchors = NULL;
        if (SecTrustCopyAnchorCertificates(&anchors) == errSecSuccess && anchors) {
            CFIndex count = CFArrayGetCount(anchors);
            for (CFIndex i = 0; i < count; i++) {
                SecCertificateRef cert = (SecCertificateRef)CFArrayGetValueAtIndex(anchors, i);
                CFDataRef der = SecCertificateCopyData(cert);
                if (der) [list addObject:(__bridge_transfer NSData *)der];
            }
            CFRelease(anchors);
        }
#endif
        cached = [list copy];
    });
    return cached;
}

static NSUInteger nx_select_cert_count(NSArray<NSData *> *certs, unsigned max_certs, uint32_t cap_bytes) {
    NSUInteger lim = certs.count;
    if (max_certs > 0 && max_certs < lim) lim = max_certs;
    if (lim > 16) lim = 16;   // keep well inside Horizon's 48 KiB stack/heap cert buffers
    size_t total = 0;
    NSUInteger fit = 0;
    for (NSUInteger i = 0; i < lim; i++) {
        size_t next = ((total + sizeof(dm_ssl_builtin_cert_info) + 7u) & ~7u) + certs[i].length;
        if (cap_bytes && next > cap_bytes) break;
        total = next;
        fit = i + 1;
    }
    return fit;
}

uint32_t dm_nextendo_builtin_certs_bufsize(unsigned max_certs) {
    @autoreleasepool {
        NSArray<NSData *> *certs = nx_load_system_anchors();
        NSUInteger cnt = nx_select_cert_count(certs, max_certs, 40960u);
        if (cnt == 0) return 0;
        size_t total = cnt * sizeof(dm_ssl_builtin_cert_info);
        for (NSUInteger i = 0; i < cnt; i++) {
            total = (total + 7u) & ~7u;
            total += certs[i].length;
        }
        return (uint32_t)total;
    }
}

int dm_nextendo_builtin_certs_fill(void **out_infos, uint8_t *buf, uint32_t buf_size, unsigned max_certs, uint32_t *out_count) {
    @autoreleasepool {
        NSArray<NSData *> *certs = nx_load_system_anchors();
        if (out_count) *out_count = 0;
        if (!buf || certs.count == 0) return -1;
        NSUInteger cnt = nx_select_cert_count(certs, max_certs, buf_size);
        if (cnt == 0) return -1;
        dm_ssl_builtin_cert_info *infos = (dm_ssl_builtin_cert_info *)buf;
        size_t off = cnt * sizeof(dm_ssl_builtin_cert_info);
        for (NSUInteger i = 0; i < cnt; i++) {
            NSData *d = certs[i];
            off = (off + 7u) & ~7u;
            if (off + d.length > buf_size) { cnt = i; break; }
            memcpy(buf + off, d.bytes, d.length);
            infos[i].cert_id = (uint32_t)(1000u + i);
            infos[i].status = 1u;   // EnabledTrusted
            infos[i].cert_size = (uint64_t)d.length;
            infos[i].cert_data = buf + off;
            off += d.length;
        }
        if (cnt == 0) return -1;
        if (out_infos) *out_infos = buf;
        if (out_count) *out_count = (uint32_t)cnt;
        return 0;
    }
}
