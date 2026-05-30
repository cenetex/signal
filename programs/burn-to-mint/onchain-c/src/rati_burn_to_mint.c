#include <sol/cpi.h>
#include <sol/deserialize.h>
#include <sol/entrypoint.h>
#include <sol/pubkey.h>
#include <sol/types.h>

#define MAX_ACCOUNTS 13
#define PUBKEY_LEN 32
#define CONFIG_LEN 116
#define DESTINATION_TOKEN_CONFIG_LEN 188
#define SOURCE_MINT_CONFIG_LEN 150
#define ACCOUNT_VERSION 1

#define INITIALIZE_CONFIG_IX_LEN 2
#define REGISTER_DESTINATION_MINT_IX_LEN 75
#define REGISTER_SOURCE_MINT_IX_LEN 21
#define SET_SOURCE_ENABLED_IX_LEN 2
#define MIGRATE_IX_LEN 26
#define PAUSE_IX_LEN 2
#define FINALIZE_SOURCE_MINT_IX_LEN 1
#define TRANSFER_AUTHORITY_BEGIN_IX_LEN 33
#define TRANSFER_AUTHORITY_ACCEPT_IX_LEN 1
#define RETIRE_AUTHORITY_IX_LEN 1

#define TOKEN_BURN_CHECKED 15
#define TOKEN_MINT_TO_CHECKED 14
#define TOKEN_CHECKED_IX_LEN 10
#define SPL_MINT_BASE_LEN 82
#define SPL_MINT_AUTHORITY_OPTION_OFFSET 0
#define SPL_MINT_AUTHORITY_OFFSET 4
#define SPL_MINT_DECIMALS_OFFSET 44
#define SPL_MINT_IS_INITIALIZED_OFFSET 45
#define SPL_MINT_FREEZE_AUTHORITY_OPTION_OFFSET 46
#define SPL_TOKEN_ACCOUNT_BASE_LEN 165
#define SPL_TOKEN_ACCOUNT_MINT_OFFSET 0
#define SPL_TOKEN_ACCOUNT_OWNER_OFFSET 32
#define SYSTEM_CREATE_ACCOUNT_DISCRIMINATOR 0
#define SYSTEM_CREATE_ACCOUNT_IX_LEN 52
#define RENT_ACCOUNT_STORAGE_OVERHEAD 128
#define DEFAULT_RENT_EXEMPT_LAMPORTS_PER_BYTE 6960

#define RATI_ERROR_BASE 0x52415400
#define ERR_EXECUTOR_NOT_IMPLEMENTED (RATI_ERROR_BASE + 1)
#define ERR_ACCOUNT_COUNT_MISMATCH (RATI_ERROR_BASE + 2)
#define ERR_PROTOCOL_FEE_FORBIDDEN (RATI_ERROR_BASE + 3)
#define ERR_MISSING_REQUIRED_SIGNER (RATI_ERROR_BASE + 4)
#define ERR_MISSING_WRITABLE_ACCOUNT (RATI_ERROR_BASE + 5)
#define ERR_ACCOUNT_DATA_LEN_MISMATCH (RATI_ERROR_BASE + 6)
#define ERR_ACCOUNT_NOT_PROGRAM_OWNED (RATI_ERROR_BASE + 7)
#define ERR_AUTHORITY_MISMATCH (RATI_ERROR_BASE + 8)
#define ERR_ACCOUNT_ADDRESS_MISMATCH (RATI_ERROR_BASE + 9)
#define ERR_RELATIONSHIP_MISMATCH (RATI_ERROR_BASE + 10)
#define ERR_PENDING_AUTHORITY_NOT_SET (RATI_ERROR_BASE + 11)
#define ERR_PENDING_AUTHORITY_MISMATCH (RATI_ERROR_BASE + 12)
#define ERR_TOKEN_ACCOUNT_DATA_TOO_SHORT (RATI_ERROR_BASE + 13)
#define ERR_TOKEN_ACCOUNT_MINT_MISMATCH (RATI_ERROR_BASE + 14)
#define ERR_TOKEN_ACCOUNT_OWNER_MISMATCH (RATI_ERROR_BASE + 15)
#define ERR_TOKEN_PROGRAM_MISMATCH (RATI_ERROR_BASE + 16)
#define ERR_MINT_DECIMALS_MISMATCH (RATI_ERROR_BASE + 17)
#define ERR_MINT_AUTHORITY_PDA_MISMATCH (RATI_ERROR_BASE + 18)
#define ERR_SYSTEM_PROGRAM_MISMATCH (RATI_ERROR_BASE + 19)
#define ERR_CONFIG_PDA_MISMATCH (RATI_ERROR_BASE + 20)
#define ERR_DESTINATION_CONFIG_PDA_MISMATCH (RATI_ERROR_BASE + 21)
#define ERR_SOURCE_CONFIG_PDA_MISMATCH (RATI_ERROR_BASE + 22)
#define ERR_PDA_SEED_SHAPE (RATI_ERROR_BASE + 23)
#define ERR_ACCOUNT_ALREADY_INITIALIZED (RATI_ERROR_BASE + 24)
#define ERR_PROGRAM_PAUSED (RATI_ERROR_BASE + 25)
#define ERR_SOURCE_FINALIZED (RATI_ERROR_BASE + 26)
#define ERR_MINT_AUTHORITY_MISMATCH (RATI_ERROR_BASE + 27)
#define ERR_FREEZE_AUTHORITY_PRESENT (RATI_ERROR_BASE + 28)
#define ERR_MINT_NOT_INITIALIZED (RATI_ERROR_BASE + 29)
#define ERR_TOKEN_ID_HASH_ZERO (RATI_ERROR_BASE + 30)
#define ERR_CORE_BASE (RATI_ERROR_BASE + 0x100)

static const uint8_t SYSTEM_PROGRAM_ID_BYTES[32] = {0};
static const uint8_t PDA_SEED_PREFIX[] = "rati";
static const uint8_t CONFIG_SEED_KIND[] = "burn-to-mint";
static const uint8_t CONFIG_SEED_ACCOUNT[] = "config";
static const uint8_t PDA_SEED_VERSION[] = "v1";
static const uint8_t DESTINATION_CONFIG_SEED_KIND[] = "destination";
static const uint8_t SOURCE_CONFIG_SEED_KIND[] = "source";
static const uint8_t MINT_AUTHORITY_SEED_KIND[] = "mint-authority";

typedef enum {
  CORE_INVALID_LENGTH = 1,
  CORE_INVALID_VERSION = 2,
  CORE_INVALID_DISCRIMINANT = 3,
  CORE_INVALID_BOOL = 4,
  CORE_ZERO_AMOUNT = 5,
  CORE_SOURCE_DISABLED = 6,
  CORE_SOURCE_DESTINATION_MISMATCH = 7,
  CORE_DESTINATION_NOT_ENABLED = 8,
  CORE_DESTINATION_FINALIZED = 9,
  CORE_DESTINATION_PAUSED = 10,
  CORE_INVALID_RATIO = 11,
  CORE_FRACTIONAL_RATIO = 12,
  CORE_DIVISION_BY_ZERO = 13,
  CORE_SLIPPAGE_EXCEEDED = 14,
  CORE_DESTINATION_AMOUNT_TOO_SMALL = 15,
  CORE_SUPPLY_CAP_EXCEEDED = 16,
  CORE_UNSUPPORTED_MIGRATION_MODE = 17,
  CORE_ARITHMETIC_OVERFLOW = 18,
} CoreErrorCode;

typedef enum {
  STATUS_PLANNED = 0,
  STATUS_CANDIDATE = 1,
  STATUS_ENABLED = 2,
  STATUS_PAUSED = 3,
  STATUS_FINALIZED = 4,
} LaunchStatus;

typedef enum {
  MIGRATION_BURN_TO_MINT = 0,
  MIGRATION_PROOF_ONLY = 1,
} MigrationMode;

typedef enum {
  PRICE_FIXED_RATIO = 0,
  PRICE_BONDING_CURVE = 1,
} PriceMode;

typedef enum {
  IX_INITIALIZE_CONFIG = 0,
  IX_REGISTER_DESTINATION_MINT = 1,
  IX_REGISTER_SOURCE_MINT = 2,
  IX_SET_SOURCE_ENABLED = 3,
  IX_MIGRATE = 4,
  IX_PAUSE = 5,
  IX_FINALIZE_SOURCE_MINT = 6,
  IX_TRANSFER_AUTHORITY_BEGIN = 7,
  IX_TRANSFER_AUTHORITY_ACCEPT = 8,
  IX_RETIRE_AUTHORITY = 9,
} InstructionKind;

typedef struct {
  uint8_t admin[32];
  uint8_t pause_authority[32];
  uint8_t launch_status;
  uint32_t destination_count;
  uint32_t source_count;
  uint8_t config_bump;
  uint8_t pending_authority[32];
  bool pending_authority_set;
  uint8_t reserved[8];
} Config;

typedef struct {
  uint8_t destination_mint[32];
  uint64_t mint_vanity_nonce;
  uint8_t mint_bump;
  uint8_t token_id_hash[32];
  uint8_t decimals;
  uint8_t token_program[32];
  uint8_t mint_authority[32];
  uint64_t max_supply;
  uint64_t total_minted;
  uint64_t bonding_min;
  uint64_t bonding_range;
  uint64_t min_dest_amount;
  uint8_t status;
  uint8_t reserved[8];
} DestinationConfig;

typedef struct {
  uint8_t source_mint[32];
  uint8_t destination_mint[32];
  uint8_t source_token_program[32];
  uint8_t source_decimals;
  uint8_t migration_mode;
  uint8_t price_mode;
  uint64_t fixed_ratio_source_amount;
  uint64_t fixed_ratio_destination_amount;
  bool enabled;
  uint64_t burned_base_units;
  uint64_t minted_destination_base_units;
  uint64_t migration_count;
  uint8_t bump;
  bool finalized;
  uint8_t reserved[7];
} SourceConfig;

typedef struct {
  uint64_t source_amount_to_burn;
  uint64_t destination_amount_to_mint;
} MigrationQuote;

typedef struct {
  uint8_t signer;
  uint8_t writable;
} AccountRule;

typedef struct {
  uint8_t kind;
  uint8_t config_bump;
  uint8_t token_id_hash[32];
  uint64_t mint_vanity_nonce;
  uint8_t mint_bump;
  uint8_t decimals;
  uint64_t max_supply;
  uint64_t bonding_min;
  uint64_t bonding_range;
  uint64_t min_dest_amount;
  uint8_t source_decimals;
  uint8_t migration_mode;
  uint8_t price_mode;
  uint64_t fixed_ratio_source_amount;
  uint64_t fixed_ratio_destination_amount;
  uint8_t bump;
  bool enabled;
  bool paused;
  uint64_t desired_destination_amount;
  uint64_t max_source_amount;
  uint64_t user_nonce;
  bool create_receipt;
  uint8_t pending_authority[32];
} Instruction;

static uint64_t custom(uint32_t error) {
  return error;
}

static bool eq32(const uint8_t *a, const uint8_t *b) {
  for (uint64_t i = 0; i < 32; i++) {
    if (a[i] != b[i]) {
      return false;
    }
  }
  return true;
}

static void copy32(uint8_t *dst, const uint8_t *src) {
  for (uint64_t i = 0; i < 32; i++) {
    dst[i] = src[i];
  }
}

static bool any_nonzero32(const uint8_t *value) {
  for (uint64_t i = 0; i < 32; i++) {
    if (value[i] != 0) {
      return true;
    }
  }
  return false;
}

static uint32_t read_u32(const uint8_t *input, uint64_t offset) {
  return ((uint32_t)input[offset]) |
         ((uint32_t)input[offset + 1] << 8) |
         ((uint32_t)input[offset + 2] << 16) |
         ((uint32_t)input[offset + 3] << 24);
}

static uint64_t read_u64(const uint8_t *input, uint64_t offset) {
  uint64_t value = 0;
  for (uint64_t i = 0; i < 8; i++) {
    value |= ((uint64_t)input[offset + i]) << (8 * i);
  }
  return value;
}

static void write_u32(uint8_t *out, uint64_t offset, uint32_t value) {
  for (uint64_t i = 0; i < 4; i++) {
    out[offset + i] = (uint8_t)(value >> (8 * i));
  }
}

static void write_u64(uint8_t *out, uint64_t offset, uint64_t value) {
  for (uint64_t i = 0; i < 8; i++) {
    out[offset + i] = (uint8_t)(value >> (8 * i));
  }
}

static uint64_t read_bool(uint8_t value, bool *out) {
  if (value == 0) {
    *out = false;
    return SUCCESS;
  }
  if (value == 1) {
    *out = true;
    return SUCCESS;
  }
  return custom(ERR_CORE_BASE + CORE_INVALID_BOOL);
}

static uint64_t require_len(uint64_t actual, uint64_t expected) {
  return actual == expected ? SUCCESS : custom(ERR_CORE_BASE + CORE_INVALID_LENGTH);
}

static uint64_t unpack_config(const uint8_t *input, uint64_t len, Config *out) {
  uint64_t err = require_len(len, CONFIG_LEN);
  if (err) return err;
  if (input[0] != ACCOUNT_VERSION) return custom(ERR_CORE_BASE + CORE_INVALID_VERSION);
  copy32(out->admin, input + 1);
  copy32(out->pause_authority, input + 33);
  if (input[65] > STATUS_FINALIZED) return custom(ERR_CORE_BASE + CORE_INVALID_DISCRIMINANT);
  out->launch_status = input[65];
  out->destination_count = read_u32(input, 66);
  out->source_count = read_u32(input, 70);
  out->config_bump = input[74];
  copy32(out->pending_authority, input + 75);
  err = read_bool(input[107], &out->pending_authority_set);
  if (err) return err;
  for (uint64_t i = 0; i < 8; i++) out->reserved[i] = input[108 + i];
  return SUCCESS;
}

static uint64_t pack_config(uint8_t *out, uint64_t len, const Config *config) {
  uint64_t err = require_len(len, CONFIG_LEN);
  if (err) return err;
  out[0] = ACCOUNT_VERSION;
  copy32(out + 1, config->admin);
  copy32(out + 33, config->pause_authority);
  out[65] = config->launch_status;
  write_u32(out, 66, config->destination_count);
  write_u32(out, 70, config->source_count);
  out[74] = config->config_bump;
  copy32(out + 75, config->pending_authority);
  out[107] = config->pending_authority_set ? 1 : 0;
  for (uint64_t i = 0; i < 8; i++) out[108 + i] = config->reserved[i];
  return SUCCESS;
}

static uint64_t unpack_destination(const uint8_t *input, uint64_t len, DestinationConfig *out) {
  uint64_t err = require_len(len, DESTINATION_TOKEN_CONFIG_LEN);
  if (err) return err;
  if (input[0] != ACCOUNT_VERSION) return custom(ERR_CORE_BASE + CORE_INVALID_VERSION);
  copy32(out->destination_mint, input + 1);
  out->mint_vanity_nonce = read_u64(input, 33);
  out->mint_bump = input[41];
  copy32(out->token_id_hash, input + 42);
  out->decimals = input[74];
  copy32(out->token_program, input + 75);
  copy32(out->mint_authority, input + 107);
  out->max_supply = read_u64(input, 139);
  out->total_minted = read_u64(input, 147);
  out->bonding_min = read_u64(input, 155);
  out->bonding_range = read_u64(input, 163);
  out->min_dest_amount = read_u64(input, 171);
  if (input[179] > STATUS_FINALIZED) return custom(ERR_CORE_BASE + CORE_INVALID_DISCRIMINANT);
  out->status = input[179];
  for (uint64_t i = 0; i < 8; i++) out->reserved[i] = input[180 + i];
  return SUCCESS;
}

static uint64_t pack_destination(uint8_t *out, uint64_t len, const DestinationConfig *config) {
  uint64_t err = require_len(len, DESTINATION_TOKEN_CONFIG_LEN);
  if (err) return err;
  out[0] = ACCOUNT_VERSION;
  copy32(out + 1, config->destination_mint);
  write_u64(out, 33, config->mint_vanity_nonce);
  out[41] = config->mint_bump;
  copy32(out + 42, config->token_id_hash);
  out[74] = config->decimals;
  copy32(out + 75, config->token_program);
  copy32(out + 107, config->mint_authority);
  write_u64(out, 139, config->max_supply);
  write_u64(out, 147, config->total_minted);
  write_u64(out, 155, config->bonding_min);
  write_u64(out, 163, config->bonding_range);
  write_u64(out, 171, config->min_dest_amount);
  out[179] = config->status;
  for (uint64_t i = 0; i < 8; i++) out[180 + i] = config->reserved[i];
  return SUCCESS;
}

static uint64_t unpack_source(const uint8_t *input, uint64_t len, SourceConfig *out) {
  uint64_t err = require_len(len, SOURCE_MINT_CONFIG_LEN);
  if (err) return err;
  if (input[0] != ACCOUNT_VERSION) return custom(ERR_CORE_BASE + CORE_INVALID_VERSION);
  copy32(out->source_mint, input + 1);
  copy32(out->destination_mint, input + 33);
  copy32(out->source_token_program, input + 65);
  out->source_decimals = input[97];
  if (input[98] > MIGRATION_PROOF_ONLY || input[99] > PRICE_BONDING_CURVE) {
    return custom(ERR_CORE_BASE + CORE_INVALID_DISCRIMINANT);
  }
  out->migration_mode = input[98];
  out->price_mode = input[99];
  out->fixed_ratio_source_amount = read_u64(input, 100);
  out->fixed_ratio_destination_amount = read_u64(input, 108);
  err = read_bool(input[116], &out->enabled);
  if (err) return err;
  out->burned_base_units = read_u64(input, 117);
  out->minted_destination_base_units = read_u64(input, 125);
  out->migration_count = read_u64(input, 133);
  out->bump = input[141];
  err = read_bool(input[142], &out->finalized);
  if (err) return err;
  for (uint64_t i = 0; i < 7; i++) out->reserved[i] = input[143 + i];
  return SUCCESS;
}

static uint64_t pack_source(uint8_t *out, uint64_t len, const SourceConfig *config) {
  uint64_t err = require_len(len, SOURCE_MINT_CONFIG_LEN);
  if (err) return err;
  out[0] = ACCOUNT_VERSION;
  copy32(out + 1, config->source_mint);
  copy32(out + 33, config->destination_mint);
  copy32(out + 65, config->source_token_program);
  out[97] = config->source_decimals;
  out[98] = config->migration_mode;
  out[99] = config->price_mode;
  write_u64(out, 100, config->fixed_ratio_source_amount);
  write_u64(out, 108, config->fixed_ratio_destination_amount);
  out[116] = config->enabled ? 1 : 0;
  write_u64(out, 117, config->burned_base_units);
  write_u64(out, 125, config->minted_destination_base_units);
  write_u64(out, 133, config->migration_count);
  out[141] = config->bump;
  out[142] = config->finalized ? 1 : 0;
  for (uint64_t i = 0; i < 7; i++) out[143 + i] = config->reserved[i];
  return SUCCESS;
}

static uint64_t unpack_instruction(const uint8_t *input, uint64_t len, Instruction *out) {
  if (len == 0) return ERROR_INVALID_INSTRUCTION_DATA;
  out->kind = input[0];
  uint64_t err = SUCCESS;
  switch (out->kind) {
    case IX_INITIALIZE_CONFIG:
      err = require_len(len, INITIALIZE_CONFIG_IX_LEN);
      out->config_bump = input[1];
      return err;
    case IX_REGISTER_DESTINATION_MINT:
      err = require_len(len, REGISTER_DESTINATION_MINT_IX_LEN);
      if (err) return err;
      copy32(out->token_id_hash, input + 1);
      out->mint_vanity_nonce = read_u64(input, 33);
      out->mint_bump = input[41];
      out->decimals = input[42];
      out->max_supply = read_u64(input, 43);
      out->bonding_min = read_u64(input, 51);
      out->bonding_range = read_u64(input, 59);
      out->min_dest_amount = read_u64(input, 67);
      return SUCCESS;
    case IX_REGISTER_SOURCE_MINT:
      err = require_len(len, REGISTER_SOURCE_MINT_IX_LEN);
      if (err) return err;
      out->source_decimals = input[1];
      if (input[2] > MIGRATION_PROOF_ONLY || input[3] > PRICE_BONDING_CURVE) {
        return custom(ERR_CORE_BASE + CORE_INVALID_DISCRIMINANT);
      }
      out->migration_mode = input[2];
      out->price_mode = input[3];
      out->fixed_ratio_source_amount = read_u64(input, 4);
      out->fixed_ratio_destination_amount = read_u64(input, 12);
      out->bump = input[20];
      return SUCCESS;
    case IX_SET_SOURCE_ENABLED:
      err = require_len(len, SET_SOURCE_ENABLED_IX_LEN);
      if (err) return err;
      return read_bool(input[1], &out->enabled);
    case IX_MIGRATE:
      err = require_len(len, MIGRATE_IX_LEN);
      if (err) return err;
      out->desired_destination_amount = read_u64(input, 1);
      out->max_source_amount = read_u64(input, 9);
      out->user_nonce = read_u64(input, 17);
      return read_bool(input[25], &out->create_receipt);
    case IX_PAUSE:
      err = require_len(len, PAUSE_IX_LEN);
      if (err) return err;
      return read_bool(input[1], &out->paused);
    case IX_FINALIZE_SOURCE_MINT:
      return require_len(len, FINALIZE_SOURCE_MINT_IX_LEN);
    case IX_TRANSFER_AUTHORITY_BEGIN:
      err = require_len(len, TRANSFER_AUTHORITY_BEGIN_IX_LEN);
      if (err) return err;
      copy32(out->pending_authority, input + 1);
      return SUCCESS;
    case IX_TRANSFER_AUTHORITY_ACCEPT:
      return require_len(len, TRANSFER_AUTHORITY_ACCEPT_IX_LEN);
    case IX_RETIRE_AUTHORITY:
      return require_len(len, RETIRE_AUTHORITY_IX_LEN);
    default:
      return custom(ERR_CORE_BASE + CORE_INVALID_DISCRIMINANT);
  }
}

static const AccountRule INITIALIZE_CONFIG_RULES[] = {
  {1, 1}, {0, 1}, {0, 0}, {0, 0}, {0, 0},
};
static const AccountRule REGISTER_DESTINATION_MINT_RULES[] = {
  {1, 1}, {0, 1}, {0, 1}, {0, 1}, {0, 0}, {0, 0}, {0, 0},
};
static const AccountRule REGISTER_SOURCE_MINT_RULES[] = {
  {1, 1}, {0, 1}, {0, 0}, {0, 1}, {0, 0}, {0, 0}, {0, 0}, {0, 0},
};
static const AccountRule SET_SOURCE_ENABLED_RULES[] = {
  {1, 0}, {0, 0}, {0, 1}, {0, 0},
};
static const AccountRule MIGRATE_BASE_RULES[] = {
  {1, 0}, {0, 0}, {0, 1}, {0, 1}, {0, 1}, {0, 1},
  {0, 1}, {0, 1}, {0, 0}, {0, 0}, {0, 0},
};
static const AccountRule MIGRATE_WITH_RECEIPT_RULES[] = {
  {1, 0}, {0, 0}, {0, 1}, {0, 1}, {0, 1}, {0, 1}, {0, 1},
  {0, 1}, {0, 0}, {0, 0}, {0, 0}, {0, 1}, {0, 0},
};
static const AccountRule PAUSE_RULES[] = {
  {1, 0}, {0, 1}, {0, 1},
};
static const AccountRule FINALIZE_SOURCE_MINT_RULES[] = {
  {1, 0}, {0, 0}, {0, 1}, {0, 0},
};
static const AccountRule TRANSFER_AUTHORITY_BEGIN_RULES[] = {
  {1, 0}, {0, 1}, {0, 0},
};
static const AccountRule TRANSFER_AUTHORITY_ACCEPT_RULES[] = {
  {1, 0}, {0, 1}, {0, 0},
};
static const AccountRule RETIRE_AUTHORITY_RULES[] = {
  {1, 0}, {0, 1}, {1, 0},
};

static const AccountRule *rules_for(const Instruction *ix, uint64_t *len) {
  switch (ix->kind) {
    case IX_INITIALIZE_CONFIG:
      *len = SOL_ARRAY_SIZE(INITIALIZE_CONFIG_RULES);
      return INITIALIZE_CONFIG_RULES;
    case IX_REGISTER_DESTINATION_MINT:
      *len = SOL_ARRAY_SIZE(REGISTER_DESTINATION_MINT_RULES);
      return REGISTER_DESTINATION_MINT_RULES;
    case IX_REGISTER_SOURCE_MINT:
      *len = SOL_ARRAY_SIZE(REGISTER_SOURCE_MINT_RULES);
      return REGISTER_SOURCE_MINT_RULES;
    case IX_SET_SOURCE_ENABLED:
      *len = SOL_ARRAY_SIZE(SET_SOURCE_ENABLED_RULES);
      return SET_SOURCE_ENABLED_RULES;
    case IX_MIGRATE:
      if (ix->create_receipt) {
        *len = SOL_ARRAY_SIZE(MIGRATE_WITH_RECEIPT_RULES);
        return MIGRATE_WITH_RECEIPT_RULES;
      }
      *len = SOL_ARRAY_SIZE(MIGRATE_BASE_RULES);
      return MIGRATE_BASE_RULES;
    case IX_PAUSE:
      *len = SOL_ARRAY_SIZE(PAUSE_RULES);
      return PAUSE_RULES;
    case IX_FINALIZE_SOURCE_MINT:
      *len = SOL_ARRAY_SIZE(FINALIZE_SOURCE_MINT_RULES);
      return FINALIZE_SOURCE_MINT_RULES;
    case IX_TRANSFER_AUTHORITY_BEGIN:
      *len = SOL_ARRAY_SIZE(TRANSFER_AUTHORITY_BEGIN_RULES);
      return TRANSFER_AUTHORITY_BEGIN_RULES;
    case IX_TRANSFER_AUTHORITY_ACCEPT:
      *len = SOL_ARRAY_SIZE(TRANSFER_AUTHORITY_ACCEPT_RULES);
      return TRANSFER_AUTHORITY_ACCEPT_RULES;
    case IX_RETIRE_AUTHORITY:
      *len = SOL_ARRAY_SIZE(RETIRE_AUTHORITY_RULES);
      return RETIRE_AUTHORITY_RULES;
    default:
      *len = 0;
      return NULL;
  }
}

static uint64_t validate_accounts(const SolParameters *params, const Instruction *ix) {
  uint64_t len = 0;
  const AccountRule *rules = rules_for(ix, &len);
  if (rules == NULL) return ERROR_INVALID_INSTRUCTION_DATA;
  if (params->ka_num < len) return custom(ERR_ACCOUNT_COUNT_MISMATCH);
  if (params->ka_num > len) return custom(ERR_PROTOCOL_FEE_FORBIDDEN);
  for (uint64_t i = 0; i < len; i++) {
    if (rules[i].signer && !params->ka[i].is_signer) {
      return custom(ERR_MISSING_REQUIRED_SIGNER);
    }
    if (rules[i].writable && !params->ka[i].is_writable) {
      return custom(ERR_MISSING_WRITABLE_ACCOUNT);
    }
  }
  return SUCCESS;
}

static uint64_t require_program_owned(const SolAccountInfo *account, const SolPubkey *program_id) {
  return SolPubkey_same(account->owner, program_id) ? SUCCESS : custom(ERR_ACCOUNT_NOT_PROGRAM_OWNED);
}

static uint64_t require_data_len(const SolAccountInfo *account, uint64_t expected) {
  return account->data_len == expected ? SUCCESS : custom(ERR_ACCOUNT_DATA_LEN_MISMATCH);
}

static uint64_t require_min_data_len(const SolAccountInfo *account, uint64_t expected) {
  return account->data_len >= expected ? SUCCESS : custom(ERR_TOKEN_ACCOUNT_DATA_TOO_SHORT);
}

static uint64_t require_uninitialized(const SolAccountInfo *account) {
  return account->data_len > 0 && account->data[0] == 0
           ? SUCCESS
           : custom(ERR_ACCOUNT_ALREADY_INITIALIZED);
}

static uint64_t require_account_address(const uint8_t *expected, const SolAccountInfo *account) {
  return eq32(expected, account->key->x) ? SUCCESS : custom(ERR_ACCOUNT_ADDRESS_MISMATCH);
}

static uint64_t require_authority(const uint8_t *expected, const SolAccountInfo *account) {
  return eq32(expected, account->key->x) ? SUCCESS : custom(ERR_AUTHORITY_MISMATCH);
}

static uint64_t require_system_program(const SolAccountInfo *account) {
  return eq32(account->key->x, SYSTEM_PROGRAM_ID_BYTES) ? SUCCESS : custom(ERR_SYSTEM_PROGRAM_MISMATCH);
}

static uint64_t read_config_account(const SolPubkey *program_id, const SolAccountInfo *account, Config *out) {
  uint64_t err = require_program_owned(account, program_id);
  if (err) return err;
  return unpack_config(account->data, account->data_len, out);
}

static uint64_t write_config_account(const SolAccountInfo *account, const Config *config) {
  return pack_config(account->data, account->data_len, config);
}

static uint64_t read_destination_account(const SolPubkey *program_id, const SolAccountInfo *account, DestinationConfig *out) {
  uint64_t err = require_program_owned(account, program_id);
  if (err) return err;
  return unpack_destination(account->data, account->data_len, out);
}

static uint64_t write_destination_account(const SolAccountInfo *account, const DestinationConfig *config) {
  return pack_destination(account->data, account->data_len, config);
}

static uint64_t read_source_account(const SolPubkey *program_id, const SolAccountInfo *account, SourceConfig *out) {
  uint64_t err = require_program_owned(account, program_id);
  if (err) return err;
  return unpack_source(account->data, account->data_len, out);
}

static uint64_t write_source_account(const SolAccountInfo *account, const SourceConfig *config) {
  return pack_source(account->data, account->data_len, config);
}

static uint64_t minimum_balance(uint64_t space) {
  // Release gate: replace or explicitly approve this formula before RC.
  // The active Solana C SDK headers do not expose a rent sysvar helper.
  return (space + RENT_ACCOUNT_STORAGE_OVERHEAD) * DEFAULT_RENT_EXEMPT_LAMPORTS_PER_BYTE;
}

static uint64_t require_pda(
  const SolPubkey *program_id,
  const SolAccountInfo *account,
  const SolSignerSeed *seeds,
  int seed_count,
  uint32_t error,
  uint8_t *bump
) {
  SolPubkey expected;
  uint64_t result = sol_try_find_program_address(seeds, seed_count, program_id, &expected, bump);
  if (result != 0 || !SolPubkey_same(&expected, account->key)) {
    return custom(error);
  }
  return SUCCESS;
}

static uint64_t create_account_signed(
  const SolAccountInfo *system_program,
  const SolAccountInfo *payer,
  const SolAccountInfo *new_account,
  const SolPubkey *owner,
  uint64_t space,
  const SolSignerSeed *signer_seeds,
  int signer_seed_count
) {
  uint8_t data[SYSTEM_CREATE_ACCOUNT_IX_LEN] = {0};
  write_u32(data, 0, SYSTEM_CREATE_ACCOUNT_DISCRIMINATOR);
  write_u64(data, 4, minimum_balance(space));
  write_u64(data, 12, space);
  copy32(data + 20, owner->x);

  SolAccountMeta metas[] = {
    {payer->key, true, true},
    {new_account->key, true, true},
  };
  SolInstruction instruction = {
    system_program->key,
    metas,
    SOL_ARRAY_SIZE(metas),
    data,
    sizeof(data),
  };
  SolAccountInfo infos[] = {*payer, *new_account, *system_program};
  SolSignerSeeds signers[] = {{signer_seeds, (uint64_t)signer_seed_count}};
  return sol_invoke_signed(&instruction, infos, SOL_ARRAY_SIZE(infos), signers, SOL_ARRAY_SIZE(signers));
}

static uint64_t ensure_program_account(
  const SolPubkey *program_id,
  const SolAccountInfo *payer,
  SolAccountInfo *account,
  const SolAccountInfo *system_program,
  uint64_t space,
  const SolSignerSeed *signer_seeds,
  int signer_seed_count
) {
  if (SolPubkey_same(account->owner, program_id)) {
    return require_data_len(account, space);
  }
  uint64_t err = require_system_program(system_program);
  if (err) return err;
  err = create_account_signed(system_program, payer, account, program_id, space, signer_seeds, signer_seed_count);
  if (err) return err;
  account->data_len = space;
  copy32(account->owner->x, program_id->x);
  err = require_program_owned(account, program_id);
  if (err) return err;
  return require_data_len(account, space);
}

static uint64_t require_relationship(bool condition, uint32_t error) {
  return condition ? SUCCESS : custom(error);
}

static uint64_t read_pubkey_at(const SolAccountInfo *account, uint64_t offset, uint8_t *out) {
  if (account->data_len < offset + 32) return custom(ERR_TOKEN_ACCOUNT_DATA_TOO_SHORT);
  copy32(out, account->data + offset);
  return SUCCESS;
}

static uint64_t read_coption_tag(const uint8_t *data, uint64_t len, uint64_t offset, uint32_t *out) {
  if (len < offset + 4) return custom(ERR_TOKEN_ACCOUNT_DATA_TOO_SHORT);
  *out = read_u32(data, offset);
  return SUCCESS;
}

static uint64_t require_owned_by(const SolAccountInfo *account, const SolAccountInfo *owner) {
  return SolPubkey_same(account->owner, owner->key) ? SUCCESS : custom(ERR_TOKEN_PROGRAM_MISMATCH);
}

static uint64_t require_token_account_matches(const SolAccountInfo *account, const uint8_t *mint, const uint8_t *owner) {
  uint64_t err = require_min_data_len(account, SPL_TOKEN_ACCOUNT_BASE_LEN);
  if (err) return err;
  uint8_t actual[32];
  err = read_pubkey_at(account, SPL_TOKEN_ACCOUNT_MINT_OFFSET, actual);
  if (err) return err;
  if (!eq32(actual, mint)) return custom(ERR_TOKEN_ACCOUNT_MINT_MISMATCH);
  err = read_pubkey_at(account, SPL_TOKEN_ACCOUNT_OWNER_OFFSET, actual);
  if (err) return err;
  return eq32(actual, owner) ? SUCCESS : custom(ERR_TOKEN_ACCOUNT_OWNER_MISMATCH);
}

static uint64_t require_mint_decimals(const SolAccountInfo *account, uint8_t decimals) {
  uint64_t err = require_min_data_len(account, SPL_MINT_BASE_LEN);
  if (err) return err;
  return account->data[SPL_MINT_DECIMALS_OFFSET] == decimals ? SUCCESS : custom(ERR_MINT_DECIMALS_MISMATCH);
}

static uint64_t require_destination_mint_setup(
  const SolAccountInfo *mint,
  const SolAccountInfo *token_program,
  uint8_t decimals,
  const uint8_t *expected_mint_authority
) {
  uint64_t err = require_owned_by(mint, token_program);
  if (err) return err;
  err = require_min_data_len(mint, SPL_MINT_BASE_LEN);
  if (err) return err;
  if (mint->data[SPL_MINT_IS_INITIALIZED_OFFSET] != 1) return custom(ERR_MINT_NOT_INITIALIZED);
  if (mint->data[SPL_MINT_DECIMALS_OFFSET] != decimals) return custom(ERR_MINT_DECIMALS_MISMATCH);
  uint32_t tag = 0;
  err = read_coption_tag(mint->data, mint->data_len, SPL_MINT_AUTHORITY_OPTION_OFFSET, &tag);
  if (err) return err;
  if (tag != 1) return custom(ERR_MINT_AUTHORITY_MISMATCH);
  if (!eq32(mint->data + SPL_MINT_AUTHORITY_OFFSET, expected_mint_authority)) {
    return custom(ERR_MINT_AUTHORITY_MISMATCH);
  }
  err = read_coption_tag(mint->data, mint->data_len, SPL_MINT_FREEZE_AUTHORITY_OPTION_OFFSET, &tag);
  if (err) return err;
  return tag == 0 ? SUCCESS : custom(ERR_FREEZE_AUTHORITY_PRESENT);
}

static uint64_t require_mint_authority_pda(
  const SolPubkey *program_id,
  const uint8_t *destination_mint,
  const SolAccountInfo *mint_authority,
  uint8_t *bump
) {
  SolSignerSeed seeds[] = {
    {PDA_SEED_PREFIX, sizeof(PDA_SEED_PREFIX) - 1},
    {MINT_AUTHORITY_SEED_KIND, sizeof(MINT_AUTHORITY_SEED_KIND) - 1},
    {destination_mint, 32},
  };
  return require_pda(program_id, mint_authority, seeds, SOL_ARRAY_SIZE(seeds), ERR_MINT_AUTHORITY_PDA_MISMATCH, bump);
}

static uint64_t checked_add_u64(uint64_t a, uint64_t b, uint64_t *out) {
  if (UINT64_MAX - a < b) return custom(ERR_CORE_BASE + CORE_ARITHMETIC_OVERFLOW);
  *out = a + b;
  return SUCCESS;
}

static uint64_t fixed_ratio_source_amount(uint64_t destination_amount, uint64_t source_ratio, uint64_t destination_ratio, uint64_t *out) {
  if (source_ratio == 0 || destination_ratio == 0) return custom(ERR_CORE_BASE + CORE_INVALID_RATIO);
  unsigned __int128 product = ((unsigned __int128)destination_amount) * source_ratio;
  if (product % destination_ratio != 0) return custom(ERR_CORE_BASE + CORE_FRACTIONAL_RATIO);
  unsigned __int128 quotient = product / destination_ratio;
  if (quotient > UINT64_MAX) return custom(ERR_CORE_BASE + CORE_ARITHMETIC_OVERFLOW);
  *out = (uint64_t)quotient;
  return SUCCESS;
}

static uint64_t bonding_curve_source_amount(
  uint64_t current_minted,
  uint64_t destination_amount,
  uint64_t max_supply,
  uint64_t bonding_min,
  uint64_t bonding_range,
  uint64_t *out
) {
  if (max_supply == 0) return custom(ERR_CORE_BASE + CORE_DIVISION_BY_ZERO);
  uint64_t next_total = 0;
  uint64_t err = checked_add_u64(current_minted, destination_amount, &next_total);
  if (err) return err;
  if (next_total > max_supply) return custom(ERR_CORE_BASE + CORE_SUPPLY_CAP_EXCEEDED);
  unsigned __int128 numerator =
    ((unsigned __int128)bonding_range) * (((unsigned __int128)current_minted) * 2 + destination_amount);
  unsigned __int128 denominator = ((unsigned __int128)max_supply) * 2;
  unsigned __int128 avg_price = ((unsigned __int128)bonding_min) + (numerator / denominator);
  unsigned __int128 source = ((unsigned __int128)destination_amount) * avg_price;
  if (source > UINT64_MAX) return custom(ERR_CORE_BASE + CORE_ARITHMETIC_OVERFLOW);
  *out = (uint64_t)source;
  return SUCCESS;
}

static uint64_t quote_migration(const DestinationConfig *destination, const SourceConfig *source, uint64_t desired_destination_amount, uint64_t max_source_amount, MigrationQuote *out) {
  if (!source->enabled) return custom(ERR_CORE_BASE + CORE_SOURCE_DISABLED);
  if (source->finalized) return custom(ERR_CORE_BASE + CORE_DESTINATION_FINALIZED);
  if (!eq32(source->destination_mint, destination->destination_mint)) {
    return custom(ERR_CORE_BASE + CORE_SOURCE_DESTINATION_MISMATCH);
  }
  if (source->migration_mode != MIGRATION_BURN_TO_MINT) {
    return custom(ERR_CORE_BASE + CORE_UNSUPPORTED_MIGRATION_MODE);
  }
  if (destination->status != STATUS_ENABLED) {
    return destination->status == STATUS_PAUSED
             ? custom(ERR_CORE_BASE + CORE_DESTINATION_PAUSED)
             : custom(ERR_CORE_BASE + CORE_DESTINATION_NOT_ENABLED);
  }
  if (desired_destination_amount < destination->min_dest_amount) {
    return custom(ERR_CORE_BASE + CORE_DESTINATION_AMOUNT_TOO_SMALL);
  }
  uint64_t next_total = 0;
  uint64_t err = checked_add_u64(destination->total_minted, desired_destination_amount, &next_total);
  if (err) return err;
  if (destination->max_supply != 0 && next_total > destination->max_supply) {
    return custom(ERR_CORE_BASE + CORE_SUPPLY_CAP_EXCEEDED);
  }
  uint64_t source_amount = 0;
  if (source->price_mode == PRICE_FIXED_RATIO) {
    err = fixed_ratio_source_amount(
      desired_destination_amount,
      source->fixed_ratio_source_amount,
      source->fixed_ratio_destination_amount,
      &source_amount
    );
  } else {
    err = bonding_curve_source_amount(
      destination->total_minted,
      desired_destination_amount,
      destination->max_supply,
      destination->bonding_min,
      destination->bonding_range,
      &source_amount
    );
  }
  if (err) return err;
  if (source_amount == 0) return custom(ERR_CORE_BASE + CORE_ZERO_AMOUNT);
  if (source_amount > max_source_amount) return custom(ERR_CORE_BASE + CORE_SLIPPAGE_EXCEEDED);
  out->source_amount_to_burn = source_amount;
  out->destination_amount_to_mint = desired_destination_amount;
  return SUCCESS;
}

static uint64_t apply_migration_counters(DestinationConfig *destination, SourceConfig *source, const MigrationQuote *quote) {
  uint64_t err = checked_add_u64(destination->total_minted, quote->destination_amount_to_mint, &destination->total_minted);
  if (err) return err;
  err = checked_add_u64(source->burned_base_units, quote->source_amount_to_burn, &source->burned_base_units);
  if (err) return err;
  err = checked_add_u64(source->minted_destination_base_units, quote->destination_amount_to_mint, &source->minted_destination_base_units);
  if (err) return err;
  return checked_add_u64(source->migration_count, 1, &source->migration_count);
}

static void checked_token_ix_data(uint8_t discriminator, uint64_t amount, uint8_t decimals, uint8_t *out) {
  out[0] = discriminator;
  write_u64(out, 1, amount);
  out[9] = decimals;
}

static uint64_t invoke_burn_checked(
  const SolAccountInfo *token_program,
  const SolAccountInfo *account,
  const SolAccountInfo *mint,
  const SolAccountInfo *authority,
  uint64_t amount,
  uint8_t decimals
) {
  uint8_t data[TOKEN_CHECKED_IX_LEN] = {0};
  checked_token_ix_data(TOKEN_BURN_CHECKED, amount, decimals, data);
  SolAccountMeta metas[] = {
    {account->key, true, false},
    {mint->key, true, false},
    {authority->key, false, true},
  };
  SolInstruction instruction = {token_program->key, metas, SOL_ARRAY_SIZE(metas), data, sizeof(data)};
  SolAccountInfo infos[] = {*account, *mint, *authority, *token_program};
  return sol_invoke(&instruction, infos, SOL_ARRAY_SIZE(infos));
}

static uint64_t invoke_mint_to_checked(
  const SolAccountInfo *token_program,
  const SolAccountInfo *mint,
  const SolAccountInfo *account,
  const SolAccountInfo *mint_authority,
  uint64_t amount,
  uint8_t decimals,
  const uint8_t *destination_mint,
  uint8_t bump
) {
  uint8_t data[TOKEN_CHECKED_IX_LEN] = {0};
  checked_token_ix_data(TOKEN_MINT_TO_CHECKED, amount, decimals, data);
  SolAccountMeta metas[] = {
    {mint->key, true, false},
    {account->key, true, false},
    {mint_authority->key, false, true},
  };
  SolInstruction instruction = {token_program->key, metas, SOL_ARRAY_SIZE(metas), data, sizeof(data)};
  SolAccountInfo infos[] = {*mint, *account, *mint_authority, *token_program};
  SolSignerSeed seeds[] = {
    {PDA_SEED_PREFIX, sizeof(PDA_SEED_PREFIX) - 1},
    {MINT_AUTHORITY_SEED_KIND, sizeof(MINT_AUTHORITY_SEED_KIND) - 1},
    {destination_mint, 32},
    {&bump, 1},
  };
  SolSignerSeeds signers[] = {{seeds, SOL_ARRAY_SIZE(seeds)}};
  return sol_invoke_signed(&instruction, infos, SOL_ARRAY_SIZE(infos), signers, SOL_ARRAY_SIZE(signers));
}

static uint64_t execute_initialize_config(const SolPubkey *program_id, SolAccountInfo *accounts, const Instruction *ix) {
  SolSignerSeed pda_seeds[] = {
    {PDA_SEED_PREFIX, sizeof(PDA_SEED_PREFIX) - 1},
    {CONFIG_SEED_KIND, sizeof(CONFIG_SEED_KIND) - 1},
    {CONFIG_SEED_ACCOUNT, sizeof(CONFIG_SEED_ACCOUNT) - 1},
    {PDA_SEED_VERSION, sizeof(PDA_SEED_VERSION) - 1},
  };
  uint8_t bump = 0;
  uint64_t err = require_pda(program_id, &accounts[1], pda_seeds, SOL_ARRAY_SIZE(pda_seeds), ERR_CONFIG_PDA_MISMATCH, &bump);
  if (err) return err;
  if (bump != ix->config_bump) return custom(ERR_CONFIG_PDA_MISMATCH);
  SolSignerSeed signer_seeds[] = {
    pda_seeds[0], pda_seeds[1], pda_seeds[2], pda_seeds[3], {&bump, 1},
  };
  err = ensure_program_account(program_id, &accounts[0], &accounts[1], &accounts[4], CONFIG_LEN, signer_seeds, SOL_ARRAY_SIZE(signer_seeds));
  if (err) return err;
  err = require_uninitialized(&accounts[1]);
  if (err) return err;
  Config config = {0};
  copy32(config.admin, accounts[2].key->x);
  copy32(config.pause_authority, accounts[3].key->x);
  config.launch_status = STATUS_CANDIDATE;
  config.config_bump = ix->config_bump;
  return write_config_account(&accounts[1], &config);
}

static uint64_t execute_register_destination_mint(const SolPubkey *program_id, SolAccountInfo *accounts, const Instruction *ix) {
  Config config;
  uint64_t err = read_config_account(program_id, &accounts[1], &config);
  if (err) return err;
  err = require_authority(config.admin, &accounts[0]);
  if (err) return err;
  if (!any_nonzero32(ix->token_id_hash)) return custom(ERR_TOKEN_ID_HASH_ZERO);
  SolSignerSeed pda_seeds[] = {
    {PDA_SEED_PREFIX, sizeof(PDA_SEED_PREFIX) - 1},
    {DESTINATION_CONFIG_SEED_KIND, sizeof(DESTINATION_CONFIG_SEED_KIND) - 1},
    {accounts[3].key->x, 32},
  };
  uint8_t bump = 0;
  err = require_pda(program_id, &accounts[2], pda_seeds, SOL_ARRAY_SIZE(pda_seeds), ERR_DESTINATION_CONFIG_PDA_MISMATCH, &bump);
  if (err) return err;
  SolSignerSeed signer_seeds[] = {pda_seeds[0], pda_seeds[1], pda_seeds[2], {&bump, 1}};
  err = ensure_program_account(program_id, &accounts[0], &accounts[2], &accounts[6], DESTINATION_TOKEN_CONFIG_LEN, signer_seeds, SOL_ARRAY_SIZE(signer_seeds));
  if (err) return err;
  err = require_uninitialized(&accounts[2]);
  if (err) return err;
  uint8_t mint_authority_bump = 0;
  err = require_mint_authority_pda(program_id, accounts[3].key->x, &accounts[4], &mint_authority_bump);
  if (err) return err;
  err = require_destination_mint_setup(&accounts[3], &accounts[5], ix->decimals, accounts[4].key->x);
  if (err) return err;

  DestinationConfig destination = {0};
  copy32(destination.destination_mint, accounts[3].key->x);
  destination.mint_vanity_nonce = ix->mint_vanity_nonce;
  destination.mint_bump = ix->mint_bump;
  copy32(destination.token_id_hash, ix->token_id_hash);
  destination.decimals = ix->decimals;
  copy32(destination.token_program, accounts[5].key->x);
  copy32(destination.mint_authority, accounts[4].key->x);
  destination.max_supply = ix->max_supply;
  destination.bonding_min = ix->bonding_min;
  destination.bonding_range = ix->bonding_range;
  destination.min_dest_amount = ix->min_dest_amount;
  destination.status = STATUS_ENABLED;
  uint64_t next_count = 0;
  err = checked_add_u64(config.destination_count, 1, &next_count);
  if (err) return err;
  config.destination_count = (uint32_t)next_count;
  err = write_destination_account(&accounts[2], &destination);
  if (err) return err;
  return write_config_account(&accounts[1], &config);
}

static uint64_t execute_register_source_mint(const SolPubkey *program_id, SolAccountInfo *accounts, const Instruction *ix) {
  Config config;
  DestinationConfig destination;
  uint64_t err = read_config_account(program_id, &accounts[1], &config);
  if (err) return err;
  err = require_authority(config.admin, &accounts[0]);
  if (err) return err;
  err = read_destination_account(program_id, &accounts[2], &destination);
  if (err) return err;
  err = require_account_address(destination.destination_mint, &accounts[5]);
  if (err) return err;
  SolSignerSeed pda_seeds[] = {
    {PDA_SEED_PREFIX, sizeof(PDA_SEED_PREFIX) - 1},
    {SOURCE_CONFIG_SEED_KIND, sizeof(SOURCE_CONFIG_SEED_KIND) - 1},
    {accounts[4].key->x, 32},
    {destination.destination_mint, 32},
  };
  uint8_t bump = 0;
  err = require_pda(program_id, &accounts[3], pda_seeds, SOL_ARRAY_SIZE(pda_seeds), ERR_SOURCE_CONFIG_PDA_MISMATCH, &bump);
  if (err) return err;
  if (bump != ix->bump) return custom(ERR_SOURCE_CONFIG_PDA_MISMATCH);
  SolSignerSeed signer_seeds[] = {pda_seeds[0], pda_seeds[1], pda_seeds[2], pda_seeds[3], {&bump, 1}};
  err = ensure_program_account(program_id, &accounts[0], &accounts[3], &accounts[7], SOURCE_MINT_CONFIG_LEN, signer_seeds, SOL_ARRAY_SIZE(signer_seeds));
  if (err) return err;
  err = require_uninitialized(&accounts[3]);
  if (err) return err;

  SourceConfig source = {0};
  copy32(source.source_mint, accounts[4].key->x);
  copy32(source.destination_mint, destination.destination_mint);
  copy32(source.source_token_program, accounts[6].key->x);
  source.source_decimals = ix->source_decimals;
  source.migration_mode = ix->migration_mode;
  source.price_mode = ix->price_mode;
  source.fixed_ratio_source_amount = ix->fixed_ratio_source_amount;
  source.fixed_ratio_destination_amount = ix->fixed_ratio_destination_amount;
  source.enabled = false;
  source.bump = ix->bump;
  uint64_t next_count = 0;
  err = checked_add_u64(config.source_count, 1, &next_count);
  if (err) return err;
  config.source_count = (uint32_t)next_count;
  err = write_source_account(&accounts[3], &source);
  if (err) return err;
  return write_config_account(&accounts[1], &config);
}

static uint64_t execute_set_source_enabled(const SolPubkey *program_id, SolAccountInfo *accounts, bool enabled) {
  Config config;
  DestinationConfig destination;
  SourceConfig source;
  uint64_t err = read_config_account(program_id, &accounts[1], &config);
  if (err) return err;
  err = require_authority(config.admin, &accounts[0]);
  if (err) return err;
  err = read_destination_account(program_id, &accounts[3], &destination);
  if (err) return err;
  err = read_source_account(program_id, &accounts[2], &source);
  if (err) return err;
  err = require_relationship(eq32(source.destination_mint, destination.destination_mint), ERR_RELATIONSHIP_MISMATCH);
  if (err) return err;
  if (enabled && source.finalized) return custom(ERR_SOURCE_FINALIZED);
  source.enabled = enabled;
  return write_source_account(&accounts[2], &source);
}

static uint64_t execute_pause(const SolPubkey *program_id, SolAccountInfo *accounts, bool paused) {
  Config config;
  uint64_t err = read_config_account(program_id, &accounts[1], &config);
  if (err) return err;
  err = require_authority(config.pause_authority, &accounts[0]);
  if (err) return err;
  config.launch_status = paused ? STATUS_PAUSED : STATUS_ENABLED;
  err = write_config_account(&accounts[1], &config);
  if (err) return err;
  if (accounts[2].data_len == CONFIG_LEN) {
    if (!SolPubkey_same(accounts[2].key, accounts[1].key)) {
      Config target;
      err = read_config_account(program_id, &accounts[2], &target);
      if (err) return err;
      target.launch_status = config.launch_status;
      return write_config_account(&accounts[2], &target);
    }
    return SUCCESS;
  }
  if (accounts[2].data_len == DESTINATION_TOKEN_CONFIG_LEN) {
    DestinationConfig destination;
    err = read_destination_account(program_id, &accounts[2], &destination);
    if (err) return err;
    destination.status = paused ? STATUS_PAUSED : STATUS_ENABLED;
    return write_destination_account(&accounts[2], &destination);
  }
  if (accounts[2].data_len == SOURCE_MINT_CONFIG_LEN) {
    if (paused) {
      SourceConfig source;
      err = read_source_account(program_id, &accounts[2], &source);
      if (err) return err;
      source.enabled = false;
      return write_source_account(&accounts[2], &source);
    }
    return SUCCESS;
  }
  return custom(ERR_ACCOUNT_DATA_LEN_MISMATCH);
}

static uint64_t execute_finalize_source_mint(const SolPubkey *program_id, SolAccountInfo *accounts) {
  Config config;
  DestinationConfig destination;
  SourceConfig source;
  uint64_t err = read_config_account(program_id, &accounts[1], &config);
  if (err) return err;
  err = require_authority(config.admin, &accounts[0]);
  if (err) return err;
  err = read_destination_account(program_id, &accounts[3], &destination);
  if (err) return err;
  err = read_source_account(program_id, &accounts[2], &source);
  if (err) return err;
  err = require_relationship(eq32(source.destination_mint, destination.destination_mint), ERR_RELATIONSHIP_MISMATCH);
  if (err) return err;
  source.enabled = false;
  source.finalized = true;
  return write_source_account(&accounts[2], &source);
}

static uint64_t execute_migrate(const SolPubkey *program_id, SolAccountInfo *accounts, const Instruction *ix) {
  if (ix->create_receipt) return custom(ERR_EXECUTOR_NOT_IMPLEMENTED);
  Config config;
  SourceConfig source;
  DestinationConfig destination;
  uint64_t err = read_config_account(program_id, &accounts[1], &config);
  if (err) return err;
  if (config.launch_status == STATUS_PAUSED || config.launch_status == STATUS_FINALIZED) {
    return custom(ERR_PROGRAM_PAUSED);
  }
  err = read_source_account(program_id, &accounts[2], &source);
  if (err) return err;
  err = read_destination_account(program_id, &accounts[3], &destination);
  if (err) return err;
  err = require_relationship(eq32(source.destination_mint, destination.destination_mint), ERR_RELATIONSHIP_MISMATCH);
  if (err) return err;
  err = require_account_address(source.source_mint, &accounts[6]);
  if (err) return err;
  err = require_account_address(destination.destination_mint, &accounts[7]);
  if (err) return err;
  err = require_account_address(destination.mint_authority, &accounts[8]);
  if (err) return err;
  if (!eq32(source.source_token_program, accounts[9].key->x)) return custom(ERR_TOKEN_PROGRAM_MISMATCH);
  if (!eq32(destination.token_program, accounts[10].key->x)) return custom(ERR_TOKEN_PROGRAM_MISMATCH);
  err = require_owned_by(&accounts[4], &accounts[9]);
  if (err) return err;
  err = require_owned_by(&accounts[5], &accounts[10]);
  if (err) return err;
  err = require_owned_by(&accounts[6], &accounts[9]);
  if (err) return err;
  err = require_owned_by(&accounts[7], &accounts[10]);
  if (err) return err;
  err = require_token_account_matches(&accounts[4], source.source_mint, accounts[0].key->x);
  if (err) return err;
  err = require_token_account_matches(&accounts[5], destination.destination_mint, accounts[0].key->x);
  if (err) return err;
  err = require_mint_decimals(&accounts[6], source.source_decimals);
  if (err) return err;
  err = require_mint_decimals(&accounts[7], destination.decimals);
  if (err) return err;
  uint8_t mint_authority_bump = 0;
  err = require_mint_authority_pda(program_id, destination.destination_mint, &accounts[8], &mint_authority_bump);
  if (err) return err;
  MigrationQuote quote;
  err = quote_migration(&destination, &source, ix->desired_destination_amount, ix->max_source_amount, &quote);
  if (err) return err;
  err = invoke_burn_checked(&accounts[9], &accounts[4], &accounts[6], &accounts[0], quote.source_amount_to_burn, source.source_decimals);
  if (err) return err;
  err = invoke_mint_to_checked(&accounts[10], &accounts[7], &accounts[5], &accounts[8], quote.destination_amount_to_mint, destination.decimals, destination.destination_mint, mint_authority_bump);
  if (err) return err;
  err = apply_migration_counters(&destination, &source, &quote);
  if (err) return err;
  err = write_destination_account(&accounts[3], &destination);
  if (err) return err;
  return write_source_account(&accounts[2], &source);
}

static uint64_t execute_transfer_authority_begin(const SolPubkey *program_id, SolAccountInfo *accounts, const uint8_t *pending_authority) {
  Config config;
  uint64_t err = read_config_account(program_id, &accounts[1], &config);
  if (err) return err;
  err = require_authority(config.admin, &accounts[0]);
  if (err) return err;
  if (!eq32(pending_authority, accounts[2].key->x)) return custom(ERR_PENDING_AUTHORITY_MISMATCH);
  copy32(config.pending_authority, pending_authority);
  config.pending_authority_set = true;
  return write_config_account(&accounts[1], &config);
}

static uint64_t execute_transfer_authority_accept(const SolPubkey *program_id, SolAccountInfo *accounts) {
  Config config;
  uint64_t err = read_config_account(program_id, &accounts[1], &config);
  if (err) return err;
  err = require_authority(config.admin, &accounts[2]);
  if (err) return err;
  if (!config.pending_authority_set) return custom(ERR_PENDING_AUTHORITY_NOT_SET);
  if (!eq32(config.pending_authority, accounts[0].key->x)) return custom(ERR_PENDING_AUTHORITY_MISMATCH);
  copy32(config.admin, config.pending_authority);
  for (uint64_t i = 0; i < 32; i++) config.pending_authority[i] = 0;
  config.pending_authority_set = false;
  return write_config_account(&accounts[1], &config);
}

static uint64_t execute_retire_authority(const SolPubkey *program_id, SolAccountInfo *accounts) {
  Config config;
  uint64_t err = read_config_account(program_id, &accounts[1], &config);
  if (err) return err;
  err = require_authority(config.admin, &accounts[0]);
  if (err) return err;
  err = require_authority(config.pause_authority, &accounts[2]);
  if (err) return err;
  copy32(config.admin, SYSTEM_PROGRAM_ID_BYTES);
  copy32(config.pause_authority, SYSTEM_PROGRAM_ID_BYTES);
  copy32(config.pending_authority, SYSTEM_PROGRAM_ID_BYTES);
  config.pending_authority_set = false;
  return write_config_account(&accounts[1], &config);
}

static uint64_t execute_instruction(const SolPubkey *program_id, SolAccountInfo *accounts, const Instruction *ix) {
  switch (ix->kind) {
    case IX_INITIALIZE_CONFIG:
      return execute_initialize_config(program_id, accounts, ix);
    case IX_REGISTER_DESTINATION_MINT:
      return execute_register_destination_mint(program_id, accounts, ix);
    case IX_REGISTER_SOURCE_MINT:
      return execute_register_source_mint(program_id, accounts, ix);
    case IX_SET_SOURCE_ENABLED:
      return execute_set_source_enabled(program_id, accounts, ix->enabled);
    case IX_MIGRATE:
      return execute_migrate(program_id, accounts, ix);
    case IX_PAUSE:
      return execute_pause(program_id, accounts, ix->paused);
    case IX_FINALIZE_SOURCE_MINT:
      return execute_finalize_source_mint(program_id, accounts);
    case IX_TRANSFER_AUTHORITY_BEGIN:
      return execute_transfer_authority_begin(program_id, accounts, ix->pending_authority);
    case IX_TRANSFER_AUTHORITY_ACCEPT:
      return execute_transfer_authority_accept(program_id, accounts);
    case IX_RETIRE_AUTHORITY:
      return execute_retire_authority(program_id, accounts);
    default:
      return ERROR_INVALID_INSTRUCTION_DATA;
  }
}

uint64_t entrypoint(const uint8_t *input) {
  SolAccountInfo accounts[MAX_ACCOUNTS];
  SolParameters params = {0};
  params.ka = accounts;
  if (!sol_deserialize(input, &params, MAX_ACCOUNTS)) {
    return ERROR_INVALID_ARGUMENT;
  }
  Instruction ix = {0};
  uint64_t err = unpack_instruction(params.data, params.data_len, &ix);
  if (err) return err;
  err = validate_accounts(&params, &ix);
  if (err) return err;
  return execute_instruction(params.program_id, params.ka, &ix);
}
