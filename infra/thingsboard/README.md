# ThingsBoard CE Server Infrastructure

This directory contains the production-ready Docker Compose stack and server-side bootstrap script for ThingsBoard Community Edition (CE) in the `fishpump-online` system.

## Quick Start (On Server: LAN Mini-PC or VPS)

To deploy or start ThingsBoard, execute:

```bash
cd infra/thingsboard
chmod +x setup.sh
./setup.sh
```

On the first run, `setup.sh` will:
1. Generate `.env` from `.env.example` if not present.
2. Spin up `postgres` and wait for health check.
3. Automatically provision ThingsBoard database schema (`INSTALL_TB=true`).
4. Start both `thingsboard-ce` and `postgres` containers in the background.
5. Print access URLs with detected server IP.

## Management Commands

| Command | Description |
| :--- | :--- |
| `./setup.sh` (or `./setup.sh start`) | Start services (initializes DB if first run) |
| `./setup.sh down` | Stop all containers gracefully |
| `./setup.sh logs` | Follow combined real-time container logs |
| `./setup.sh status` | Inspect container status and health |
| `./setup.sh reset` | Wipe containers, volumes, and state to start clean |

## Connectivity

- **Web UI & REST API:** `http://<SERVER_IP>:8080`
  - Default Sysadmin: `sysadmin@thingsboard.org` / `sysadmin`
  - Default Tenant Admin: `tenant@thingsboard.org` / `tenant`
- **MQTT Broker for ESP32-S3:** `mqtt://<SERVER_IP>:1883`
- **PostgreSQL:** Internal to Docker bridge network (`tb-net`) on port `5432` (not exposed to host for security).

## CI/CD Deployment (GitHub Actions)

When updating the infrastructure on a remote server via SSH:

```bash
cd infra/thingsboard && docker compose pull && docker compose up -d
```
