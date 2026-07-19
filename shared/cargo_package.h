/* cargo_package.h — reversible Merkle grouping, separate from carrier cells. */
#ifndef CARGO_PACKAGE_H
#define CARGO_PACKAGE_H

#include "types.h"

enum {
    CARGO_PACKAGE_VERSION = 1,
    CARGO_PACKAGE_HANDLING_BATCH = 4,
    CARGO_PACKAGE_MAX_MEMBERS = 4,
    CARGO_PACKAGE_HEADER_BYTES = 76,
};

typedef enum {
    CARGO_PACKAGE_PROVENANCE_UNKNOWN = 0,
    CARGO_PACKAGE_PROVENANCE_KNOWN = 1,
} cargo_package_provenance_t;

typedef enum {
    CARGO_PACKAGE_CUSTODY_NONE = 0,
    CARGO_PACKAGE_CUSTODY_CARRIER,
    CARGO_PACKAGE_CUSTODY_SHIP,
    CARGO_PACKAGE_CUSTODY_STATION,
} cargo_package_custody_t;

enum {
    CARGO_PACKAGE_FLAG_PARTIAL = 1u << 0,
};

typedef struct {
    uint8_t version;
    uint8_t flags;
    uint8_t provenance; /* cargo_package_provenance_t */
    uint8_t custody;    /* cargo_package_custody_t */
    uint16_t custody_index;
    uint8_t member_count;
    uint8_t _reserved;
    uint8_t package_pub[32];
    uint8_t carrier_shell_pub[32];
    cargo_unit_t members[CARGO_PACKAGE_MAX_MEMBERS];
} cargo_package_t;

bool cargo_package_pack(const cargo_unit_t *members, size_t member_count,
                        cargo_package_t *out);
bool cargo_package_unpack(const cargo_package_t *package,
                          cargo_unit_t *members_out, size_t cap,
                          size_t *member_count_out);
bool cargo_package_validate(const cargo_package_t *package);
bool cargo_package_attach_shell(cargo_package_t *package,
                                const uint8_t shell_pub[32]);
bool cargo_package_move(cargo_package_t *package,
                        cargo_package_custody_t custody,
                        uint16_t custody_index);
bool cargo_package_is_partial(const cargo_package_t *package);

size_t cargo_package_encoded_size(const cargo_package_t *package);
bool cargo_package_encode(const cargo_package_t *package,
                          uint8_t *out, size_t cap, size_t *written);
bool cargo_package_decode(const uint8_t *data, size_t len,
                          cargo_package_t *out, size_t *consumed);

#endif /* CARGO_PACKAGE_H */
