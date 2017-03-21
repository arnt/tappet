/*
 * Abhijit Menon-Sen <ams@toroid.org>
 * 2014-09-20
 */

#include "tappet.h"

int tunnel(int listen, const struct sockaddr *server, socklen_t srvlen,
           int tap, int udp, uint32_t nonce_prefix,
           unsigned char oursk[KEYBYTES],
           unsigned char theirpk[KEYBYTES]);
int send_keepalive(int listen, int udp, uint16_t size, const struct sockaddr *peer,
                   socklen_t peerlen, unsigned char nonce[NONCEBYTES],
                   unsigned char k[crypto_box_BEFORENMBYTES]);

int main(int argc, char *argv[])
{
    int n, tap, udp, listen;
    uint32_t nonce_prefix;
    unsigned char oursk[KEYBYTES];
    unsigned char theirpk[KEYBYTES];
    struct sockaddr *server;
    socklen_t srvlen;

    if (argc < 7) {
        fprintf(stderr, "Usage: tappet [-l] ifaceN /our/privkey address port"
                "/their/pubkey /their/nonce\n");
        return -1;
    }

    /*
     * If the first argument is an optional -l, we will listen for
     * incoming packets on the given address:port.
     */

    n = 1;
    listen = strcmp(argv[n], "-l") == 0;
    if (listen)
        n++;

    /*
     * Next is the name of a TAP interface, which must be created and
     * configured beforehand. We want to attach to it as an ordinary
     * user so that we can't create it by mistake.
     */

    if (geteuid() == 0) {
        fprintf(stderr, "Please run tappet as an ordinary user\n");
        return -1;
    }

    tap = tap_attach(argv[n]);
    if (tap < 0)
        return -1;

    /*
     * Next is the path to our private key, which we assume was created
     * using tappet-keygen.
     */

    n++;
    if (read_key(argv[n], oursk) < 0)
        return -1;

    /*
     * The next two arguments are an address (which may be either IPv4
     * or IPv6, but not a hostname) and a port number, so we convert it
     * into a sockaddr.
     */

    n++;
    if (get_sockaddr(argv[n], argv[n+1], &server, &srvlen) < 0)
        return -1;
    n++;

    /*
     * The next two arguments are the path to a peer's public key (which
     * we assume was created using tappet-keygen) and a nonce file.
     */

    n++;
    if (read_key(argv[n], theirpk) < 0)
        return -1;

    /*
     * Read a four-byte value from the given nonce file, increment it,
     * write it back out, and use the value as the nonce prefix.
     */

    n++;
    nonce_prefix = get_nonce_prefix(argv[n]);
    if (nonce_prefix == 0)
        return -1;

    /*
     * Now we create a UDP socket and, if -l was the first argument,
     * also bind the server sockaddr to it.
     */

    udp = udp_socket(listen, server, srvlen);
    if (udp < 0)
        return -1;

    /*
     * Now we start the encrypted tunnel and let it run.
     */

    return tunnel(listen, server, srvlen, tap, udp, nonce_prefix,
                  oursk, theirpk);
}

/*
 * Stays in a loop reading packets from both the TAP device and the UDP
 * socket. Encrypts and forwards packets from TAP→UDP, and decrypts and
 * forwards in the other direction.
 */

int tunnel(int listen, const struct sockaddr *server, socklen_t srvlen,
           int tap, int udp, uint32_t nonce_prefix,
           unsigned char oursk[KEYBYTES],
           unsigned char theirpk[KEYBYTES])
{
    int maxfd;
    uint16_t biggest_rcvd;
    uint16_t biggest_sent;
    uint16_t biggest_tried;
    unsigned char ptbuf[2048];
    unsigned char ctbuf[2048];
    unsigned char ournonce[NONCEBYTES];
    unsigned char theirnonce[NONCEBYTES];
    unsigned char k[crypto_box_BEFORENMBYTES];
    struct sockaddr_storage peeraddr;
    struct sockaddr *peer;
    socklen_t peerlen;

    /*
     * Generate a nonce, zero bytes that should be zero, and precompute
     * a shared secret from the two keys.
     */

    generate_nonce(nonce_prefix, ournonce);
    memset(ptbuf, 0, ZEROBYTES);
    memset(theirnonce, 0, sizeof(theirnonce));
    crypto_box_beforenm(k, theirpk, oursk);

    /*
     * Each side remembers its peer: for the client, it's the server.
     * For the server, it's whoever sends it valid encrypted packets.
     */

    peer = (struct sockaddr *) &peeraddr;
    peerlen = sizeof(peeraddr);
    memset(peer, 0, peerlen);

    if (listen == 0) {
        memcpy(peer, server, srvlen);
        peerlen = srvlen;

        /*
         * Speed things up by telling the server who we are
         * straightaway, before any traffic needs to be sent.
         */

        if (send_keepalive(listen, udp, 0, peer, peerlen, ournonce, k) < 0)
            return -1;
    }

    /*
     * We set DF on outgoing UDP packets, but we cannot rely solely upon
     * path MTU discovery working correctly. So each side keeps track of
     * the largest packet it tries to send and the largest valid packet
     * it receives and informs its peer of the latter through keepalive
     * messages. If all goes well, one side's biggest_sent (== tried)
     * should be the other side's biggest_rcvd.
     */

    biggest_tried = biggest_sent = biggest_rcvd = 0;

    /*
     * Now both sides loop waiting for readability events on their fds.
     */

    maxfd = tap > udp ? tap : udp;

    while (1) {
        fd_set r;
        int n, nfds;
        struct timeval tv;

        tv.tv_sec = 10;
        tv.tv_usec = 0;

        FD_ZERO(&r);
        FD_SET(udp, &r);

        /*
         * Don't listen for TAP packets unless we know where to send
         * them (which the client always does).
         */

        if (peer->sa_family != 0)
            FD_SET(tap, &r);

        nfds = select(maxfd+1, &r, NULL, NULL, &tv);
        if (nfds < 0) {
            fprintf(stderr, "select() failed: %s\n", strerror(errno));
            return nfds;
        }

        /*
         * We read a packet from the UDP socket and try to decrypt it.
         * If that fails, we discard the packet silently. Otherwise we
         * write the decrypted result to the TAP device.
         */

        if (FD_ISSET(udp, &r)) {
            while (1) {
                unsigned char newnonce[NONCEBYTES];
                struct sockaddr_storage newpeer;
                socklen_t newpeerlen = sizeof(newpeer);
                uint16_t rcvd;

                n = udp_read(udp, newnonce, ctbuf, sizeof(ctbuf),
                             (struct sockaddr *) &newpeer, &newpeerlen);

                if (n == 0)
                    break;

                rcvd = n;
                if (n > 0 && memcmp(theirnonce, newnonce, NONCEBYTES) >= 0)
                    n = -1;
                if (n > 0)
                    n = decrypt(k, newnonce, ctbuf, n, ptbuf);

                /*
                 * For some errors, we can drop the packet and carry on.
                 * Others we can't recover from.
                 */

                if (n == -1)
                    continue;

                if (n < -2)
                    return n;

                /*
                 * We received a valid encrypted packet, so now we can
                 * update our record of the peer's address and nonce.
                 */

                memcpy(theirnonce, newnonce, sizeof(newnonce));
                memcpy(peer, &newpeer, newpeerlen);
                peerlen = newpeerlen;

                if (biggest_rcvd < rcvd)
                    biggest_rcvd = rcvd;

                /*
                 * If the decrypted packet is not long enough to be an
                 * Ethernet frame, we treat it as a keepalive and ignore
                 * it. Otherwise we inject it into the local network.
                 */

                if (n < 64) {
                    unsigned char *p = ptbuf + ZEROBYTES;
                    if (n-ZEROBYTES == 3 && *p++ == 0xFE) {
                        uint16_t size = (*p << 8) | *(p+1);
                        if (biggest_sent < size)
                            biggest_sent = size;
                    }
                    continue;
                }

                if (tap_write(tap, ptbuf+ZEROBYTES, n-ZEROBYTES) < 0)
                    return -1;
            }
        }

        /*
         * Similarly, we read ethernet frames from the TAP device and
         * write them to the UDP socket after encryption.
         */

        if (FD_ISSET(tap, &r)) {
            while (1) {
                n = tap_read(tap, ptbuf+ZEROBYTES, sizeof(ptbuf)-ZEROBYTES);
                if (n > 0) {
                    update_nonce(ournonce);
                    n = encrypt(k, ournonce, ptbuf, n+ZEROBYTES, ctbuf);
                }

                if (n == 0)
                    break;

                if (n < 0)
                    return n;

                if (biggest_tried < n+NONCEBYTES)
                    biggest_tried = n+NONCEBYTES;

                if (udp_write(udp, ournonce, ctbuf, n, peer, peerlen) < 0)
                    return -1;
            }
        }

        /*
         * If 10 seconds have elapsed without any traffic, we send a
         * keepalive packet to our peer. (This will ensure that both
         * peers find out about IP address changes.)
         */

        if (nfds == 0 && peer->sa_family != 0) {
            update_nonce(ournonce);
            if (send_keepalive(listen, udp, biggest_rcvd, peer, peerlen,
                               ournonce, k) < 0)
                return -1;
        }
    }
}


/*
 * Sends an encrypted keepalive packet with the given size to the peer.
 * Uses the nonce without updating it. Returns 0 on success, -1 on
 * failure.
 */

int send_keepalive(int listen, int udp, uint16_t size,
                   const struct sockaddr *peer, socklen_t peerlen,
                   unsigned char nonce[NONCEBYTES],
                   unsigned char k[crypto_box_BEFORENMBYTES])
{
    int n;
    unsigned char p[ZEROBYTES+3];
    unsigned char c[ZEROBYTES+3];

    n = ZEROBYTES;
    memset(p, 0, n);
    p[n++] = 0xFE;
    p[n++] = size >> 8;
    p[n++] = size & 0xFF;

    n = encrypt(k, nonce, p, sizeof (p), c);
    if (n < 0)
        return -1;

    if (udp_write(udp, nonce, c, n, peer, peerlen) < 0)
        return -1;

    return 0;
}
