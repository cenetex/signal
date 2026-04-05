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
      \"temperature\": 0.9
    }" 2>/dev/null)

  MOTD=$(echo "$RESPONSE" | python3 -c "
import sys, json
try:
    r = json.load(sys.stdin)
    text = r['choices'][0]['message']['content'].strip()
    # Clean up markdown artifacts
    for ch in ['**', '*', '#', '>', '---']:
        text = text.replace(ch, '')
    # Remove leading/trailing whitespace and newlines
    text = ' '.join(text.split())
    # Truncate to 255 chars (hail_message limit)
    if text:
        print(text[:255])
    else:
        print('Station online. Welcome, pilot.')
except Exception as e:
    print('Station online. Welcome, pilot.')
" 2>/dev/null)

  echo "  MOTD: $MOTD"
  echo -n "$MOTD" > "/tmp/${SHORT}_motd.txt"
  aws s3 cp "/tmp/${SHORT}_motd.txt" "s3://$S3_BUCKET/stations/$SHORT/motd.txt" \
    --content-type "text/plain" --quiet
  echo "  Uploaded to S3"
done

echo "All MOTDs generated and uploaded."
