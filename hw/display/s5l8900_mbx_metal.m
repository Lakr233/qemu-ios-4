/*
 * Experimental Metal compute backend for the measured PowerVR MBX source-over
 * operation.  Command decoding, bounds validation and GART ownership remain in
 * s5l8900_mbx.c; only an already-staged integer pixel operation is submitted.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/display/s5l8900_mbx_metal.h"

#import <Metal/Metal.h>

struct S5L8900MBXMetal {
    id<MTLDevice> device;
    id<MTLCommandQueue> queue;
    id<MTLComputePipelineState> source_over;
    char *device_name;
};

static uint32_t source_over_reference(uint32_t destination, uint32_t source)
{
    uint32_t inverse = 256u - (source >> 24);
    uint32_t result = 0;
    for (unsigned shift = 0; shift < 32; shift += 8) {
        uint32_t value = ((source >> shift) & 0xffu) +
                         ((((destination >> shift) & 0xffu) * inverse) >> 8);
        result |= MIN(value, 0xffu) << shift;
    }
    return result;
}

static NSString *const source_over_shader =
    @"#include <metal_stdlib>\n"
     "using namespace metal;\n"
     "kernel void mbx_source_over(device uint *destination [[buffer(0)]],\n"
     "                            device const uint *source [[buffer(1)]],\n"
     "                            constant uint &count [[buffer(2)]],\n"
     "                            uint index [[thread_position_in_grid]]) {\n"
     "  if (index >= count) return;\n"
     "  uint src = source[index];\n"
     "  uint dst = destination[index];\n"
     "  uint inv = 256u - (src >> 24);\n"
     "  uint result = 0u;\n"
     "  for (uint shift = 0u; shift < 32u; shift += 8u) {\n"
     "    uint value = ((src >> shift) & 255u) +\n"
     "                 ((((dst >> shift) & 255u) * inv) >> 8);\n"
     "    result |= min(value, 255u) << shift;\n"
     "  }\n"
     "  destination[index] = result;\n"
     "}\n";

S5L8900MBXMetal *s5l8900_mbx_metal_create(Error **errp)
{
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device) {
            error_setg(errp, "Metal requested but no system Metal device exists");
            return NULL;
        }
        NSError *error = nil;
        id<MTLLibrary> library = [device newLibraryWithSource:source_over_shader
                                                     options:nil error:&error];
        if (!library) {
            error_setg(errp, "could not compile MBX Metal kernel: %s",
                       error.localizedDescription.UTF8String);
            return NULL;
        }
        id<MTLFunction> function = [library newFunctionWithName:@"mbx_source_over"];
        id<MTLComputePipelineState> pipeline = function ?
            [device newComputePipelineStateWithFunction:function error:&error] : nil;
        [function release];
        [library release];
        if (!pipeline) {
            error_setg(errp, "could not create MBX Metal pipeline: %s",
                       error.localizedDescription.UTF8String);
            return NULL;
        }
        id<MTLCommandQueue> queue = [device newCommandQueue];
        if (!queue) {
            error_setg(errp, "could not create MBX Metal command queue");
            return NULL;
        }
        S5L8900MBXMetal *metal = g_new0(S5L8900MBXMetal, 1);
        metal->device = device;
        metal->queue = queue;
        metal->source_over = pipeline;
        metal->device_name = g_strdup(device.name.UTF8String);
        uint32_t destination[] = {
            0x10203040u, 0xffffffffu, 0x80402010u, 0x01020304u,
        };
        const uint32_t source[] = {
            0x00000000u, 0xff123456u, 0x80402010u, 0x01010101u,
        };
        uint32_t expected[G_N_ELEMENTS(destination)];
        for (size_t i = 0; i < G_N_ELEMENTS(destination); i++) {
            expected[i] = source_over_reference(destination[i], source[i]);
        }
        if (!s5l8900_mbx_metal_source_over(
                metal, (uint8_t *)destination, (const uint8_t *)source,
                G_N_ELEMENTS(destination)) ||
            memcmp(destination, expected, sizeof(destination))) {
            error_setg(errp, "MBX Metal integer source-over self-test failed");
            s5l8900_mbx_metal_destroy(metal);
            return NULL;
        }
        return metal;
    }
}

void s5l8900_mbx_metal_destroy(S5L8900MBXMetal *metal)
{
    if (!metal) {
        return;
    }
    @autoreleasepool {
        [metal->source_over release];
        [metal->queue release];
        [metal->device release];
    }
    g_free(metal->device_name);
    g_free(metal);
}

bool s5l8900_mbx_metal_source_over(S5L8900MBXMetal *metal,
                                   uint8_t *destination,
                                   const uint8_t *source,
                                   uint32_t pixels)
{
    if (!metal || !destination || !source || !pixels) {
        return false;
    }
    @autoreleasepool {
        size_t length = (size_t)pixels * sizeof(uint32_t);
        id<MTLBuffer> destination_buffer =
            [metal->device newBufferWithBytes:destination length:length
                                      options:MTLResourceStorageModeShared];
        id<MTLBuffer> source_buffer =
            [metal->device newBufferWithBytes:source length:length
                                      options:MTLResourceStorageModeShared];
        id<MTLCommandBuffer> command = [metal->queue commandBuffer];
        id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
        if (!destination_buffer || !source_buffer || !command || !encoder) {
            [destination_buffer release];
            [source_buffer release];
            return false;
        }
        [encoder setComputePipelineState:metal->source_over];
        [encoder setBuffer:destination_buffer offset:0 atIndex:0];
        [encoder setBuffer:source_buffer offset:0 atIndex:1];
        [encoder setBytes:&pixels length:sizeof(pixels) atIndex:2];
        NSUInteger width = MIN((NSUInteger)pixels,
                               metal->source_over.maxTotalThreadsPerThreadgroup);
        [encoder dispatchThreads:MTLSizeMake(pixels, 1, 1)
           threadsPerThreadgroup:MTLSizeMake(width, 1, 1)];
        [encoder endEncoding];
        [command commit];
        [command waitUntilCompleted];
        if (command.status != MTLCommandBufferStatusCompleted) {
            [destination_buffer release];
            [source_buffer release];
            return false;
        }
        memcpy(destination, destination_buffer.contents, length);
        [destination_buffer release];
        [source_buffer release];
        return true;
    }
}

const char *s5l8900_mbx_metal_device_name(const S5L8900MBXMetal *metal)
{
    return metal && metal->device_name ? metal->device_name : "none";
}
