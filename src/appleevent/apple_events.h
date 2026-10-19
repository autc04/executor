#if !defined(__rsys_apple_events_h__)
#define __rsys_apple_events_h__
namespace Executor
{

extern void AE_init(void);
extern void AE_reinit(void);

/* Look up a handler for dispatching, applying the wildcard specificity rules
 * of AEProcessAppleEvent.  Returns false when no entry matches. */
extern bool ae_lookup_event_handler(AEEventClass event_class,
                                    AEEventID event_id, bool system_p,
                                    GUEST<AEEventHandlerUPP> *handler_out,
                                    GUEST<int32_t> *refcon_out);

/* Look up a special (pre-dispatch) handler. */
extern bool ae_lookup_special_handler(AEKeyword function_class, bool system_p,
                                      GUEST<AEEventHandlerUPP> *handler_out,
                                      GUEST<int32_t> *refcon_out);

/* Default coercions performed when no installed handler matches (IM-IAC,
 * table 4-1). */
extern OSErr AE_default_coerce(DescType from_type, const void *data,
                               Size data_size, DescType to_type,
                               AEDesc *result);
extern OSErr AE_default_coerce_desc(const AEDesc *desc, DescType to_type,
                                    AEDesc *result);

/* Allocate a session-unique non-zero return ID (kAutoGenerateReturnID). */
extern int16_t AE_generate_return_id(void);

struct inline_desc_t
{
    GUEST_STRUCT;
    GUEST<DescType> type;
    GUEST<uint32_t> size;
    GUEST<char> data[0];
};

struct inline_key_desc_t
{
    GUEST_STRUCT;
    GUEST<int32_t> key;
    GUEST<DescType> type;
    GUEST<uint32_t> size;
    GUEST<char> data[0];
};

typedef struct list_header
{
    GUEST_STRUCT;
    GUEST<uint32_t> unknown_1;          /* #### always zero (?) */
    GUEST<uint32_t> unknown_2;          /* #### contains an applzone address */
    GUEST<uint32_t> param_offset;       /* contains a tick that identifies the object, either `list' or
                                           `reco'; for an apple event, this fiend contains the offset to the
                                           parameter section */
    GUEST<uint32_t> attribute_count;    
    GUEST<uint32_t> param_count;
    GUEST<int32_t> unknown_3;           /* ### always zero (?) */
    GUEST<char> data[0];                /* offset: 0x18 */
} list_header_t;

typedef list_header_t *list_header_ptr;

typedef GUEST<list_header_ptr> *list_header_h;

#define PARAM_OFFSET(aggr_desc_h) \
    ((*(list_header_h)aggr_desc_h)->param_offset)
#define PARAM_COUNT(aggr_desc_h) \
    ((*(list_header_h)aggr_desc_h)->param_count)
#define ATTRIBUTE_COUNT(aggr_desc_h) \
    ((*(list_header_h)aggr_desc_h)->attribute_count)

/* #### always zero (?) */

/* #### contains unknown values */

/* #### zero pad, use unknown */

/* #### takes on various values, no idea */

/* beginning of target inline descriptor; `target->size' determines
     target's actual size */

/* for show */
/* #### contains `aevt' tick */

/* contains 0x00010001 */

/* marker containing tick `;;;;' */

typedef struct ae_header
{
    GUEST_STRUCT;
    GUEST<uint32_t> unknown_1;
    GUEST<uint32_t> unknown_2;
    GUEST<uint32_t> param_offset;
    GUEST<uint32_t> attribute_count;
    GUEST<uint32_t> param_count;
    GUEST<char[26]> pad_1;
    GUEST<AEEventClass> event_class;
    GUEST<AEEventID> event_id;
    GUEST<uint32_t> unknown_3;
    GUEST<inline_desc_t> target;
#if 0
    GUEST< uint32_t> unknown_4;
    GUEST< uint32_t> unknown_5;
    GUEST< char[...]> attribute_data;
    GUEST< uint32_t> unknown_6;
    GUEST< char[...]> param_data;
#endif
} ae_header_t;

typedef struct subdesc_info
{
    int count;
    int base_offset;
    GUEST<uint32_t> *count_p;
    bool key_p;
    int inline_desc_header_size;
} subdesc_info_t;

static_assert(sizeof(inline_desc_t) == 8);
static_assert(sizeof(inline_key_desc_t) == 12);
static_assert(sizeof(list_header_t) == 24);
static_assert(sizeof(ae_header_t) == 66);
}
#endif /* !defined (__rsys_apple_events_h__) */
