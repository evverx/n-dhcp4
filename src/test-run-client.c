/*
 * DHCP Client Runner
 *
 * This test implements a DHCP client. It takes parameters via the command-line
 * and runs a DHCP client. It is mainly meant for testing, as such it allows
 * tweaking that an exported DHCP client should not provide.
 */

#include <assert.h>
#include <errno.h>
#include <getopt.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "n-dhcp4.h"
#include "n-dhcp4-private.h"
#include "test.h"

typedef struct Manager Manager;

enum {
        _MAIN_SUCCESS,
        MAIN_EXIT,
        MAIN_FAILED,
};

struct Manager {
        NDhcp4Client *client;
        NDhcp4ClientProbe *probe;
};

#define MANAGER_NULL(_x) {}

static uint8_t*         main_arg_broadcast_mac = NULL;
static size_t           main_arg_n_broadcast_mac = 0;
static int              main_arg_ifindex = 0;
static uint8_t*         main_arg_mac = NULL;
static size_t           main_arg_n_mac = 0;
static bool             main_arg_test = false;

static Manager *manager_free(Manager *manager) {
        if (!manager)
                return NULL;

        n_dhcp4_client_probe_free(manager->probe);
        n_dhcp4_client_unref(manager->client);
        free(manager);

        return NULL;
}

static void manager_freep(Manager **manager) {
        manager_free(*manager);
}

static int manager_new(Manager **managerp) {
        _cleanup_(n_dhcp4_client_config_freep) NDhcp4ClientConfig *config = NULL;
        _cleanup_(manager_freep) Manager *manager = NULL;
        int r;

        manager = malloc(sizeof(*manager));
        if (!manager)
                return -ENOMEM;

        *manager = (Manager)MANAGER_NULL(*manager);

        r = n_dhcp4_client_config_new(&config);
        if (r)
                return r;

        n_dhcp4_client_config_set_broadcast_mac(config,
                                                main_arg_broadcast_mac,
                                                main_arg_n_broadcast_mac);
        n_dhcp4_client_config_set_ifindex(config, main_arg_ifindex);
        n_dhcp4_client_config_set_mac(config, main_arg_mac, main_arg_n_mac);

        r = n_dhcp4_client_new(&manager->client, config);
        if (r)
                return r;

        *managerp = manager;
        manager = NULL;
        return 0;
}

static int manager_dispatch(Manager *manager) {
        NDhcp4ClientEvent *event;
        int r;

        r = n_dhcp4_client_dispatch(manager->client);
        if (r)
                return r;

        for (;;) {
                r = n_dhcp4_client_pop_event(manager->client, &event);
                if (r)
                        return r;

                if (!event)
                        break;

                fprintf(stderr, "Event: %u\n", event->event);
        }

        return 0;
}

static int manager_run(Manager *manager) {
        _cleanup_(n_dhcp4_client_probe_config_freep) NDhcp4ClientProbeConfig *config = NULL;
        int r;

        r = n_dhcp4_client_probe_config_new(&config);
        if (r)
                return r;

        r = n_dhcp4_client_probe(manager->client, &manager->probe, config);
        if (r)
                return r;

        /*
         * The test-suite runs this with the --test argument. So far, we do not
         * perform any fancy runtime tests, but simply exit the main-loop
         * immediately. We can add more elaborate tests in the future.
         */
        if (main_arg_test)
                return 0;

        for (;;) {
                struct pollfd pfds[] = {
                        { .fd = -1, .events = POLLIN },
                };
                size_t i;
                int n;

                n_dhcp4_client_get_fd(manager->client, &pfds[0].fd);

                n = poll(pfds, sizeof(pfds) / sizeof(*pfds), -1);
                if (n < 0)
                        return -errno;

                for (i = 0; i < (size_t)n; ++i) {
                        if (pfds[i].revents & ~POLLIN)
                                return -ENOTRECOVERABLE;

                        if (!(pfds[i].revents & POLLIN))
                                continue;

                        r = manager_dispatch(manager);
                        if (r)
                                return r;
                }
        }

        return 0;
}

static int run(void) {
        _cleanup_(manager_freep) Manager *manager = NULL;
        int r;

        r = manager_new(&manager);
        if (r)
                return r;

        return manager_run(manager);
}

static void print_help(void) {
        printf("%s [GLOBALS...] ...\n\n"
               "DHCP Test Client\n\n"
               "  -h --help                     Show this help\n"
               "     --test                     Run as part of the test suite\n"
               "     --ifindex IDX              Index of interface to run on\n"
               "     --mac HEX                  Hardware address to use\n"
               "     --broadcast-mac HEX        Broadcast hardware address to use\n"
               , program_invocation_short_name);
}

static int setup_test(void) {
        uint8_t *b;
        size_t n;

        test_setup();

        /* --broadcast-mac */
        {
                n = 6;
                b = malloc(n);
                assert(b);
                memcpy(b, (uint8_t[]) { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }, n);

                free(main_arg_broadcast_mac);
                main_arg_broadcast_mac = b;
                main_arg_n_broadcast_mac = n;
        }

        /* --ifindex */
        {
                main_arg_ifindex = 1;
        }

        /* --mac */
        {
                n = 6;
                b = malloc(n);
                assert(b);
                memcpy(b, (uint8_t[]) { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }, n);

                free(main_arg_mac);
                main_arg_mac = b;
                main_arg_n_mac = n;
        }

        return 0;
}

static int parse_hexstr(const char *in, uint8_t **strp, size_t *n_strp) {
        _cleanup_(n_dhcp4_freep) uint8_t *out = NULL;
        size_t i, n_in, n_out;

        n_in = strlen(in);
        n_out = (n_in + 1) / 2;

        out = malloc(n_out);
        if (!out)
                return -ENOMEM;

        for (i = 0; i < n_in; ++i) {
                uint8_t v = 0;

                switch (in[i]) {
                case '0'...'9':
                        v = in[i] - '0';
                        break;
                case 'a'...'f':
                        v = in[i] - 'a' + 0xa;
                        break;
                case 'A'...'F':
                        v = in[i] - 'A' + 0xa;
                        break;
                }

                if (i % 2) {
                        out[i / 2] <<= 4;
                        out[i / 2] |= v;
                } else {
                        out[i / 2] = v;
                }
        }

        *strp = out;
        *n_strp = n_out;
        out = NULL;
        return 0;
}

static int parse_argv(int argc, char **argv) {
        enum {
                _ARG_0 = 0x100,
                ARG_BROADCAST_MAC,
                ARG_IFINDEX,
                ARG_MAC,
                ARG_TEST,
        };
        static const struct option options[] = {
                { "help",               no_argument,            NULL,   'h'                     },
                { "broadcast-mac",      required_argument,      NULL,   ARG_BROADCAST_MAC       },
                { "ifindex",            required_argument,      NULL,   ARG_IFINDEX             },
                { "mac",                required_argument,      NULL,   ARG_MAC                 },
                { "test",               no_argument,            NULL,   ARG_TEST                },
                {}
        };
        size_t n;
        void *t;
        int r, c;

        /*
         * Most of the argument-parsers are short-and-dirty hacks to make the
         * conversions work. This is sufficient for a test-client, but needs
         * proper error-checking if done outside of tests.
         */

        while ((c = getopt_long(argc, argv, "h", options, NULL)) >= 0) {
                switch (c) {
                case 'h':
                        print_help();
                        return MAIN_EXIT;

                case ARG_BROADCAST_MAC:
                        r = parse_hexstr(optarg, (uint8_t **)&t, &n);
                        if (r)
                                return r;

                        free(main_arg_broadcast_mac);
                        main_arg_broadcast_mac = t;
                        main_arg_n_broadcast_mac = n;
                        break;

                case ARG_IFINDEX:
                        main_arg_ifindex = atoi(optarg);
                        break;

                case ARG_MAC:
                        r = parse_hexstr(optarg, (uint8_t **)&t, &n);
                        if (r)
                                return r;

                        free(main_arg_mac);
                        main_arg_mac = t;
                        main_arg_n_mac = n;
                        break;

                case ARG_TEST:
                        r = setup_test();
                        if (r)
                                return r;

                        main_arg_test = true;
                        break;

                case '?':
                        /* getopt_long() prints warning */
                        return MAIN_FAILED;

                default:
                        return -ENOTRECOVERABLE;
                }
        }

        if (optind != argc) {
                fprintf(stderr,
                        "%s: invalid arguments -- '%s'\n",
                        program_invocation_name,
                        argv[optind]);
                return MAIN_FAILED;
        }

        if (!main_arg_broadcast_mac ||
            !main_arg_ifindex ||
            !main_arg_mac) {
                fprintf(stderr,
                        "%s: required arguments: broadcast-mac, ifindex, mac\n",
                        program_invocation_name);
                return MAIN_FAILED;
        }

        return 0;
}

int main(int argc, char **argv) {
        int r;

        r = parse_argv(argc, argv);
        if (r)
                goto exit;

        r = run();

exit:
        if (r == MAIN_EXIT) {
                r = 0;
        } else if (r < 0) {
                errno = -r;
                fprintf(stderr, "Failed with system errno %d: %m\n", r);
                r = 127;
        } else if (r > 0) {
                fprintf(stderr, "Failed with internal error %d\n", r);
        }

        free(main_arg_broadcast_mac);
        free(main_arg_mac);

        return r;
}