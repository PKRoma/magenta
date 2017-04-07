// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#define _POSIX_C_SOURCE 200809L

#define _GNU_SOURCE
#define _DARWIN_C_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "netprotocol.h"

static bool fuchsia_address = false;
static const char* hostname;

static bool on_device(device_info_t* device, void* cookie) {
    if (hostname != NULL && strcmp(hostname, device->nodename)) {
        // Asking for a specific address and this isn't it.
        return false;
    }

    struct sockaddr_in6 addr = device->inet6_addr;
    if (fuchsia_address) {
        // Make it a valid link-local address by fiddling some bits.
        addr.sin6_addr.s6_addr[11] = 0xFF;
    }

    // Get the string form of the address.
    char addr_s[INET6_ADDRSTRLEN];
    inet_ntop(AF_INET6, &addr.sin6_addr, addr_s, sizeof(addr_s));

    // Get the name of the interface.
    char ifname[IF_NAMESIZE];
    if_indextoname(addr.sin6_scope_id, ifname);
    fprintf(stdout, "%s%%%s\n", addr_s, ifname);

    exit(0);
}

static struct option longopts[] = {
    {"help", no_argument, NULL, 'h'},
    {"fuchsia", no_argument, NULL, 'f'},
    {NULL, 0, NULL, 0},
};

int main(int argc, char** argv) {
    int ch;
    while ((ch = getopt_long_only(argc, argv, "hf", longopts, NULL)) != -1) {
        switch (ch) {
        case 'f':
            fuchsia_address = true;
            break;
        default:
            fprintf(stderr, "%s [--fuchsia] [hostname]\n", argv[0]);
            return 1;
        }
    }

    if (optind < argc) {
        hostname = argv[optind];
    }

    netboot_discover(NB_SERVER_PORT, NULL, on_device, NULL);
    fprintf(stderr, "Failed to discover %s\n", hostname?hostname:"");
    return 1;
}