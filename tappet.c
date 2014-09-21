/*
 * Abhijit Menon-Sen <ams@toroid.org>
 * Public domain; 2014-09-20
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <linux/if.h>
#include <linux/if_tun.h>
#include <netinet/in.h>

#include "tweetnacl.h"

static struct sockaddr_in in_addr;
static struct sockaddr_in6 in6_addr;

int tap_attach(const char *name);
int read_pubkey(const char *name, unsigned char pk[crypto_box_PUBLICKEYBYTES]);
int read_keypair(const char *name, unsigned char sk[crypto_box_SECRETKEYBYTES],
                 unsigned char pk[crypto_box_PUBLICKEYBYTES]);
int get_sockaddr(const char *address, const char *sport, struct sockaddr **addr);
int udp_socket(struct sockaddr *server, int role);

int main(int argc, char *argv[])
{
    int fd, sock, role;
    unsigned char oursk[crypto_box_SECRETKEYBYTES];
    unsigned char ourpk[crypto_box_PUBLICKEYBYTES];
    unsigned char theirpk[crypto_box_PUBLICKEYBYTES];
    struct sockaddr *server;

    /*
     * We require exactly five arguments: the interface name, the name
     * of a file containing our keypair, the name of a file containing
     * the other side's public key, and the address and port of the
     * server side.
     */

    if (argc < 6) {
        fprintf(stderr, "Usage: tappet ifaceN /our/keypair /their/pubkey address port [-l]\n");
        return -1;
    }

    /*
     * Attach to the given TAP interface as an ordinary user (so that we
     * don't create it by mistake; we assume it's already configured).
     */

    if (geteuid() == 0) {
        fprintf(stderr, "Please run tappet as an ordinary user\n");
        return -1;
    }

    fd = tap_attach(argv[1]);
    if (fd < 0)
        return -1;

    /*
     * Load our own keypair and the other side's public key from the
     * given files. We assume that the keys have been competently
     * generated.
     */

    if (read_keypair(argv[2], oursk, ourpk) < 0)
        return -1;

    if (read_pubkey(argv[3], theirpk) < 0)
        return -1;

    /*
     * The next two arguments are an address (which may be either IPv4
     * or IPv6, but not a hostname) and a port number, so we convert it
     * into a sockaddr for later use.
     */

    if (get_sockaddr(argv[4], argv[5], &server) < 0)
        return -1;

    /*
     * Now we create a UDP socket. If there's a remaining -l, we'll bind
     * the server sockaddr to it, otherwise we'll connect to it.
     */

    role = argc > 6 && strcmp(argv[6], "-l") == 0;
    sock = udp_socket(server, role);
    if (sock < 0)
        return -1;

    /* … */

    return 0;
}

/*
 * Attaches to the TAP interface with the given name and returns an fd
 * (as described in linux/Documentation/networking/tuntap.txt).
 *
 * If this code is run as root, it will create the interface if it does
 * not exist. (It would be nice to report a more useful error when the
 * interface doesn't exist, but TUNGETIFF works on the attached fd; we
 * have only an interface name.)
 */

int tap_attach(const char *name)
{
    int n, fd;
    struct ifreq ifr;

    fd = open("/dev/net/tun", O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "Couldn't open /dev/net/tun: %s\n", strerror(errno));
        return -1;
    }

    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, name, IFNAMSIZ);
    ifr.ifr_flags = IFF_TAP;

    n = ioctl(fd, TUNSETIFF, (void *) &ifr);
    if (n < 0) {
        fprintf(stderr, "Couldn't attach to %s: %s\n", name, strerror(errno));
        close(fd);
        return -1;
    }

    return fd;
}


/*
 * Reads two hex characters from the source pointer and writes a single
 * byte to the destination pointer. Returns -1 if any characters are not
 * valid hex.
 */

int decode_hex(char *s, char *t)
{
    char a, b;

    a = *s | 0x20;
    if (a >= '0' && a <= '9')
        a -= '0';
    else if (a >= 'a' && a <= 'f')
        a -= 'a';
    else
        return -1;

    b = *(s+1) | 0x20;
    if (b >= '0' && b <= '9')
        b -= '0';
    else if (b >= 'a' && b <= 'f')
        b -= 'a';
    else
        return -1;

    *t = a << 4 | b;
    return 0;
}


/*
 * Tries to read 64 hex bytes followed by a newline from the given file
 * handle and write the decoded 32-byte key to the given array. Returns
 * 0 on success, -1 on failure. Does not print any error message.
 *
 * Note that this won't work if crypto_box_SECRETKEYBYTES ever differs
 * from crypto_box_PUBLICKEYBYTES, hence the hardcoded 32.
 */

int read_hexkey(FILE *f, unsigned char key[32])
{
    char line[32*2+2];
    char *p, *q;

    if (fgets(line, 66, f) == NULL || strlen(line) != 65 || line[64] != '\n')
        return -1;

    p = line;
    q = (char *) key;

    while (*p != '\n') {
        if (decode_hex(p, q) < 0)
            return -1;
        p += 2;
        q++;
    }

    return 0;
}


/*
 * Reads secret and public keys in hex format from the first two lines
 * of the given file into the sk and pk arrays. Returns -1 on any error,
 * or 0 on success.
 */

int read_keypair(const char *name,
                 unsigned char sk[crypto_box_SECRETKEYBYTES],
                 unsigned char pk[crypto_box_PUBLICKEYBYTES])
{
    FILE *f;

    f = fopen(name, "r");
    if (!f) {
        fprintf(stderr, "Couldn't open keypair file %s: %s\n", name, strerror(errno));
        return -1;
    }

    if (read_hexkey(f, sk) < 0) {
        fprintf(stderr, "Couldn't read private key (64 hex characters) from %s\n", name);
        return -1;
    }

    if (read_hexkey(f, pk) < 0) {
        fprintf(stderr, "Couldn't read public key (64 hex characters) from %s\n", name);
        return -1;
    }

    (void) fclose(f);
    return 0;
}


/*
 * Reads a public key in hex format from the first line of the given
 * file into the pk array. Returns -1 on any error, or 0 on success.
 */

int read_pubkey(const char *name, unsigned char pk[crypto_box_PUBLICKEYBYTES])
{
    FILE *f;

    f = fopen(name, "r");
    if (!f) {
        fprintf(stderr, "Couldn't open public key file %s: %s\n", name, strerror(errno));
        return -1;
    }

    if (read_hexkey(f, pk) < 0) {
        fprintf(stderr, "Couldn't read public key (64 hex characters) from %s\n", name);
        return -1;
    }

    (void) fclose(f);
    return 0;
}


/*
 * Takes two command line arguments and tries to parse them as an IP
 * (v4 or v6) address and a port number. If it succeeds, it stores the
 * pointer to the resulting (statically allocated) sockaddr and returns
 * 0, or else returns -1 on failure.
 */

int get_sockaddr(const char *address, const char *sport, struct sockaddr **addr)
{
    int n;
    long int port;

    errno = 0;
    port = strtol(sport, NULL, 10);
    if (errno != 0 || port == 0 || port >= 0xFFFF) {
        fprintf(stderr, "Couldn't parse '%s' as port number\n", sport);
        return -1;
    }

    n = inet_pton(AF_INET6, address, (void *) &in6_addr.sin6_addr);
    if (n == 1) {
        in6_addr.sin6_family = AF_INET6;
        in6_addr.sin6_port = htons((short) port);
        *addr = (struct sockaddr *) &in6_addr;
        return 0;
    }

    n = inet_pton(AF_INET, address, (void *) &in_addr.sin_addr);
    if (n == 1) {
        in_addr.sin_family = AF_INET;
        in_addr.sin_port = htons((short) port);
        *addr = (struct sockaddr *) &in_addr;
        return 0;
    }

    fprintf(stderr, "Couldn't parse '%s' as an IP address\n", address);

    return -1;
}


/*
 * Given a sockaddr and a role (1 for server, 0 for client), creates a
 * UDP socket and either binds or connects the given sockaddr to it.
 * Returns the socket on success, or -1 on failure.
 */

int udp_socket(struct sockaddr *server, int role)
{
    int sock;
    socklen_t len;

    sock = socket(server->sa_family, SOCK_DGRAM, 0);
    if (sock < 0) {
        fprintf(stderr, "Couldn't create socket: %s\n", strerror(errno));
        return -1;
    }

    len = sizeof(struct sockaddr_in);
    if (server->sa_family == AF_INET6)
        len = sizeof(struct sockaddr_in6);

    if (role == 1 && bind(sock, server, len) < 0) {
        fprintf(stderr, "Can't bind socket: %s\n", strerror(errno));
        return -1;
    }

    else if (role == 0 && connect(sock, server, len) < 0) {
        fprintf(stderr, "Can't connect socket: %s\n", strerror(errno));
        return -1;
    }

    return sock;
}
