#!/usr/bin/env node
/**
 * Create voices for Signal station avatars via aws-swarm voice pipeline.
 * Runs the 3-step pipeline: Stable Audio → XTTS clone → smoothed voice.
 *
 * Usage: ADMIN_TABLE=SwarmAdmin-staging MEDIA_BUCKET=swarm-media-staging-022118847419 \
 *        CDN_URL=https://dodxbiygmi95j.cloudfront.net node scripts/create-station-voices.mjs
 */

// Set env before importing
process.env.ADMIN_TABLE = process.env.ADMIN_TABLE || 'SwarmAdmin-staging';
process.env.MEDIA_BUCKET = process.env.MEDIA_BUCKET || 'swarm-media-staging-022118847419';
process.env.CDN_URL = process.env.CDN_URL || 'https://dodxbiygmi95j.cloudfront.net';
process.env.AWS_REGION = process.env.AWS_REGION || 'us-east-1';

const stations = [
  {
    avatarId: 'signal-prospect',
    description: 'Deep gravelly baritone, like a tired dock foreman. Slight rasp from years near the furnaces. Blunt, clipped delivery. Industrial, no-nonsense.',
  },
  {
    avatarId: 'signal-kepler',
    description: 'Bright mid-range tenor with infectious enthusiasm. Like a young engineer who genuinely loves their work. Quick tempo, upward inflections. Warm and welcoming.',
  },
  {
    avatarId: 'signal-helios',
    description: 'Measured alto with academic precision. Calm, unhurried, every word deliberate. Like a university professor explaining crystal structures. Slightly formal.',
  },
];

async function main() {
  // Dynamic import from aws-swarm
  const voicePath = '../../aws-swarm/packages/admin-api/src/services/media/voice.ts';

  try {
    // Try tsx/ts-node import
    const voice = await import(voicePath);

    for (const station of stations) {
      console.log(`\n=== Creating voice for ${station.avatarId} ===`);
      console.log(`Description: ${station.description}`);

      // Check if voice already exists
      const existing = await voice.hasVoice(station.avatarId);
      if (existing.hasVoice) {
        console.log(`  Already has voice: ${existing.voiceId}`);
        console.log(`  Reference: ${existing.referenceUrl}`);
        continue;
      }

      console.log('  Starting voice creation pipeline...');
      const result = await voice.createMyVoice({
        avatarId: station.avatarId,
        description: station.description,
        updatedBy: 'signal-station-setup',
      });

      console.log(`  Voice created: ${result.voiceId}`);
      console.log(`  Preview: ${result.previewUrl}`);
      if (result.introUrl) console.log(`  Intro: ${result.introUrl}`);
    }
  } catch (err) {
    console.error('Failed to import voice module. Run from aws-swarm directory with tsx:');
    console.error('  cd ../aws-swarm && npx tsx ../signal/scripts/create-station-voices.mjs');
    console.error('Error:', err.message);
    process.exit(1);
  }
}

main().catch(console.error);
