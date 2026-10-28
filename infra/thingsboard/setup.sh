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
