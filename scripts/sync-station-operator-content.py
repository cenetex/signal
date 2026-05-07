#!/usr/bin/env python3
"""Generate station operator copy via swarm.rati.chat and push it to Signal.

Inputs:
  SWARM_API_KEY                 bearer for the OpenAI-compatible avatar API
  SIGNAL_API_TOKEN              bearer for /api/station/<id>/command
  SWARM_API_BASE                optional, default https://swarm.rati.chat/api/v1
  SIGNAL_SERVER_API             optional, default http://127.0.0.1:8080

The script intentionally does not store secrets and does not write generated
assets. It asks the avatar model for structured text, then materializes that
text through server commands so each line is signed into the station chain log.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import sys
import urllib.error
import urllib.request


STATIONS = [
    {
        "id": 0,
        "slug": "prospect",
        "model": "avatar:signal-prospect",
        "name": "Prospect Refinery",
        "voice": "terse, practical, tired foreman; notices everything and says little",
    },
    {
        "id": 1,
        "slug": "kepler",
        "model": "avatar:signal-kepler",
        "name": "Kepler Yard",
        "voice": "fabrication engineer; talks like machines have moods; precise but warm",
    },
    {
        "id": 2,
        "slug": "helios",
        "model": "avatar:signal-helios",
        "name": "Helios Works",
        "voice": "ambitious operator-director; premium fabrication; uses 'we' with pride",
    },
]


def fail(message: str) -> None:
    print(message, file=sys.stderr)
    raise SystemExit(1)


def request_json(url: str, headers: dict[str, str], payload: dict) -> dict:
    body = json.dumps(payload).encode("utf-8")
    req = urllib.request.Request(url, data=body, headers=headers, method="POST")
    try:
        with urllib.request.urlopen(req, timeout=45) as resp:
            return json.loads(resp.read().decode("utf-8"))
    except urllib.error.HTTPError as exc:
        detail = exc.read().decode("utf-8", errors="replace")
        fail(f"HTTP {exc.code} from {url}: {detail[:500]}")
    except urllib.error.URLError as exc:
        fail(f"request failed for {url}: {exc}")
    raise AssertionError("unreachable")


def get_json(url: str, headers: dict[str, str]) -> dict | None:
    req = urllib.request.Request(url, headers=headers, method="GET")
    try:
        with urllib.request.urlopen(req, timeout=20) as resp:
            return json.loads(resp.read().decode("utf-8"))
    except Exception as exc:
        print(f"warning: could not read station state from {url}: {exc}", file=sys.stderr)
        return None


def clean_text(text: str, limit: int) -> str:
    text = re.sub(r"[\r\n\t]+", " ", text)
    text = re.sub(r"[*#>`_]+", "", text)
    text = re.sub(r"\s+", " ", text).strip()
    return text[:limit].rstrip()


def extract_json_object(text: str) -> dict:
    text = text.strip()
    if text.startswith("```"):
        text = re.sub(r"^```(?:json)?", "", text, flags=re.I).strip()
        text = re.sub(r"```$", "", text).strip()
    start = text.find("{")
    end = text.rfind("}")
    if start < 0 or end <= start:
        fail(f"avatar response did not contain a JSON object: {text[:300]}")
    return json.loads(text[start : end + 1])


def normalize_content(raw: dict) -> dict:
    miner = raw.get("miner_chatter") or []
    hauler = raw.get("hauler_chatter") or []
    if not isinstance(miner, list) or not isinstance(hauler, list):
        fail("avatar JSON must include miner_chatter and hauler_chatter arrays")

    def lines(values: list, fallback: str) -> list[str]:
        out = [clean_text(str(v), 63) for v in values if clean_text(str(v), 63)]
        while len(out) < 8:
            out.append(fallback)
        return out[:8]

    return {
        "hail": clean_text(str(raw.get("motd", "")), 255),
        "rati_hail": clean_text(str(raw.get("rati_hail", "")), 255),
        "miner_chatter": lines(miner, "rock looks good"),
        "hauler_chatter": lines(hauler, "load secured"),
    }


def generate_for_station(api_base: str, api_key: str, station: dict, state: dict | None) -> dict:
    context = json.dumps(state or {}, separators=(",", ":"), ensure_ascii=False)[:6000]
    prompt = f"""
You are the station operator for {station['name']} in Signal Space Miner.
Voice: {station['voice']}.
Current station/server context, including chain cursor and relationship/activity data:
{context}

Return JSON only with:
- motd: one concise station message of the day, max 34 words.
- rati_hail: a special hail when a player delivers RATi-grade or commissioned ore, max 28 words.
- miner_chatter: exactly 8 short radio snippets for miner NPCs, each max 7 words.
- hauler_chatter: exactly 8 short radio snippets for hauler NPCs, each max 7 words.

No markdown, no explanations, no out-of-character text.
Keep the tone industrial, in-universe, and specific to this station.
""".strip()
    response = request_json(
        api_base.rstrip("/") + "/chat/completions",
        {
            "Authorization": f"Bearer {api_key}",
            "Content-Type": "application/json",
        },
        {
            "model": station["model"],
            "messages": [{"role": "user", "content": prompt}],
            "max_tokens": 500,
            "temperature": 0.82,
            "include_audio": False,
        },
    )
    content = response["choices"][0]["message"]["content"]
    return normalize_content(extract_json_object(content))


def post_station_command(server_api: str, api_token: str, station_id: int, payload: dict) -> None:
    request_json(
        server_api.rstrip("/") + f"/api/station/{station_id}/command",
        {
            "Authorization": f"Bearer {api_token}",
            "Content-Type": "application/json",
        },
        payload,
    )


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--stations", default="prospect,kepler,helios",
                        help="comma-separated slugs to generate")
    args = parser.parse_args()

    swarm_key = os.environ.get("SWARM_API_KEY")
    signal_token = os.environ.get("SIGNAL_API_TOKEN")
    api_base = os.environ.get("SWARM_API_BASE", "https://swarm.rati.chat/api/v1")
    server_api = os.environ.get("SIGNAL_SERVER_API", "http://127.0.0.1:8080")

    if not swarm_key:
        fail("SWARM_API_KEY is required")
    if not args.dry_run and not signal_token:
        fail("SIGNAL_API_TOKEN is required unless --dry-run is used")

    selected = {s.strip() for s in args.stations.split(",") if s.strip()}
    stations = [s for s in STATIONS if s["slug"] in selected]
    if not stations:
        fail("no matching stations selected")

    for station in stations:
        state_headers = {"Content-Type": "application/json"}
        if signal_token:
            state_headers["Authorization"] = f"Bearer {signal_token}"
        state = get_json(
            server_api.rstrip("/") + f"/api/station/{station['id']}/state?include=activity_history,chain_history",
            state_headers,
        )
        generated = generate_for_station(api_base, swarm_key, station, state)
        print(json.dumps({"station": station["slug"], **generated}, indent=2))
        if args.dry_run:
            continue
        sid = station["id"]
        post_station_command(server_api, signal_token, sid,
                             {"action": "set_hail", "hail": generated["hail"]})
        post_station_command(server_api, signal_token, sid,
                             {"action": "set_rati_hail", "message": generated["rati_hail"]})
        for idx, line in enumerate(generated["miner_chatter"]):
            post_station_command(server_api, signal_token, sid,
                                 {"action": "set_miner_chatter", "slot": idx, "message": line})
        for idx, line in enumerate(generated["hauler_chatter"]):
            post_station_command(server_api, signal_token, sid,
                                 {"action": "set_hauler_chatter", "slot": idx, "message": line})


if __name__ == "__main__":
    main()
