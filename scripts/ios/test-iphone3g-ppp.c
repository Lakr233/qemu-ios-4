/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "iphone3g-ppp.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void feed_frame(ppp_peer_t *peer, uint16_t protocol,
                       const uint8_t *packet, size_t length)
{
    uint8_t frame[2 * PPP_MAX_FRAME + 2];
    size_t frame_length = ppp_frame(protocol, packet, length, 0xffffffffu,
                                    frame, sizeof(frame));
    assert(frame_length != 0);
    ppp_input(peer, frame, frame_length);
}

int main(void)
{
    static const uint8_t request[] = {
        PPP_CONF_REQ, 1, 0, 20,
        LCP_OPT_ASYNCMAP, 6, 0, 0, 0, 0,
        LCP_OPT_MAGIC, 6, 0x79, 0x61, 0xf5, 0x1c,
        LCP_OPT_PCOMP, 2, LCP_OPT_ACCOMP, 2,
    };
    ppp_peer_t peer;
    ppp_init(&peer, NULL);
    ppp_open(&peer);
    feed_frame(&peer, PPP_PROTO_LCP, request, sizeof(request));
    assert(peer.lcp.state == PPP_S_ACKSENT);
    assert(peer.stats.frames_in == 1);
    assert(peer.stats.fcs_errors == 0);
    assert(ppp_output_pending(&peer) != 0);
    puts("iphone3g PPP protocol test passed");
    return 0;
}
