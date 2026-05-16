# Signal Operator Metrics

Production relay logs already land in CloudWatch through the ECS task
definition:

- Log group: `/ecs/signal-relay-server`
- Region: `us-east-1`
- CloudWatch metric namespace: `Signal`

The relay emits two kinds of analytics data to stdout.

1. JSON lifecycle and client telemetry events:
   - `player_connect`
   - `player_session`
   - `player_identity`
   - `player_disconnect`
   - `player_metrics`
2. CloudWatch Embedded Metric Format summaries every 60 seconds:
   - `ConnectedPlayers`
   - `ReadyPlayers`
   - `ActiveUsers1m`
   - `MetricPlayers`
   - `AvgPingMs`
   - `AvgAckMs`
   - `AvgAckGapMs`
   - `MaxAckGapMs`

`user_key` is pseudonymous. The server hashes the registered pubkey when one is
present, otherwise it hashes the session token. Raw pubkeys, session tokens, and
client IPs are not written by these analytics events.

## DAU

CloudWatch Logs Insights:

```sql
SOURCE '/ecs/signal-relay-server'
| fields @timestamp, event, user_key
| filter (event = "player_session" or event = "player_metrics")
| filter user_key != "anon"
| stats count_distinct(user_key) as dau by bin(1d)
```

For WAU/MAU, change `bin(1d)` to `bin(7d)` or `bin(30d)`.

## Average Latency

```sql
SOURCE '/ecs/signal-relay-server'
| fields @timestamp, event, ping_ms, ack_ms, ack_gap_ms
| filter event = "player_metrics"
| stats
    avg(ping_ms) as avg_ping_ms,
    avg(ack_ms) as avg_ack_ms,
    avg(ack_gap_ms) as avg_ack_gap_ms,
    pct(ack_gap_ms, 95) as p95_ack_gap_ms
  by bin(5m)
```

Interpretation:

- `ping_ms`: app-level websocket round trip.
- `ack_ms`: input sent to authoritative `WORLD_PLAYERS` acknowledgement.
- `ack_gap_ms`: `ack_ms - ping_ms`; this is the optimization budget between
  raw transport and actual authoritative feel.

## Current Concurrency

```sql
SOURCE '/ecs/signal-relay-server'
| fields @timestamp, ConnectedPlayers, ReadyPlayers, ActiveUsers1m
| filter ispresent(ConnectedPlayers)
| stats
    max(ConnectedPlayers) as connected,
    max(ReadyPlayers) as ready,
    max(ActiveUsers1m) as active_1m
  by bin(1m)
```

`ActiveUsers1m` is process-local and useful for an operator dashboard. DAU
should come from `count_distinct(user_key)` over logs, not from this one-minute
gauge.

## Worst Latency Users

```sql
SOURCE '/ecs/signal-relay-server'
| fields @timestamp, event, user_key, ping_ms, ack_ms, ack_gap_ms, unacked_inputs, replay_depth
| filter event = "player_metrics" and user_key != "anon"
| stats
    count(*) as samples,
    avg(ping_ms) as avg_ping_ms,
    avg(ack_gap_ms) as avg_gap_ms,
    max(ack_gap_ms) as max_gap_ms,
    max(unacked_inputs) as max_unacked,
    max(replay_depth) as max_replay
  by user_key
| sort avg_gap_ms desc
| limit 20
```

## Dashboard

`docs/cloudwatch-dashboard-signal-relay.json` contains a starter dashboard
using the log group above. Apply it with:

```sh
aws cloudwatch put-dashboard \
  --dashboard-name SignalRelay \
  --dashboard-body file://docs/cloudwatch-dashboard-signal-relay.json
```

