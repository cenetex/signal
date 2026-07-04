#!/bin/bash
set -eux

export DEBIAN_FRONTEND=noninteractive

apt-get update
apt-get install -y ca-certificates curl docker.io gzip tar

systemctl enable --now docker
usermod -aG docker ubuntu || true

mkdir -p /opt/signal/data /opt/signal/secrets /opt/signal/deploy
chmod 700 /opt/signal/secrets
chown -R ubuntu:ubuntu /opt/signal

cat >/etc/motd <<'MOTD'
Signal relay host. Runtime lives under /opt/signal.
MOTD
