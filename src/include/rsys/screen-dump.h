#if !defined(__rsys_screen_dump_h__)
#define __rsys_screen_dump_h__

namespace Executor
{
struct header
{
    int16_t byte_order;
    int16_t magic_number;
    int32_t ifd_offset;
};

struct __attribute__((packed)) directory_entry
{
    int16_t tag;
    int16_t type;
    int32_t count;
    union {
        int16_t value_offset_16;
        int32_t value_offset;
    };
};

struct __attribute__((packed)) ifd 
{
    int16_t count;
    struct directory_entry entries[1];
};

static_assert(sizeof(header) == 8);
static_assert(sizeof(directory_entry) == 12);
static_assert(sizeof(ifd) == 14);
}

#endif /* !defined (__rsys_screen_dump_h__) */
