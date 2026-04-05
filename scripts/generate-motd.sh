#!/bin/bash
# Generate station MOTDs via aws-swarm avatar API and upload to S3.
# Run periodically (cron) or manually to refresh station messages.

set -euo pipefail

API_BASE="https://staging-swarm.rati.chat/api/v1"
API_KEY="${SWARM_API_KEY:-sk-rati--hO4iZYH3jj4DhjiWfEJPeAjrdDdhJCR13LuJzPjRuQ}"
S3_BUCKET="signal-ratimics-assets"
PROMPT="A miner just hailed your station on the radio. Respond in character with a short greeting and message of the day. No markdown. No thinking or reasoning. Under 40 words. Only the radio message."

for SLUG in signal-prospect signal-kepler signal-helios; do
  SHORT="${SLUG#signal-}"
  echo "Generating MOTD for $SHORT..."

  RESPONSE=$(curl -s -X POST "$API_BASE/chat/completions" \
    -H "Authorization: Bearer $API_KEY" \
    -H "Content-Type: application/json" \
    -d "{
      \"model\": \"avatar:$SLUG\",
      \"messages\": [{\"role\": \"user\", \"content\": \"$PROMPT\"}],
      \"max_tokens\": 100,
      \"temperature\": 0.9,
      \"include_audio\": true
    }" 2>/dev/null)

  MOTD=$(echo "$RESPONSE" | python3 -c "
import sys, json, re
try:
    r = json.load(sys.stdin)
    text = r['choices'][0]['message']['content'].strip()
    # Strip thinking/reasoning leaks (lines that describe what the AI is doing)
    lines = text.split('\n')
    clean = []
    for line in lines:
        line = line.strip()
        if not line: continue
        # Skip meta lines
        if any(skip in line.lower() for skip in ['the user', 'i should', 'i am ', 'i need to', 'let me', 'my persona', 'stay in character', 'thinking']):
            continue
        clean.append(line)
    text = ' '.join(clean)
    # Clean up markdown
    for ch in ['**', '*', '#', '>', '---']:
        text = text.replace(ch, '')
    text = ' '.join(text.split())
    if text:
        print(text[:255])
    else:
        print('Station online. Welcome, pilot.')
except:
    print('Station online. Welcome, pilot.')
" 2>/dev/null)

  echo "  MOTD: $MOTD"
  echo -n "$MOTD" > "/tmp/${SHORT}_motd.txt"
  aws s3 cp "/tmp/${SHORT}_motd.txt" "s3://$S3_BUCKET/stations/$SHORT/motd.txt" \
    --content-type "text/plain" --quiet

  # Extract audio URL if present
  AUDIO_URL=$(echo "$RESPONSE" | python3 -c "
import sys, json
try:
    r = json.load(sys.stdin)
    audio = r['choices'][0]['message'].get('audio', {})
    print(audio.get('url', ''))
except:
    print('')
" 2>/dev/null)

  if [ -n "$AUDIO_URL" ]; then
    echo "  Voice: $AUDIO_URL"
    curl -s "$AUDIO_URL" -o "/tmp/${SHORT}_hail.wav" 2>/dev/null
    if [ -s "/tmp/${SHORT}_hail.wav" ]; then
      aws s3 cp "/tmp/${SHORT}_hail.wav" "s3://$S3_BUCKET/stations/$SHORT/hail.wav" \
        --content-type "audio/wav" --quiet
      echo "  Voice uploaded to S3"
    fi
  fi
  echo "  Done"
done

echo "All MOTDs generated and uploaded."
