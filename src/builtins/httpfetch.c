/* ============================================================================
 * vsh - Vanguard Shell
 * builtins/httpfetch.c - Fetch content from a URL via raw HTTP sockets
 * ============================================================================ */

#include "builtins.h"
#include "shell.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <ctype.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <netinet/in.h>

#define HTTP_MAX_REDIRECTS 5
#define HTTP_RECV_BUFSZ    8192
#define HTTP_TIMEOUT_SEC   10

/* ---- URL parsing -------------------------------------------------------- */

typedef struct {
    char scheme[16];
    char host[256];
    char port[8];
    char path[2048];
} ParsedURL;

/*
 * Parse a URL into its components.
 * Returns 0 on success, -1 on error.
 */
static int parse_url(const char *url, ParsedURL *out) {
    memset(out, 0, sizeof(*out));

    const char *p = url;

    /* scheme */
    const char *scheme_end = strstr(p, "://");
    if (scheme_end) {
        size_t slen = scheme_end - p;
        if (slen >= sizeof(out->scheme)) slen = sizeof(out->scheme) - 1;
        memcpy(out->scheme, p, slen);
        out->scheme[slen] = '\0';
        p = scheme_end + 3;
    } else {
        strcpy(out->scheme, "http");
    }

    /* host (and optional :port) */
    const char *slash = strchr(p, '/');
    const char *host_end = slash ? slash : p + strlen(p);

    /* Check for port */
    const char *colon = NULL;
    /* Find last colon in host part (skip IPv6 brackets) */
    if (*p == '[') {
        /* IPv6 */
        const char *bracket = strchr(p, ']');
        if (bracket && bracket < host_end) {
            colon = (bracket[1] == ':') ? bracket + 1 : NULL;
        }
    } else {
        colon = memchr(p, ':', host_end - p);
    }

    if (colon && colon < host_end) {
        size_t hlen = colon - p;
        if (hlen >= sizeof(out->host)) hlen = sizeof(out->host) - 1;
        memcpy(out->host, p, hlen);
        out->host[hlen] = '\0';

        size_t plen = host_end - colon - 1;
        if (plen >= sizeof(out->port)) plen = sizeof(out->port) - 1;
        memcpy(out->port, colon + 1, plen);
        out->port[plen] = '\0';
    } else {
        size_t hlen = host_end - p;
        if (hlen >= sizeof(out->host)) hlen = sizeof(out->host) - 1;
        memcpy(out->host, p, hlen);
        out->host[hlen] = '\0';
        strcpy(out->port, "80");
    }

    /* path */
    if (slash) {
        strncpy(out->path, slash, sizeof(out->path) - 1);
    } else {
        strcpy(out->path, "/");
    }

    if (out->host[0] == '\0') return -1;
    return 0;
}

/* ---- HTTP fetch core ---------------------------------------------------- */

/*
 * Perform a single HTTP GET and collect the full response into a
 * dynamically allocated buffer.  Caller must free *response.
 * Returns 0 on success, -1 on error.
 */
static int http_get(const ParsedURL *url, int verbose,
                    char **response, size_t *resp_len) {
    struct addrinfo hints, *res, *rp;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    int gai = getaddrinfo(url->host, url->port, &hints, &res);
    if (gai != 0) {
        fprintf(stderr, "vsh: httpfetch: DNS resolution failed for '%s': %s\n",
                url->host, gai_strerror(gai));
        return -1;
    }

    int sockfd = -1;
    for (rp = res; rp != NULL; rp = rp->ai_next) {
        sockfd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sockfd == -1) continue;

        /* Set receive timeout */
        struct timeval tv;
        tv.tv_sec  = HTTP_TIMEOUT_SEC;
        tv.tv_usec = 0;
        setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        if (connect(sockfd, rp->ai_addr, rp->ai_addrlen) == 0) break;

        close(sockfd);
        sockfd = -1;
    }
    freeaddrinfo(res);

    if (sockfd == -1) {
        fprintf(stderr, "vsh: httpfetch: connection to %s:%s failed: %s\n",
                url->host, url->port, strerror(errno));
        return -1;
    }

    /* Build HTTP request */
    char request[4096];
    int rlen = snprintf(request, sizeof(request),
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "User-Agent: vsh/1.0.0\r\n"
        "Accept: */*\r\n"
        "Connection: close\r\n"
        "\r\n",
        url->path, url->host);

    if (verbose) {
        fprintf(stderr, "\033[2m> GET %s HTTP/1.1\033[0m\n", url->path);
        fprintf(stderr, "\033[2m> Host: %s\033[0m\n", url->host);
        fprintf(stderr, "\033[2m> User-Agent: vsh/1.0.0\033[0m\n");
        fprintf(stderr, "\033[2m> Accept: */*\033[0m\n");
        fprintf(stderr, "\033[2m> Connection: close\033[0m\n");
        fprintf(stderr, "\033[2m>\033[0m\n");
    }

    /* Send request */
    ssize_t sent = 0;
    while (sent < rlen) {
        ssize_t n = send(sockfd, request + sent, rlen - sent, 0);
        if (n <= 0) {
            fprintf(stderr, "vsh: httpfetch: send failed: %s\n", strerror(errno));
            close(sockfd);
            return -1;
        }
        sent += n;
    }

    /* Read response into dynamic buffer */
    size_t capacity = HTTP_RECV_BUFSZ;
    size_t total = 0;
    char *buf = malloc(capacity);
    if (!buf) {
        fprintf(stderr, "vsh: httpfetch: out of memory\n");
        close(sockfd);
        return -1;
    }

    for (;;) {
        if (total + HTTP_RECV_BUFSZ > capacity) {
            capacity *= 2;
            char *newbuf = realloc(buf, capacity);
            if (!newbuf) {
                fprintf(stderr, "vsh: httpfetch: out of memory\n");
                free(buf);
                close(sockfd);
                return -1;
            }
            buf = newbuf;
        }
        ssize_t n = recv(sockfd, buf + total, HTTP_RECV_BUFSZ, 0);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                fprintf(stderr, "vsh: httpfetch: connection timed out\n");
                free(buf);
                close(sockfd);
                return -1;
            }
            fprintf(stderr, "vsh: httpfetch: recv error: %s\n", strerror(errno));
            free(buf);
            close(sockfd);
            return -1;
        }
        if (n == 0) break;  /* connection closed */
        total += n;
    }
    close(sockfd);

    /* Null-terminate for easy string handling */
    buf = realloc(buf, total + 1);
    if (buf) buf[total] = '\0';

    *response = buf;
    *resp_len = total;
    return 0;
}

/* Case-insensitive search for a header value.  Returns pointer into buf. */
static const char *find_header(const char *headers, size_t hdr_len,
                               const char *name) {
    size_t nlen = strlen(name);
    const char *p = headers;
    const char *end = headers + hdr_len;
    while (p < end) {
        const char *line_end = strstr(p, "\r\n");
        if (!line_end) line_end = end;
        if ((size_t)(line_end - p) > nlen + 1 && p[nlen] == ':' &&
            strncasecmp(p, name, nlen) == 0) {
            const char *val = p + nlen + 1;
            while (val < line_end && (*val == ' ' || *val == '\t')) val++;
            return val;
        }
        p = (line_end < end) ? line_end + 2 : end;
    }
    return NULL;
}

static int extract_status_code(const char *response) {
    /* "HTTP/1.x NNN ..." */
    const char *sp = strchr(response, ' ');
    if (!sp) return 0;
    return atoi(sp + 1);
}

/* ---- Main entry point --------------------------------------------------- */

/*
 * httpfetch [-H] [-v] URL
 *
 * Fetch content from a URL over HTTP using raw sockets.
 *   -H  Show response headers only
 *   -v  Verbose: show request and response headers
 */
int builtin_httpfetch(Shell *shell, int argc, char **argv) {
    (void)shell;

    int headers_only = 0;
    int verbose = 0;
    const char *url_str = NULL;

    /* Parse options */
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1] != '\0') {
            for (int j = 1; argv[i][j]; j++) {
                switch (argv[i][j]) {
                case 'H': headers_only = 1; break;
                case 'v': verbose = 1;      break;
                default:
                    fprintf(stderr, "vsh: httpfetch: unknown option '-%c'\n",
                            argv[i][j]);
                    fprintf(stderr, "Usage: httpfetch [-H] [-v] URL\n");
                    return 1;
                }
            }
        } else {
            url_str = argv[i];
        }
    }

    if (!url_str) {
        fprintf(stderr, "Usage: httpfetch [-H] [-v] URL\n");
        return 1;
    }

    ParsedURL url;
    if (parse_url(url_str, &url) != 0) {
        fprintf(stderr, "vsh: httpfetch: invalid URL '%s'\n", url_str);
        return 1;
    }

    /* Warn about HTTPS */
    if (strcasecmp(url.scheme, "https") == 0) {
        fprintf(stderr, "vsh: httpfetch: warning: HTTPS is not supported, "
                "using plain HTTP\n");
        strcpy(url.port, "80");
    } else if (strcasecmp(url.scheme, "http") != 0) {
        fprintf(stderr, "vsh: httpfetch: unsupported scheme '%s'\n",
                url.scheme);
        return 1;
    }

    int redirects = 0;

    while (redirects <= HTTP_MAX_REDIRECTS) {
        char *response = NULL;
        size_t resp_len = 0;

        if (http_get(&url, verbose, &response, &resp_len) != 0) {
            return 1;
        }

        /* Find header/body separator */
        char *sep = strstr(response, "\r\n\r\n");
        if (!sep) {
            /* No proper separator - print everything */
            fwrite(response, 1, resp_len, stdout);
            free(response);
            return 0;
        }

        size_t hdr_len = sep - response;
        char *body = sep + 4;
        size_t body_len = resp_len - (body - response);

        /* Print status line */
        char *first_line_end = strstr(response, "\r\n");
        if (first_line_end) {
            if (verbose || headers_only) {
                int code = extract_status_code(response);
                const char *color = (code >= 200 && code < 300) ? "\033[32m" :
                                    (code >= 300 && code < 400) ? "\033[33m" :
                                    "\033[31m";
                fprintf(stderr, "%s%.*s\033[0m\n", color,
                        (int)(first_line_end - response), response);
            }
        }

        /* Verbose/headers: print response headers */
        if (verbose || headers_only) {
            const char *hp = first_line_end ? first_line_end + 2 : response;
            while (hp < sep) {
                const char *le = strstr(hp, "\r\n");
                if (!le) break;
                fprintf(stderr, "\033[2m< %.*s\033[0m\n", (int)(le - hp), hp);
                hp = le + 2;
            }
            fprintf(stderr, "\n");
        }

        /* Check for redirect */
        int status = extract_status_code(response);
        if ((status == 301 || status == 302 || status == 303 ||
             status == 307 || status == 308) && redirects < HTTP_MAX_REDIRECTS) {
            const char *loc = find_header(response, hdr_len, "Location");
            if (loc) {
                /* Extract location value until \r\n */
                const char *loc_end = strstr(loc, "\r\n");
                size_t loc_len = loc_end ? (size_t)(loc_end - loc) : strlen(loc);
                char loc_str[2048];
                if (loc_len >= sizeof(loc_str)) loc_len = sizeof(loc_str) - 1;
                memcpy(loc_str, loc, loc_len);
                loc_str[loc_len] = '\0';

                if (verbose) {
                    fprintf(stderr, "\033[33m-> Redirecting to: %s\033[0m\n\n",
                            loc_str);
                }

                free(response);

                /* Parse new URL - handle relative redirects */
                if (strncmp(loc_str, "http://", 7) == 0 ||
                    strncmp(loc_str, "https://", 8) == 0) {
                    if (parse_url(loc_str, &url) != 0) {
                        fprintf(stderr,
                                "vsh: httpfetch: invalid redirect URL '%s'\n",
                                loc_str);
                        return 1;
                    }
                    if (strcasecmp(url.scheme, "https") == 0) {
                        fprintf(stderr,
                                "vsh: httpfetch: warning: redirect to HTTPS "
                                "not supported\n");
                        strcpy(url.port, "80");
                    }
                } else {
                    /* Relative redirect - keep host, update path */
                    strncpy(url.path, loc_str, sizeof(url.path) - 1);
                    url.path[sizeof(url.path) - 1] = '\0';
                }

                redirects++;
                continue;
            }
        }

        /* If max redirects exceeded */
        if (redirects >= HTTP_MAX_REDIRECTS &&
            (status == 301 || status == 302 || status == 303 ||
             status == 307 || status == 308)) {
            fprintf(stderr, "vsh: httpfetch: too many redirects\n");
            free(response);
            return 1;
        }

        /* Print body (unless headers-only) */
        if (!headers_only && body_len > 0) {
            fwrite(body, 1, body_len, stdout);
            /* Ensure trailing newline */
            if (body[body_len - 1] != '\n') putchar('\n');
        }

        free(response);
        return (status >= 200 && status < 400) ? 0 : 1;
    }

    fprintf(stderr, "vsh: httpfetch: too many redirects\n");
    return 1;
}
