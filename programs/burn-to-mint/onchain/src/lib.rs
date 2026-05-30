#![cfg_attr(target_os = "solana", no_std)]
#![deny(unsafe_op_in_unsafe_fn)]

use core::marker::PhantomData;

#[cfg(any(target_os = "solana", target_arch = "bpf"))]
use pinocchio::sysvars::{rent::Rent, Sysvar};
use pinocchio::{account::AccountView, address::Address, error::ProgramError, ProgramResult};
use rati_burn_to_mint_core::{
    apply_migration_counters, quote_migration, Config, DestinationTokenConfig, Instruction,
    LaunchStatus, ProgramError as CoreError, SourceMintConfig, CONFIG_LEN,
    DESTINATION_TOKEN_CONFIG_LEN, SOURCE_MINT_CONFIG_LEN,
};
#[cfg(feature = "receipts")]
use rati_burn_to_mint_core::{Receipt, RECEIPT_LEN};

pub const MAX_ACCOUNTS: usize = 13;
pub const TOKEN_BURN_CHECKED_DISCRIMINATOR: u8 = 15;
pub const TOKEN_MINT_TO_CHECKED_DISCRIMINATOR: u8 = 14;
pub const TOKEN_CHECKED_IX_LEN: usize = 10;
pub const SPL_MINT_BASE_LEN: usize = 82;
pub const SPL_MINT_AUTHORITY_OPTION_OFFSET: usize = 0;
pub const SPL_MINT_AUTHORITY_OFFSET: usize = 4;
pub const SPL_MINT_DECIMALS_OFFSET: usize = 44;
pub const SPL_MINT_IS_INITIALIZED_OFFSET: usize = 45;
pub const SPL_MINT_FREEZE_AUTHORITY_OPTION_OFFSET: usize = 46;
pub const SPL_TOKEN_ACCOUNT_BASE_LEN: usize = 165;
pub const SPL_TOKEN_ACCOUNT_MINT_OFFSET: usize = 0;
pub const SPL_TOKEN_ACCOUNT_OWNER_OFFSET: usize = 32;
pub const SYSTEM_CREATE_ACCOUNT_DISCRIMINATOR: u32 = 0;
pub const SYSTEM_CREATE_ACCOUNT_IX_LEN: usize = 52;
pub const SYSTEM_PROGRAM_ID_BYTES: [u8; 32] = [0; 32];
pub const PDA_SEED_PREFIX: &[u8] = b"rati";
pub const CONFIG_SEED_KIND: &[u8] = b"burn-to-mint";
pub const CONFIG_SEED_ACCOUNT: &[u8] = b"config";
pub const PDA_SEED_VERSION: &[u8] = b"v1";
pub const DESTINATION_CONFIG_SEED_KIND: &[u8] = b"destination";
pub const SOURCE_CONFIG_SEED_KIND: &[u8] = b"source";
pub const MINT_AUTHORITY_SEED_PREFIX: &[u8] = b"rati";
pub const MINT_AUTHORITY_SEED_KIND: &[u8] = b"mint-authority";
#[cfg(feature = "receipts")]
pub const RECEIPT_SEED_KIND: &[u8] = b"receipt";

pub const INITIALIZE_CONFIG_ACCOUNTS: usize = 5;
pub const REGISTER_DESTINATION_MINT_ACCOUNTS: usize = 7;
pub const REGISTER_SOURCE_MINT_ACCOUNTS: usize = 8;
pub const SET_SOURCE_ENABLED_ACCOUNTS: usize = 4;
pub const MIGRATE_BASE_ACCOUNTS: usize = 11;
pub const MIGRATE_WITH_RECEIPT_ACCOUNTS: usize = 13;
pub const PAUSE_ACCOUNTS: usize = 3;
pub const FINALIZE_SOURCE_MINT_ACCOUNTS: usize = 4;
pub const TRANSFER_AUTHORITY_BEGIN_ACCOUNTS: usize = 3;
pub const TRANSFER_AUTHORITY_ACCEPT_ACCOUNTS: usize = 3;
pub const RETIRE_AUTHORITY_ACCOUNTS: usize = 3;

pub const RATI_ERROR_BASE: u32 = 0x5241_5400;
pub const ERR_EXECUTOR_NOT_IMPLEMENTED: u32 = RATI_ERROR_BASE + 1;
pub const ERR_ACCOUNT_COUNT_MISMATCH: u32 = RATI_ERROR_BASE + 2;
pub const ERR_PROTOCOL_FEE_FORBIDDEN: u32 = RATI_ERROR_BASE + 3;
pub const ERR_MISSING_REQUIRED_SIGNER: u32 = RATI_ERROR_BASE + 4;
pub const ERR_MISSING_WRITABLE_ACCOUNT: u32 = RATI_ERROR_BASE + 5;
pub const ERR_ACCOUNT_DATA_LEN_MISMATCH: u32 = RATI_ERROR_BASE + 6;
pub const ERR_ACCOUNT_NOT_PROGRAM_OWNED: u32 = RATI_ERROR_BASE + 7;
pub const ERR_AUTHORITY_MISMATCH: u32 = RATI_ERROR_BASE + 8;
pub const ERR_ACCOUNT_ADDRESS_MISMATCH: u32 = RATI_ERROR_BASE + 9;
pub const ERR_RELATIONSHIP_MISMATCH: u32 = RATI_ERROR_BASE + 10;
pub const ERR_PENDING_AUTHORITY_NOT_SET: u32 = RATI_ERROR_BASE + 11;
pub const ERR_PENDING_AUTHORITY_MISMATCH: u32 = RATI_ERROR_BASE + 12;
pub const ERR_TOKEN_ACCOUNT_DATA_TOO_SHORT: u32 = RATI_ERROR_BASE + 13;
pub const ERR_TOKEN_ACCOUNT_MINT_MISMATCH: u32 = RATI_ERROR_BASE + 14;
pub const ERR_TOKEN_ACCOUNT_OWNER_MISMATCH: u32 = RATI_ERROR_BASE + 15;
pub const ERR_TOKEN_PROGRAM_MISMATCH: u32 = RATI_ERROR_BASE + 16;
pub const ERR_MINT_DECIMALS_MISMATCH: u32 = RATI_ERROR_BASE + 17;
pub const ERR_MINT_AUTHORITY_PDA_MISMATCH: u32 = RATI_ERROR_BASE + 18;
pub const ERR_SYSTEM_PROGRAM_MISMATCH: u32 = RATI_ERROR_BASE + 19;
pub const ERR_CONFIG_PDA_MISMATCH: u32 = RATI_ERROR_BASE + 20;
pub const ERR_DESTINATION_CONFIG_PDA_MISMATCH: u32 = RATI_ERROR_BASE + 21;
pub const ERR_SOURCE_CONFIG_PDA_MISMATCH: u32 = RATI_ERROR_BASE + 22;
pub const ERR_PDA_SEED_SHAPE: u32 = RATI_ERROR_BASE + 23;
pub const ERR_ACCOUNT_ALREADY_INITIALIZED: u32 = RATI_ERROR_BASE + 24;
pub const ERR_PROGRAM_PAUSED: u32 = RATI_ERROR_BASE + 25;
pub const ERR_SOURCE_FINALIZED: u32 = RATI_ERROR_BASE + 26;
pub const ERR_MINT_AUTHORITY_MISMATCH: u32 = RATI_ERROR_BASE + 27;
pub const ERR_FREEZE_AUTHORITY_PRESENT: u32 = RATI_ERROR_BASE + 28;
pub const ERR_MINT_NOT_INITIALIZED: u32 = RATI_ERROR_BASE + 29;
pub const ERR_TOKEN_ID_HASH_ZERO: u32 = RATI_ERROR_BASE + 30;
pub const ERR_RECEIPT_ACCOUNT_MISMATCH: u32 = RATI_ERROR_BASE + 31;
pub const ERR_CORE_BASE: u32 = RATI_ERROR_BASE + 0x100;

#[cfg(feature = "bpf-entrypoint")]
pinocchio::program_entrypoint!(process_instruction, MAX_ACCOUNTS);

#[cfg(feature = "bpf-entrypoint")]
pinocchio::no_allocator!();

#[cfg(feature = "bpf-entrypoint")]
pinocchio::nostd_panic_handler!();

#[cfg(any(target_os = "solana", target_arch = "bpf"))]
#[no_mangle]
pub unsafe extern "C" fn sol_memcpy_(dst: *mut u8, src: *const u8, n: u64) {
    // LLVM lowers some slice copies to this symbol; route it through the static syscall table.
    unsafe { pinocchio::syscalls::sol_memcpy_(dst, src, n) }
}

#[cfg(any(target_os = "solana", target_arch = "bpf"))]
#[no_mangle]
pub unsafe extern "C" fn sol_memcmp_(s1: *const u8, s2: *const u8, n: u64, result: *mut i32) {
    // LLVM lowers some byte comparisons to this symbol; route it through the static syscall table.
    unsafe { pinocchio::syscalls::sol_memcmp_(s1, s2, n, result) }
}

#[cfg(any(target_os = "solana", target_arch = "bpf"))]
#[no_mangle]
pub unsafe extern "C" fn sol_memset_(s: *mut u8, c: u8, n: u64) {
    // LLVM lowers some zeroing operations to this symbol; route it through the static syscall table.
    unsafe { pinocchio::syscalls::sol_memset_(s, c, n) }
}

pub fn process_instruction(
    program_id: &Address,
    accounts: &[AccountView],
    instruction_data: &[u8],
) -> ProgramResult {
    let instruction = Instruction::unpack(instruction_data).map_err(map_core_error)?;
    let rules = expected_account_rules(&instruction);
    validate_account_rules(accounts, rules)?;

    execute_instruction(program_id, accounts, instruction)
}

pub fn expected_account_count(instruction: &Instruction) -> usize {
    match instruction {
        Instruction::InitializeConfig { .. } => INITIALIZE_CONFIG_ACCOUNTS,
        Instruction::RegisterDestinationMint { .. } => REGISTER_DESTINATION_MINT_ACCOUNTS,
        Instruction::RegisterSourceMint { .. } => REGISTER_SOURCE_MINT_ACCOUNTS,
        Instruction::SetSourceEnabled { .. } => SET_SOURCE_ENABLED_ACCOUNTS,
        Instruction::Migrate { create_receipt, .. } => {
            if *create_receipt {
                MIGRATE_WITH_RECEIPT_ACCOUNTS
            } else {
                MIGRATE_BASE_ACCOUNTS
            }
        }
        Instruction::Pause { .. } => PAUSE_ACCOUNTS,
        Instruction::FinalizeSourceMint => FINALIZE_SOURCE_MINT_ACCOUNTS,
        Instruction::TransferAuthorityBegin { .. } => TRANSFER_AUTHORITY_BEGIN_ACCOUNTS,
        Instruction::TransferAuthorityAccept => TRANSFER_AUTHORITY_ACCEPT_ACCOUNTS,
        Instruction::RetireAuthority => RETIRE_AUTHORITY_ACCOUNTS,
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum AccountRole {
    Payer,
    Config,
    AdminAuthority,
    PauseAuthority,
    SystemProgram,
    DestinationTokenConfig,
    DestinationMint,
    MintAuthority,
    DestinationTokenProgram,
    SourceMintConfig,
    SourceMint,
    SourceTokenProgram,
    User,
    UserSourceTokenAccount,
    UserDestinationTokenAccount,
    Receipt,
    AnyConfig,
    PendingAuthority,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct AccountRule {
    pub role: AccountRole,
    pub signer: bool,
    pub writable: bool,
}

const fn rule(role: AccountRole, signer: bool, writable: bool) -> AccountRule {
    AccountRule {
        role,
        signer,
        writable,
    }
}

pub const INITIALIZE_CONFIG_RULES: &[AccountRule] = &[
    rule(AccountRole::Payer, true, true),
    rule(AccountRole::Config, false, true),
    rule(AccountRole::AdminAuthority, false, false),
    rule(AccountRole::PauseAuthority, false, false),
    rule(AccountRole::SystemProgram, false, false),
];

pub const REGISTER_DESTINATION_MINT_RULES: &[AccountRule] = &[
    rule(AccountRole::AdminAuthority, true, true),
    rule(AccountRole::Config, false, true),
    rule(AccountRole::DestinationTokenConfig, false, true),
    rule(AccountRole::DestinationMint, false, true),
    rule(AccountRole::MintAuthority, false, false),
    rule(AccountRole::DestinationTokenProgram, false, false),
    rule(AccountRole::SystemProgram, false, false),
];

pub const REGISTER_SOURCE_MINT_RULES: &[AccountRule] = &[
    rule(AccountRole::AdminAuthority, true, true),
    rule(AccountRole::Config, false, true),
    rule(AccountRole::DestinationTokenConfig, false, false),
    rule(AccountRole::SourceMintConfig, false, true),
    rule(AccountRole::SourceMint, false, false),
    rule(AccountRole::DestinationMint, false, false),
    rule(AccountRole::SourceTokenProgram, false, false),
    rule(AccountRole::SystemProgram, false, false),
];

pub const SET_SOURCE_ENABLED_RULES: &[AccountRule] = &[
    rule(AccountRole::AdminAuthority, true, false),
    rule(AccountRole::Config, false, false),
    rule(AccountRole::SourceMintConfig, false, true),
    rule(AccountRole::DestinationTokenConfig, false, false),
];

pub const MIGRATE_BASE_RULES: &[AccountRule] = &[
    rule(AccountRole::User, true, false),
    rule(AccountRole::Config, false, false),
    rule(AccountRole::SourceMintConfig, false, true),
    rule(AccountRole::DestinationTokenConfig, false, true),
    rule(AccountRole::UserSourceTokenAccount, false, true),
    rule(AccountRole::UserDestinationTokenAccount, false, true),
    rule(AccountRole::SourceMint, false, true),
    rule(AccountRole::DestinationMint, false, true),
    rule(AccountRole::MintAuthority, false, false),
    rule(AccountRole::SourceTokenProgram, false, false),
    rule(AccountRole::DestinationTokenProgram, false, false),
];

pub const MIGRATE_WITH_RECEIPT_RULES: &[AccountRule] = &[
    rule(AccountRole::User, true, false),
    rule(AccountRole::Config, false, false),
    rule(AccountRole::SourceMintConfig, false, true),
    rule(AccountRole::DestinationTokenConfig, false, true),
    rule(AccountRole::UserSourceTokenAccount, false, true),
    rule(AccountRole::UserDestinationTokenAccount, false, true),
    rule(AccountRole::SourceMint, false, true),
    rule(AccountRole::DestinationMint, false, true),
    rule(AccountRole::MintAuthority, false, false),
    rule(AccountRole::SourceTokenProgram, false, false),
    rule(AccountRole::DestinationTokenProgram, false, false),
    rule(AccountRole::Receipt, false, true),
    rule(AccountRole::SystemProgram, false, false),
];

pub const PAUSE_RULES: &[AccountRule] = &[
    rule(AccountRole::PauseAuthority, true, false),
    rule(AccountRole::Config, false, true),
    rule(AccountRole::AnyConfig, false, true),
];

pub const FINALIZE_SOURCE_MINT_RULES: &[AccountRule] = &[
    rule(AccountRole::AdminAuthority, true, false),
    rule(AccountRole::Config, false, false),
    rule(AccountRole::SourceMintConfig, false, true),
    rule(AccountRole::DestinationTokenConfig, false, false),
];

pub const TRANSFER_AUTHORITY_BEGIN_RULES: &[AccountRule] = &[
    rule(AccountRole::AdminAuthority, true, false),
    rule(AccountRole::Config, false, true),
    rule(AccountRole::PendingAuthority, false, false),
];

pub const TRANSFER_AUTHORITY_ACCEPT_RULES: &[AccountRule] = &[
    rule(AccountRole::PendingAuthority, true, false),
    rule(AccountRole::Config, false, true),
    rule(AccountRole::AdminAuthority, false, false),
];

pub const RETIRE_AUTHORITY_RULES: &[AccountRule] = &[
    rule(AccountRole::AdminAuthority, true, false),
    rule(AccountRole::Config, false, true),
    rule(AccountRole::PauseAuthority, true, false),
];

pub fn expected_account_rules(instruction: &Instruction) -> &'static [AccountRule] {
    match instruction {
        Instruction::InitializeConfig { .. } => INITIALIZE_CONFIG_RULES,
        Instruction::RegisterDestinationMint { .. } => REGISTER_DESTINATION_MINT_RULES,
        Instruction::RegisterSourceMint { .. } => REGISTER_SOURCE_MINT_RULES,
        Instruction::SetSourceEnabled { .. } => SET_SOURCE_ENABLED_RULES,
        Instruction::Migrate { create_receipt, .. } => {
            if *create_receipt {
                MIGRATE_WITH_RECEIPT_RULES
            } else {
                MIGRATE_BASE_RULES
            }
        }
        Instruction::Pause { .. } => PAUSE_RULES,
        Instruction::FinalizeSourceMint => FINALIZE_SOURCE_MINT_RULES,
        Instruction::TransferAuthorityBegin { .. } => TRANSFER_AUTHORITY_BEGIN_RULES,
        Instruction::TransferAuthorityAccept => TRANSFER_AUTHORITY_ACCEPT_RULES,
        Instruction::RetireAuthority => RETIRE_AUTHORITY_RULES,
    }
}

fn require_account_count(actual: usize, expected: usize) -> ProgramResult {
    if actual < expected {
        Err(ProgramError::Custom(ERR_ACCOUNT_COUNT_MISMATCH))
    } else if actual > expected {
        Err(ProgramError::Custom(ERR_PROTOCOL_FEE_FORBIDDEN))
    } else {
        Ok(())
    }
}

fn validate_account_rules(accounts: &[AccountView], rules: &[AccountRule]) -> ProgramResult {
    require_account_count(accounts.len(), rules.len())?;

    let mut index = 0;
    while index < rules.len() {
        let account = &accounts[index];
        let rule = rules[index];
        if rule.signer && !account.is_signer() {
            return Err(ProgramError::Custom(ERR_MISSING_REQUIRED_SIGNER));
        }
        if rule.writable && !account.is_writable() {
            return Err(ProgramError::Custom(ERR_MISSING_WRITABLE_ACCOUNT));
        }
        index += 1;
    }

    Ok(())
}

fn execute_instruction(
    program_id: &Address,
    accounts: &[AccountView],
    instruction: Instruction,
) -> ProgramResult {
    match instruction {
        Instruction::InitializeConfig { config_bump } => {
            execute_initialize_config(program_id, accounts, config_bump)
        }
        Instruction::RegisterDestinationMint {
            token_id_hash,
            mint_vanity_nonce,
            mint_bump,
            decimals,
            max_supply,
            bonding_min,
            bonding_range,
            min_dest_amount,
        } => execute_register_destination_mint(
            program_id,
            accounts,
            token_id_hash,
            mint_vanity_nonce,
            mint_bump,
            decimals,
            max_supply,
            bonding_min,
            bonding_range,
            min_dest_amount,
        ),
        Instruction::RegisterSourceMint {
            source_decimals,
            migration_mode,
            price_mode,
            fixed_ratio_source_amount,
            fixed_ratio_destination_amount,
            bump,
        } => execute_register_source_mint(
            program_id,
            accounts,
            source_decimals,
            migration_mode,
            price_mode,
            fixed_ratio_source_amount,
            fixed_ratio_destination_amount,
            bump,
        ),
        Instruction::SetSourceEnabled { enabled } => {
            execute_set_source_enabled(program_id, accounts, enabled)
        }
        Instruction::Pause { paused } => execute_pause(program_id, accounts, paused),
        Instruction::FinalizeSourceMint => execute_finalize_source_mint(program_id, accounts),
        Instruction::Migrate {
            desired_destination_amount,
            max_source_amount,
            user_nonce,
            create_receipt,
            ..
        } => execute_migrate(
            program_id,
            accounts,
            desired_destination_amount,
            max_source_amount,
            user_nonce,
            create_receipt,
        ),
        Instruction::TransferAuthorityBegin { pending_authority } => {
            execute_transfer_authority_begin(program_id, accounts, pending_authority)
        }
        Instruction::TransferAuthorityAccept => {
            execute_transfer_authority_accept(program_id, accounts)
        }
        Instruction::RetireAuthority => execute_retire_authority(program_id, accounts),
    }
}

fn execute_initialize_config(
    program_id: &Address,
    accounts: &[AccountView],
    config_bump: u8,
) -> ProgramResult {
    let config_account = &accounts[1];
    let seeds = [
        PDA_SEED_PREFIX,
        CONFIG_SEED_KIND,
        CONFIG_SEED_ACCOUNT,
        PDA_SEED_VERSION,
    ];
    let derived_bump = require_pda(program_id, config_account, &seeds, ERR_CONFIG_PDA_MISMATCH)?;
    require_expected_bump(derived_bump, config_bump, ERR_CONFIG_PDA_MISMATCH)?;
    ensure_program_account(
        program_id,
        &accounts[0],
        config_account,
        &accounts[4],
        CONFIG_LEN,
        &seeds,
        derived_bump,
    )?;
    require_uninitialized_account(config_account)?;

    let config = Config {
        admin: address_bytes(&accounts[2]),
        pause_authority: address_bytes(&accounts[3]),
        launch_status: LaunchStatus::Candidate,
        destination_count: 0,
        source_count: 0,
        config_bump,
        pending_authority: [0; 32],
        pending_authority_set: false,
        reserved: [0; 8],
    };
    write_config(config_account, &config)
}

#[allow(clippy::too_many_arguments)]
fn execute_register_destination_mint(
    program_id: &Address,
    accounts: &[AccountView],
    token_id_hash: [u8; 32],
    mint_vanity_nonce: u64,
    mint_bump: u8,
    decimals: u8,
    max_supply: u64,
    bonding_min: u64,
    bonding_range: u64,
    min_dest_amount: u64,
) -> ProgramResult {
    let mut config = read_config(program_id, &accounts[1])?;
    require_authority(&config.admin, &accounts[0])?;
    require_nonzero_bytes(&token_id_hash, ERR_TOKEN_ID_HASH_ZERO)?;
    let destination_seeds = [
        PDA_SEED_PREFIX,
        DESTINATION_CONFIG_SEED_KIND,
        accounts[3].address().as_array().as_slice(),
    ];
    let destination_bump = require_pda(
        program_id,
        &accounts[2],
        &destination_seeds,
        ERR_DESTINATION_CONFIG_PDA_MISMATCH,
    )?;
    ensure_program_account(
        program_id,
        &accounts[0],
        &accounts[2],
        &accounts[6],
        DESTINATION_TOKEN_CONFIG_LEN,
        &destination_seeds,
        destination_bump,
    )?;
    require_uninitialized_account(&accounts[2])?;
    require_mint_authority_pda(program_id, &address_bytes(&accounts[3]), &accounts[4])?;
    require_destination_mint_setup(
        &accounts[3],
        &accounts[5],
        decimals,
        &address_bytes(&accounts[4]),
    )?;

    let destination = DestinationTokenConfig {
        destination_mint: address_bytes(&accounts[3]),
        mint_vanity_nonce,
        mint_bump,
        token_id_hash,
        decimals,
        token_program: address_bytes(&accounts[5]),
        mint_authority: address_bytes(&accounts[4]),
        max_supply,
        total_minted: 0,
        bonding_min,
        bonding_range,
        min_dest_amount,
        status: LaunchStatus::Enabled,
        reserved: [0; 8],
    };
    config.destination_count = config
        .destination_count
        .checked_add(1)
        .ok_or(map_core_error(CoreError::ArithmeticOverflow))?;

    write_destination(&accounts[2], &destination)?;
    write_config(&accounts[1], &config)
}

#[allow(clippy::too_many_arguments)]
fn execute_register_source_mint(
    program_id: &Address,
    accounts: &[AccountView],
    source_decimals: u8,
    migration_mode: rati_burn_to_mint_core::MigrationMode,
    price_mode: rati_burn_to_mint_core::PriceMode,
    fixed_ratio_source_amount: u64,
    fixed_ratio_destination_amount: u64,
    bump: u8,
) -> ProgramResult {
    let mut config = read_config(program_id, &accounts[1])?;
    require_authority(&config.admin, &accounts[0])?;
    let destination = read_destination(program_id, &accounts[2])?;
    require_account_address(&destination.destination_mint, &accounts[5])?;
    let source_seeds = [
        PDA_SEED_PREFIX,
        SOURCE_CONFIG_SEED_KIND,
        accounts[4].address().as_array().as_slice(),
        destination.destination_mint.as_slice(),
    ];
    let source_config_bump = require_pda(
        program_id,
        &accounts[3],
        &source_seeds,
        ERR_SOURCE_CONFIG_PDA_MISMATCH,
    )?;
    require_expected_bump(source_config_bump, bump, ERR_SOURCE_CONFIG_PDA_MISMATCH)?;
    ensure_program_account(
        program_id,
        &accounts[0],
        &accounts[3],
        &accounts[7],
        SOURCE_MINT_CONFIG_LEN,
        &source_seeds,
        source_config_bump,
    )?;
    require_uninitialized_account(&accounts[3])?;

    let source = SourceMintConfig {
        source_mint: address_bytes(&accounts[4]),
        destination_mint: destination.destination_mint,
        source_token_program: address_bytes(&accounts[6]),
        source_decimals,
        migration_mode,
        price_mode,
        fixed_ratio_source_amount,
        fixed_ratio_destination_amount,
        enabled: false,
        burned_base_units: 0,
        minted_destination_base_units: 0,
        migration_count: 0,
        bump,
        finalized: false,
        reserved: [0; 7],
    };
    config.source_count = config
        .source_count
        .checked_add(1)
        .ok_or(map_core_error(CoreError::ArithmeticOverflow))?;

    write_source(&accounts[3], &source)?;
    write_config(&accounts[1], &config)
}

fn execute_set_source_enabled(
    program_id: &Address,
    accounts: &[AccountView],
    enabled: bool,
) -> ProgramResult {
    let config = read_config(program_id, &accounts[1])?;
    require_authority(&config.admin, &accounts[0])?;
    let destination = read_destination(program_id, &accounts[3])?;
    let mut source = read_source(program_id, &accounts[2])?;
    require_relationship(
        source.destination_mint == destination.destination_mint,
        ERR_RELATIONSHIP_MISMATCH,
    )?;
    if enabled && source.finalized {
        return Err(ProgramError::Custom(ERR_SOURCE_FINALIZED));
    }

    source.enabled = enabled;
    write_source(&accounts[2], &source)
}

fn execute_pause(program_id: &Address, accounts: &[AccountView], paused: bool) -> ProgramResult {
    let mut config = read_config(program_id, &accounts[1])?;
    require_authority(&config.pause_authority, &accounts[0])?;

    config.launch_status = if paused {
        LaunchStatus::Paused
    } else {
        LaunchStatus::Enabled
    };
    write_config(&accounts[1], &config)?;

    match accounts[2].data_len() {
        CONFIG_LEN => {
            if accounts[2].address() != accounts[1].address() {
                let mut target = read_config(program_id, &accounts[2])?;
                target.launch_status = config.launch_status;
                write_config(&accounts[2], &target)?;
            }
            Ok(())
        }
        DESTINATION_TOKEN_CONFIG_LEN => {
            let mut destination = read_destination(program_id, &accounts[2])?;
            destination.status = if paused {
                LaunchStatus::Paused
            } else {
                LaunchStatus::Enabled
            };
            write_destination(&accounts[2], &destination)
        }
        SOURCE_MINT_CONFIG_LEN => {
            if paused {
                let mut source = read_source(program_id, &accounts[2])?;
                source.enabled = false;
                write_source(&accounts[2], &source)
            } else {
                Ok(())
            }
        }
        _ => Err(ProgramError::Custom(ERR_ACCOUNT_DATA_LEN_MISMATCH)),
    }
}

fn execute_finalize_source_mint(program_id: &Address, accounts: &[AccountView]) -> ProgramResult {
    let config = read_config(program_id, &accounts[1])?;
    require_authority(&config.admin, &accounts[0])?;
    let destination = read_destination(program_id, &accounts[3])?;
    let mut source = read_source(program_id, &accounts[2])?;
    require_relationship(
        source.destination_mint == destination.destination_mint,
        ERR_RELATIONSHIP_MISMATCH,
    )?;

    source.enabled = false;
    source.finalized = true;
    write_source(&accounts[2], &source)
}

fn execute_migrate(
    program_id: &Address,
    accounts: &[AccountView],
    desired_destination_amount: u64,
    max_source_amount: u64,
    user_nonce: u64,
    create_receipt: bool,
) -> ProgramResult {
    #[cfg(not(feature = "receipts"))]
    {
        if create_receipt {
            return Err(ProgramError::Custom(ERR_EXECUTOR_NOT_IMPLEMENTED));
        }
        let _ = user_nonce;
    }

    let config = read_config(program_id, &accounts[1])?;
    require_program_active(&config)?;
    let mut source = read_source(program_id, &accounts[2])?;
    let mut destination = read_destination(program_id, &accounts[3])?;

    require_relationship(
        source.destination_mint == destination.destination_mint,
        ERR_RELATIONSHIP_MISMATCH,
    )?;
    require_account_address(&source.source_mint, &accounts[6])?;
    require_account_address(&destination.destination_mint, &accounts[7])?;
    require_account_address(&destination.mint_authority, &accounts[8])?;
    require_account_address(&source.source_token_program, &accounts[9])
        .map_err(|_| ProgramError::Custom(ERR_TOKEN_PROGRAM_MISMATCH))?;
    require_account_address(&destination.token_program, &accounts[10])
        .map_err(|_| ProgramError::Custom(ERR_TOKEN_PROGRAM_MISMATCH))?;

    require_owned_by(&accounts[4], &accounts[9])?;
    require_owned_by(&accounts[5], &accounts[10])?;
    require_owned_by(&accounts[6], &accounts[9])?;
    require_owned_by(&accounts[7], &accounts[10])?;

    require_token_account_matches(
        &accounts[4],
        &source.source_mint,
        &address_bytes(&accounts[0]),
    )?;
    require_token_account_matches(
        &accounts[5],
        &destination.destination_mint,
        &address_bytes(&accounts[0]),
    )?;
    require_mint_decimals(&accounts[6], source.source_decimals)?;
    require_mint_decimals(&accounts[7], destination.decimals)?;
    let mint_authority_bump =
        require_mint_authority_pda(program_id, &destination.destination_mint, &accounts[8])?;

    #[cfg(feature = "receipts")]
    let user_nonce_bytes = user_nonce.to_le_bytes();
    #[cfg(feature = "receipts")]
    let receipt_seeds = [
        PDA_SEED_PREFIX,
        RECEIPT_SEED_KIND,
        accounts[0].address().as_array().as_slice(),
        source.source_mint.as_slice(),
        destination.destination_mint.as_slice(),
        user_nonce_bytes.as_slice(),
    ];
    #[cfg(feature = "receipts")]
    let receipt_bump = if create_receipt {
        let bump = require_pda(
            program_id,
            &accounts[11],
            &receipt_seeds,
            ERR_RECEIPT_ACCOUNT_MISMATCH,
        )?;
        if accounts[11].owned_by(program_id) {
            require_data_len(&accounts[11], RECEIPT_LEN)?;
            require_uninitialized_account(&accounts[11])?;
        }
        Some(bump)
    } else {
        None
    };

    let quote = quote_migration(
        &destination,
        &source,
        desired_destination_amount,
        max_source_amount,
    )
    .map_err(map_core_error)?;

    invoke_burn_checked(
        &accounts[9],
        &accounts[4],
        &accounts[6],
        &accounts[0],
        quote.source_amount_to_burn,
        source.source_decimals,
    )?;
    invoke_mint_to_checked(
        &accounts[10],
        &accounts[7],
        &accounts[5],
        &accounts[8],
        quote.destination_amount_to_mint,
        destination.decimals,
        &destination.destination_mint,
        mint_authority_bump,
    )?;

    #[cfg(feature = "receipts")]
    if let Some(bump) = receipt_bump {
        if !accounts[11].owned_by(program_id) {
            invoke_create_account_signed(
                &accounts[12],
                &accounts[0],
                &accounts[11],
                program_id,
                RECEIPT_LEN,
                &receipt_seeds,
                bump,
            )?;
            require_data_len(&accounts[11], RECEIPT_LEN)?;
        }

        let receipt = Receipt {
            user: address_bytes(&accounts[0]),
            source_mint: source.source_mint,
            destination_mint: destination.destination_mint,
            source_amount_burned: quote.source_amount_to_burn,
            destination_amount_minted: quote.destination_amount_to_mint,
            slot: 0,
            user_nonce,
            bump,
        };
        let mut data = accounts[11].try_borrow_mut()?;
        receipt.pack(&mut data).map_err(map_core_error)?;
    }

    apply_migration_counters(&mut destination, &mut source, quote).map_err(map_core_error)?;
    write_destination(&accounts[3], &destination)?;
    write_source(&accounts[2], &source)
}

fn execute_transfer_authority_begin(
    program_id: &Address,
    accounts: &[AccountView],
    pending_authority: [u8; 32],
) -> ProgramResult {
    let mut config = read_config(program_id, &accounts[1])?;
    require_authority(&config.admin, &accounts[0])?;
    require_account_address(&pending_authority, &accounts[2])
        .map_err(|_| ProgramError::Custom(ERR_PENDING_AUTHORITY_MISMATCH))?;

    config.pending_authority = pending_authority;
    config.pending_authority_set = true;
    write_config(&accounts[1], &config)
}

fn execute_transfer_authority_accept(
    program_id: &Address,
    accounts: &[AccountView],
) -> ProgramResult {
    let mut config = read_config(program_id, &accounts[1])?;
    require_authority(&config.admin, &accounts[2])?;
    if !config.pending_authority_set {
        return Err(ProgramError::Custom(ERR_PENDING_AUTHORITY_NOT_SET));
    }
    require_account_address(&config.pending_authority, &accounts[0])
        .map_err(|_| ProgramError::Custom(ERR_PENDING_AUTHORITY_MISMATCH))?;

    config.admin = config.pending_authority;
    config.pending_authority = [0; 32];
    config.pending_authority_set = false;
    write_config(&accounts[1], &config)
}

fn execute_retire_authority(program_id: &Address, accounts: &[AccountView]) -> ProgramResult {
    let mut config = read_config(program_id, &accounts[1])?;
    require_authority(&config.admin, &accounts[0])?;
    require_authority(&config.pause_authority, &accounts[2])?;

    config.admin = [0; 32];
    config.pause_authority = [0; 32];
    config.pending_authority = [0; 32];
    config.pending_authority_set = false;
    write_config(&accounts[1], &config)
}

fn read_config(program_id: &Address, account: &AccountView) -> Result<Config, ProgramError> {
    require_program_owned(account, program_id)?;
    require_data_len(account, CONFIG_LEN)?;
    let data = account.try_borrow()?;
    Config::unpack(&data).map_err(map_core_error)
}

fn write_config(account: &AccountView, config: &Config) -> ProgramResult {
    require_data_len(account, CONFIG_LEN)?;
    let mut data = account.try_borrow_mut()?;
    config.pack(&mut data).map_err(map_core_error)
}

fn read_destination(
    program_id: &Address,
    account: &AccountView,
) -> Result<DestinationTokenConfig, ProgramError> {
    require_program_owned(account, program_id)?;
    require_data_len(account, DESTINATION_TOKEN_CONFIG_LEN)?;
    let data = account.try_borrow()?;
    DestinationTokenConfig::unpack(&data).map_err(map_core_error)
}

fn write_destination(account: &AccountView, destination: &DestinationTokenConfig) -> ProgramResult {
    require_data_len(account, DESTINATION_TOKEN_CONFIG_LEN)?;
    let mut data = account.try_borrow_mut()?;
    destination.pack(&mut data).map_err(map_core_error)
}

fn read_source(
    program_id: &Address,
    account: &AccountView,
) -> Result<SourceMintConfig, ProgramError> {
    require_program_owned(account, program_id)?;
    require_data_len(account, SOURCE_MINT_CONFIG_LEN)?;
    let data = account.try_borrow()?;
    SourceMintConfig::unpack(&data).map_err(map_core_error)
}

fn write_source(account: &AccountView, source: &SourceMintConfig) -> ProgramResult {
    require_data_len(account, SOURCE_MINT_CONFIG_LEN)?;
    let mut data = account.try_borrow_mut()?;
    source.pack(&mut data).map_err(map_core_error)
}

fn require_program_owned(account: &AccountView, program_id: &Address) -> ProgramResult {
    if account.owned_by(program_id) {
        Ok(())
    } else {
        Err(ProgramError::Custom(ERR_ACCOUNT_NOT_PROGRAM_OWNED))
    }
}

fn require_data_len(account: &AccountView, expected: usize) -> ProgramResult {
    if account.data_len() == expected {
        Ok(())
    } else {
        Err(ProgramError::Custom(ERR_ACCOUNT_DATA_LEN_MISMATCH))
    }
}

fn require_uninitialized_account(account: &AccountView) -> ProgramResult {
    let data = account.try_borrow()?;
    if data.first().copied().unwrap_or_default() == 0 {
        Ok(())
    } else {
        Err(ProgramError::Custom(ERR_ACCOUNT_ALREADY_INITIALIZED))
    }
}

fn require_program_active(config: &Config) -> ProgramResult {
    match config.launch_status {
        LaunchStatus::Paused | LaunchStatus::Finalized => {
            Err(ProgramError::Custom(ERR_PROGRAM_PAUSED))
        }
        _ => Ok(()),
    }
}

fn require_authority(expected: &[u8; 32], account: &AccountView) -> ProgramResult {
    require_account_address(expected, account)
        .map_err(|_| ProgramError::Custom(ERR_AUTHORITY_MISMATCH))
}

fn require_account_address(expected: &[u8; 32], account: &AccountView) -> ProgramResult {
    if expected == account.address().as_array() {
        Ok(())
    } else {
        Err(ProgramError::Custom(ERR_ACCOUNT_ADDRESS_MISMATCH))
    }
}

fn require_system_program(account: &AccountView) -> ProgramResult {
    if account.address().as_array() == &SYSTEM_PROGRAM_ID_BYTES {
        Ok(())
    } else {
        Err(ProgramError::Custom(ERR_SYSTEM_PROGRAM_MISMATCH))
    }
}

fn require_pda(
    program_id: &Address,
    account: &AccountView,
    seeds: &[&[u8]],
    error: u32,
) -> Result<u8, ProgramError> {
    #[cfg(any(target_os = "solana", target_arch = "bpf"))]
    {
        let Some((expected, bump)) = Address::try_find_program_address(seeds, program_id) else {
            return Err(ProgramError::Custom(error));
        };
        if expected.as_array() == account.address().as_array() {
            Ok(bump)
        } else {
            Err(ProgramError::Custom(error))
        }
    }

    #[cfg(not(any(target_os = "solana", target_arch = "bpf")))]
    {
        core::hint::black_box((program_id, account, seeds, error));
        Ok(0)
    }
}

fn require_expected_bump(actual: u8, expected: u8, error: u32) -> ProgramResult {
    #[cfg(any(target_os = "solana", target_arch = "bpf"))]
    {
        if actual == expected {
            Ok(())
        } else {
            Err(ProgramError::Custom(error))
        }
    }

    #[cfg(not(any(target_os = "solana", target_arch = "bpf")))]
    {
        core::hint::black_box((actual, expected, error));
        Ok(())
    }
}

fn ensure_program_account(
    program_id: &Address,
    payer: &AccountView,
    account: &AccountView,
    system_program: &AccountView,
    space: usize,
    seeds: &[&[u8]],
    bump: u8,
) -> ProgramResult {
    if account.owned_by(program_id) {
        return require_data_len(account, space);
    }

    require_system_program(system_program)?;
    invoke_create_account_signed(
        system_program,
        payer,
        account,
        program_id,
        space,
        seeds,
        bump,
    )?;
    require_program_owned(account, program_id)?;
    require_data_len(account, space)
}

fn require_relationship(condition: bool, error: u32) -> ProgramResult {
    if condition {
        Ok(())
    } else {
        Err(ProgramError::Custom(error))
    }
}

fn require_nonzero_bytes(value: &[u8; 32], error: u32) -> ProgramResult {
    if value.iter().any(|byte| *byte != 0) {
        Ok(())
    } else {
        Err(ProgramError::Custom(error))
    }
}

fn address_bytes(account: &AccountView) -> [u8; 32] {
    *account.address().as_array()
}

fn require_owned_by(account: &AccountView, owner: &AccountView) -> ProgramResult {
    if account.owned_by(owner.address()) {
        Ok(())
    } else {
        Err(ProgramError::Custom(ERR_TOKEN_PROGRAM_MISMATCH))
    }
}

fn require_token_account_matches(
    account: &AccountView,
    expected_mint: &[u8; 32],
    expected_owner: &[u8; 32],
) -> ProgramResult {
    require_min_data_len(account, SPL_TOKEN_ACCOUNT_BASE_LEN)?;
    let mint = read_account_pubkey(account, SPL_TOKEN_ACCOUNT_MINT_OFFSET)?;
    if &mint != expected_mint {
        return Err(ProgramError::Custom(ERR_TOKEN_ACCOUNT_MINT_MISMATCH));
    }
    let owner = read_account_pubkey(account, SPL_TOKEN_ACCOUNT_OWNER_OFFSET)?;
    if &owner != expected_owner {
        return Err(ProgramError::Custom(ERR_TOKEN_ACCOUNT_OWNER_MISMATCH));
    }
    Ok(())
}

fn require_mint_decimals(account: &AccountView, expected_decimals: u8) -> ProgramResult {
    require_min_data_len(account, SPL_MINT_BASE_LEN)?;
    let data = account.try_borrow()?;
    if data[SPL_MINT_DECIMALS_OFFSET] == expected_decimals {
        Ok(())
    } else {
        Err(ProgramError::Custom(ERR_MINT_DECIMALS_MISMATCH))
    }
}

fn require_destination_mint_setup(
    mint: &AccountView,
    token_program: &AccountView,
    expected_decimals: u8,
    expected_mint_authority: &[u8; 32],
) -> ProgramResult {
    require_owned_by(mint, token_program)?;
    require_min_data_len(mint, SPL_MINT_BASE_LEN)?;
    let data = mint.try_borrow()?;
    if data[SPL_MINT_IS_INITIALIZED_OFFSET] != 1 {
        return Err(ProgramError::Custom(ERR_MINT_NOT_INITIALIZED));
    }
    if data[SPL_MINT_DECIMALS_OFFSET] != expected_decimals {
        return Err(ProgramError::Custom(ERR_MINT_DECIMALS_MISMATCH));
    }
    if read_coption_tag(&data, SPL_MINT_AUTHORITY_OPTION_OFFSET)? != 1 {
        return Err(ProgramError::Custom(ERR_MINT_AUTHORITY_MISMATCH));
    }
    if &data[SPL_MINT_AUTHORITY_OFFSET..SPL_MINT_AUTHORITY_OFFSET + 32] != expected_mint_authority {
        return Err(ProgramError::Custom(ERR_MINT_AUTHORITY_MISMATCH));
    }
    if read_coption_tag(&data, SPL_MINT_FREEZE_AUTHORITY_OPTION_OFFSET)? != 0 {
        return Err(ProgramError::Custom(ERR_FREEZE_AUTHORITY_PRESENT));
    }
    Ok(())
}

fn require_min_data_len(account: &AccountView, min_len: usize) -> ProgramResult {
    if account.data_len() >= min_len {
        Ok(())
    } else {
        Err(ProgramError::Custom(ERR_TOKEN_ACCOUNT_DATA_TOO_SHORT))
    }
}

fn read_coption_tag(data: &[u8], offset: usize) -> Result<u32, ProgramError> {
    if data.len() < offset + 4 {
        return Err(ProgramError::Custom(ERR_TOKEN_ACCOUNT_DATA_TOO_SHORT));
    }
    Ok(u32::from_le_bytes([
        data[offset],
        data[offset + 1],
        data[offset + 2],
        data[offset + 3],
    ]))
}

fn read_account_pubkey(account: &AccountView, offset: usize) -> Result<[u8; 32], ProgramError> {
    if account.data_len() < offset + 32 {
        return Err(ProgramError::Custom(ERR_TOKEN_ACCOUNT_DATA_TOO_SHORT));
    }
    let data = account.try_borrow()?;
    let mut value = [0u8; 32];
    value.copy_from_slice(&data[offset..offset + 32]);
    Ok(value)
}

fn require_mint_authority_pda(
    program_id: &Address,
    destination_mint: &[u8; 32],
    mint_authority: &AccountView,
) -> Result<u8, ProgramError> {
    #[cfg(any(target_os = "solana", target_arch = "bpf"))]
    {
        let seeds = [
            MINT_AUTHORITY_SEED_PREFIX,
            MINT_AUTHORITY_SEED_KIND,
            destination_mint.as_slice(),
        ];
        let Some((expected, bump)) = Address::try_find_program_address(&seeds, program_id) else {
            return Err(ProgramError::Custom(ERR_MINT_AUTHORITY_PDA_MISMATCH));
        };
        if expected.as_array() == mint_authority.address().as_array() {
            Ok(bump)
        } else {
            Err(ProgramError::Custom(ERR_MINT_AUTHORITY_PDA_MISMATCH))
        }
    }

    #[cfg(not(any(target_os = "solana", target_arch = "bpf")))]
    {
        core::hint::black_box((program_id, destination_mint, mint_authority));
        Ok(0)
    }
}

fn invoke_burn_checked(
    token_program: &AccountView,
    account: &AccountView,
    mint: &AccountView,
    authority: &AccountView,
    amount: u64,
    decimals: u8,
) -> ProgramResult {
    let instruction_accounts = [
        CpiInstructionAccount::writable(account.address()),
        CpiInstructionAccount::writable(mint.address()),
        CpiInstructionAccount::new(authority.address(), false, true),
    ];
    let cpi_accounts = [
        CpiAccount::from_account(account),
        CpiAccount::from_account(mint),
        CpiAccount::from_account(authority),
        CpiAccount::from_account(token_program),
    ];
    let instruction_data =
        token_checked_instruction_data(TOKEN_BURN_CHECKED_DISCRIMINATOR, amount, decimals);

    invoke_cpi(
        token_program.address(),
        &instruction_accounts,
        &instruction_data,
        &cpi_accounts,
        &[],
    )
}

#[allow(clippy::too_many_arguments)]
fn invoke_mint_to_checked(
    token_program: &AccountView,
    mint: &AccountView,
    account: &AccountView,
    mint_authority: &AccountView,
    amount: u64,
    decimals: u8,
    destination_mint: &[u8; 32],
    mint_authority_bump: u8,
) -> ProgramResult {
    let instruction_accounts = [
        CpiInstructionAccount::writable(mint.address()),
        CpiInstructionAccount::writable(account.address()),
        CpiInstructionAccount::new(mint_authority.address(), false, true),
    ];
    let cpi_accounts = [
        CpiAccount::from_account(mint),
        CpiAccount::from_account(account),
        CpiAccount::from_account(mint_authority),
        CpiAccount::from_account(token_program),
    ];
    let instruction_data =
        token_checked_instruction_data(TOKEN_MINT_TO_CHECKED_DISCRIMINATOR, amount, decimals);
    let bump = [mint_authority_bump];
    let seeds = [
        CpiSeed::from_slice(MINT_AUTHORITY_SEED_PREFIX),
        CpiSeed::from_slice(MINT_AUTHORITY_SEED_KIND),
        CpiSeed::from_slice(destination_mint),
        CpiSeed::from_slice(&bump),
    ];
    let signer = CpiSigner::from_seeds(&seeds);

    invoke_cpi(
        token_program.address(),
        &instruction_accounts,
        &instruction_data,
        &cpi_accounts,
        &[signer],
    )
}

fn token_checked_instruction_data(discriminator: u8, amount: u64, decimals: u8) -> [u8; 10] {
    let mut data = [0u8; TOKEN_CHECKED_IX_LEN];
    data[0] = discriminator;
    data[1..9].copy_from_slice(&amount.to_le_bytes());
    data[9] = decimals;
    data
}

fn invoke_create_account_signed(
    system_program: &AccountView,
    payer: &AccountView,
    new_account: &AccountView,
    owner: &Address,
    space: usize,
    seeds: &[&[u8]],
    bump: u8,
) -> ProgramResult {
    #[cfg(any(target_os = "solana", target_arch = "bpf"))]
    let lamports = Rent::get()?.try_minimum_balance(space)?;

    #[cfg(not(any(target_os = "solana", target_arch = "bpf")))]
    let lamports = {
        core::hint::black_box((
            system_program,
            payer,
            new_account,
            owner,
            space,
            seeds,
            bump,
        ));
        1
    };

    let instruction_accounts = [
        CpiInstructionAccount::new(payer.address(), true, true),
        CpiInstructionAccount::new(new_account.address(), true, true),
    ];
    let cpi_accounts = [
        CpiAccount::from_account(payer),
        CpiAccount::from_account(new_account),
        CpiAccount::from_account(system_program),
    ];
    let instruction_data = system_create_account_instruction_data(lamports, space as u64, owner);
    let bump_seed = [bump];

    match seeds.len() {
        3 => {
            let signer_seeds = [
                CpiSeed::from_slice(seeds[0]),
                CpiSeed::from_slice(seeds[1]),
                CpiSeed::from_slice(seeds[2]),
                CpiSeed::from_slice(&bump_seed),
            ];
            let signer = CpiSigner::from_seeds(&signer_seeds);
            invoke_cpi(
                system_program.address(),
                &instruction_accounts,
                &instruction_data,
                &cpi_accounts,
                &[signer],
            )
        }
        4 => {
            let signer_seeds = [
                CpiSeed::from_slice(seeds[0]),
                CpiSeed::from_slice(seeds[1]),
                CpiSeed::from_slice(seeds[2]),
                CpiSeed::from_slice(seeds[3]),
                CpiSeed::from_slice(&bump_seed),
            ];
            let signer = CpiSigner::from_seeds(&signer_seeds);
            invoke_cpi(
                system_program.address(),
                &instruction_accounts,
                &instruction_data,
                &cpi_accounts,
                &[signer],
            )
        }
        6 => {
            let signer_seeds = [
                CpiSeed::from_slice(seeds[0]),
                CpiSeed::from_slice(seeds[1]),
                CpiSeed::from_slice(seeds[2]),
                CpiSeed::from_slice(seeds[3]),
                CpiSeed::from_slice(seeds[4]),
                CpiSeed::from_slice(seeds[5]),
                CpiSeed::from_slice(&bump_seed),
            ];
            let signer = CpiSigner::from_seeds(&signer_seeds);
            invoke_cpi(
                system_program.address(),
                &instruction_accounts,
                &instruction_data,
                &cpi_accounts,
                &[signer],
            )
        }
        _ => Err(ProgramError::Custom(ERR_PDA_SEED_SHAPE)),
    }
}

fn system_create_account_instruction_data(
    lamports: u64,
    space: u64,
    owner: &Address,
) -> [u8; SYSTEM_CREATE_ACCOUNT_IX_LEN] {
    let mut data = [0u8; SYSTEM_CREATE_ACCOUNT_IX_LEN];
    data[0..4].copy_from_slice(&SYSTEM_CREATE_ACCOUNT_DISCRIMINATOR.to_le_bytes());
    data[4..12].copy_from_slice(&lamports.to_le_bytes());
    data[12..20].copy_from_slice(&space.to_le_bytes());
    data[20..52].copy_from_slice(owner.as_array());
    data
}

#[repr(C)]
struct CpiInstructionAccount<'account> {
    address: &'account Address,
    is_writable: bool,
    is_signer: bool,
}

impl<'account> CpiInstructionAccount<'account> {
    const fn new(address: &'account Address, is_writable: bool, is_signer: bool) -> Self {
        Self {
            address,
            is_writable,
            is_signer,
        }
    }

    const fn writable(address: &'account Address) -> Self {
        Self::new(address, true, false)
    }
}

#[repr(C)]
struct CpiAccount<'account> {
    address: *const Address,
    lamports: *const u64,
    data_len: u64,
    data: *const u8,
    owner: *const Address,
    rent_epoch: u64,
    is_signer: u8,
    is_writable: u8,
    executable: u8,
    _padding: u8,
    _account_view: PhantomData<&'account AccountView>,
}

impl<'account> CpiAccount<'account> {
    fn from_account(account: &'account AccountView) -> Self {
        let raw = account.account_ptr();
        // SAFETY: `AccountView` guarantees that `account_ptr` points to a valid
        // runtime account for the life of the view. The CPI struct only stores
        // raw pointers consumed synchronously by the Solana CPI syscall.
        unsafe {
            Self {
                address: core::ptr::addr_of!((*raw).address),
                lamports: core::ptr::addr_of!((*raw).lamports),
                data_len: (*raw).data_len,
                data: account.data_ptr() as *const u8,
                owner: core::ptr::addr_of!((*raw).owner),
                rent_epoch: 0,
                is_signer: (*raw).is_signer,
                is_writable: (*raw).is_writable,
                executable: (*raw).executable,
                _padding: 0,
                _account_view: PhantomData,
            }
        }
    }
}

#[repr(C)]
struct CpiSeed<'bytes> {
    seed: *const u8,
    len: u64,
    _bytes: PhantomData<&'bytes [u8]>,
}

impl<'bytes> CpiSeed<'bytes> {
    fn from_slice(value: &'bytes [u8]) -> Self {
        Self {
            seed: value.as_ptr(),
            len: value.len() as u64,
            _bytes: PhantomData,
        }
    }
}

#[repr(C)]
#[derive(Clone, Copy)]
struct CpiSigner<'bytes, 'seeds> {
    seeds: *const CpiSeed<'bytes>,
    len: u64,
    _seeds: PhantomData<&'seeds [CpiSeed<'bytes>]>,
}

impl<'bytes, 'seeds> CpiSigner<'bytes, 'seeds> {
    fn from_seeds(value: &'seeds [CpiSeed<'bytes>]) -> Self {
        Self {
            seeds: value.as_ptr(),
            len: value.len() as u64,
            _seeds: PhantomData,
        }
    }
}

fn invoke_cpi(
    program_id: &Address,
    instruction_accounts: &[CpiInstructionAccount],
    instruction_data: &[u8],
    cpi_accounts: &[CpiAccount],
    signers: &[CpiSigner],
) -> ProgramResult {
    #[cfg(any(target_os = "solana", target_arch = "bpf"))]
    {
        #[repr(C)]
        struct CInstruction<'account> {
            program_id: *const Address,
            accounts: *const CpiInstructionAccount<'account>,
            accounts_len: u64,
            data: *const u8,
            data_len: u64,
        }

        let instruction = CInstruction {
            program_id,
            accounts: instruction_accounts.as_ptr(),
            accounts_len: instruction_accounts.len() as u64,
            data: instruction_data.as_ptr(),
            data_len: instruction_data.len() as u64,
        };

        core::sync::atomic::compiler_fence(core::sync::atomic::Ordering::SeqCst);

        // SAFETY: `instruction`, `cpi_accounts`, and `signers` point to stack
        // data that remains alive for the duration of the synchronous CPI call,
        // and their layouts match the Solana C CPI ABI.
        let result = unsafe {
            pinocchio::syscalls::sol_invoke_signed_c(
                &instruction as *const _ as *const u8,
                cpi_accounts.as_ptr() as *const u8,
                cpi_accounts.len() as u64,
                signers.as_ptr() as *const u8,
                signers.len() as u64,
            )
        };

        if result == 0 {
            Ok(())
        } else {
            Err(ProgramError::from(result))
        }
    }

    #[cfg(not(any(target_os = "solana", target_arch = "bpf")))]
    {
        core::hint::black_box((
            program_id,
            instruction_accounts,
            instruction_data,
            cpi_accounts,
            signers,
        ));
        Ok(())
    }
}

fn map_core_error(error: CoreError) -> ProgramError {
    ProgramError::Custom(ERR_CORE_BASE + core_error_code(error))
}

fn core_error_code(error: CoreError) -> u32 {
    match error {
        CoreError::InvalidLength => 1,
        CoreError::InvalidVersion => 2,
        CoreError::InvalidDiscriminant => 3,
        CoreError::InvalidBool => 4,
        CoreError::ZeroAmount => 5,
        CoreError::SourceDisabled => 6,
        CoreError::SourceDestinationMismatch => 7,
        CoreError::DestinationNotEnabled => 8,
        CoreError::DestinationFinalized => 9,
        CoreError::DestinationPaused => 10,
        CoreError::InvalidRatio => 11,
        CoreError::FractionalRatio => 12,
        CoreError::DivisionByZero => 13,
        CoreError::SlippageExceeded => 14,
        CoreError::DestinationAmountTooSmall => 15,
        CoreError::SupplyCapExceeded => 16,
        CoreError::UnsupportedMigrationMode => 17,
        CoreError::ArithmeticOverflow => 18,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use pinocchio::account::{RuntimeAccount, NOT_BORROWED};
    use rati_burn_to_mint_core::{
        MigrationMode, PriceMode, CONFIG_LEN, DESTINATION_TOKEN_CONFIG_LEN, SOURCE_MINT_CONFIG_LEN,
    };
    #[cfg(feature = "receipts")]
    use rati_burn_to_mint_core::{Receipt, RECEIPT_LEN};
    use std::{mem::size_of, vec::Vec};

    fn address(byte: u8) -> Address {
        Address::new_from_array([byte; 32])
    }

    fn pack_instruction(instruction: &Instruction) -> Vec<u8> {
        let mut data = vec![0u8; instruction.packed_len()];
        instruction.pack(&mut data).expect("pack instruction");
        data
    }

    struct MockAccount {
        storage: Vec<u64>,
    }

    impl MockAccount {
        fn new(
            address_byte: u8,
            owner: Address,
            data_len: usize,
            signer: bool,
            writable: bool,
        ) -> Self {
            let words = (size_of::<RuntimeAccount>() + data_len).div_ceil(size_of::<u64>());
            let mut storage = vec![0u64; words.max(1)];
            let raw = storage.as_mut_ptr() as *mut RuntimeAccount;
            unsafe {
                *raw = RuntimeAccount {
                    borrow_state: NOT_BORROWED,
                    is_signer: signer as u8,
                    is_writable: writable as u8,
                    executable: 0,
                    resize_delta: 0,
                    address: address(address_byte),
                    owner,
                    lamports: 1,
                    data_len: data_len as u64,
                };
            }
            Self { storage }
        }

        fn view(&mut self) -> AccountView {
            unsafe { AccountView::new_unchecked(self.storage.as_mut_ptr() as *mut RuntimeAccount) }
        }
    }

    fn process_with_accounts(
        program_id: &Address,
        accounts: &mut [MockAccount],
        instruction: &Instruction,
    ) -> ProgramResult {
        let instruction_data = pack_instruction(instruction);
        let views = accounts
            .iter_mut()
            .map(MockAccount::view)
            .collect::<Vec<_>>();
        process_instruction(program_id, &views, &instruction_data)
    }

    fn read_config_from(account: &mut MockAccount) -> Config {
        let view = account.view();
        let data = view.try_borrow().expect("borrow config");
        Config::unpack(&data).expect("unpack config")
    }

    fn write_config_to(account: &mut MockAccount, config: &Config) {
        let view = account.view();
        let mut data = view.try_borrow_mut().expect("borrow config mut");
        config.pack(&mut data).expect("pack config");
    }

    fn read_destination_from(account: &mut MockAccount) -> DestinationTokenConfig {
        let view = account.view();
        let data = view.try_borrow().expect("borrow destination");
        DestinationTokenConfig::unpack(&data).expect("unpack destination")
    }

    fn write_destination_to(account: &mut MockAccount, destination: &DestinationTokenConfig) {
        let view = account.view();
        let mut data = view.try_borrow_mut().expect("borrow destination mut");
        destination.pack(&mut data).expect("pack destination");
    }

    fn read_source_from(account: &mut MockAccount) -> SourceMintConfig {
        let view = account.view();
        let data = view.try_borrow().expect("borrow source");
        SourceMintConfig::unpack(&data).expect("unpack source")
    }

    fn write_source_to(account: &mut MockAccount, source: &SourceMintConfig) {
        let view = account.view();
        let mut data = view.try_borrow_mut().expect("borrow source mut");
        source.pack(&mut data).expect("pack source");
    }

    #[cfg(feature = "receipts")]
    fn read_receipt_from(account: &mut MockAccount) -> Receipt {
        let view = account.view();
        let data = view.try_borrow().expect("borrow receipt");
        Receipt::unpack(&data).expect("unpack receipt")
    }

    fn sample_config(admin: u8, pause_authority: u8) -> Config {
        Config {
            admin: [admin; 32],
            pause_authority: [pause_authority; 32],
            launch_status: LaunchStatus::Candidate,
            destination_count: 0,
            source_count: 0,
            config_bump: 255,
            pending_authority: [0; 32],
            pending_authority_set: false,
            reserved: [0; 8],
        }
    }

    fn destination() -> DestinationTokenConfig {
        DestinationTokenConfig {
            destination_mint: [30; 32],
            mint_vanity_nonce: 7,
            mint_bump: 254,
            token_id_hash: [0; 32],
            decimals: 6,
            token_program: [60; 32],
            mint_authority: [50; 32],
            max_supply: 1_000_000,
            total_minted: 0,
            bonding_min: 1,
            bonding_range: 9,
            min_dest_amount: 1,
            status: LaunchStatus::Enabled,
            reserved: [0; 8],
        }
    }

    fn source(enabled: bool) -> SourceMintConfig {
        SourceMintConfig {
            source_mint: [40; 32],
            destination_mint: [30; 32],
            source_token_program: [70; 32],
            source_decimals: 6,
            migration_mode: MigrationMode::BurnToMint,
            price_mode: PriceMode::FixedRatio,
            fixed_ratio_source_amount: 1,
            fixed_ratio_destination_amount: 1,
            enabled,
            burned_base_units: 0,
            minted_destination_base_units: 0,
            migration_count: 0,
            bump: 253,
            finalized: false,
            reserved: [0; 7],
        }
    }

    fn account(address_byte: u8, data_len: usize, signer: bool, writable: bool) -> MockAccount {
        MockAccount::new(address_byte, address(200), data_len, signer, writable)
    }

    fn external_account(address_byte: u8, signer: bool, writable: bool) -> MockAccount {
        MockAccount::new(address_byte, address(0), 0, signer, writable)
    }

    fn owned_account(
        address_byte: u8,
        owner_byte: u8,
        data_len: usize,
        signer: bool,
        writable: bool,
    ) -> MockAccount {
        MockAccount::new(
            address_byte,
            address(owner_byte),
            data_len,
            signer,
            writable,
        )
    }

    fn write_pubkey_field(account: &mut MockAccount, offset: usize, address_byte: u8) {
        let view = account.view();
        let mut data = view.try_borrow_mut().expect("borrow account mut");
        data[offset..offset + 32].copy_from_slice(&[address_byte; 32]);
    }

    fn write_byte_field(account: &mut MockAccount, offset: usize, value: u8) {
        let view = account.view();
        let mut data = view.try_borrow_mut().expect("borrow account mut");
        data[offset] = value;
    }

    fn write_u32_field(account: &mut MockAccount, offset: usize, value: u32) {
        let view = account.view();
        let mut data = view.try_borrow_mut().expect("borrow account mut");
        data[offset..offset + 4].copy_from_slice(&value.to_le_bytes());
    }

    fn token_account(
        address_byte: u8,
        token_program_byte: u8,
        mint_byte: u8,
        owner_byte: u8,
    ) -> MockAccount {
        let mut account = owned_account(
            address_byte,
            token_program_byte,
            SPL_TOKEN_ACCOUNT_BASE_LEN,
            false,
            true,
        );
        write_pubkey_field(&mut account, SPL_TOKEN_ACCOUNT_MINT_OFFSET, mint_byte);
        write_pubkey_field(&mut account, SPL_TOKEN_ACCOUNT_OWNER_OFFSET, owner_byte);
        account
    }

    fn mint_account_with_authorities(
        address_byte: u8,
        token_program_byte: u8,
        decimals: u8,
        mint_authority_byte: Option<u8>,
        freeze_authority_byte: Option<u8>,
    ) -> MockAccount {
        let mut account = owned_account(
            address_byte,
            token_program_byte,
            SPL_MINT_BASE_LEN,
            false,
            true,
        );
        write_byte_field(&mut account, SPL_MINT_DECIMALS_OFFSET, decimals);
        write_byte_field(&mut account, SPL_MINT_IS_INITIALIZED_OFFSET, 1);
        if let Some(authority) = mint_authority_byte {
            write_u32_field(&mut account, SPL_MINT_AUTHORITY_OPTION_OFFSET, 1);
            write_pubkey_field(&mut account, SPL_MINT_AUTHORITY_OFFSET, authority);
        }
        if let Some(authority) = freeze_authority_byte {
            write_u32_field(&mut account, SPL_MINT_FREEZE_AUTHORITY_OPTION_OFFSET, 1);
            write_pubkey_field(
                &mut account,
                SPL_MINT_FREEZE_AUTHORITY_OPTION_OFFSET + 4,
                authority,
            );
        }
        account
    }

    fn mint_account(address_byte: u8, token_program_byte: u8, decimals: u8) -> MockAccount {
        mint_account_with_authorities(address_byte, token_program_byte, decimals, None, None)
    }

    fn destination_mint_account() -> MockAccount {
        mint_account_with_authorities(30, 60, 6, Some(50), None)
    }

    fn destination_instruction() -> Instruction {
        Instruction::RegisterDestinationMint {
            token_id_hash: [8; 32],
            mint_vanity_nonce: 7,
            mint_bump: 254,
            decimals: 6,
            max_supply: 1_000_000,
            bonding_min: 1,
            bonding_range: 9,
            min_dest_amount: 1,
        }
    }

    #[test]
    fn expected_account_counts_match_instruction_shape() {
        assert_eq!(
            expected_account_count(&Instruction::InitializeConfig { config_bump: 255 }),
            INITIALIZE_CONFIG_ACCOUNTS
        );
        assert_eq!(
            expected_account_count(&Instruction::Migrate {
                desired_destination_amount: 1,
                max_source_amount: 1,
                user_nonce: 7,
                create_receipt: false,
            }),
            MIGRATE_BASE_ACCOUNTS
        );
        assert_eq!(
            expected_account_count(&Instruction::Migrate {
                desired_destination_amount: 1,
                max_source_amount: 1,
                user_nonce: 7,
                create_receipt: true,
            }),
            MIGRATE_WITH_RECEIPT_ACCOUNTS
        );
    }

    #[test]
    fn every_instruction_has_a_bounded_account_count() {
        let instructions = [
            Instruction::InitializeConfig { config_bump: 255 },
            Instruction::RegisterDestinationMint {
                token_id_hash: [8; 32],
                mint_vanity_nonce: 1,
                mint_bump: 255,
                decimals: 6,
                max_supply: 0,
                bonding_min: 1,
                bonding_range: 1,
                min_dest_amount: 1,
            },
            Instruction::RegisterSourceMint {
                source_decimals: 6,
                migration_mode: MigrationMode::BurnToMint,
                price_mode: PriceMode::FixedRatio,
                fixed_ratio_source_amount: 1,
                fixed_ratio_destination_amount: 1,
                bump: 255,
            },
            Instruction::SetSourceEnabled { enabled: true },
            Instruction::Migrate {
                desired_destination_amount: 1,
                max_source_amount: 1,
                user_nonce: 7,
                create_receipt: true,
            },
            Instruction::Pause { paused: true },
            Instruction::FinalizeSourceMint,
            Instruction::TransferAuthorityBegin {
                pending_authority: [7; 32],
            },
            Instruction::TransferAuthorityAccept,
            Instruction::RetireAuthority,
        ];

        for instruction in instructions {
            assert!(expected_account_count(&instruction) <= MAX_ACCOUNTS);
            assert_eq!(
                expected_account_count(&instruction),
                expected_account_rules(&instruction).len()
            );
        }
    }

    #[test]
    fn migrate_rules_require_user_signature_and_writable_token_accounts() {
        let rules = expected_account_rules(&Instruction::Migrate {
            desired_destination_amount: 1,
            max_source_amount: 1,
            user_nonce: 7,
            create_receipt: false,
        });

        assert_eq!(rules[0].role, AccountRole::User);
        assert!(rules[0].signer);
        assert_eq!(rules[1].role, AccountRole::Config);
        assert_eq!(rules[4].role, AccountRole::UserSourceTokenAccount);
        assert!(rules[4].writable);
        assert_eq!(rules[5].role, AccountRole::UserDestinationTokenAccount);
        assert!(rules[5].writable);
        assert_eq!(rules[10].role, AccountRole::DestinationTokenProgram);
    }

    #[test]
    fn receipt_migration_rules_append_receipt_and_system_program() {
        let rules = expected_account_rules(&Instruction::Migrate {
            desired_destination_amount: 1,
            max_source_amount: 1,
            user_nonce: 7,
            create_receipt: true,
        });

        assert_eq!(rules.len(), MIGRATE_WITH_RECEIPT_ACCOUNTS);
        assert_eq!(rules[11].role, AccountRole::Receipt);
        assert!(rules[11].writable);
        assert_eq!(rules[12].role, AccountRole::SystemProgram);
    }

    #[test]
    fn register_destination_rules_require_writable_config_for_count_update() {
        let rules = expected_account_rules(&Instruction::RegisterDestinationMint {
            token_id_hash: [8; 32],
            mint_vanity_nonce: 1,
            mint_bump: 255,
            decimals: 6,
            max_supply: 0,
            bonding_min: 1,
            bonding_range: 1,
            min_dest_amount: 1,
        });

        assert_eq!(rules[1].role, AccountRole::Config);
        assert!(rules[1].writable);
        assert_eq!(rules[6].role, AccountRole::SystemProgram);
        assert!(rules[0].writable);
    }

    #[test]
    fn extra_accounts_are_rejected_as_fee_surface() {
        assert_eq!(
            require_account_count(MIGRATE_BASE_ACCOUNTS + 1, MIGRATE_BASE_ACCOUNTS),
            Err(ProgramError::Custom(ERR_PROTOCOL_FEE_FORBIDDEN))
        );
    }

    #[test]
    fn missing_accounts_are_rejected_as_shape_mismatch() {
        assert_eq!(
            require_account_count(MIGRATE_BASE_ACCOUNTS - 1, MIGRATE_BASE_ACCOUNTS),
            Err(ProgramError::Custom(ERR_ACCOUNT_COUNT_MISMATCH))
        );
    }

    #[test]
    fn invalid_instruction_data_maps_to_rati_core_error_range() {
        let accounts = [];
        let err = process_instruction_stub(&accounts, &[]).expect_err("empty instruction fails");
        assert_eq!(err, ProgramError::Custom(ERR_CORE_BASE + 1));
    }

    #[test]
    fn initialize_config_writes_authorities() {
        let program_id = address(200);
        let mut accounts = vec![
            external_account(10, true, true),
            account(20, CONFIG_LEN, false, true),
            external_account(30, false, false),
            external_account(40, false, false),
            external_account(0, false, false),
        ];

        process_with_accounts(
            &program_id,
            &mut accounts,
            &Instruction::InitializeConfig { config_bump: 251 },
        )
        .expect("initialize config");

        let config = read_config_from(&mut accounts[1]);
        assert_eq!(config.admin, [30; 32]);
        assert_eq!(config.pause_authority, [40; 32]);
        assert_eq!(config.launch_status, LaunchStatus::Candidate);
        assert_eq!(config.config_bump, 251);
    }

    #[test]
    fn initialize_config_rejects_reinitialization() {
        let program_id = address(200);
        let mut accounts = vec![
            external_account(10, true, true),
            account(20, CONFIG_LEN, false, true),
            external_account(99, false, false),
            external_account(98, false, false),
            external_account(0, false, false),
        ];
        write_config_to(&mut accounts[1], &sample_config(30, 40));

        let err = process_with_accounts(
            &program_id,
            &mut accounts,
            &Instruction::InitializeConfig { config_bump: 251 },
        )
        .expect_err("reinitialize config rejected");

        assert_eq!(err, ProgramError::Custom(ERR_ACCOUNT_ALREADY_INITIALIZED));
        assert_eq!(read_config_from(&mut accounts[1]).admin, [30; 32]);
    }

    #[test]
    fn register_destination_writes_enabled_destination_and_counts() {
        let program_id = address(200);
        let mut accounts = vec![
            external_account(30, true, true),
            account(20, CONFIG_LEN, false, true),
            account(21, DESTINATION_TOKEN_CONFIG_LEN, false, true),
            destination_mint_account(),
            external_account(50, false, false),
            external_account(60, false, false),
            external_account(0, false, false),
        ];
        write_config_to(&mut accounts[1], &sample_config(30, 40));

        process_with_accounts(&program_id, &mut accounts, &destination_instruction())
            .expect("register destination");

        let config = read_config_from(&mut accounts[1]);
        let destination = read_destination_from(&mut accounts[2]);
        assert_eq!(config.destination_count, 1);
        assert_eq!(destination.destination_mint, [30; 32]);
        assert_eq!(destination.token_id_hash, [8; 32]);
        assert_eq!(destination.mint_authority, [50; 32]);
        assert_eq!(destination.token_program, [60; 32]);
        assert_eq!(destination.status, LaunchStatus::Enabled);
    }

    #[test]
    fn register_destination_rejects_existing_config_account() {
        let program_id = address(200);
        let mut accounts = vec![
            external_account(30, true, true),
            account(20, CONFIG_LEN, false, true),
            account(21, DESTINATION_TOKEN_CONFIG_LEN, false, true),
            destination_mint_account(),
            external_account(50, false, false),
            external_account(60, false, false),
            external_account(0, false, false),
        ];
        write_config_to(&mut accounts[1], &sample_config(30, 40));
        write_destination_to(&mut accounts[2], &destination());

        let err = process_with_accounts(&program_id, &mut accounts, &destination_instruction())
            .expect_err("existing destination rejected");

        assert_eq!(err, ProgramError::Custom(ERR_ACCOUNT_ALREADY_INITIALIZED));
        assert_eq!(read_config_from(&mut accounts[1]).destination_count, 0);
    }

    #[test]
    fn register_destination_rejects_wrong_mint_authority() {
        let program_id = address(200);
        let mut accounts = vec![
            external_account(30, true, true),
            account(20, CONFIG_LEN, false, true),
            account(21, DESTINATION_TOKEN_CONFIG_LEN, false, true),
            mint_account_with_authorities(30, 60, 6, Some(51), None),
            external_account(50, false, false),
            external_account(60, false, false),
            external_account(0, false, false),
        ];
        write_config_to(&mut accounts[1], &sample_config(30, 40));

        let err = process_with_accounts(&program_id, &mut accounts, &destination_instruction())
            .expect_err("wrong mint authority rejected");

        assert_eq!(err, ProgramError::Custom(ERR_MINT_AUTHORITY_MISMATCH));
        assert_eq!(read_config_from(&mut accounts[1]).destination_count, 0);
    }

    #[test]
    fn register_destination_rejects_zero_token_id_hash() {
        let program_id = address(200);
        let mut accounts = vec![
            external_account(30, true, true),
            account(20, CONFIG_LEN, false, true),
            account(21, DESTINATION_TOKEN_CONFIG_LEN, false, true),
            destination_mint_account(),
            external_account(50, false, false),
            external_account(60, false, false),
            external_account(0, false, false),
        ];
        let instruction = Instruction::RegisterDestinationMint {
            token_id_hash: [0; 32],
            mint_vanity_nonce: 7,
            mint_bump: 254,
            decimals: 6,
            max_supply: 1_000_000,
            bonding_min: 1,
            bonding_range: 9,
            min_dest_amount: 1,
        };
        write_config_to(&mut accounts[1], &sample_config(30, 40));

        let err = process_with_accounts(&program_id, &mut accounts, &instruction)
            .expect_err("zero token id hash rejected");

        assert_eq!(err, ProgramError::Custom(ERR_TOKEN_ID_HASH_ZERO));
        assert_eq!(read_config_from(&mut accounts[1]).destination_count, 0);
    }

    #[test]
    fn register_destination_rejects_freeze_authority() {
        let program_id = address(200);
        let mut accounts = vec![
            external_account(30, true, true),
            account(20, CONFIG_LEN, false, true),
            account(21, DESTINATION_TOKEN_CONFIG_LEN, false, true),
            mint_account_with_authorities(30, 60, 6, Some(50), Some(99)),
            external_account(50, false, false),
            external_account(60, false, false),
            external_account(0, false, false),
        ];
        write_config_to(&mut accounts[1], &sample_config(30, 40));

        let err = process_with_accounts(&program_id, &mut accounts, &destination_instruction())
            .expect_err("freeze authority rejected");

        assert_eq!(err, ProgramError::Custom(ERR_FREEZE_AUTHORITY_PRESENT));
        assert_eq!(read_config_from(&mut accounts[1]).destination_count, 0);
    }

    #[test]
    fn register_source_defaults_disabled_then_enable_toggles_it() {
        let program_id = address(200);
        let mut accounts = vec![
            external_account(30, true, true),
            account(20, CONFIG_LEN, false, true),
            account(21, DESTINATION_TOKEN_CONFIG_LEN, false, false),
            account(22, SOURCE_MINT_CONFIG_LEN, false, true),
            external_account(40, false, false),
            external_account(30, false, false),
            external_account(70, false, false),
            external_account(0, false, false),
        ];
        write_config_to(&mut accounts[1], &sample_config(30, 40));
        write_destination_to(&mut accounts[2], &destination());

        process_with_accounts(
            &program_id,
            &mut accounts,
            &Instruction::RegisterSourceMint {
                source_decimals: 6,
                migration_mode: MigrationMode::BurnToMint,
                price_mode: PriceMode::FixedRatio,
                fixed_ratio_source_amount: 1,
                fixed_ratio_destination_amount: 1,
                bump: 253,
            },
        )
        .expect("register source");

        let config = read_config_from(&mut accounts[1]);
        let source = read_source_from(&mut accounts[3]);
        assert_eq!(config.source_count, 1);
        assert_eq!(source.source_mint, [40; 32]);
        assert!(!source.enabled);

        let mut enable_accounts = vec![
            external_account(30, true, false),
            account(20, CONFIG_LEN, false, false),
            account(22, SOURCE_MINT_CONFIG_LEN, false, true),
            account(21, DESTINATION_TOKEN_CONFIG_LEN, false, false),
        ];
        write_config_to(&mut enable_accounts[1], &sample_config(30, 40));
        write_source_to(&mut enable_accounts[2], &source);
        write_destination_to(&mut enable_accounts[3], &destination());

        process_with_accounts(
            &program_id,
            &mut enable_accounts,
            &Instruction::SetSourceEnabled { enabled: true },
        )
        .expect("enable source");

        assert!(read_source_from(&mut enable_accounts[2]).enabled);
    }

    #[test]
    fn register_source_rejects_existing_config_account() {
        let program_id = address(200);
        let mut accounts = vec![
            external_account(30, true, true),
            account(20, CONFIG_LEN, false, true),
            account(21, DESTINATION_TOKEN_CONFIG_LEN, false, false),
            account(22, SOURCE_MINT_CONFIG_LEN, false, true),
            external_account(40, false, false),
            external_account(30, false, false),
            external_account(70, false, false),
            external_account(0, false, false),
        ];
        write_config_to(&mut accounts[1], &sample_config(30, 40));
        write_destination_to(&mut accounts[2], &destination());
        write_source_to(&mut accounts[3], &source(true));

        let err = process_with_accounts(
            &program_id,
            &mut accounts,
            &Instruction::RegisterSourceMint {
                source_decimals: 6,
                migration_mode: MigrationMode::BurnToMint,
                price_mode: PriceMode::FixedRatio,
                fixed_ratio_source_amount: 1,
                fixed_ratio_destination_amount: 1,
                bump: 253,
            },
        )
        .expect_err("existing source rejected");

        assert_eq!(err, ProgramError::Custom(ERR_ACCOUNT_ALREADY_INITIALIZED));
        assert_eq!(read_config_from(&mut accounts[1]).source_count, 0);
    }

    #[test]
    fn pause_updates_global_and_target_destination_status() {
        let program_id = address(200);
        let mut accounts = vec![
            external_account(40, true, false),
            account(20, CONFIG_LEN, false, true),
            account(21, DESTINATION_TOKEN_CONFIG_LEN, false, true),
        ];
        write_config_to(&mut accounts[1], &sample_config(30, 40));
        write_destination_to(&mut accounts[2], &destination());

        process_with_accounts(
            &program_id,
            &mut accounts,
            &Instruction::Pause { paused: true },
        )
        .expect("pause destination");

        assert_eq!(
            read_config_from(&mut accounts[1]).launch_status,
            LaunchStatus::Paused
        );
        assert_eq!(
            read_destination_from(&mut accounts[2]).status,
            LaunchStatus::Paused
        );
    }

    #[test]
    fn pause_authority_cannot_enable_source_on_unpause() {
        let program_id = address(200);
        let mut accounts = vec![
            external_account(40, true, false),
            account(20, CONFIG_LEN, false, true),
            account(22, SOURCE_MINT_CONFIG_LEN, false, true),
        ];
        write_config_to(&mut accounts[1], &sample_config(30, 40));
        write_source_to(&mut accounts[2], &source(false));

        process_with_accounts(
            &program_id,
            &mut accounts,
            &Instruction::Pause { paused: false },
        )
        .expect("unpause source no-op");

        assert!(!read_source_from(&mut accounts[2]).enabled);
    }

    #[test]
    fn finalize_source_disables_source() {
        let program_id = address(200);
        let mut accounts = vec![
            external_account(30, true, false),
            account(20, CONFIG_LEN, false, false),
            account(22, SOURCE_MINT_CONFIG_LEN, false, true),
            account(21, DESTINATION_TOKEN_CONFIG_LEN, false, false),
        ];
        write_config_to(&mut accounts[1], &sample_config(30, 40));
        write_source_to(&mut accounts[2], &source(true));
        write_destination_to(&mut accounts[3], &destination());

        process_with_accounts(&program_id, &mut accounts, &Instruction::FinalizeSourceMint)
            .expect("finalize source");

        let source = read_source_from(&mut accounts[2]);
        assert!(!source.enabled);
        assert!(source.finalized);
    }

    #[test]
    fn finalized_source_cannot_be_reenabled() {
        let program_id = address(200);
        let mut finalized_source = source(false);
        finalized_source.finalized = true;
        let mut accounts = vec![
            external_account(30, true, false),
            account(20, CONFIG_LEN, false, false),
            account(22, SOURCE_MINT_CONFIG_LEN, false, true),
            account(21, DESTINATION_TOKEN_CONFIG_LEN, false, false),
        ];
        write_config_to(&mut accounts[1], &sample_config(30, 40));
        write_source_to(&mut accounts[2], &finalized_source);
        write_destination_to(&mut accounts[3], &destination());

        let err = process_with_accounts(
            &program_id,
            &mut accounts,
            &Instruction::SetSourceEnabled { enabled: true },
        )
        .expect_err("enable finalized source");

        assert_eq!(err, ProgramError::Custom(ERR_SOURCE_FINALIZED));
        assert!(!read_source_from(&mut accounts[2]).enabled);
    }

    #[test]
    fn register_destination_rejects_wrong_admin() {
        let program_id = address(200);
        let mut accounts = vec![
            external_account(31, true, true),
            account(20, CONFIG_LEN, false, true),
            account(21, DESTINATION_TOKEN_CONFIG_LEN, false, true),
            external_account(30, false, true),
            external_account(50, false, false),
            external_account(60, false, false),
            external_account(0, false, false),
        ];
        write_config_to(&mut accounts[1], &sample_config(30, 40));

        let err = process_with_accounts(
            &program_id,
            &mut accounts,
            &Instruction::RegisterDestinationMint {
                token_id_hash: [8; 32],
                mint_vanity_nonce: 7,
                mint_bump: 254,
                decimals: 6,
                max_supply: 1_000_000,
                bonding_min: 1,
                bonding_range: 9,
                min_dest_amount: 1,
            },
        )
        .expect_err("wrong admin rejected");

        assert_eq!(err, ProgramError::Custom(ERR_AUTHORITY_MISMATCH));
    }

    #[test]
    fn transfer_authority_begin_and_accept_moves_admin() {
        let program_id = address(200);
        let mut begin_accounts = vec![
            external_account(30, true, false),
            account(20, CONFIG_LEN, false, true),
            external_account(80, false, false),
        ];
        write_config_to(&mut begin_accounts[1], &sample_config(30, 40));

        process_with_accounts(
            &program_id,
            &mut begin_accounts,
            &Instruction::TransferAuthorityBegin {
                pending_authority: [80; 32],
            },
        )
        .expect("begin authority transfer");

        let pending_config = read_config_from(&mut begin_accounts[1]);
        assert_eq!(pending_config.pending_authority, [80; 32]);
        assert!(pending_config.pending_authority_set);

        let mut accept_accounts = vec![
            external_account(80, true, false),
            account(20, CONFIG_LEN, false, true),
            external_account(30, false, false),
        ];
        write_config_to(&mut accept_accounts[1], &pending_config);

        process_with_accounts(
            &program_id,
            &mut accept_accounts,
            &Instruction::TransferAuthorityAccept,
        )
        .expect("accept authority transfer");

        let accepted = read_config_from(&mut accept_accounts[1]);
        assert_eq!(accepted.admin, [80; 32]);
        assert_eq!(accepted.pending_authority, [0; 32]);
        assert!(!accepted.pending_authority_set);
    }

    #[test]
    fn transfer_authority_accept_requires_pending_authority() {
        let program_id = address(200);
        let mut accounts = vec![
            external_account(80, true, false),
            account(20, CONFIG_LEN, false, true),
            external_account(30, false, false),
        ];
        write_config_to(&mut accounts[1], &sample_config(30, 40));

        let err = process_with_accounts(
            &program_id,
            &mut accounts,
            &Instruction::TransferAuthorityAccept,
        )
        .expect_err("missing pending authority rejected");

        assert_eq!(err, ProgramError::Custom(ERR_PENDING_AUTHORITY_NOT_SET));
    }

    #[test]
    fn retire_authority_zeroes_admin_pause_and_pending_authority() {
        let program_id = address(200);
        let mut accounts = vec![
            external_account(30, true, false),
            account(20, CONFIG_LEN, false, true),
            external_account(40, true, false),
        ];
        let mut config = sample_config(30, 40);
        config.pending_authority = [80; 32];
        config.pending_authority_set = true;
        write_config_to(&mut accounts[1], &config);

        process_with_accounts(&program_id, &mut accounts, &Instruction::RetireAuthority)
            .expect("retire authority");

        let retired = read_config_from(&mut accounts[1]);
        assert_eq!(retired.admin, [0; 32]);
        assert_eq!(retired.pause_authority, [0; 32]);
        assert_eq!(retired.pending_authority, [0; 32]);
        assert!(!retired.pending_authority_set);
    }

    #[test]
    fn retire_authority_requires_current_pause_authority() {
        let program_id = address(200);
        let mut accounts = vec![
            external_account(30, true, false),
            account(20, CONFIG_LEN, false, true),
            external_account(41, true, false),
        ];
        write_config_to(&mut accounts[1], &sample_config(30, 40));

        let err = process_with_accounts(&program_id, &mut accounts, &Instruction::RetireAuthority)
            .expect_err("wrong pause authority rejected");

        assert_eq!(err, ProgramError::Custom(ERR_AUTHORITY_MISMATCH));
        let config = read_config_from(&mut accounts[1]);
        assert_eq!(config.admin, [30; 32]);
        assert_eq!(config.pause_authority, [40; 32]);
    }

    #[test]
    fn retired_authorities_cannot_mutate_privileged_state() {
        let program_id = address(200);
        let mut config = sample_config(0, 0);
        config.launch_status = LaunchStatus::Enabled;

        let mut enable_accounts = vec![
            external_account(30, true, false),
            account(20, CONFIG_LEN, false, false),
            account(22, SOURCE_MINT_CONFIG_LEN, false, true),
            account(21, DESTINATION_TOKEN_CONFIG_LEN, false, false),
        ];
        write_config_to(&mut enable_accounts[1], &config);
        write_source_to(&mut enable_accounts[2], &source(false));
        write_destination_to(&mut enable_accounts[3], &destination());

        let err = process_with_accounts(
            &program_id,
            &mut enable_accounts,
            &Instruction::SetSourceEnabled { enabled: true },
        )
        .expect_err("retired admin rejected");
        assert_eq!(err, ProgramError::Custom(ERR_AUTHORITY_MISMATCH));
        assert!(!read_source_from(&mut enable_accounts[2]).enabled);

        let mut pause_accounts = vec![
            external_account(40, true, false),
            account(20, CONFIG_LEN, false, true),
            account(21, DESTINATION_TOKEN_CONFIG_LEN, false, true),
        ];
        write_config_to(&mut pause_accounts[1], &config);
        write_destination_to(&mut pause_accounts[2], &destination());

        let err = process_with_accounts(
            &program_id,
            &mut pause_accounts,
            &Instruction::Pause { paused: true },
        )
        .expect_err("retired pause authority rejected");
        assert_eq!(err, ProgramError::Custom(ERR_AUTHORITY_MISMATCH));
        assert_eq!(
            read_config_from(&mut pause_accounts[1]).launch_status,
            LaunchStatus::Enabled
        );
    }

    #[test]
    fn migration_still_works_after_authority_retirement() {
        let program_id = address(200);
        let mut config = sample_config(0, 0);
        config.launch_status = LaunchStatus::Enabled;
        let mut accounts = vec![
            external_account(10, true, false),
            account(20, CONFIG_LEN, false, false),
            account(22, SOURCE_MINT_CONFIG_LEN, false, true),
            account(21, DESTINATION_TOKEN_CONFIG_LEN, false, true),
            token_account(80, 70, 40, 10),
            token_account(81, 60, 30, 10),
            mint_account(40, 70, 6),
            mint_account(30, 60, 6),
            external_account(50, false, false),
            external_account(70, false, false),
            external_account(60, false, false),
        ];
        write_config_to(&mut accounts[1], &config);
        write_source_to(&mut accounts[2], &source(true));
        write_destination_to(&mut accounts[3], &destination());

        process_with_accounts(
            &program_id,
            &mut accounts,
            &Instruction::Migrate {
                desired_destination_amount: 77,
                max_source_amount: 77,
                user_nonce: 7,
                create_receipt: false,
            },
        )
        .expect("migrate after retirement");

        assert_eq!(read_source_from(&mut accounts[2]).migration_count, 1);
        assert_eq!(read_destination_from(&mut accounts[3]).total_minted, 77);
    }

    #[test]
    fn migrate_no_receipt_updates_counters_after_token_cpi() {
        let program_id = address(200);
        let mut accounts = vec![
            external_account(10, true, false),
            account(20, CONFIG_LEN, false, false),
            account(22, SOURCE_MINT_CONFIG_LEN, false, true),
            account(21, DESTINATION_TOKEN_CONFIG_LEN, false, true),
            token_account(80, 70, 40, 10),
            token_account(81, 60, 30, 10),
            mint_account(40, 70, 6),
            mint_account(30, 60, 6),
            external_account(50, false, false),
            external_account(70, false, false),
            external_account(60, false, false),
        ];
        write_config_to(&mut accounts[1], &sample_config(30, 40));
        write_source_to(&mut accounts[2], &source(true));
        write_destination_to(&mut accounts[3], &destination());

        process_with_accounts(
            &program_id,
            &mut accounts,
            &Instruction::Migrate {
                desired_destination_amount: 77,
                max_source_amount: 77,
                user_nonce: 7,
                create_receipt: false,
            },
        )
        .expect("migrate");

        let migrated_source = read_source_from(&mut accounts[2]);
        let migrated_destination = read_destination_from(&mut accounts[3]);
        assert_eq!(migrated_source.burned_base_units, 77);
        assert_eq!(migrated_source.minted_destination_base_units, 77);
        assert_eq!(migrated_source.migration_count, 1);
        assert_eq!(migrated_destination.total_minted, 77);
    }

    #[test]
    fn migrate_rejects_global_pause() {
        let program_id = address(200);
        let mut accounts = vec![
            external_account(10, true, false),
            account(20, CONFIG_LEN, false, false),
            account(22, SOURCE_MINT_CONFIG_LEN, false, true),
            account(21, DESTINATION_TOKEN_CONFIG_LEN, false, true),
            token_account(80, 70, 40, 10),
            token_account(81, 60, 30, 10),
            mint_account(40, 70, 6),
            mint_account(30, 60, 6),
            external_account(50, false, false),
            external_account(70, false, false),
            external_account(60, false, false),
        ];
        let mut config = sample_config(30, 40);
        config.launch_status = LaunchStatus::Paused;
        write_config_to(&mut accounts[1], &config);
        write_source_to(&mut accounts[2], &source(true));
        write_destination_to(&mut accounts[3], &destination());

        let err = process_with_accounts(
            &program_id,
            &mut accounts,
            &Instruction::Migrate {
                desired_destination_amount: 77,
                max_source_amount: 77,
                user_nonce: 7,
                create_receipt: false,
            },
        )
        .expect_err("global pause rejected");

        assert_eq!(err, ProgramError::Custom(ERR_PROGRAM_PAUSED));
    }

    #[test]
    #[cfg(feature = "receipts")]
    fn migrate_receipt_path_writes_receipt_record() {
        let program_id = address(200);
        let mut accounts = vec![
            external_account(10, true, false),
            account(20, CONFIG_LEN, false, false),
            account(22, SOURCE_MINT_CONFIG_LEN, false, true),
            account(21, DESTINATION_TOKEN_CONFIG_LEN, false, true),
            token_account(80, 70, 40, 10),
            token_account(81, 60, 30, 10),
            mint_account(40, 70, 6),
            mint_account(30, 60, 6),
            external_account(50, false, false),
            external_account(70, false, false),
            external_account(60, false, false),
            account(90, RECEIPT_LEN, false, true),
            external_account(1, false, false),
        ];
        write_config_to(&mut accounts[1], &sample_config(30, 40));
        write_source_to(&mut accounts[2], &source(true));
        write_destination_to(&mut accounts[3], &destination());

        process_with_accounts(
            &program_id,
            &mut accounts,
            &Instruction::Migrate {
                desired_destination_amount: 77,
                max_source_amount: 77,
                user_nonce: 7,
                create_receipt: true,
            },
        )
        .expect("receipt path");

        let receipt = read_receipt_from(&mut accounts[11]);
        assert_eq!(receipt.user, [10; 32]);
        assert_eq!(receipt.source_mint, [40; 32]);
        assert_eq!(receipt.destination_mint, [30; 32]);
        assert_eq!(receipt.source_amount_burned, 77);
        assert_eq!(receipt.destination_amount_minted, 77);
        assert_eq!(receipt.user_nonce, 7);
    }

    #[cfg(not(feature = "receipts"))]
    #[test]
    fn migrate_receipt_path_is_disabled_in_low_cost_build() {
        let program_id = address(200);
        let mut accounts = vec![
            external_account(10, true, false),
            account(20, CONFIG_LEN, false, false),
            account(22, SOURCE_MINT_CONFIG_LEN, false, true),
            account(21, DESTINATION_TOKEN_CONFIG_LEN, false, true),
            token_account(80, 70, 40, 10),
            token_account(81, 60, 30, 10),
            mint_account(40, 70, 6),
            mint_account(30, 60, 6),
            external_account(50, false, false),
            external_account(70, false, false),
            external_account(60, false, false),
            account(90, 128, false, true),
            external_account(1, false, false),
        ];
        write_config_to(&mut accounts[1], &sample_config(30, 40));
        write_source_to(&mut accounts[2], &source(true));
        write_destination_to(&mut accounts[3], &destination());

        let err = process_with_accounts(
            &program_id,
            &mut accounts,
            &Instruction::Migrate {
                desired_destination_amount: 77,
                max_source_amount: 77,
                user_nonce: 7,
                create_receipt: true,
            },
        )
        .expect_err("receipt path disabled in low-cost build");

        assert_eq!(err, ProgramError::Custom(ERR_EXECUTOR_NOT_IMPLEMENTED));
    }

    #[test]
    fn migrate_rejects_wrong_destination_token_account_mint() {
        let program_id = address(200);
        let mut accounts = vec![
            external_account(10, true, false),
            account(20, CONFIG_LEN, false, false),
            account(22, SOURCE_MINT_CONFIG_LEN, false, true),
            account(21, DESTINATION_TOKEN_CONFIG_LEN, false, true),
            token_account(80, 70, 40, 10),
            token_account(81, 60, 31, 10),
            mint_account(40, 70, 6),
            mint_account(30, 60, 6),
            external_account(50, false, false),
            external_account(70, false, false),
            external_account(60, false, false),
        ];
        write_config_to(&mut accounts[1], &sample_config(30, 40));
        write_source_to(&mut accounts[2], &source(true));
        write_destination_to(&mut accounts[3], &destination());

        let err = process_with_accounts(
            &program_id,
            &mut accounts,
            &Instruction::Migrate {
                desired_destination_amount: 77,
                max_source_amount: 77,
                user_nonce: 7,
                create_receipt: false,
            },
        )
        .expect_err("wrong token account mint rejected");

        assert_eq!(err, ProgramError::Custom(ERR_TOKEN_ACCOUNT_MINT_MISMATCH));
    }

    fn process_instruction_stub(
        accounts: &[AccountView],
        instruction_data: &[u8],
    ) -> Result<(), ProgramError> {
        let instruction = Instruction::unpack(instruction_data).map_err(map_core_error)?;
        validate_account_rules(accounts, expected_account_rules(&instruction))?;
        Ok(())
    }
}
