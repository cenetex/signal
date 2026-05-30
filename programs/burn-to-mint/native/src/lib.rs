#![cfg_attr(not(test), no_std)]
#![forbid(unsafe_code)]

pub const PUBKEY_LEN: usize = 32;
pub const TOKEN_ID_HASH_LEN: usize = 32;
pub const ACCOUNT_VERSION: u8 = 1;

pub type PubkeyBytes = [u8; PUBKEY_LEN];
pub type TokenIdHash = [u8; TOKEN_ID_HASH_LEN];

pub const CONFIG_LEN: usize = 116;
pub const DESTINATION_TOKEN_CONFIG_LEN: usize = 188;
pub const SOURCE_MINT_CONFIG_LEN: usize = 150;
pub const RECEIPT_LEN: usize = 130;
pub const PROTOCOL_FEE_BPS: u16 = 0;
pub const DIRECT_PROTOCOL_REVENUE_ENABLED: bool = false;

pub const INITIALIZE_CONFIG_IX_LEN: usize = 2;
pub const REGISTER_DESTINATION_MINT_IX_LEN: usize = 75;
pub const REGISTER_SOURCE_MINT_IX_LEN: usize = 21;
pub const SET_SOURCE_ENABLED_IX_LEN: usize = 2;
pub const MIGRATE_IX_LEN: usize = 26;
pub const PAUSE_IX_LEN: usize = 2;
pub const FINALIZE_SOURCE_MINT_IX_LEN: usize = 1;
pub const TRANSFER_AUTHORITY_BEGIN_IX_LEN: usize = 33;
pub const TRANSFER_AUTHORITY_ACCEPT_IX_LEN: usize = 1;
pub const RETIRE_AUTHORITY_IX_LEN: usize = 1;
pub const MAX_INSTRUCTION_LEN: usize = REGISTER_DESTINATION_MINT_IX_LEN;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ProgramError {
    InvalidLength,
    InvalidVersion,
    InvalidDiscriminant,
    InvalidBool,
    ZeroAmount,
    SourceDisabled,
    SourceDestinationMismatch,
    DestinationNotEnabled,
    DestinationFinalized,
    DestinationPaused,
    InvalidRatio,
    FractionalRatio,
    DivisionByZero,
    SlippageExceeded,
    DestinationAmountTooSmall,
    SupplyCapExceeded,
    UnsupportedMigrationMode,
    ArithmeticOverflow,
}

pub type Result<T> = core::result::Result<T, ProgramError>;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub enum LaunchStatus {
    Planned = 0,
    Candidate = 1,
    Enabled = 2,
    Paused = 3,
    Finalized = 4,
}

impl TryFrom<u8> for LaunchStatus {
    type Error = ProgramError;

    fn try_from(value: u8) -> Result<Self> {
        match value {
            0 => Ok(Self::Planned),
            1 => Ok(Self::Candidate),
            2 => Ok(Self::Enabled),
            3 => Ok(Self::Paused),
            4 => Ok(Self::Finalized),
            _ => Err(ProgramError::InvalidDiscriminant),
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub enum MigrationMode {
    BurnToMint = 0,
    ProofOfBurnBadgeOnly = 1,
}

impl TryFrom<u8> for MigrationMode {
    type Error = ProgramError;

    fn try_from(value: u8) -> Result<Self> {
        match value {
            0 => Ok(Self::BurnToMint),
            1 => Ok(Self::ProofOfBurnBadgeOnly),
            _ => Err(ProgramError::InvalidDiscriminant),
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub enum PriceMode {
    FixedRatio = 0,
    BondingCurve = 1,
}

impl TryFrom<u8> for PriceMode {
    type Error = ProgramError;

    fn try_from(value: u8) -> Result<Self> {
        match value {
            0 => Ok(Self::FixedRatio),
            1 => Ok(Self::BondingCurve),
            _ => Err(ProgramError::InvalidDiscriminant),
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Config {
    pub admin: PubkeyBytes,
    pub pause_authority: PubkeyBytes,
    pub launch_status: LaunchStatus,
    pub destination_count: u32,
    pub source_count: u32,
    pub config_bump: u8,
    pub pending_authority: PubkeyBytes,
    pub pending_authority_set: bool,
    pub reserved: [u8; 8],
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct DestinationTokenConfig {
    pub destination_mint: PubkeyBytes,
    pub mint_vanity_nonce: u64,
    pub mint_bump: u8,
    pub token_id_hash: TokenIdHash,
    pub decimals: u8,
    pub token_program: PubkeyBytes,
    pub mint_authority: PubkeyBytes,
    pub max_supply: u64,
    pub total_minted: u64,
    pub bonding_min: u64,
    pub bonding_range: u64,
    pub min_dest_amount: u64,
    pub status: LaunchStatus,
    pub reserved: [u8; 8],
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct SourceMintConfig {
    pub source_mint: PubkeyBytes,
    pub destination_mint: PubkeyBytes,
    pub source_token_program: PubkeyBytes,
    pub source_decimals: u8,
    pub migration_mode: MigrationMode,
    pub price_mode: PriceMode,
    pub fixed_ratio_source_amount: u64,
    pub fixed_ratio_destination_amount: u64,
    pub enabled: bool,
    pub burned_base_units: u64,
    pub minted_destination_base_units: u64,
    pub migration_count: u64,
    pub bump: u8,
    pub finalized: bool,
    pub reserved: [u8; 7],
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Receipt {
    pub user: PubkeyBytes,
    pub source_mint: PubkeyBytes,
    pub destination_mint: PubkeyBytes,
    pub source_amount_burned: u64,
    pub destination_amount_minted: u64,
    pub slot: u64,
    pub user_nonce: u64,
    pub bump: u8,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub enum InstructionDiscriminant {
    InitializeConfig = 0,
    RegisterDestinationMint = 1,
    RegisterSourceMint = 2,
    SetSourceEnabled = 3,
    Migrate = 4,
    Pause = 5,
    FinalizeSourceMint = 6,
    TransferAuthorityBegin = 7,
    TransferAuthorityAccept = 8,
    RetireAuthority = 9,
}

impl TryFrom<u8> for InstructionDiscriminant {
    type Error = ProgramError;

    fn try_from(value: u8) -> Result<Self> {
        match value {
            0 => Ok(Self::InitializeConfig),
            1 => Ok(Self::RegisterDestinationMint),
            2 => Ok(Self::RegisterSourceMint),
            3 => Ok(Self::SetSourceEnabled),
            4 => Ok(Self::Migrate),
            5 => Ok(Self::Pause),
            6 => Ok(Self::FinalizeSourceMint),
            7 => Ok(Self::TransferAuthorityBegin),
            8 => Ok(Self::TransferAuthorityAccept),
            9 => Ok(Self::RetireAuthority),
            _ => Err(ProgramError::InvalidDiscriminant),
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Instruction {
    InitializeConfig {
        config_bump: u8,
    },
    RegisterDestinationMint {
        token_id_hash: TokenIdHash,
        mint_vanity_nonce: u64,
        mint_bump: u8,
        decimals: u8,
        max_supply: u64,
        bonding_min: u64,
        bonding_range: u64,
        min_dest_amount: u64,
    },
    RegisterSourceMint {
        source_decimals: u8,
        migration_mode: MigrationMode,
        price_mode: PriceMode,
        fixed_ratio_source_amount: u64,
        fixed_ratio_destination_amount: u64,
        bump: u8,
    },
    SetSourceEnabled {
        enabled: bool,
    },
    Migrate {
        desired_destination_amount: u64,
        max_source_amount: u64,
        user_nonce: u64,
        create_receipt: bool,
    },
    Pause {
        paused: bool,
    },
    FinalizeSourceMint,
    TransferAuthorityBegin {
        pending_authority: PubkeyBytes,
    },
    TransferAuthorityAccept,
    RetireAuthority,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct MigrationQuote {
    pub source_amount_to_burn: u64,
    pub destination_amount_to_mint: u64,
}

pub fn migrate(
    destination: &mut DestinationTokenConfig,
    source: &mut SourceMintConfig,
    desired_destination_amount: u64,
    max_source_amount: u64,
) -> Result<MigrationQuote> {
    let quote = quote_migration(
        destination,
        source,
        desired_destination_amount,
        max_source_amount,
    )?;
    apply_migration_counters(destination, source, quote)?;
    Ok(quote)
}

pub fn quote_migration(
    destination: &DestinationTokenConfig,
    source: &SourceMintConfig,
    desired_destination_amount: u64,
    max_source_amount: u64,
) -> Result<MigrationQuote> {
    if desired_destination_amount == 0 || max_source_amount == 0 {
        return Err(ProgramError::ZeroAmount);
    }
    if source.finalized || !source.enabled {
        return Err(ProgramError::SourceDisabled);
    }
    if source.migration_mode != MigrationMode::BurnToMint {
        return Err(ProgramError::UnsupportedMigrationMode);
    }
    if source.destination_mint != destination.destination_mint {
        return Err(ProgramError::SourceDestinationMismatch);
    }
    if destination.status != LaunchStatus::Enabled {
        if destination.status == LaunchStatus::Finalized {
            return Err(ProgramError::DestinationFinalized);
        }
        if destination.status == LaunchStatus::Paused {
            return Err(ProgramError::DestinationPaused);
        }
        return Err(ProgramError::DestinationNotEnabled);
    }
    if desired_destination_amount < destination.min_dest_amount {
        return Err(ProgramError::DestinationAmountTooSmall);
    }

    let next_total = destination
        .total_minted
        .checked_add(desired_destination_amount)
        .ok_or(ProgramError::ArithmeticOverflow)?;
    if destination.max_supply != 0 && next_total > destination.max_supply {
        return Err(ProgramError::SupplyCapExceeded);
    }

    let source_amount = match source.price_mode {
        PriceMode::FixedRatio => fixed_ratio_source_amount(
            desired_destination_amount,
            source.fixed_ratio_source_amount,
            source.fixed_ratio_destination_amount,
        )?,
        PriceMode::BondingCurve => bonding_curve_source_amount(
            destination.total_minted,
            desired_destination_amount,
            destination.max_supply,
            destination.bonding_min,
            destination.bonding_range,
        )?,
    };

    if source_amount == 0 {
        return Err(ProgramError::ZeroAmount);
    }
    if source_amount > max_source_amount {
        return Err(ProgramError::SlippageExceeded);
    }

    Ok(MigrationQuote {
        source_amount_to_burn: source_amount,
        destination_amount_to_mint: desired_destination_amount,
    })
}

pub fn apply_migration_counters(
    destination: &mut DestinationTokenConfig,
    source: &mut SourceMintConfig,
    quote: MigrationQuote,
) -> Result<()> {
    destination.total_minted = destination
        .total_minted
        .checked_add(quote.destination_amount_to_mint)
        .ok_or(ProgramError::ArithmeticOverflow)?;
    source.burned_base_units = source
        .burned_base_units
        .checked_add(quote.source_amount_to_burn)
        .ok_or(ProgramError::ArithmeticOverflow)?;
    source.minted_destination_base_units = source
        .minted_destination_base_units
        .checked_add(quote.destination_amount_to_mint)
        .ok_or(ProgramError::ArithmeticOverflow)?;
    source.migration_count = source
        .migration_count
        .checked_add(1)
        .ok_or(ProgramError::ArithmeticOverflow)?;
    Ok(())
}

pub fn fixed_ratio_source_amount(
    destination_amount: u64,
    source_ratio: u64,
    destination_ratio: u64,
) -> Result<u64> {
    if source_ratio == 0 || destination_ratio == 0 {
        return Err(ProgramError::InvalidRatio);
    }
    let product = (destination_amount as u128)
        .checked_mul(source_ratio as u128)
        .ok_or(ProgramError::ArithmeticOverflow)?;
    let divisor = destination_ratio as u128;
    if product % divisor != 0 {
        return Err(ProgramError::FractionalRatio);
    }
    u64::try_from(product / divisor).map_err(|_| ProgramError::ArithmeticOverflow)
}

pub fn bonding_curve_source_amount(
    current_minted: u64,
    destination_amount: u64,
    max_supply: u64,
    bonding_min: u64,
    bonding_range: u64,
) -> Result<u64> {
    if max_supply == 0 {
        return Err(ProgramError::DivisionByZero);
    }
    let next_total = current_minted
        .checked_add(destination_amount)
        .ok_or(ProgramError::ArithmeticOverflow)?;
    if next_total > max_supply {
        return Err(ProgramError::SupplyCapExceeded);
    }
    let numerator = (bonding_range as u128)
        .checked_mul(
            (current_minted as u128)
                .checked_mul(2)
                .and_then(|value| value.checked_add(destination_amount as u128))
                .ok_or(ProgramError::ArithmeticOverflow)?,
        )
        .ok_or(ProgramError::ArithmeticOverflow)?;
    let denominator = (max_supply as u128)
        .checked_mul(2)
        .ok_or(ProgramError::ArithmeticOverflow)?;
    let avg_price = (bonding_min as u128)
        .checked_add(numerator / denominator)
        .ok_or(ProgramError::ArithmeticOverflow)?;
    let source = (destination_amount as u128)
        .checked_mul(avg_price)
        .ok_or(ProgramError::ArithmeticOverflow)?;
    u64::try_from(source).map_err(|_| ProgramError::ArithmeticOverflow)
}

impl Config {
    pub fn pack(&self, out: &mut [u8]) -> Result<()> {
        if out.len() != CONFIG_LEN {
            return Err(ProgramError::InvalidLength);
        }
        out[0] = ACCOUNT_VERSION;
        out[1..33].copy_from_slice(&self.admin);
        out[33..65].copy_from_slice(&self.pause_authority);
        out[65] = self.launch_status as u8;
        write_u32(out, 66, self.destination_count);
        write_u32(out, 70, self.source_count);
        out[74] = self.config_bump;
        out[75..107].copy_from_slice(&self.pending_authority);
        out[107] = bool_to_u8(self.pending_authority_set);
        out[108..116].copy_from_slice(&self.reserved);
        Ok(())
    }

    pub fn unpack(input: &[u8]) -> Result<Self> {
        if input.len() != CONFIG_LEN {
            return Err(ProgramError::InvalidLength);
        }
        require_version(input[0])?;
        Ok(Self {
            admin: read_pubkey(input, 1)?,
            pause_authority: read_pubkey(input, 33)?,
            launch_status: LaunchStatus::try_from(input[65])?,
            destination_count: read_u32(input, 66)?,
            source_count: read_u32(input, 70)?,
            config_bump: input[74],
            pending_authority: read_pubkey(input, 75)?,
            pending_authority_set: read_bool(input[107])?,
            reserved: read_array::<8>(input, 108)?,
        })
    }
}

impl DestinationTokenConfig {
    pub fn pack(&self, out: &mut [u8]) -> Result<()> {
        if out.len() != DESTINATION_TOKEN_CONFIG_LEN {
            return Err(ProgramError::InvalidLength);
        }
        out[0] = ACCOUNT_VERSION;
        out[1..33].copy_from_slice(&self.destination_mint);
        write_u64(out, 33, self.mint_vanity_nonce);
        out[41] = self.mint_bump;
        out[42..74].copy_from_slice(&self.token_id_hash);
        out[74] = self.decimals;
        out[75..107].copy_from_slice(&self.token_program);
        out[107..139].copy_from_slice(&self.mint_authority);
        write_u64(out, 139, self.max_supply);
        write_u64(out, 147, self.total_minted);
        write_u64(out, 155, self.bonding_min);
        write_u64(out, 163, self.bonding_range);
        write_u64(out, 171, self.min_dest_amount);
        out[179] = self.status as u8;
        out[180..188].copy_from_slice(&self.reserved);
        Ok(())
    }

    pub fn unpack(input: &[u8]) -> Result<Self> {
        if input.len() != DESTINATION_TOKEN_CONFIG_LEN {
            return Err(ProgramError::InvalidLength);
        }
        require_version(input[0])?;
        Ok(Self {
            destination_mint: read_pubkey(input, 1)?,
            mint_vanity_nonce: read_u64(input, 33)?,
            mint_bump: input[41],
            token_id_hash: read_array::<32>(input, 42)?,
            decimals: input[74],
            token_program: read_pubkey(input, 75)?,
            mint_authority: read_pubkey(input, 107)?,
            max_supply: read_u64(input, 139)?,
            total_minted: read_u64(input, 147)?,
            bonding_min: read_u64(input, 155)?,
            bonding_range: read_u64(input, 163)?,
            min_dest_amount: read_u64(input, 171)?,
            status: LaunchStatus::try_from(input[179])?,
            reserved: read_array::<8>(input, 180)?,
        })
    }
}

impl SourceMintConfig {
    pub fn pack(&self, out: &mut [u8]) -> Result<()> {
        if out.len() != SOURCE_MINT_CONFIG_LEN {
            return Err(ProgramError::InvalidLength);
        }
        out[0] = ACCOUNT_VERSION;
        out[1..33].copy_from_slice(&self.source_mint);
        out[33..65].copy_from_slice(&self.destination_mint);
        out[65..97].copy_from_slice(&self.source_token_program);
        out[97] = self.source_decimals;
        out[98] = self.migration_mode as u8;
        out[99] = self.price_mode as u8;
        write_u64(out, 100, self.fixed_ratio_source_amount);
        write_u64(out, 108, self.fixed_ratio_destination_amount);
        out[116] = bool_to_u8(self.enabled);
        write_u64(out, 117, self.burned_base_units);
        write_u64(out, 125, self.minted_destination_base_units);
        write_u64(out, 133, self.migration_count);
        out[141] = self.bump;
        out[142] = bool_to_u8(self.finalized);
        out[143..150].copy_from_slice(&self.reserved);
        Ok(())
    }

    pub fn unpack(input: &[u8]) -> Result<Self> {
        if input.len() != SOURCE_MINT_CONFIG_LEN {
            return Err(ProgramError::InvalidLength);
        }
        require_version(input[0])?;
        Ok(Self {
            source_mint: read_pubkey(input, 1)?,
            destination_mint: read_pubkey(input, 33)?,
            source_token_program: read_pubkey(input, 65)?,
            source_decimals: input[97],
            migration_mode: MigrationMode::try_from(input[98])?,
            price_mode: PriceMode::try_from(input[99])?,
            fixed_ratio_source_amount: read_u64(input, 100)?,
            fixed_ratio_destination_amount: read_u64(input, 108)?,
            enabled: read_bool(input[116])?,
            burned_base_units: read_u64(input, 117)?,
            minted_destination_base_units: read_u64(input, 125)?,
            migration_count: read_u64(input, 133)?,
            bump: input[141],
            finalized: read_bool(input[142])?,
            reserved: read_array::<7>(input, 143)?,
        })
    }
}

impl Receipt {
    pub fn pack(&self, out: &mut [u8]) -> Result<()> {
        if out.len() != RECEIPT_LEN {
            return Err(ProgramError::InvalidLength);
        }
        out[0] = ACCOUNT_VERSION;
        out[1..33].copy_from_slice(&self.user);
        out[33..65].copy_from_slice(&self.source_mint);
        out[65..97].copy_from_slice(&self.destination_mint);
        write_u64(out, 97, self.source_amount_burned);
        write_u64(out, 105, self.destination_amount_minted);
        write_u64(out, 113, self.slot);
        write_u64(out, 121, self.user_nonce);
        out[129] = self.bump;
        Ok(())
    }

    pub fn unpack(input: &[u8]) -> Result<Self> {
        if input.len() != RECEIPT_LEN {
            return Err(ProgramError::InvalidLength);
        }
        require_version(input[0])?;
        Ok(Self {
            user: read_pubkey(input, 1)?,
            source_mint: read_pubkey(input, 33)?,
            destination_mint: read_pubkey(input, 65)?,
            source_amount_burned: read_u64(input, 97)?,
            destination_amount_minted: read_u64(input, 105)?,
            slot: read_u64(input, 113)?,
            user_nonce: read_u64(input, 121)?,
            bump: input[129],
        })
    }
}

impl Instruction {
    pub fn packed_len(&self) -> usize {
        match self {
            Self::InitializeConfig { .. } => INITIALIZE_CONFIG_IX_LEN,
            Self::RegisterDestinationMint { .. } => REGISTER_DESTINATION_MINT_IX_LEN,
            Self::RegisterSourceMint { .. } => REGISTER_SOURCE_MINT_IX_LEN,
            Self::SetSourceEnabled { .. } => SET_SOURCE_ENABLED_IX_LEN,
            Self::Migrate { .. } => MIGRATE_IX_LEN,
            Self::Pause { .. } => PAUSE_IX_LEN,
            Self::FinalizeSourceMint => FINALIZE_SOURCE_MINT_IX_LEN,
            Self::TransferAuthorityBegin { .. } => TRANSFER_AUTHORITY_BEGIN_IX_LEN,
            Self::TransferAuthorityAccept => TRANSFER_AUTHORITY_ACCEPT_IX_LEN,
            Self::RetireAuthority => RETIRE_AUTHORITY_IX_LEN,
        }
    }

    pub fn pack(&self, out: &mut [u8]) -> Result<()> {
        if out.len() != self.packed_len() {
            return Err(ProgramError::InvalidLength);
        }
        match self {
            Self::InitializeConfig { config_bump } => {
                out[0] = InstructionDiscriminant::InitializeConfig as u8;
                out[1] = *config_bump;
            }
            Self::RegisterDestinationMint {
                token_id_hash,
                mint_vanity_nonce,
                mint_bump,
                decimals,
                max_supply,
                bonding_min,
                bonding_range,
                min_dest_amount,
            } => {
                out[0] = InstructionDiscriminant::RegisterDestinationMint as u8;
                out[1..33].copy_from_slice(token_id_hash);
                write_u64(out, 33, *mint_vanity_nonce);
                out[41] = *mint_bump;
                out[42] = *decimals;
                write_u64(out, 43, *max_supply);
                write_u64(out, 51, *bonding_min);
                write_u64(out, 59, *bonding_range);
                write_u64(out, 67, *min_dest_amount);
            }
            Self::RegisterSourceMint {
                source_decimals,
                migration_mode,
                price_mode,
                fixed_ratio_source_amount,
                fixed_ratio_destination_amount,
                bump,
            } => {
                out[0] = InstructionDiscriminant::RegisterSourceMint as u8;
                out[1] = *source_decimals;
                out[2] = *migration_mode as u8;
                out[3] = *price_mode as u8;
                write_u64(out, 4, *fixed_ratio_source_amount);
                write_u64(out, 12, *fixed_ratio_destination_amount);
                out[20] = *bump;
            }
            Self::SetSourceEnabled { enabled } => {
                out[0] = InstructionDiscriminant::SetSourceEnabled as u8;
                out[1] = bool_to_u8(*enabled);
            }
            Self::Migrate {
                desired_destination_amount,
                max_source_amount,
                user_nonce,
                create_receipt,
            } => {
                out[0] = InstructionDiscriminant::Migrate as u8;
                write_u64(out, 1, *desired_destination_amount);
                write_u64(out, 9, *max_source_amount);
                write_u64(out, 17, *user_nonce);
                out[25] = bool_to_u8(*create_receipt);
            }
            Self::Pause { paused } => {
                out[0] = InstructionDiscriminant::Pause as u8;
                out[1] = bool_to_u8(*paused);
            }
            Self::FinalizeSourceMint => {
                out[0] = InstructionDiscriminant::FinalizeSourceMint as u8;
            }
            Self::TransferAuthorityBegin { pending_authority } => {
                out[0] = InstructionDiscriminant::TransferAuthorityBegin as u8;
                out[1..33].copy_from_slice(pending_authority);
            }
            Self::TransferAuthorityAccept => {
                out[0] = InstructionDiscriminant::TransferAuthorityAccept as u8;
            }
            Self::RetireAuthority => {
                out[0] = InstructionDiscriminant::RetireAuthority as u8;
            }
        }
        Ok(())
    }

    pub fn unpack(input: &[u8]) -> Result<Self> {
        if input.is_empty() {
            return Err(ProgramError::InvalidLength);
        }
        let discriminant = InstructionDiscriminant::try_from(input[0])?;
        match discriminant {
            InstructionDiscriminant::InitializeConfig => {
                require_len(input, INITIALIZE_CONFIG_IX_LEN)?;
                Ok(Self::InitializeConfig {
                    config_bump: input[1],
                })
            }
            InstructionDiscriminant::RegisterDestinationMint => {
                require_len(input, REGISTER_DESTINATION_MINT_IX_LEN)?;
                Ok(Self::RegisterDestinationMint {
                    token_id_hash: read_array::<32>(input, 1)?,
                    mint_vanity_nonce: read_u64(input, 33)?,
                    mint_bump: input[41],
                    decimals: input[42],
                    max_supply: read_u64(input, 43)?,
                    bonding_min: read_u64(input, 51)?,
                    bonding_range: read_u64(input, 59)?,
                    min_dest_amount: read_u64(input, 67)?,
                })
            }
            InstructionDiscriminant::RegisterSourceMint => {
                require_len(input, REGISTER_SOURCE_MINT_IX_LEN)?;
                Ok(Self::RegisterSourceMint {
                    source_decimals: input[1],
                    migration_mode: MigrationMode::try_from(input[2])?,
                    price_mode: PriceMode::try_from(input[3])?,
                    fixed_ratio_source_amount: read_u64(input, 4)?,
                    fixed_ratio_destination_amount: read_u64(input, 12)?,
                    bump: input[20],
                })
            }
            InstructionDiscriminant::SetSourceEnabled => {
                require_len(input, SET_SOURCE_ENABLED_IX_LEN)?;
                Ok(Self::SetSourceEnabled {
                    enabled: read_bool(input[1])?,
                })
            }
            InstructionDiscriminant::Migrate => {
                require_len(input, MIGRATE_IX_LEN)?;
                Ok(Self::Migrate {
                    desired_destination_amount: read_u64(input, 1)?,
                    max_source_amount: read_u64(input, 9)?,
                    user_nonce: read_u64(input, 17)?,
                    create_receipt: read_bool(input[25])?,
                })
            }
            InstructionDiscriminant::Pause => {
                require_len(input, PAUSE_IX_LEN)?;
                Ok(Self::Pause {
                    paused: read_bool(input[1])?,
                })
            }
            InstructionDiscriminant::FinalizeSourceMint => {
                require_len(input, FINALIZE_SOURCE_MINT_IX_LEN)?;
                Ok(Self::FinalizeSourceMint)
            }
            InstructionDiscriminant::TransferAuthorityBegin => {
                require_len(input, TRANSFER_AUTHORITY_BEGIN_IX_LEN)?;
                Ok(Self::TransferAuthorityBegin {
                    pending_authority: read_pubkey(input, 1)?,
                })
            }
            InstructionDiscriminant::TransferAuthorityAccept => {
                require_len(input, TRANSFER_AUTHORITY_ACCEPT_IX_LEN)?;
                Ok(Self::TransferAuthorityAccept)
            }
            InstructionDiscriminant::RetireAuthority => {
                require_len(input, RETIRE_AUTHORITY_IX_LEN)?;
                Ok(Self::RetireAuthority)
            }
        }
    }
}

fn require_version(version: u8) -> Result<()> {
    if version == ACCOUNT_VERSION {
        Ok(())
    } else {
        Err(ProgramError::InvalidVersion)
    }
}

fn require_len(input: &[u8], expected: usize) -> Result<()> {
    if input.len() == expected {
        Ok(())
    } else {
        Err(ProgramError::InvalidLength)
    }
}

fn read_array<const N: usize>(input: &[u8], offset: usize) -> Result<[u8; N]> {
    let end = offset
        .checked_add(N)
        .ok_or(ProgramError::ArithmeticOverflow)?;
    if input.len() < end {
        return Err(ProgramError::InvalidLength);
    }
    let mut out = [0u8; N];
    out.copy_from_slice(&input[offset..end]);
    Ok(out)
}

fn read_pubkey(input: &[u8], offset: usize) -> Result<PubkeyBytes> {
    read_array::<PUBKEY_LEN>(input, offset)
}

fn read_u32(input: &[u8], offset: usize) -> Result<u32> {
    Ok(u32::from_le_bytes(read_array::<4>(input, offset)?))
}

fn read_u64(input: &[u8], offset: usize) -> Result<u64> {
    Ok(u64::from_le_bytes(read_array::<8>(input, offset)?))
}

fn write_u32(out: &mut [u8], offset: usize, value: u32) {
    out[offset..offset + 4].copy_from_slice(&value.to_le_bytes());
}

fn write_u64(out: &mut [u8], offset: usize, value: u64) {
    out[offset..offset + 8].copy_from_slice(&value.to_le_bytes());
}

fn bool_to_u8(value: bool) -> u8 {
    if value {
        1
    } else {
        0
    }
}

fn read_bool(value: u8) -> Result<bool> {
    match value {
        0 => Ok(false),
        1 => Ok(true),
        _ => Err(ProgramError::InvalidBool),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn key(byte: u8) -> PubkeyBytes {
        [byte; PUBKEY_LEN]
    }

    fn destination() -> DestinationTokenConfig {
        DestinationTokenConfig {
            destination_mint: key(1),
            mint_vanity_nonce: 42,
            mint_bump: 253,
            token_id_hash: [9; TOKEN_ID_HASH_LEN],
            decimals: 6,
            token_program: key(2),
            mint_authority: key(3),
            max_supply: 1_000_000,
            total_minted: 10_000,
            bonding_min: 1,
            bonding_range: 9,
            min_dest_amount: 1,
            status: LaunchStatus::Enabled,
            reserved: [0; 8],
        }
    }

    fn source(price_mode: PriceMode) -> SourceMintConfig {
        SourceMintConfig {
            source_mint: key(4),
            destination_mint: key(1),
            source_token_program: key(2),
            source_decimals: 6,
            migration_mode: MigrationMode::BurnToMint,
            price_mode,
            fixed_ratio_source_amount: 1,
            fixed_ratio_destination_amount: 1,
            enabled: true,
            burned_base_units: 0,
            minted_destination_base_units: 0,
            migration_count: 0,
            bump: 252,
            finalized: false,
            reserved: [0; 7],
        }
    }

    #[test]
    fn fixed_ratio_quote_is_exact_for_ruby_parity() {
        let quote = quote_migration(
            &destination(),
            &source(PriceMode::FixedRatio),
            100_000,
            100_000,
        )
        .expect("fixed ratio quote");
        assert_eq!(quote.source_amount_to_burn, 100_000);
        assert_eq!(quote.destination_amount_to_mint, 100_000);
    }

    #[test]
    fn protocol_fee_is_not_part_of_migration_quote() {
        let quote = quote_migration(
            &destination(),
            &source(PriceMode::FixedRatio),
            100_000,
            100_000,
        )
        .expect("fixed ratio quote");

        assert_eq!(PROTOCOL_FEE_BPS, 0);
        let direct_protocol_revenue_enabled = DIRECT_PROTOCOL_REVENUE_ENABLED;
        assert!(!direct_protocol_revenue_enabled);
        assert_eq!(
            quote.source_amount_to_burn,
            quote.destination_amount_to_mint
        );
    }

    #[test]
    fn fixed_ratio_rejects_fractional_amounts() {
        assert_eq!(
            fixed_ratio_source_amount(3, 1, 2),
            Err(ProgramError::FractionalRatio)
        );
    }

    #[test]
    fn quote_rejects_disabled_source() {
        let mut source = source(PriceMode::FixedRatio);
        source.enabled = false;
        assert_eq!(
            quote_migration(&destination(), &source, 1, 1),
            Err(ProgramError::SourceDisabled)
        );
    }

    #[test]
    fn quote_rejects_finalized_source() {
        let mut source = source(PriceMode::FixedRatio);
        source.finalized = true;
        assert_eq!(
            quote_migration(&destination(), &source, 1, 1),
            Err(ProgramError::SourceDisabled)
        );
    }

    #[test]
    fn quote_rejects_proof_only_source() {
        let mut source = source(PriceMode::FixedRatio);
        source.migration_mode = MigrationMode::ProofOfBurnBadgeOnly;
        assert_eq!(
            quote_migration(&destination(), &source, 1, 1),
            Err(ProgramError::UnsupportedMigrationMode)
        );
    }

    #[test]
    fn quote_rejects_source_destination_mismatch() {
        let mut source = source(PriceMode::FixedRatio);
        source.destination_mint = key(99);
        assert_eq!(
            quote_migration(&destination(), &source, 1, 1),
            Err(ProgramError::SourceDestinationMismatch)
        );
    }

    #[test]
    fn quote_distinguishes_paused_destination() {
        let mut destination = destination();
        destination.status = LaunchStatus::Paused;
        assert_eq!(
            quote_migration(&destination, &source(PriceMode::FixedRatio), 1, 1),
            Err(ProgramError::DestinationPaused)
        );
    }

    #[test]
    fn quote_enforces_slippage_ceiling() {
        assert_eq!(
            quote_migration(&destination(), &source(PriceMode::FixedRatio), 100, 99),
            Err(ProgramError::SlippageExceeded)
        );
    }

    #[test]
    fn quote_enforces_supply_cap() {
        let mut destination = destination();
        destination.total_minted = destination.max_supply - 1;
        assert_eq!(
            quote_migration(&destination, &source(PriceMode::FixedRatio), 2, 2),
            Err(ProgramError::SupplyCapExceeded)
        );
    }

    #[test]
    fn bonding_curve_cost_increases_with_minted_supply() {
        let early = bonding_curve_source_amount(0, 100, 1_000, 1, 9).expect("early quote");
        let late = bonding_curve_source_amount(900, 100, 1_000, 1, 9).expect("late quote");
        assert!(late > early);
    }

    #[test]
    fn bonding_curve_enforces_supply_cap_when_called_directly() {
        assert_eq!(
            bonding_curve_source_amount(950, 100, 1_000, 1, 9),
            Err(ProgramError::SupplyCapExceeded)
        );
    }

    #[test]
    fn migrate_quotes_and_updates_counters() {
        let mut destination = destination();
        let mut source = source(PriceMode::FixedRatio);
        let quote = migrate(&mut destination, &mut source, 77, 77).expect("migrate");

        assert_eq!(quote.source_amount_to_burn, 77);
        assert_eq!(quote.destination_amount_to_mint, 77);
        assert_eq!(destination.total_minted, 10_077);
        assert_eq!(source.burned_base_units, 77);
        assert_eq!(source.minted_destination_base_units, 77);
        assert_eq!(source.migration_count, 1);
    }

    #[test]
    fn config_round_trips() {
        let config = Config {
            admin: key(7),
            pause_authority: key(8),
            launch_status: LaunchStatus::Candidate,
            destination_count: 3,
            source_count: 1,
            config_bump: 251,
            pending_authority: key(9),
            pending_authority_set: true,
            reserved: [0; 8],
        };
        let mut bytes = [0u8; CONFIG_LEN];
        config.pack(&mut bytes).expect("pack");
        assert_eq!(Config::unpack(&bytes), Ok(config));
    }

    #[test]
    fn destination_config_round_trips() {
        let destination = destination();
        let mut bytes = [0u8; DESTINATION_TOKEN_CONFIG_LEN];
        destination.pack(&mut bytes).expect("pack");
        assert_eq!(DestinationTokenConfig::unpack(&bytes), Ok(destination));
    }

    #[test]
    fn source_config_round_trips() {
        let mut source = source(PriceMode::FixedRatio);
        source.enabled = false;
        source.finalized = true;
        source.reserved = [7; 7];
        let mut bytes = [0u8; SOURCE_MINT_CONFIG_LEN];
        source.pack(&mut bytes).expect("pack");
        assert_eq!(SourceMintConfig::unpack(&bytes), Ok(source));
    }

    #[test]
    fn receipt_round_trips() {
        let receipt = Receipt {
            user: key(5),
            source_mint: key(4),
            destination_mint: key(1),
            source_amount_burned: 10,
            destination_amount_minted: 10,
            slot: 123,
            user_nonce: 456,
            bump: 250,
        };
        let mut bytes = [0u8; RECEIPT_LEN];
        receipt.pack(&mut bytes).expect("pack");
        assert_eq!(Receipt::unpack(&bytes), Ok(receipt));
    }

    #[test]
    fn migrate_instruction_decodes() {
        let mut bytes = [0u8; 26];
        bytes[0] = InstructionDiscriminant::Migrate as u8;
        write_u64(&mut bytes, 1, 100);
        write_u64(&mut bytes, 9, 101);
        write_u64(&mut bytes, 17, 9);
        bytes[25] = 1;

        assert_eq!(
            Instruction::unpack(&bytes),
            Ok(Instruction::Migrate {
                desired_destination_amount: 100,
                max_source_amount: 101,
                user_nonce: 9,
                create_receipt: true,
            })
        );
    }

    #[test]
    fn instructions_round_trip_through_pack_unpack() {
        let instructions = [
            Instruction::InitializeConfig { config_bump: 1 },
            Instruction::RegisterDestinationMint {
                token_id_hash: [5; TOKEN_ID_HASH_LEN],
                mint_vanity_nonce: 2,
                mint_bump: 3,
                decimals: 6,
                max_supply: 1_000,
                bonding_min: 1,
                bonding_range: 9,
                min_dest_amount: 1,
            },
            Instruction::RegisterSourceMint {
                source_decimals: 6,
                migration_mode: MigrationMode::BurnToMint,
                price_mode: PriceMode::FixedRatio,
                fixed_ratio_source_amount: 1,
                fixed_ratio_destination_amount: 1,
                bump: 4,
            },
            Instruction::SetSourceEnabled { enabled: true },
            Instruction::Migrate {
                desired_destination_amount: 100,
                max_source_amount: 101,
                user_nonce: 9,
                create_receipt: true,
            },
            Instruction::Pause { paused: true },
            Instruction::FinalizeSourceMint,
            Instruction::TransferAuthorityBegin {
                pending_authority: key(10),
            },
            Instruction::TransferAuthorityAccept,
            Instruction::RetireAuthority,
        ];

        for instruction in instructions {
            let mut bytes = [0u8; MAX_INSTRUCTION_LEN];
            let len = instruction.packed_len();
            instruction.pack(&mut bytes[..len]).expect("pack");
            assert_eq!(Instruction::unpack(&bytes[..len]), Ok(instruction));
        }
    }
}
