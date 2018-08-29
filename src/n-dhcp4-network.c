/*
 * DHCP specific low-level network helpers
 */

#include <errno.h>
#include <linux/filter.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <linux/udp.h>
#include <netinet/ip.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/socket.h>
#include "n-dhcp4-private.h"

/**
 * n_dhcp4_network_client_packet_socket_new() - create a new DHCP4 client socket
 * @sockfdp:            return argumnet for the new socket
 * @ifindex:            ifindex to bind to
 * @xid:                transaction ID to subscribe to
 *
 * Create a new AF_PACKET/SOCK_DGRAM socket usable to listen to DHCP client packets
 * before an IP address has been configured.
 *
 * Only unfragmented DHCP packets from a server to a client using the specified
 * transaction id and destined for the given ifindex is returned.
 *
 * Return: 0 on success, or a negative error code on failure.
 */
int n_dhcp4_network_client_packet_socket_new(int *sockfdp, int ifindex, uint32_t xid) {
        struct sock_filter filter[] = {
                /*
                 * IP
                 *
                 * Check
                 *  - UDP
                 *  - Unfragmented
                 *  - Large enough to fit the DHCP header
                 *
                 *  Leave X the size of the IP header, for future indirect reads.
                 */
                BPF_STMT(BPF_LD + BPF_B + BPF_ABS, offsetof(struct iphdr, protocol)),                           /* A <- IP protocol */
                BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, IPPROTO_UDP, 1, 0),                                         /* IP protocol == UDP ? */
                BPF_STMT(BPF_RET + BPF_K, 0),                                                                   /* ignore */

                BPF_STMT(BPF_LD + BPF_B + BPF_ABS, offsetof(struct iphdr, frag_off)),                           /* A <- Flags */
                BPF_STMT(BPF_ALU + BPF_AND + BPF_K, ntohs(IP_MF | IP_OFFMASK)),                                 /* A <- A & (IP_MF | IP_OFFMASK) */
                BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, 0, 1, 0),                                                   /* fragmented packet ? */
                BPF_STMT(BPF_RET + BPF_K, 0),                                                                   /* ignore */

                BPF_STMT(BPF_LDX + BPF_B + BPF_MSH, 0),                                                         /* X <- IP header length */
                BPF_STMT(BPF_LD + BPF_W + BPF_LEN, 0),                                                          /* A <- packet length */
                BPF_STMT(BPF_ALU + BPF_SUB + BPF_X, 0),                                                         /* A -= X */
                BPF_JUMP(BPF_JMP + BPF_JGE + BPF_K, sizeof(struct udphdr) + sizeof(NDhcp4Message), 1, 0),       /* packet >= DHCPPacket ? */
                BPF_STMT(BPF_RET + BPF_K, 0),                                                                   /* ignore */

                /*
                 * UDP
                 *
                 * Check
                 *  - DHCP client port
                 *
                 * Leave X the size of IP and UDP headers, for future indirect reads.
                 */
                BPF_STMT(BPF_LD + BPF_H + BPF_IND, offsetof(struct udphdr, dest)),                              /* A <- UDP destination port */
                BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, N_DHCP4_NETWORK_CLIENT_PORT, 1, 0),                         /* UDP destination port == DHCP client port ? */
                BPF_STMT(BPF_RET + BPF_K, 0),                                                                   /* ignore */

                BPF_STMT(BPF_LD + BPF_W + BPF_K, sizeof(struct udphdr)),                                        /* A <- size of UDP header */
                BPF_STMT(BPF_ALU + BPF_ADD + BPF_X, 0),                                                         /* A += X */
                BPF_STMT(BPF_MISC + BPF_TAX, 0),                                                                /* X <- A */

                /*
                 * DHCP
                 *
                 * Check
                 *  - BOOTREPLY (from server to client)
                 *  - Current transaction id
                 *  - DHCP magic cookie
                 */
                BPF_STMT(BPF_LD + BPF_B + BPF_IND, offsetof(NDhcp4Header, op)),                                 /* A <- DHCP op */
                BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, N_DHCP4_OP_BOOTREPLY, 1, 0),                                /* op == BOOTREPLY ? */
                BPF_STMT(BPF_RET + BPF_K, 0),                                                                   /* ignore */

                BPF_STMT(BPF_LD + BPF_W + BPF_IND, offsetof(NDhcp4Header, xid)),                                /* A <- transaction identifier */
                BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, xid, 1, 0),                                                 /* transaction identifier == xid ? */
                BPF_STMT(BPF_RET + BPF_K, 0),                                                                   /* ignore */

                BPF_STMT(BPF_LD + BPF_W + BPF_IND, offsetof(NDhcp4Message, magic)),                             /* A <- DHCP magic cookie */
                BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, N_DHCP4_MESSAGE_MAGIC, 1, 0),                               /* cookie == DHCP magic cookie ? */
                BPF_STMT(BPF_RET + BPF_K, 0),                                                                   /* ignore */

                BPF_STMT(BPF_RET + BPF_K, 65535),                                                               /* return all */
        };
        struct sock_fprog fprog = {
                .filter = filter,
                .len = sizeof(filter) / sizeof(filter[0]),
        };
        struct sockaddr_ll addr = {
                .sll_family = AF_PACKET,
                .sll_protocol = htons(ETH_P_IP),
                .sll_ifindex = ifindex,
        };
        int r, sockfd, on = 1;

        sockfd = socket(AF_PACKET, SOCK_DGRAM, 0);
        if (sockfd < 0)
                return -errno;

        r = setsockopt(sockfd, SOL_SOCKET, SO_ATTACH_FILTER, &fprog, sizeof(fprog));
        if (r < 0)
                return -errno;

        /*
         * We need the flag that tells us if the checksum is correct.
         */
        r = setsockopt(sockfd, SOL_PACKET, PACKET_AUXDATA, &on, sizeof(on));
        if (r < 0)
                return -errno;

        r = bind(sockfd, (struct sockaddr*)&addr, sizeof(addr));
        if (r < 0)
                return -errno;

        *sockfdp = sockfd;
        return 0;
}