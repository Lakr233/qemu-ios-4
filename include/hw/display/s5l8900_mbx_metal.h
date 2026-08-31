/* Optional macOS Metal execution backend for measured S5L8900 MBX jobs. */
#ifndef HW_DISPLAY_S5L8900_MBX_METAL_H
#define HW_DISPLAY_S5L8900_MBX_METAL_H

#include "qapi/error.h"

typedef struct S5L8900MBXMetal S5L8900MBXMetal;

S5L8900MBXMetal *s5l8900_mbx_metal_create(Error **errp);
void s5l8900_mbx_metal_destroy(S5L8900MBXMetal *metal);
bool s5l8900_mbx_metal_source_over(S5L8900MBXMetal *metal,
                                   uint8_t *destination,
                                   const uint8_t *source,
                                   uint32_t pixels);
const char *s5l8900_mbx_metal_device_name(const S5L8900MBXMetal *metal);

#endif /* HW_DISPLAY_S5L8900_MBX_METAL_H */
