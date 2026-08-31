/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "hw/display/s5l8900_mbx_metal.h"

struct S5L8900MBXMetal {
    int unused;
};

S5L8900MBXMetal *s5l8900_mbx_metal_create(Error **errp)
{
    error_setg(errp, "Metal MBX execution is unavailable on this host");
    return NULL;
}

void s5l8900_mbx_metal_destroy(S5L8900MBXMetal *metal)
{
    (void)metal;
}

bool s5l8900_mbx_metal_source_over(S5L8900MBXMetal *metal,
                                   uint8_t *destination,
                                   const uint8_t *source,
                                   uint32_t pixels)
{
    (void)metal;
    (void)destination;
    (void)source;
    (void)pixels;
    return false;
}

const char *s5l8900_mbx_metal_device_name(const S5L8900MBXMetal *metal)
{
    (void)metal;
    return "unavailable";
}
