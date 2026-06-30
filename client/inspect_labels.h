/* inspect_labels.h -- Small, testable text labels for scan/inspect rows. */
#ifndef SIGNAL_CLIENT_INSPECT_LABELS_H
#define SIGNAL_CLIENT_INSPECT_LABELS_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "net.h"
#include "../shared/types.h"

static inline bool inspect_label_hash32_is_zero(const uint8_t hash[32]) {
    if (!hash) return true;
    for (int i = 0; i < 32; i++) {
        if (hash[i] != 0) return false;
    }
    return true;
}

static inline bool inspect_label_hash32_eq(const uint8_t a[32],
                                           const uint8_t b[32]) {
    if (!a || !b) return false;
    for (int i = 0; i < 32; i++) {
        if (a[i] != b[i]) return false;
    }
    return true;
}

static inline void inspect_label_write_prefixed(char *out,
                                                size_t cap,
                                                const char *prefix,
                                                const char *text) {
    if (!out || cap == 0) return;
    size_t used = 0;
    if (prefix) {
        size_t prefix_len = strlen(prefix);
        if (prefix_len >= cap) prefix_len = cap - 1;
        memcpy(out, prefix, prefix_len);
        used = prefix_len;
    }
    if (text && used + 1 < cap) {
        size_t text_len = strlen(text);
        size_t room = cap - used - 1;
        if (text_len > room) text_len = room;
        memcpy(out + used, text, text_len);
        used += text_len;
    }
    out[used] = '\0';
}

static inline bool inspect_label_diag_is_job(uint8_t kind) {
    switch ((inspect_diag_kind_t)kind) {
    case INSPECT_DIAG_JOB_MINE:
    case INSPECT_DIAG_JOB_HAUL:
    case INSPECT_DIAG_JOB_TOW:
    case INSPECT_DIAG_JOB_DELIVER_PROOF:
    case INSPECT_DIAG_JOB_SCOUT:
    case INSPECT_DIAG_JOB_REPAIR:
        return true;
    default:
        return false;
    }
}

static inline const char *inspect_label_job_kind(uint8_t kind) {
    switch ((inspect_diag_kind_t)kind) {
    case INSPECT_DIAG_JOB_MINE:           return "mine";
    case INSPECT_DIAG_JOB_HAUL:           return "haul";
    case INSPECT_DIAG_JOB_TOW:            return "tow";
    case INSPECT_DIAG_JOB_DELIVER_PROOF:  return "proof";
    case INSPECT_DIAG_JOB_SCOUT:          return "scout";
    case INSPECT_DIAG_JOB_REPAIR:         return "repair";
    default:                              return "job";
    }
}

static inline const char *inspect_label_job_reason(uint8_t reason) {
    switch ((inspect_job_reason_t)reason) {
    case INSPECT_JOB_REASON_LOCAL_CONTRACT:     return "known contract";
    case INSPECT_JOB_REASON_MARKET_DEMAND:      return "heard demand";
    case INSPECT_JOB_REASON_REMOTE_SUPPLY:      return "remote supply";
    case INSPECT_JOB_REASON_RECEIPT_PROOF:      return "receipt proof";
    case INSPECT_JOB_REASON_STATION_TRUST:      return "station trust";
    case INSPECT_JOB_REASON_STATION_RISK:       return "risk adjusted";
    case INSPECT_JOB_REASON_HNN_RESONANCE:      return "hologram match";
    case INSPECT_JOB_REASON_ORE_PRESSURE:       return "ore pressure";
    case INSPECT_JOB_REASON_CONSTRUCTION_PLAN:  return "build plan";
    case INSPECT_JOB_REASON_DELIVERY_PROOF:     return "delivery proof";
    case INSPECT_JOB_REASON_DISTRESS_SIGNAL:    return "distress signal";
    case INSPECT_JOB_REASON_REPAIR_NEED:        return "repair need";
    case INSPECT_JOB_REASON_ROUTE_MEMORY:       return "route memory";
    case INSPECT_JOB_REASON_ROUTE_RISK:         return "route risk";
    case INSPECT_JOB_REASON_GOSSIP_COURIER:     return "gossip courier";
    case INSPECT_JOB_REASON_NONE:
    default:                                    return "pending evidence";
    }
}

static inline const char *inspect_label_job_factor(uint8_t idx) {
    switch (idx) {
    case INSPECT_JOB_FACTOR_VALUE:      return "pay";
    case INSPECT_JOB_FACTOR_DEMAND:     return "demand";
    case INSPECT_JOB_FACTOR_SUPPLY:     return "supply";
    case INSPECT_JOB_FACTOR_ROUTE:      return "route";
    case INSPECT_JOB_FACTOR_FRESHNESS:  return "fresh";
    case INSPECT_JOB_FACTOR_CAPABILITY: return "hull";
    case INSPECT_JOB_FACTOR_PROOF:      return "proof";
    case INSPECT_JOB_FACTOR_HOLOGRAM:   return "holo";
    default:                            return "score";
    }
}

static inline unsigned inspect_label_job_factor_digit(
    const NetInspectSnapshotRow *row,
    uint8_t idx) {
    if (!row || idx >= INSPECT_JOB_FACTOR_COUNT) return 0;
    return (unsigned)((row->cargo_pub[idx] * 9u + 127u) / 255u);
}

static inline void inspect_label_job_top_factors(
    const NetInspectSnapshotRow *row,
    char *out,
    size_t cap) {
    if (!out || cap == 0) return;
    out[0] = '\0';
    if (!row) return;

    int top[3] = {-1, -1, -1};
    uint8_t score[3] = {0, 0, 0};
    for (uint8_t i = 0; i < INSPECT_JOB_FACTOR_COUNT; i++) {
        uint8_t v = row->cargo_pub[i];
        if (v > score[0]) {
            top[2] = top[1]; score[2] = score[1];
            top[1] = top[0]; score[1] = score[0];
            top[0] = (int)i; score[0] = v;
        } else if (v > score[1]) {
            top[2] = top[1]; score[2] = score[1];
            top[1] = (int)i; score[1] = v;
        } else if (v > score[2]) {
            top[2] = (int)i; score[2] = v;
        }
    }

    if (top[0] < 0 || score[0] == 0) {
        snprintf(out, cap, "top: pending");
    } else if (top[2] >= 0 && score[2] > 0) {
        snprintf(out, cap, "top: %s%u %s%u %s%u",
                 inspect_label_job_factor((uint8_t)top[0]),
                 inspect_label_job_factor_digit(row, (uint8_t)top[0]),
                 inspect_label_job_factor((uint8_t)top[1]),
                 inspect_label_job_factor_digit(row, (uint8_t)top[1]),
                 inspect_label_job_factor((uint8_t)top[2]),
                 inspect_label_job_factor_digit(row, (uint8_t)top[2]));
    } else if (top[1] >= 0 && score[1] > 0) {
        snprintf(out, cap, "top: %s%u %s%u",
                 inspect_label_job_factor((uint8_t)top[0]),
                 inspect_label_job_factor_digit(row, (uint8_t)top[0]),
                 inspect_label_job_factor((uint8_t)top[1]),
                 inspect_label_job_factor_digit(row, (uint8_t)top[1]));
    } else {
        snprintf(out, cap, "top: %s%u",
                 inspect_label_job_factor((uint8_t)top[0]),
                 inspect_label_job_factor_digit(row, (uint8_t)top[0]));
    }
}

static inline void inspect_label_hash_detail(const uint8_t hash[32],
                                             char *out,
                                             size_t cap) {
    if (!out || cap == 0) return;
    out[0] = '\0';
    if (inspect_label_hash32_is_zero(hash)) return;
    snprintf(out, cap, "%02x%02x%02x%02x%02x%02x%02x%02x",
             hash[0], hash[1], hash[2], hash[3],
             hash[4], hash[5], hash[6], hash[7]);
}

static inline const char *inspect_label_job_memory(uint8_t kind) {
    switch ((market_memory_kind_t)kind) {
    case MARKET_MEMORY_DEMAND:             return "demand";
    case MARKET_MEMORY_SUPPLY:             return "supply";
    case MARKET_MEMORY_ROUTE_DANGER:       return "danger";
    case MARKET_MEMORY_ROUTE_SUCCESS:      return "route";
    case MARKET_MEMORY_DELIVERY_RECEIPT:   return "receipt";
    case MARKET_MEMORY_ROUTE_REPUTATION:   return "route rep";
    case MARKET_MEMORY_ROUTE_RISK:         return "route risk";
    case MARKET_MEMORY_STATION_TRUST:      return "trust";
    case MARKET_MEMORY_STATION_RISK:       return "risk";
    case MARKET_MEMORY_ORE_PRESSURE:       return "ore";
    case MARKET_MEMORY_SCAFFOLD_PRESSURE:  return "build";
    case MARKET_MEMORY_NONE:
    default:                               return "memory";
    }
}

static inline const char *inspect_label_job_proof_kind(uint8_t kind) {
    switch ((inspect_job_proof_t)kind) {
    case INSPECT_JOB_PROOF_SUBJECT_HASH:  return "subject";
    case INSPECT_JOB_PROOF_CHAIN_ANCHOR:  return "anchor";
    case INSPECT_JOB_PROOF_WITNESS_HASH:  return "witness";
    case INSPECT_JOB_PROOF_NONE:
    default:                              return NULL;
    }
}

static inline void inspect_label_job_proof_prefix(const NetInspectSnapshotRow *row,
                                                  char *out,
                                                  size_t cap) {
    if (!out || cap == 0) return;
    out[0] = '\0';
    if (!row) return;
    if (row->cargo_pub[INSPECT_JOB_META_PROOF_KIND] ==
        (uint8_t)INSPECT_JOB_PROOF_NONE) {
        return;
    }
    snprintf(out, cap, "#%02x%02x%02x%02x",
             row->cargo_pub[INSPECT_JOB_META_PROOF0],
             row->cargo_pub[INSPECT_JOB_META_PROOF1],
             row->cargo_pub[INSPECT_JOB_META_PROOF2],
             row->cargo_pub[INSPECT_JOB_META_PROOF3]);
}

static inline bool inspect_label_job_source_chain(
    const NetInspectSnapshotRow *row,
    const char *station_name,
    char *out,
    size_t cap) {
    if (!out || cap == 0) return false;
    out[0] = '\0';
    if (!row) return false;

    uint8_t memory_kind = row->cargo_pub[INSPECT_JOB_META_MEMORY_KIND];
    uint8_t hops = row->cargo_pub[INSPECT_JOB_META_HOPS];
    uint8_t age = row->cargo_pub[INSPECT_JOB_META_AGE];
    uint8_t proof_kind = row->cargo_pub[INSPECT_JOB_META_PROOF_KIND];
    if (memory_kind == (uint8_t)MARKET_MEMORY_NONE &&
        proof_kind == (uint8_t)INSPECT_JOB_PROOF_NONE) {
        return false;
    }

    char detail[20];
    char proof[12];
    inspect_label_hash_detail(row->receipt_head, detail, sizeof(detail));
    inspect_label_job_proof_prefix(row, proof, sizeof(proof));
    const char *mem_label = inspect_label_job_memory(memory_kind);
    const char *proof_label = inspect_label_job_proof_kind(proof_kind);
    const char *station = (station_name && station_name[0]) ? station_name : "?";

    if (proof_label && detail[0]) {
        snprintf(out, cap, "heard %s @%.6s h%u age%u %s %.8s",
                 mem_label, station, (unsigned)hops, (unsigned)age,
                 proof_label, detail);
    } else if (proof_label && proof[0]) {
        snprintf(out, cap, "heard %s @%.6s h%u age%u %s %.8s",
                 mem_label, station, (unsigned)hops, (unsigned)age,
                 proof_label, proof);
    } else {
        snprintf(out, cap, "heard %s @%.6s h%u age%u",
                 mem_label, station, (unsigned)hops, (unsigned)age);
    }
    return true;
}

static inline bool inspect_label_job_contact_card(
    const NetInspectSnapshotRow *job,
    const char *commodity_name,
    const char *source_station,
    const char *dest_station,
    char *line1,
    size_t line1_cap,
    char *line2,
    size_t line2_cap,
    char *line3,
    size_t line3_cap) {
    if (!job) return false;
    if (line1 && line1_cap > 0) line1[0] = '\0';
    if (line2 && line2_cap > 0) line2[0] = '\0';
    if (line3 && line3_cap > 0) line3[0] = '\0';

    const char *kind = inspect_label_job_kind(job->commodity);
    const char *commodity =
        (commodity_name && commodity_name[0]) ? commodity_name : "work";
    const char *source =
        (source_station && source_station[0]) ? source_station : "?";
    const char *dest = (dest_station && dest_station[0]) ? dest_station : "?";

    if (line1 && line1_cap > 0) {
        snprintf(line1, line1_cap, "%s %s -> %.12s",
                 kind, commodity, dest);
    }
    if (line2 && line2_cap > 0) {
        snprintf(line2, line2_cap, "because %s",
                 inspect_label_job_reason(
                     job->cargo_pub[INSPECT_JOB_META_REASON]));
    }
    if (line3 && line3_cap > 0) {
        if (!inspect_label_job_source_chain(job, source, line3, line3_cap))
            snprintf(line3, line3_cap, "source: local scan");
    }
    return true;
}

static inline bool inspect_label_market_source_chain(
    const NetInspectSnapshotRow *row,
    char *out,
    size_t cap) {
    if (!out || cap == 0) return false;
    out[0] = '\0';
    if (!row) return false;

    char anchor[20];
    char source[20];
    char witness[20];
    char subject[20];
    inspect_label_hash_detail(row->receipt_head, anchor, sizeof(anchor));
    inspect_label_hash_detail(row->origin_station, source, sizeof(source));
    inspect_label_hash_detail(row->latest_station, witness, sizeof(witness));
    inspect_label_hash_detail(row->cargo_pub, subject, sizeof(subject));

    if (anchor[0]) {
        if (source[0] && witness[0]) {
            snprintf(out, cap, "anchor %.8s relay %.8s witness %.8s",
                     anchor, source, witness);
        } else if (source[0]) {
            snprintf(out, cap, "anchor %.8s relay %.8s",
                     anchor, source);
        } else {
            snprintf(out, cap, "anchor %.8s", anchor);
        }
    } else if (witness[0]) {
        snprintf(out, cap, "witness %.8s%s%.8s",
                 witness, source[0] ? " relay " : "",
                 source[0] ? source : "");
    } else if (source[0]) {
        snprintf(out, cap, "relay %.8s%s%.8s",
                 source, subject[0] ? " subject " : "",
                 subject[0] ? subject : "");
    } else if (subject[0]) {
        snprintf(out, cap, "subject %.8s", subject);
    } else {
        return false;
    }
    return true;
}

typedef struct {
    const NetInspectSnapshotRow *job;
    const NetInspectSnapshotRow *source_memory;
    const NetInspectSnapshotRow *receipt;
    const NetInspectSnapshotRow *first_receipt_link;
    const NetInspectSnapshotRow *last_receipt_link;
    int receipt_link_count;
} InspectJobCause;

static inline bool inspect_label_find_job_cause(const NetInspectSnapshot *snap,
                                                InspectJobCause *out) {
    if (!snap || !out) return false;
    memset(out, 0, sizeof(*out));

    const NetInspectSnapshotRow *fallback_job = NULL;
    for (int i = 0; i < snap->row_count && i < INSPECT_SNAPSHOT_MAX_ROWS; i++) {
        const NetInspectSnapshotRow *row = &snap->rows[i];
        if (!(row->flags & INSPECT_ROW_DIAGNOSTIC)) continue;
        if (!inspect_label_diag_is_job(row->commodity)) continue;
        if (!fallback_job) fallback_job = row;
        if (row->chain_len >= 200) {
            out->job = row;
            break;
        }
    }
    if (!out->job) out->job = fallback_job;
    if (!out->job) return false;

    const uint8_t *proof = out->job->receipt_head;
    bool has_proof = !inspect_label_hash32_is_zero(proof);
    bool receipt_seen = false;
    for (int i = 0; i < snap->row_count && i < INSPECT_SNAPSHOT_MAX_ROWS; i++) {
        const NetInspectSnapshotRow *row = &snap->rows[i];
        bool diagnostic = (row->flags & INSPECT_ROW_DIAGNOSTIC) != 0;
        if (diagnostic && row->commodity == (uint8_t)INSPECT_DIAG_RECEIPT_LINK) {
            if (receipt_seen) {
                if (!out->first_receipt_link)
                    out->first_receipt_link = row;
                out->last_receipt_link = row;
                out->receipt_link_count++;
            }
            continue;
        }
        if (!has_proof) continue;
        if (row == out->job) continue;
        if (diagnostic) {
            if (!out->source_memory &&
                !inspect_label_diag_is_job(row->commodity) &&
                inspect_label_hash32_eq(row->receipt_head, proof)) {
                out->source_memory = row;
            }
        } else if (!out->receipt &&
                   (row->flags & INSPECT_ROW_HAS_RECEIPT) &&
                   inspect_label_hash32_eq(row->receipt_head, proof)) {
            out->receipt = row;
            receipt_seen = true;
        }
    }
    return true;
}

static inline bool inspect_label_receipt_link_line(
    const NetInspectSnapshotRow *link,
    char *out,
    size_t cap) {
    if (!out || cap == 0) return false;
    out[0] = '\0';
    if (!link) return false;
    if (!(link->flags & INSPECT_ROW_DIAGNOSTIC) ||
        link->commodity != (uint8_t)INSPECT_DIAG_RECEIPT_LINK) {
        return false;
    }

    char head[20];
    char author[20];
    char recipient[20];
    inspect_label_hash_detail(link->receipt_head, head, sizeof(head));
    inspect_label_hash_detail(link->origin_station, author, sizeof(author));
    inspect_label_hash_detail(link->latest_station, recipient, sizeof(recipient));
    if (!head[0] && !author[0] && !recipient[0]) return false;

    if (author[0] && recipient[0]) {
        snprintf(out, cap, "link: %u/%u ev%llu %.8s>%.8s",
                 (unsigned)link->grade,
                 (unsigned)link->chain_len,
                 (unsigned long long)link->event_id,
                 author,
                 recipient);
    } else if (author[0]) {
        snprintf(out, cap, "link: %u/%u ev%llu auth %.8s",
                 (unsigned)link->grade,
                 (unsigned)link->chain_len,
                 (unsigned long long)link->event_id,
                 author);
    } else if (head[0]) {
        snprintf(out, cap, "link: %u/%u ev%llu head %.8s",
                 (unsigned)link->grade,
                 (unsigned)link->chain_len,
                 (unsigned long long)link->event_id,
                 head);
    } else {
        return false;
    }
    return true;
}

static inline bool inspect_label_receipt_link_line_named(
    const NetInspectSnapshotRow *link,
    const char *author_label,
    const char *recipient_label,
    char *out,
    size_t cap) {
    if (!out || cap == 0) return false;
    out[0] = '\0';
    if (!link) return false;
    if (!(link->flags & INSPECT_ROW_DIAGNOSTIC) ||
        link->commodity != (uint8_t)INSPECT_DIAG_RECEIPT_LINK) {
        return false;
    }

    char head[20];
    inspect_label_hash_detail(link->receipt_head, head, sizeof(head));
    const char *author = (author_label && author_label[0]) ? author_label : NULL;
    const char *recipient = (recipient_label && recipient_label[0])
        ? recipient_label : NULL;

    if (author && recipient) {
        snprintf(out, cap, "link: %u/%u ev%llu %.12s>%.12s",
                 (unsigned)link->grade,
                 (unsigned)link->chain_len,
                 (unsigned long long)link->event_id,
                 author,
                 recipient);
    } else if (author) {
        snprintf(out, cap, "link: %u/%u ev%llu auth %.12s",
                 (unsigned)link->grade,
                 (unsigned)link->chain_len,
                 (unsigned long long)link->event_id,
                 author);
    } else if (head[0]) {
        snprintf(out, cap, "link: %u/%u ev%llu head %.8s",
                 (unsigned)link->grade,
                 (unsigned)link->chain_len,
                 (unsigned long long)link->event_id,
                 head);
    } else {
        return false;
    }
    return true;
}

static inline int inspect_label_receipt_link_lines_for_cause(
    const NetInspectSnapshot *snap,
    const InspectJobCause *cause,
    char out[][64],
    int max_lines);

static inline int inspect_label_receipt_link_lines_for_cause_page(
    const NetInspectSnapshot *snap,
    const InspectJobCause *cause,
    int skip_lines,
    char out[][64],
    int max_lines) {
    if (!snap || !cause || !cause->receipt || !out || max_lines <= 0)
        return 0;
    for (int i = 0; i < max_lines; i++)
        out[i][0] = '\0';
    if (skip_lines < 0) skip_lines = 0;

    bool receipt_seen = false;
    int count = 0;
    int seen_links = 0;
    for (int i = 0; i < snap->row_count && i < INSPECT_SNAPSHOT_MAX_ROWS; i++) {
        const NetInspectSnapshotRow *row = &snap->rows[i];
        if (row == cause->receipt) {
            receipt_seen = true;
            continue;
        }
        if (!receipt_seen) continue;
        if (!(row->flags & INSPECT_ROW_DIAGNOSTIC) ||
            row->commodity != (uint8_t)INSPECT_DIAG_RECEIPT_LINK) {
            continue;
        }
        if (seen_links++ < skip_lines) continue;
        if (inspect_label_receipt_link_line(row, out[count], 64))
            count++;
        if (count >= max_lines) break;
    }
    return count;
}

static inline int inspect_label_receipt_link_lines_for_cause(
    const NetInspectSnapshot *snap,
    const InspectJobCause *cause,
    char out[][64],
    int max_lines) {
    return inspect_label_receipt_link_lines_for_cause_page(
        snap, cause, 0, out, max_lines);
}

static inline bool inspect_label_receipt_browser_footer(int link_total,
                                                        int page_start,
                                                        int page_count,
                                                        bool multi_page,
                                                        char *out,
                                                        size_t cap) {
    if (!out || cap == 0) return false;
    out[0] = '\0';
    if (link_total <= 0) {
        snprintf(out, cap, "no local receipt links  [Shift+TAB] close");
        return true;
    }
    if (page_count < 0) page_count = 0;
    if (page_start < 0) page_start = 0;
    int end = page_start + page_count;
    if (end > link_total) end = link_total;
    if (end < page_start) end = page_start;
    if (multi_page) {
        snprintf(out, cap, "links %d-%d/%d  [TAB] next  [Shift+TAB] close",
                 page_start + 1, end, link_total);
    } else {
        snprintf(out, cap, "links %d/%d  [Shift+TAB] close",
                 link_total, link_total);
    }
    return true;
}

static inline bool inspect_label_job_cause_lines(
    const InspectJobCause *cause,
    const char *job_station_name,
    char *line1,
    size_t line1_cap,
    char *line2,
    size_t line2_cap,
    char *line3,
    size_t line3_cap) {
    if (!cause || !cause->job) return false;
    if (line1 && line1_cap > 0) line1[0] = '\0';
    if (line2 && line2_cap > 0) line2[0] = '\0';
    if (line3 && line3_cap > 0) line3[0] = '\0';

    char job_source[96];
    if (inspect_label_job_source_chain(cause->job, job_station_name,
                                       job_source, sizeof(job_source))) {
        if (line1 && line1_cap > 0)
            snprintf(line1, line1_cap, "cause: %s", job_source);
    } else if (line1 && line1_cap > 0) {
        snprintf(line1, line1_cap, "cause: selected job");
    }

    char source_chain[128];
    if (cause->source_memory &&
        inspect_label_market_source_chain(cause->source_memory,
                                          source_chain,
                                          sizeof(source_chain))) {
        if (line2 && line2_cap > 0)
            inspect_label_write_prefixed(line2, line2_cap,
                                         "source: ", source_chain);
    } else if (line2 && line2_cap > 0) {
        if (!inspect_label_hash32_is_zero(cause->job->receipt_head))
            snprintf(line2, line2_cap, "source: proof anchor only");
        else
            snprintf(line2, line2_cap, "source: no source memory");
    }

    if (line3 && line3_cap > 0) {
        if (cause->receipt) {
            const char *where =
                (cause->receipt->flags & INSPECT_ROW_STATION_RECEIPT)
                ? "station chain" : "carried chain";
            snprintf(line3, line3_cap, "receipt: %s %u link%s",
                     where,
                     (unsigned)cause->receipt->chain_len,
                     cause->receipt->chain_len == 1 ? "" : "s");
        } else if (cause->source_memory &&
                   !inspect_label_hash32_is_zero(cause->job->receipt_head)) {
            snprintf(line3, line3_cap, "receipt: anchor known, not local");
        } else if (!inspect_label_hash32_is_zero(cause->job->receipt_head)) {
            snprintf(line3, line3_cap, "receipt: proof anchor only");
        } else {
            snprintf(line3, line3_cap, "receipt: no proof");
        }
    }
    return true;
}

static inline bool inspect_label_job_detail_lines(
    const InspectJobCause *cause,
    const char *job_station_name,
    char *line1,
    size_t line1_cap,
    char *line2,
    size_t line2_cap,
    char *line3,
    size_t line3_cap,
    char *line4,
    size_t line4_cap,
    char *line5,
    size_t line5_cap,
    char *line6,
    size_t line6_cap) {
    if (!cause || !cause->job) return false;
    if (line1 && line1_cap > 0) line1[0] = '\0';
    if (line2 && line2_cap > 0) line2[0] = '\0';
    if (line3 && line3_cap > 0) line3[0] = '\0';
    if (line4 && line4_cap > 0) line4[0] = '\0';
    if (line5 && line5_cap > 0) line5[0] = '\0';
    if (line6 && line6_cap > 0) line6[0] = '\0';

    const NetInspectSnapshotRow *job = cause->job;
    const char *status = job->chain_len >= 200 ? "selected" : "candidate";
    if (line1 && line1_cap > 0) {
        snprintf(line1, line1_cap, "job: %s %s sc%u",
                 inspect_label_job_kind(job->commodity),
                 status,
                 (unsigned)job->grade);
    }
    if (line2 && line2_cap > 0) {
        snprintf(line2, line2_cap, "why: %s",
                 inspect_label_job_reason(
                     job->cargo_pub[INSPECT_JOB_META_REASON]));
    }
    inspect_label_job_top_factors(job, line3, line3_cap);

    uint8_t memory_kind = job->cargo_pub[INSPECT_JOB_META_MEMORY_KIND];
    uint8_t hops = job->cargo_pub[INSPECT_JOB_META_HOPS];
    uint8_t age = job->cargo_pub[INSPECT_JOB_META_AGE];
    const char *station = (job_station_name && job_station_name[0])
        ? job_station_name : "?";
    if (line4 && line4_cap > 0) {
        if (memory_kind != (uint8_t)MARKET_MEMORY_NONE) {
            snprintf(line4, line4_cap, "heard: %s @%.6s h%u age%u",
                     inspect_label_job_memory(memory_kind),
                     station,
                     (unsigned)hops,
                     (unsigned)age);
        } else {
            snprintf(line4, line4_cap, "heard: local job evidence");
        }
    }

    char anchor[20];
    char relay[20];
    char witness[20];
    inspect_label_hash_detail(job->receipt_head, anchor, sizeof(anchor));
    if (cause->source_memory) {
        inspect_label_hash_detail(cause->source_memory->origin_station,
                                  relay, sizeof(relay));
        inspect_label_hash_detail(cause->source_memory->latest_station,
                                  witness, sizeof(witness));
    } else {
        relay[0] = '\0';
        witness[0] = '\0';
    }
    if (line5 && line5_cap > 0) {
        if (anchor[0] && relay[0]) {
            snprintf(line5, line5_cap, "proof: %.8s relay %.8s",
                     anchor, relay);
        } else if (anchor[0]) {
            snprintf(line5, line5_cap, "proof: %.8s anchor only", anchor);
        } else {
            snprintf(line5, line5_cap, "proof: none");
        }
    }
    if (line6 && line6_cap > 0) {
        if (cause->first_receipt_link) {
            const char *where =
                (cause->receipt &&
                 (cause->receipt->flags & INSPECT_ROW_STATION_RECEIPT))
                ? "station" : "local";
            snprintf(line6, line6_cap, "links: %d %s signed row%s",
                     cause->receipt_link_count,
                     where,
                     cause->receipt_link_count == 1 ? "" : "s");
        } else if (cause->receipt) {
            const char *where =
                (cause->receipt->flags & INSPECT_ROW_STATION_RECEIPT)
                ? "station chain" : "carried chain";
            snprintf(line6, line6_cap, "links: %s, no local rows", where);
        } else if (witness[0]) {
            snprintf(line6, line6_cap, "links: anchor known, chain not local");
        } else if (anchor[0]) {
            snprintf(line6, line6_cap, "links: proof anchor only");
        } else {
            snprintf(line6, line6_cap, "links: no proof");
        }
    }
    return true;
}

#endif /* SIGNAL_CLIENT_INSPECT_LABELS_H */
