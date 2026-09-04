#ifndef TYPES_H
#define TYPES_H

#include <stdint.h>
#include <inttypes.h>

/* Vertex id: 32 bits, enough for graphs up to ~4.29 billion vertices. */
typedef uint32_t vid_t;

/* Edge index: MUST be 64 bits -- some target graphs exceed 4 billion
 * directed entries, which would silently wrap a 32-bit counter. */
typedef uint64_t eid_t;

#define PRIvid PRIu32
#define PRIeid PRIu64
#define VID_MAX UINT32_MAX

#endif 

