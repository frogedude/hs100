#include <stddef.h>
#include <inttypes.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>

#ifdef _WIN32
#define _WINSOCK_DEPRECATED_NO_WARNINGS   // Disable deprecation warnings for inet_addr
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#define SOCKET_TYPE SOCKET
#define SOCKET_INVALID INVALID_SOCKET
#define SOCKET_ERR(x) ((x) == INVALID_SOCKET)
#define CLOSE_SOCKET(s) closesocket(s)
#define GET_LAST_ERROR WSAGetLastError()
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <netdb.h>
#define SOCKET_TYPE int
#define SOCKET_INVALID -1
#define SOCKET_ERR(x) ((x) < 0)
#define CLOSE_SOCKET(s) close(s)
#define GET_LAST_ERROR errno
#endif

#include "comms.h"

#define RECV_BUF_SIZE	4096

// --------------------------------------------------------------------
// Encryption/Decryption (unchanged)
// --------------------------------------------------------------------
bool hs100_encrypt(uint8_t* d, uint8_t* s, size_t len)
{
    if (!d || !s || len == 0) return false;
    uint8_t key = 0xab;
    for (size_t i = 0; i < len; i++) {
        uint8_t temp = key ^ s[i];
        key = temp;
        d[i] = temp;
    }
    return true;
}

bool hs100_decrypt(uint8_t* d, uint8_t* s, size_t len)
{
    if (!d || !s || len == 0) return false;
    uint8_t key = 0xab;
    for (size_t i = 0; i < len; i++) {
        uint8_t temp = key ^ s[i];
        key = s[i];
        d[i] = temp;
    }
    return true;
}

// --------------------------------------------------------------------
// Encode/Decode (with casts for C++ compatibility)
// --------------------------------------------------------------------
uint8_t* hs100_encode(size_t* outlen, char* srcmsg)
{
    if (!srcmsg) return NULL;
    size_t srcmsg_len = strlen(srcmsg);
    *outlen = srcmsg_len + 4;
    uint8_t* d = (uint8_t*)calloc(1, *outlen);
    if (!d) return NULL;
    if (!hs100_encrypt(d + 4, (uint8_t*)srcmsg, srcmsg_len)) {
        free(d);
        return NULL;
    }
    uint32_t temp = htonl((uint32_t)srcmsg_len);
    memcpy(d, &temp, 4);
    return d;
}

char* hs100_decode(uint8_t* s, size_t s_len)
{
    if (!s || s_len <= 4) return NULL;
    uint32_t in_s_len;
    memcpy(&in_s_len, s, 4);
    in_s_len = ntohl(in_s_len);
    if ((s_len - 4) < in_s_len)
        in_s_len = (uint32_t)(s_len - 4);
    char* outbuf = (char*)calloc(1, in_s_len + 1);
    if (!outbuf) return NULL;
    if (!hs100_decrypt((uint8_t*)outbuf, s + 4, in_s_len)) {
        free(outbuf);
        return NULL;
    }
    return outbuf;
}

// --------------------------------------------------------------------
// Winsock Initialization (Windows only)
// --------------------------------------------------------------------
#ifdef _WIN32
static int winsock_initted = 0;
static void cleanup_winsock(void) {
    WSACleanup();
}
static int ensure_winsock(void) {
    if (!winsock_initted) {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
            return 0;
        winsock_initted = 1;
        atexit(cleanup_winsock);
    }
    return 1;
}
#endif

// --------------------------------------------------------------------
// Send function (cross‑platform)
// --------------------------------------------------------------------
char* hs100_send(char* servaddr, char* msg)
{
    size_t s_len;
    uint8_t* s = hs100_encode(&s_len, msg);
    if (!s) return NULL;

#ifdef _WIN32
    if (!ensure_winsock()) {
        free(s);
        return NULL;
    }
#endif

    SOCKET_TYPE sock = socket(AF_INET, SOCK_STREAM, 0);
    if (SOCKET_ERR(sock)) {
        free(s);
        return NULL;
    }

    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(9999);

    // Convert IP address or hostname
    if (inet_pton(AF_INET, servaddr, &address.sin_addr) <= 0) {
        // Fallback to inet_addr (deprecated but works; warning suppressed)
        address.sin_addr.s_addr = inet_addr(servaddr);
        if (address.sin_addr.s_addr == INADDR_NONE) {
            struct hostent* he = gethostbyname(servaddr);
            if (!he || he->h_addrtype != AF_INET) {
                CLOSE_SOCKET(sock);
                free(s);
                return NULL;
            }
            memcpy(&address.sin_addr, he->h_addr_list[0], he->h_length);
        }
    }

    if (connect(sock, (struct sockaddr*)&address, sizeof(address)) < 0) {
        CLOSE_SOCKET(sock);
        free(s);
        return NULL;
    }

    // Send encoded message
    if (send(sock, (const char*)s, (int)s_len, 0) != (int)s_len) {
        CLOSE_SOCKET(sock);
        free(s);
        return NULL;
    }
    free(s);

    // Read length prefix
    uint32_t msglen = 0;
    int received = recv(sock, (char*)&msglen, 4, MSG_PEEK);
    if (received != 4) {
        CLOSE_SOCKET(sock);
        return NULL;
    }
    msglen = ntohl(msglen);
    size_t total_len = msglen + 4;
    uint8_t* recvbuf = (uint8_t*)malloc(total_len);
    if (!recvbuf) {
        CLOSE_SOCKET(sock);
        return NULL;
    }

    size_t total = 0;
    while (total < total_len) {
        int r = recv(sock, (char*)recvbuf + total, (int)(total_len - total), 0);
        if (r <= 0) {
            free(recvbuf);
            CLOSE_SOCKET(sock);
            return NULL;
        }
        total += r;
    }
    CLOSE_SOCKET(sock);

    char* recvmsg = hs100_decode(recvbuf, total_len);
    free(recvbuf);
    return recvmsg;
}