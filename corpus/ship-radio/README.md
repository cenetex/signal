# Signal Ship Radio Corpus

Seed corpus for training a tiny local model to speak for Signal ships and to
obey the bounded hail-choice protocol used by the in-process game client.

## Files

- `ship-radio-sft.jsonl`: chat-style SFT records with `messages`.
- `ship-radio-completions.jsonl`: prompt/completion view of the same records.
- `ship-radio-voice.txt`: raw voice lines for style mixing or continued pretraining.
- `manifest.json`: counts and SHA-256 hashes for reproducibility.

## Tasks

- `choice_batch`: given a prompt shaped like the live C hail prompt, return
  only assignments such as `YOU=1,N00=3,N01=2`.
- `ship_radio_line`: given a single ship's grounded facts, emit one short
  in-world radio line.

The choice task is the safest runtime target. C still owns station, commodity,
contract, and route grounding; the model only chooses among legal candidates.

## Regenerate

```sh
node scripts/build-ship-radio-corpus.mjs
```

Current manifest summary:

```json
{
  "format_version": 1,
  "choice_records": 90,
  "line_records": 132,
  "sft_records": 222,
  "raw_voice_lines": 90,
  "salts": 9,
  "max_speakers": 4
}
```
