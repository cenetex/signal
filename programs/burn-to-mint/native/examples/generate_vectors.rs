use rati_burn_to_mint_core::{
    apply_migration_counters, quote_migration, Config, DestinationTokenConfig, Instruction,
    LaunchStatus, MigrationMode, PriceMode, ProgramError, Receipt, SourceMintConfig, CONFIG_LEN,
    DESTINATION_TOKEN_CONFIG_LEN, FINALIZE_SOURCE_MINT_IX_LEN, INITIALIZE_CONFIG_IX_LEN,
    MIGRATE_IX_LEN, PAUSE_IX_LEN, RECEIPT_LEN, REGISTER_DESTINATION_MINT_IX_LEN,
    REGISTER_SOURCE_MINT_IX_LEN, RETIRE_AUTHORITY_IX_LEN, SET_SOURCE_ENABLED_IX_LEN,
    SOURCE_MINT_CONFIG_LEN, TRANSFER_AUTHORITY_ACCEPT_IX_LEN, TRANSFER_AUTHORITY_BEGIN_IX_LEN,
};

fn key(byte: u8) -> [u8; 32] {
    [byte; 32]
}

fn hex(bytes: &[u8]) -> String {
    const HEX: &[u8; 16] = b"0123456789abcdef";
    let mut out = String::with_capacity(bytes.len() * 2);
    for byte in bytes {
        out.push(HEX[(byte >> 4) as usize] as char);
        out.push(HEX[(byte & 0x0f) as usize] as char);
    }
    out
}

fn instruction_hex(instruction: &Instruction) -> String {
    let mut bytes = vec![0u8; instruction.packed_len()];
    instruction.pack(&mut bytes).expect("pack instruction");
    hex(&bytes)
}

fn config_fixture() -> Config {
    Config {
        admin: key(0x11),
        pause_authority: key(0x22),
        launch_status: LaunchStatus::Candidate,
        destination_count: 3,
        source_count: 1,
        config_bump: 254,
        pending_authority: key(0x33),
        pending_authority_set: true,
        reserved: [0; 8],
    }
}

fn destination_fixture() -> DestinationTokenConfig {
    DestinationTokenConfig {
        destination_mint: key(0x44),
        mint_vanity_nonce: 42,
        mint_bump: 253,
        token_id_hash: [0x55; 32],
        decimals: 6,
        token_program: key(0x66),
        mint_authority: key(0x77),
        max_supply: 1_000_000_000,
        total_minted: 10_000,
        bonding_min: 1,
        bonding_range: 9,
        min_dest_amount: 1,
        status: LaunchStatus::Enabled,
        reserved: [0; 8],
    }
}

fn source_fixture() -> SourceMintConfig {
    SourceMintConfig {
        source_mint: key(0x88),
        destination_mint: key(0x44),
        source_token_program: key(0x66),
        source_decimals: 6,
        migration_mode: MigrationMode::BurnToMint,
        price_mode: PriceMode::FixedRatio,
        fixed_ratio_source_amount: 1_000_000,
        fixed_ratio_destination_amount: 1_000_000,
        enabled: true,
        burned_base_units: 0,
        minted_destination_base_units: 0,
        migration_count: 0,
        bump: 252,
        finalized: false,
        reserved: [0; 7],
    }
}

fn receipt_fixture() -> Receipt {
    Receipt {
        user: key(0x99),
        source_mint: key(0x88),
        destination_mint: key(0x44),
        source_amount_burned: 1_000_000,
        destination_amount_minted: 1_000_000,
        slot: 0,
        user_nonce: 7,
        bump: 251,
    }
}

fn packed_config_hex(config: &Config) -> String {
    let mut bytes = [0u8; CONFIG_LEN];
    config.pack(&mut bytes).expect("pack config");
    hex(&bytes)
}

fn packed_destination_hex(destination: &DestinationTokenConfig) -> String {
    let mut bytes = [0u8; DESTINATION_TOKEN_CONFIG_LEN];
    destination.pack(&mut bytes).expect("pack destination");
    hex(&bytes)
}

fn packed_source_hex(source: &SourceMintConfig) -> String {
    let mut bytes = [0u8; SOURCE_MINT_CONFIG_LEN];
    source.pack(&mut bytes).expect("pack source");
    hex(&bytes)
}

fn packed_receipt_hex(receipt: &Receipt) -> String {
    let mut bytes = [0u8; RECEIPT_LEN];
    receipt.pack(&mut bytes).expect("pack receipt");
    hex(&bytes)
}

fn error_name(error: ProgramError) -> &'static str {
    match error {
        ProgramError::InvalidLength => "InvalidLength",
        ProgramError::InvalidVersion => "InvalidVersion",
        ProgramError::InvalidDiscriminant => "InvalidDiscriminant",
        ProgramError::InvalidBool => "InvalidBool",
        ProgramError::ZeroAmount => "ZeroAmount",
        ProgramError::SourceDisabled => "SourceDisabled",
        ProgramError::SourceDestinationMismatch => "SourceDestinationMismatch",
        ProgramError::DestinationNotEnabled => "DestinationNotEnabled",
        ProgramError::DestinationFinalized => "DestinationFinalized",
        ProgramError::DestinationPaused => "DestinationPaused",
        ProgramError::InvalidRatio => "InvalidRatio",
        ProgramError::FractionalRatio => "FractionalRatio",
        ProgramError::DivisionByZero => "DivisionByZero",
        ProgramError::SlippageExceeded => "SlippageExceeded",
        ProgramError::DestinationAmountTooSmall => "DestinationAmountTooSmall",
        ProgramError::SupplyCapExceeded => "SupplyCapExceeded",
        ProgramError::UnsupportedMigrationMode => "UnsupportedMigrationMode",
        ProgramError::ArithmeticOverflow => "ArithmeticOverflow",
    }
}

fn main() {
    let config = config_fixture();
    let destination = destination_fixture();
    let source = source_fixture();
    let receipt = receipt_fixture();

    let quote = quote_migration(&destination, &source, 1_000_000, 1_000_000).expect("fixed quote");
    let mut after_destination = destination.clone();
    let mut after_source = source.clone();
    apply_migration_counters(&mut after_destination, &mut after_source, quote)
        .expect("apply counters");

    let mut disabled_source = source.clone();
    disabled_source.enabled = false;
    let disabled_error =
        quote_migration(&destination, &disabled_source, 1, 1).expect_err("disabled source error");

    let fractional_error =
        rati_burn_to_mint_core::fixed_ratio_source_amount(3, 1, 2).expect_err("fractional");

    let mut paused_destination = destination.clone();
    paused_destination.status = LaunchStatus::Paused;
    let paused_error =
        quote_migration(&paused_destination, &source, 1, 1).expect_err("paused destination");

    println!("{{");
    println!("  \"schema\": \"rati.burn-to-mint.golden-vectors/v1\",");
    println!("  \"source\": \"programs/burn-to-mint/native\",");
    println!("  \"constants\": {{");
    println!("    \"configLen\": {CONFIG_LEN},");
    println!("    \"destinationTokenConfigLen\": {DESTINATION_TOKEN_CONFIG_LEN},");
    println!("    \"sourceMintConfigLen\": {SOURCE_MINT_CONFIG_LEN},");
    println!("    \"receiptLen\": {RECEIPT_LEN},");
    println!("    \"initializeConfigIxLen\": {INITIALIZE_CONFIG_IX_LEN},");
    println!("    \"registerDestinationMintIxLen\": {REGISTER_DESTINATION_MINT_IX_LEN},");
    println!("    \"registerSourceMintIxLen\": {REGISTER_SOURCE_MINT_IX_LEN},");
    println!("    \"setSourceEnabledIxLen\": {SET_SOURCE_ENABLED_IX_LEN},");
    println!("    \"migrateIxLen\": {MIGRATE_IX_LEN},");
    println!("    \"pauseIxLen\": {PAUSE_IX_LEN},");
    println!("    \"finalizeSourceMintIxLen\": {FINALIZE_SOURCE_MINT_IX_LEN},");
    println!("    \"transferAuthorityBeginIxLen\": {TRANSFER_AUTHORITY_BEGIN_IX_LEN},");
    println!("    \"transferAuthorityAcceptIxLen\": {TRANSFER_AUTHORITY_ACCEPT_IX_LEN},");
    println!("    \"retireAuthorityIxLen\": {RETIRE_AUTHORITY_IX_LEN}");
    println!("  }},");
    println!("  \"instructions\": {{");
    println!(
        "    \"initializeConfig\": \"{}\",",
        instruction_hex(&Instruction::InitializeConfig { config_bump: 254 })
    );
    println!(
        "    \"registerDestinationMint\": \"{}\",",
        instruction_hex(&Instruction::RegisterDestinationMint {
            token_id_hash: [0x55; 32],
            mint_vanity_nonce: 42,
            mint_bump: 253,
            decimals: 6,
            max_supply: 1_000_000_000,
            bonding_min: 1,
            bonding_range: 9,
            min_dest_amount: 1,
        })
    );
    println!(
        "    \"registerSourceMint\": \"{}\",",
        instruction_hex(&Instruction::RegisterSourceMint {
            source_decimals: 6,
            migration_mode: MigrationMode::BurnToMint,
            price_mode: PriceMode::FixedRatio,
            fixed_ratio_source_amount: 1_000_000,
            fixed_ratio_destination_amount: 1_000_000,
            bump: 252,
        })
    );
    println!(
        "    \"setSourceEnabled\": \"{}\",",
        instruction_hex(&Instruction::SetSourceEnabled { enabled: true })
    );
    println!(
        "    \"migrateNoReceipt\": \"{}\",",
        instruction_hex(&Instruction::Migrate {
            desired_destination_amount: 1_000_000,
            max_source_amount: 1_000_000,
            user_nonce: 7,
            create_receipt: false,
        })
    );
    println!(
        "    \"migrateWithReceipt\": \"{}\",",
        instruction_hex(&Instruction::Migrate {
            desired_destination_amount: 1_000_000,
            max_source_amount: 1_000_000,
            user_nonce: 7,
            create_receipt: true,
        })
    );
    println!(
        "    \"pause\": \"{}\",",
        instruction_hex(&Instruction::Pause { paused: true })
    );
    println!(
        "    \"finalizeSourceMint\": \"{}\",",
        instruction_hex(&Instruction::FinalizeSourceMint)
    );
    println!(
        "    \"transferAuthorityBegin\": \"{}\",",
        instruction_hex(&Instruction::TransferAuthorityBegin {
            pending_authority: key(0xaa),
        })
    );
    println!(
        "    \"transferAuthorityAccept\": \"{}\",",
        instruction_hex(&Instruction::TransferAuthorityAccept)
    );
    println!(
        "    \"retireAuthority\": \"{}\"",
        instruction_hex(&Instruction::RetireAuthority)
    );
    println!("  }},");
    println!("  \"accounts\": {{");
    println!("    \"config\": \"{}\",", packed_config_hex(&config));
    println!(
        "    \"destination\": \"{}\",",
        packed_destination_hex(&destination)
    );
    println!("    \"source\": \"{}\",", packed_source_hex(&source));
    println!("    \"receipt\": \"{}\"", packed_receipt_hex(&receipt));
    println!("  }},");
    println!("  \"migration\": {{");
    println!("    \"desiredDestinationAmount\": \"1000000\",");
    println!("    \"maxSourceAmount\": \"1000000\",");
    println!(
        "    \"sourceAmountToBurn\": \"{}\",",
        quote.source_amount_to_burn
    );
    println!(
        "    \"destinationAmountToMint\": \"{}\",",
        quote.destination_amount_to_mint
    );
    println!(
        "    \"afterDestinationTotalMinted\": \"{}\",",
        after_destination.total_minted
    );
    println!(
        "    \"afterSourceBurnedBaseUnits\": \"{}\",",
        after_source.burned_base_units
    );
    println!(
        "    \"afterSourceMintedDestinationBaseUnits\": \"{}\",",
        after_source.minted_destination_base_units
    );
    println!(
        "    \"afterSourceMigrationCount\": \"{}\"",
        after_source.migration_count
    );
    println!("  }},");
    println!("  \"errors\": {{");
    println!(
        "    \"disabledSource\": \"{}\",",
        error_name(disabled_error)
    );
    println!(
        "    \"fractionalFixedRatio\": \"{}\",",
        error_name(fractional_error)
    );
    println!(
        "    \"pausedDestination\": \"{}\"",
        error_name(paused_error)
    );
    println!("  }}");
    println!("}}");
}
