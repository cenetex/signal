#!/bin/bash
# Generate station MOTDs via aws-swarm avatar API and upload to S3.
# Produces multi-tier MOTD JSON with rarity bands.
# Run periodically (cron) or manually to refresh station messages.

set -euo pipefail

API_BASE="https://staging-swarm.rati.chat/api/v1"
API_KEY="${SWARM_API_KEY:-sk-rati--hO4iZYH3jj4DhjiWfEJPeAjrdDdhJCR13LuJzPjRuQ}"
S3_BUCKET="signal-ratimics-assets"

# Tier-specific prompts
PROMPTS=(
  "A miner just hailed your station. Respond as station operator with a brief, friendly greeting and current status. No markdown. Under 40 words. Only the message."
  "You're talking to a miner over weak radio signal. Share a brief station anecdote or tip. No markdown. Under 40 words."
  "In the signal fringe, you receive a garbled transmission. Share something mysterious about your station's history or purpose. Cryptic but intriguing. No markdown. Under 40 words."
  "In deep space far from station signal, whisper something secret or dangerous-sounding. Fragmented transmission. No markdown. Under 40 words."
)

generate_tier_text() {
    local slug="$1"
    local prompt="$2"

    RESPONSE=$(curl -s -X POST "$API_BASE/chat/completions" \
      -H "Authorization: Bearer $API_KEY" \
      -H "Content-Type: application/json" \
      -d "{
        \"model\": \"avatar:$slug\",
        \"messages\": [{\"role\": \"user\", \"content\": \"$prompt\"}],
        \"max_tokens\": 100,
        \"temperature\": 0.85,
        \"include_audio\": false
      }" 2>/dev/null)

    echo "$RESPONSE" | python3 -c "
import sys, json
try:
    r = json.load(sys.stdin)
    text = r['choices'][0]['message']['content'].strip()
    lines = text.split('\n')
    clean = []
    for line in lines:
        line = line.strip()
        if not line: continue
        if any(skip in line.lower() for skip in ['the user', 'i should', 'i am ', 'i need to', 'let me', 'my persona', 'stay in character', 'thinking']):
            continue
        clean.append(line)
    text = ' '.join(clean)
    for ch in ['**', '*', '#', '>', '---']:
        text = text.replace(ch, '')
    text = ' '.join(text.split())
    if text:
        print(text[:255])
    else:
        print('Station online.')
except:
    print('Station online.')
" 2>/dev/null
}

for SLUG in signal-prospect signal-kepler signal-helios; do
  SHORT="${SLUG#signal-}"
  echo "Generating multi-tier MOTD for $SHORT..."

  TIMESTAMP=$(date +%s)
  SEED=$(($(echo "$SHORT" | cksum | cut -d' ' -f1) % 65536))

  # Generate 4 tier messages
  TIERS=("common" "uncommon" "rare" "ultra_rare")
  declare -a MESSAGES

  for i in 0 1 2 3; do
    echo "  Generating ${TIERS[$i]}..."
    TEXT=$(generate_tier_text "$SLUG" "${PROMPTS[$i]}")
    MESSAGES[$i]="$TEXT"
    echo "    ${TIERS[$i]}: $TEXT"
  done

  # Build multi-tier JSON with proper escaping
  python3 << PYSCRIPT
import json
data = {
    "generated_at": $TIMESTAMP,
    "seed": $SEED,
    "messages": {
        "common": """${MESSAGES[0]}""",
        "uncommon": """${MESSAGES[1]}""",
        "rare": """${MESSAGES[2]}""",
        "ultra_rare": """${MESSAGES[3]}"""
    },
    "bands": {
        "common": [0.80, 1.00],
        "uncommon": [0.50, 0.80],
        "rare": [0.20, 0.50],
        "ultra_rare": [0.00, 0.20]
    }
}
print(json.dumps(data, ensure_ascii=False))
PYSCRIPT

  # Save and upload to S3
  python3 << PYSCRIPT > "/tmp/${SHORT}_motd.json"
import json
data = {
    "generated_at": $TIMESTAMP,
    "seed": $SEED,
    "messages": {
        "common": """${MESSAGES[0]}""",
        "uncommon": """${MESSAGES[1]}""",
        "rare": """${MESSAGES[2]}""",
        "ultra_rare": """${MESSAGES[3]}"""
    },
    "bands": {
        "common": [0.80, 1.00],
        "uncommon": [0.50, 0.80],
        "rare": [0.20, 0.50],
        "ultra_rare": [0.00, 0.20]
    }
}
print(json.dumps(data, ensure_ascii=False))
PYSCRIPT

  echo "  Uploading motd.json to S3..."
  aws s3 cp "/tmp/${SHORT}_motd.json" "s3://$S3_BUCKET/stations/$SHORT/motd.json" \
    --content-type "application/json" --quiet
  echo "  Done"
done

echo "All MOTDs generated and uploaded."
