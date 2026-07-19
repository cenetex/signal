#include "cargo_package.h"

#include "manifest.h"
#include "sha256.h"

#include <stdlib.h>
#include <string.h>

static const uint8_t PACKAGE_DOMAIN[8] = {
    'S', 'I', 'G', 'P', 'K', 'G', 'v', '1'
};

static bool bytes_all_zero(const uint8_t *bytes, size_t count) {
    uint8_t any = 0;
    for (size_t i = 0; bytes && i < count; i++) any |= bytes[i];
    return any == 0;
}

static int compare_unit_pub(const void *lhs, const void *rhs) {
    const cargo_unit_t *a = (const cargo_unit_t *)lhs;
    const cargo_unit_t *b = (const cargo_unit_t *)rhs;
    int pub_order = memcmp(a->pub, b->pub, 32);
    return pub_order != 0 ? pub_order : memcmp(a, b, sizeof(*a));
}

static bool package_root(const cargo_unit_t *members, size_t count,
                         uint8_t out[32]) {
    uint8_t pubs[CARGO_PACKAGE_MAX_MEMBERS][32];
    uint8_t merkle[32];
    uint8_t preimage[8 + 1 + 1 + 32];
    if (!members || !out || count == 0 ||
        count > CARGO_PACKAGE_MAX_MEMBERS) return false;
    for (size_t i = 0; i < count; i++) {
        if (bytes_all_zero(members[i].pub, 32)) return false;
        memcpy(pubs[i], members[i].pub, 32);
    }
    if (!hash_merkle_root((const uint8_t (*)[32])pubs, count, merkle))
        return false;
    memcpy(preimage, PACKAGE_DOMAIN, sizeof(PACKAGE_DOMAIN));
    preimage[8] = CARGO_PACKAGE_VERSION;
    preimage[9] = (uint8_t)count;
    memcpy(&preimage[10], merkle, 32);
    sha256_bytes(preimage, sizeof(preimage), out);
    return true;
}

bool cargo_package_pack(const cargo_unit_t *members, size_t member_count,
                        cargo_package_t *out) {
    if (!members || !out || member_count == 0 ||
        member_count > CARGO_PACKAGE_MAX_MEMBERS) return false;
    cargo_package_t package;
    memset(&package, 0, sizeof(package));
    package.version = CARGO_PACKAGE_VERSION;
    package.member_count = (uint8_t)member_count;
    package.flags = member_count < CARGO_PACKAGE_HANDLING_BATCH
        ? CARGO_PACKAGE_FLAG_PARTIAL : 0;
    memcpy(package.members, members, member_count * sizeof(members[0]));
    qsort(package.members, member_count, sizeof(package.members[0]),
          compare_unit_pub);
    for (size_t i = 1; i < member_count; i++) {
        if (memcmp(package.members[i - 1].pub,
                   package.members[i].pub, 32) == 0 &&
            !bytes_all_zero(package.members[i].pub, 32)) return false;
    }
    package.provenance = CARGO_PACKAGE_PROVENANCE_KNOWN;
    if (!package_root(package.members, member_count, package.package_pub)) {
        /* Legacy rows remain addressable members but do not receive a
         * fabricated package proof. */
        package.provenance = CARGO_PACKAGE_PROVENANCE_UNKNOWN;
        memset(package.package_pub, 0, sizeof(package.package_pub));
    }
    *out = package;
    return cargo_package_validate(out);
}

bool cargo_package_validate(const cargo_package_t *package) {
    if (!package || package->version != CARGO_PACKAGE_VERSION ||
        package->member_count == 0 ||
        package->member_count > CARGO_PACKAGE_MAX_MEMBERS ||
        package->custody > CARGO_PACKAGE_CUSTODY_STATION) return false;
    bool partial = package->member_count < CARGO_PACKAGE_HANDLING_BATCH;
    if (((package->flags & CARGO_PACKAGE_FLAG_PARTIAL) != 0) != partial)
        return false;
    for (uint8_t i = 1; i < package->member_count; i++) {
        int pub_order = memcmp(package->members[i - 1].pub,
                               package->members[i].pub, 32);
        if (compare_unit_pub(&package->members[i - 1],
                             &package->members[i]) > 0 ||
            (pub_order == 0 &&
             !bytes_all_zero(package->members[i].pub, 32))) return false;
    }
    if (package->provenance == CARGO_PACKAGE_PROVENANCE_KNOWN) {
        uint8_t expected[32];
        return package_root(package->members, package->member_count,
                            expected) &&
               memcmp(expected, package->package_pub, 32) == 0;
    }
    return package->provenance == CARGO_PACKAGE_PROVENANCE_UNKNOWN &&
           bytes_all_zero(package->package_pub, 32);
}

bool cargo_package_unpack(const cargo_package_t *package,
                          cargo_unit_t *members_out, size_t cap,
                          size_t *member_count_out) {
    if (member_count_out) *member_count_out = 0;
    if (!cargo_package_validate(package) || !members_out ||
        cap < package->member_count) return false;
    memcpy(members_out, package->members,
           (size_t)package->member_count * sizeof(package->members[0]));
    if (member_count_out) *member_count_out = package->member_count;
    return true;
}

bool cargo_package_attach_shell(cargo_package_t *package,
                                const uint8_t shell_pub[32]) {
    if (!cargo_package_validate(package) || !shell_pub ||
        bytes_all_zero(shell_pub, 32) ||
        memcmp(shell_pub, package->package_pub, 32) == 0) return false;
    memcpy(package->carrier_shell_pub, shell_pub, 32);
    return true;
}

bool cargo_package_move(cargo_package_t *package,
                        cargo_package_custody_t custody,
                        uint16_t custody_index) {
    if (!cargo_package_validate(package) ||
        custody < CARGO_PACKAGE_CUSTODY_NONE ||
        custody > CARGO_PACKAGE_CUSTODY_STATION) return false;
    package->custody = (uint8_t)custody;
    package->custody_index = custody == CARGO_PACKAGE_CUSTODY_NONE
        ? 0 : custody_index;
    return true;
}

bool cargo_package_is_partial(const cargo_package_t *package) {
    return package &&
           (package->flags & CARGO_PACKAGE_FLAG_PARTIAL) != 0;
}

size_t cargo_package_encoded_size(const cargo_package_t *package) {
    return package && package->member_count <= CARGO_PACKAGE_MAX_MEMBERS
        ? CARGO_PACKAGE_HEADER_BYTES +
          (size_t)package->member_count * sizeof(cargo_unit_t) : 0;
}

static void write_u16(uint8_t *out, uint16_t value) {
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8);
}

static uint16_t read_u16(const uint8_t *data) {
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

bool cargo_package_encode(const cargo_package_t *package,
                          uint8_t *out, size_t cap, size_t *written) {
    if (written) *written = 0;
    if (!cargo_package_validate(package) || !out) return false;
    size_t need = cargo_package_encoded_size(package);
    if (cap < need) return false;
    memcpy(out, "PKG1", 4);
    out[4] = package->version;
    out[5] = package->flags;
    out[6] = package->provenance;
    out[7] = package->custody;
    write_u16(&out[8], package->custody_index);
    out[10] = package->member_count;
    out[11] = 0;
    memcpy(&out[12], package->package_pub, 32);
    memcpy(&out[44], package->carrier_shell_pub, 32);
    memcpy(&out[CARGO_PACKAGE_HEADER_BYTES], package->members,
           (size_t)package->member_count * sizeof(package->members[0]));
    if (written) *written = need;
    return true;
}

bool cargo_package_decode(const uint8_t *data, size_t len,
                          cargo_package_t *out, size_t *consumed) {
    if (consumed) *consumed = 0;
    if (!data || !out || len < CARGO_PACKAGE_HEADER_BYTES ||
        memcmp(data, "PKG1", 4) != 0 ||
        data[4] != CARGO_PACKAGE_VERSION || data[10] == 0 ||
        data[10] > CARGO_PACKAGE_MAX_MEMBERS) return false;
    size_t need = CARGO_PACKAGE_HEADER_BYTES +
                  (size_t)data[10] * sizeof(cargo_unit_t);
    if (len < need) return false;
    cargo_package_t package;
    memset(&package, 0, sizeof(package));
    package.version = data[4];
    package.flags = data[5];
    package.provenance = data[6];
    package.custody = data[7];
    package.custody_index = read_u16(&data[8]);
    package.member_count = data[10];
    memcpy(package.package_pub, &data[12], 32);
    memcpy(package.carrier_shell_pub, &data[44], 32);
    memcpy(package.members, &data[CARGO_PACKAGE_HEADER_BYTES],
           (size_t)package.member_count * sizeof(package.members[0]));
    if (!cargo_package_validate(&package)) return false;
    *out = package;
    if (consumed) *consumed = need;
    return true;
}
