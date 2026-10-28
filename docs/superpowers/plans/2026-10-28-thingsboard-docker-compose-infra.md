# ThingsBoard CE Docker Compose Infrastructure Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Create a standalone, server-scoped ThingsBoard CE Docker Compose infrastructure with an automated one-click bootstrap script (`setup.sh`) under `infra/thingsboard/` for LAN testing and VPS production deployment.

**Architecture:** A multi-container Docker Compose setup containing `postgres:16-alpine` and `thingsboard/tb-node:latest` orchestrated via environment variables in `.env`. An idempotent server-side bash script (`setup.sh`) automates pre-flight checks, `.env` generation, database schema initialization (`INSTALL_TB=true`), and container lifecycle operations strictly within the Docker boundary.

**Tech Stack:** Docker Compose v2, ThingsBoard CE (`thingsboard/tb-node:latest`), PostgreSQL 16 Alpine, Bash.

## Global Constraints

- Directory location: strictly contained within `infra/thingsboard/`.
- Server-scope: `setup.sh` must not modify host system files outside `infra/thingsboard/` (no `apt`, no `/etc` modifications).
- Port assignments: Web UI on `8080`, MQTT on `1883`, PostgreSQL port `5432` kept internal on Docker bridge network.
- Persistence: All data stored in Docker named volumes (`postgres-data`, `tb-data`, `tb-log`).
- Idempotency: Running `setup.sh` multiple times must preserve existing database records and dashboards.

---

### Task 1: Environment Template & Git Ignore Setup

**Files:**
- Create: `infra/thingsboard/.gitignore`
- Create: `infra/thingsboard/.env.example`

**Interfaces:**
- Consumes: None
- Produces: `.env.example` containing `TB_HTTP_PORT`, `TB_MQTT_PORT`, `POSTGRES_DB`, `POSTGRES_USER`, `POSTGRES_PASSWORD`, `JAVA_OPTS`, `TB_QUEUE_TYPE`. `.gitignore` preventing `.env` and `.initialized` from being committed.

- [ ] **Step 1: Create `infra/thingsboard/.gitignore`**

```gitignore
# Security: Never commit actual credentials or secrets
.env

# State marker: Initialized database flag
.initialized

# ThingsBoard local runtime dumps or logs if bound
*.log
```

- [ ] **Step 2: Create `infra/thingsboard/.env.example`**

```dotenv
# =================================================================
# ThingsBoard CE Docker Compose Configuration Template
# =================================================================

# Port Configuration
TB_HTTP_PORT=8080
TB_MQTT_PORT=1883

# PostgreSQL Database Configuration
POSTGRES_DB=thingsboard
POSTGRES_USER=postgres
POSTGRES_PASSWORD=thingsboard_secret_pass

# ThingsBoard JVM & Resource Limits
# Default allocation: 512MB initial heap, 2GB maximum heap
JAVA_OPTS=-Xms512M -Xmx2G

# Queue Configuration (in-memory for standalone CE node)
TB_QUEUE_TYPE=in-memory
```

- [ ] **Step 3: Verify files exist and content is correct**

Run:
```powershell
Test-Path "infra/thingsboard/.gitignore"
Test-Path "infra/thingsboard/.env.example"
```
Expected: Both return `True`.

- [ ] **Step 4: Commit Task 1**

```bash
git add infra/thingsboard/.gitignore infra/thingsboard/.env.example
git commit -m "feat(infra): add ThingsBoard .gitignore and .env.example template"
```

---

### Task 2: Docker Compose Definition

**Files:**
- Create: `infra/thingsboard/docker-compose.yml`

**Interfaces:**
- Consumes: Environment variables defined in `infra/thingsboard/.env.example`
- Produces: Docker Compose stack with `postgres` and `thingsboard-ce` services, healthchecks, and internal network `tb-net`.

- [ ] **Step 1: Create `infra/thingsboard/docker-compose.yml`**

```yaml
services:
  postgres:
    image: postgres:16-alpine
    container_name: tb-postgres
    restart: unless-stopped
    environment:
      POSTGRES_DB: ${POSTGRES_DB:-thingsboard}
      POSTGRES_USER: ${POSTGRES_USER:-postgres}
      POSTGRES_PASSWORD: ${POSTGRES_PASSWORD:-thingsboard_secret_pass}
      PGDATA: /var/lib/postgresql/data/pgdata
    volumes:
      - postgres-data:/var/lib/postgresql/data
    networks:
      - tb-net
    healthcheck:
      test: ["CMD-SHELL", "pg_isready -U ${POSTGRES_USER:-postgres} -d ${POSTGRES_DB:-thingsboard}"]
      interval: 5s
      timeout: 5s
      retries: 10
      start_period: 10s

  thingsboard-ce:
    image: thingsboard/tb-node:latest
    container_name: tb-node
    restart: unless-stopped
    depends_on:
      postgres:
        condition: service_healthy
    ports:
      - "${TB_HTTP_PORT:-8080}:8080"
      - "${TB_MQTT_PORT:-1883}:1883"
    environment:
      TB_QUEUE_TYPE: ${TB_QUEUE_TYPE:-in-memory}
      SPRING_DATASOURCE_URL: jdbc:postgresql://postgres:5432/${POSTGRES_DB:-thingsboard}
      SPRING_DATASOURCE_USERNAME: ${POSTGRES_USER:-postgres}
      SPRING_DATASOURCE_PASSWORD: ${POSTGRES_PASSWORD:-thingsboard_secret_pass}
      JAVA_OPTS: ${JAVA_OPTS:--Xms512M -Xmx2G}
    volumes:
      - tb-log:/var/log/thingsboard
      - tb-data:/usr/share/thingsboard/data
    networks:
      - tb-net

volumes:
  postgres-data:
    name: tb_postgres_data
  tb-log:
    name: tb_log_data
  tb-data:
    name: tb_app_data

networks:
  tb-net:
    name: tb_network
    driver: bridge
```

- [ ] **Step 2: Validate Docker Compose syntax**

Run:
```bash
docker compose --env-file infra/thingsboard/.env.example -f infra/thingsboard/docker-compose.yml config
```
Expected: Syntactically valid YAML resolved without errors or missing variables.

- [ ] **Step 3: Commit Task 2**

```bash
git add infra/thingsboard/docker-compose.yml
git commit -m "feat(infra): add ThingsBoard CE docker-compose service definition"
```

---

### Task 3: Dedicated Server Setup Script (`setup.sh`)

**Files:**
- Create: `infra/thingsboard/setup.sh`

**Interfaces:**
- Consumes: `docker-compose.yml`, `.env.example`
- Produces: Executable CLI script managing Docker stack (`start`, `down`, `logs`, `status`, `reset`).

- [ ] **Step 1: Write `infra/thingsboard/setup.sh`**

```bash
#!/usr/bin/env bash
# =============================================================================
# ThingsBoard CE Server Bootstrap & Lifecycle Management Script
# =============================================================================
# Strictly scoped to Docker resources inside this directory.
# Does not modify server host packages or OS configurations.
# =============================================================================

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

ENV_FILE=".env"
ENV_EXAMPLE=".env.example"
INIT_MARKER=".initialized"

# ANSI Colors
CYAN='\033[0;36m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m' # No Color

log_info() {
    echo -e "${CYAN}[INFO]${NC} $1"
}

log_success() {
    echo -e "${GREEN}[SUCCESS]${NC} $1"
}

log_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

# 1. Pre-flight Check: Docker and Compose
check_docker() {
    if ! command -v docker >/dev/null 2>&1; then
        log_error "Docker is not installed or not in PATH. Please install Docker first."
        exit 1
    fi

    if ! docker compose version >/dev/null 2>&1; then
        log_error "Docker Compose v2 plugin ('docker compose') is required."
        exit 1
    fi

    if ! docker info >/dev/null 2>&1; then
        log_error "Docker daemon is not running or current user lacks permission to access Docker socket."
        exit 1
    fi
}

# 2. Setup Environment Variables
ensure_env() {
    if [ ! -f "$ENV_FILE" ]; then
        if [ -f "$ENV_EXAMPLE" ]; then
            log_info "No .env file found. Creating $ENV_FILE from $ENV_EXAMPLE..."
            cp "$ENV_EXAMPLE" "$ENV_FILE"
            log_success ".env created with default settings."
        else
            log_error "$ENV_EXAMPLE is missing. Cannot initialize environment."
            exit 1
        fi
    else
        log_info "Found existing $ENV_FILE."
    fi
}

# 3. Detect Host IP
detect_host_ip() {
    local host_ip
    # Try hostname -I first (common on Linux)
    host_ip=$(hostname -I 2>/dev/null | awk '{print $1}')
    if [ -z "$host_ip" ]; then
        # Fallback to ip route
        host_ip=$(ip route get 1.1.1.1 2>/dev/null | awk '{print $7}')
    fi
    if [ -z "$host_ip" ]; then
        host_ip="localhost"
    fi
    echo "$host_ip"
}

# 4. Initialize Database (One-time only)
initialize_db() {
    if [ ! -f "$INIT_MARKER" ]; then
        log_info "First-time setup detected. Starting database initialization..."
        
        # Start PostgreSQL first
        log_info "Starting PostgreSQL container..."
        docker compose up -d postgres

        log_info "Waiting for PostgreSQL to be healthy..."
        until [ "$(docker inspect -f '{{.State.Health.Status}}' tb-postgres 2>/dev/null)" = "healthy" ]; do
            sleep 2
        done
        log_success "PostgreSQL is healthy."

        log_info "Running ThingsBoard schema installation (this may take 1-2 minutes)..."
        docker compose run --rm -e INSTALL_TB=true -e LOAD_DEMO=false thingsboard-ce

        touch "$INIT_MARKER"
        log_success "ThingsBoard database schema successfully initialized."
    else
        log_info "Database already initialized ($INIT_MARKER found). Skipping schema install."
    fi
}

# 5. Start Stack
cmd_start() {
    check_docker
    ensure_env
    initialize_db

    log_info "Starting ThingsBoard stack..."
    docker compose up -d

    local host_ip
    host_ip=$(detect_host_ip)

    # Read ports from .env or defaults
    local http_port
    local mqtt_port
    http_port=$(grep -E '^TB_HTTP_PORT=' "$ENV_FILE" 2>/dev/null | cut -d '=' -f2 || echo "8080")
    mqtt_port=$(grep -E '^TB_MQTT_PORT=' "$ENV_FILE" 2>/dev/null | cut -d '=' -f2 || echo "1883")
    [ -z "$http_port" ] && http_port="8080"
    [ -z "$mqtt_port" ] && mqtt_port="1883"

    echo ""
    echo "================================================================="
    log_success "ThingsBoard CE is running!"
    echo "================================================================="
    echo -e "Web UI / REST API : ${CYAN}http://${host_ip}:${http_port}${NC}"
    echo -e "MQTT Broker (ESP) : ${CYAN}mqtt://${host_ip}:${mqtt_port}${NC}"
    echo -e "Default Sysadmin  : ${CYAN}sysadmin@thingsboard.org${NC} / ${CYAN}sysadmin${NC}"
    echo -e "Default Tenant    : ${CYAN}tenant@thingsboard.org${NC} / ${CYAN}tenant${NC}"
    echo "================================================================="
    echo "Tip: Run './setup.sh logs' to monitor container output."
    echo ""
}

# 6. Stop Stack
cmd_down() {
    check_docker
    log_info "Stopping ThingsBoard stack..."
    docker compose down
    log_success "ThingsBoard stack stopped."
}

# 7. Show Logs
cmd_logs() {
    check_docker
    docker compose logs -f
}

# 8. Show Status
cmd_status() {
    check_docker
    docker compose ps
}

# 9. Reset System (Purge all data)
cmd_reset() {
    check_docker
    log_warn "WARNING: This will completely destroy all ThingsBoard data, devices, and dashboards!"
    read -r -p "Are you sure you want to proceed? [y/N] " confirm
    case "$confirm" in
        [yY][eE][sS]|[yY])
            log_info "Purging containers and named volumes..."
            docker compose down -v --remove-orphans
            rm -f "$INIT_MARKER"
            log_success "Reset complete. Run './setup.sh' to re-initialize from scratch."
            ;;
        *)
            log_info "Reset aborted."
            ;;
    esac
}

# Main Command Dispatcher
case "${1:-start}" in
    start)
        cmd_start
        ;;
    down|stop)
        cmd_down
        ;;
    logs)
        cmd_logs
        ;;
    status|ps)
        cmd_status
        ;;
    reset)
        cmd_reset
        ;;
    *)
        echo "Usage: $0 {start|down|logs|status|reset}"
        exit 1
        ;;
esac
```

- [ ] **Step 2: Make script executable and check bash syntax**

Run:
```bash
git update-index --chmod=+x infra/thingsboard/setup.sh 2>/dev/null || true
bash -n infra/thingsboard/setup.sh
```
Expected: `bash -n` returns exit code 0 (no syntax errors).

- [ ] **Step 3: Test command dispatcher help output**

Run:
```bash
bash infra/thingsboard/setup.sh --help || true
```
Expected: Outputs `Usage: ... {start|down|logs|status|reset}`.

- [ ] **Step 4: Commit Task 3**

```bash
git add infra/thingsboard/setup.sh
git commit -m "feat(infra): add server-scoped ThingsBoard bootstrap script (setup.sh)"
```

---

### Task 4: Documentation & Monorepo Linkage

**Files:**
- Create: `infra/thingsboard/README.md`

**Interfaces:**
- Consumes: Commands and ports from Tasks 1-3
- Produces: Operational documentation for developers and server operators.

- [ ] **Step 1: Create `infra/thingsboard/README.md`**

```markdown
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
```

- [ ] **Step 2: Verify `infra/thingsboard/README.md` exists and preview render**

Run:
```powershell
Test-Path "infra/thingsboard/README.md"
```
Expected: `True`.

- [ ] **Step 3: Commit Task 4**

```bash
git add infra/thingsboard/README.md
git commit -m "docs(infra): add ThingsBoard infrastructure operations guide"
```

---

### Task 5: End-to-End Verification & Validation

**Files:**
- Test all created files in `infra/thingsboard/`

- [ ] **Step 1: Check git status and file integrity**

Run:
```bash
git status -s infra/thingsboard
```
Expected: Clean status (all files committed).

- [ ] **Step 2: Validate docker compose configuration resolution**

Run:
```bash
docker compose --env-file infra/thingsboard/.env.example -f infra/thingsboard/docker-compose.yml config --services
```
Expected:
```
postgres
thingsboard-ce
```

- [ ] **Step 3: Validate setup script syntax and dry run commands**

Run:
```bash
bash -n infra/thingsboard/setup.sh
bash infra/thingsboard/setup.sh invalid_command || true
```
Expected: `bash -n` succeeds; invalid command shows usage output.
