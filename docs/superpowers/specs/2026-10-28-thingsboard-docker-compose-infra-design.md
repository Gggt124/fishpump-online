# ThingsBoard CE Docker Compose Infrastructure Design

**Date:** 2026-10-28  
**Status:** Approved  
**Author:** Pair Programming Agent & User  
**Target Path:** `infra/thingsboard/`  

---

## 1. Executive Summary & Goals

The objective of this design is to establish a standalone, production-ready, and portable Docker Compose infrastructure for **ThingsBoard Community Edition (CE)** in the `infra/thingsboard/` directory of the `fishpump-online` monorepo.

### Key Goals:
1. **Local & LAN Testing**: Enable a central LAN test server to run ThingsBoard CE, allowing ESP32-S3 firmware nodes and developer machines to connect over the local network without running Docker on every workstation.
2. **VPS & Production Readiness**: Ensure seamless transition to a remote VPS or cloud environment using the exact same configuration files.
3. **Server-Scoped One-Click Setup (`setup.sh`)**: Provide an automated bootstrap script dedicated solely to the server machine that manages Docker resources, initializes the database schema idempotently, and starts the services without touching server host OS configurations.
4. **Data Persistence & Security**: Guarantee telemetry, rule chains, and device credentials persist across container updates/restarts, while keeping PostgreSQL isolated on an internal Docker network.

---

## 2. Architecture & File Layout

All infrastructure files are located under `infra/thingsboard/`:

```text
infra/thingsboard/
├── docker-compose.yml       # Service definitions (thingsboard-ce & postgres)
├── .env.example             # Documented default environment configuration
├── .env                     # Git-ignored runtime environment configuration
├── setup.sh                 # Executable bootstrap & management script for servers (Linux)
└── README.md                # Quick-start documentation and operational guide
```

---

## 3. Docker Services Specification

### 3.1 `postgres` Service
* **Image**: `postgres:16-alpine` (lightweight, minimal attack surface, low memory footprint)
* **Internal Network**: Joined to custom bridge network `tb-net`
* **Exposed Ports**: None to host (keeps PostgreSQL secure from external attacks on both LAN and public VPS)
* **Healthcheck**:
  ```yaml
  test: ["CMD-SHELL", "pg_isready -U ${POSTGRES_USER:-postgres} -d ${POSTGRES_DB:-thingsboard}"]
  interval: 5s
  timeout: 5s
  retries: 10
  start_period: 10s
  ```
* **Volumes**:
  * `postgres-data:/var/lib/postgresql/data`
* **Restart Policy**: `unless-stopped`

### 3.2 `thingsboard-ce` Service
* **Image**: `thingsboard/tb-node:latest`
* **Internal Network**: Joined to custom bridge network `tb-net`
* **Dependencies**: Depends on `postgres` with condition `service_healthy`
* **Port Mappings**:
  * `${TB_HTTP_PORT:-8080}:8080` (Web UI & REST API)
  * `${TB_MQTT_PORT:-1883}:1883` (MQTT Broker for ESP32-S3 telemetry/RPC)
* **Environment Configuration**:
  * `TB_QUEUE_TYPE=${TB_QUEUE_TYPE:-in-memory}`
  * `SPRING_DATASOURCE_URL=jdbc:postgresql://postgres:5432/${POSTGRES_DB:-thingsboard}`
  * `SPRING_DATASOURCE_USERNAME=${POSTGRES_USER:-postgres}`
  * `SPRING_DATASOURCE_PASSWORD=${POSTGRES_PASSWORD:-thingsboard_secret_pass}`
  * `JAVA_OPTS=${JAVA_OPTS:--Xms512M -Xmx2G}`
* **Volumes**:
  * `tb-log:/var/log/thingsboard`
  * `tb-data:/usr/share/thingsboard/data`
* **Restart Policy**: `unless-stopped`

### 3.3 Named Volumes & Networks
* **Volumes**:
  * `postgres-data`: Persistent database storage
  * `tb-log`: Log files
  * `tb-data`: ThingsBoard application state
* **Networks**:
  * `tb-net`: Custom bridge driver

---

## 4. Setup Script Specification (`setup.sh`)

### 4.1 Strict Server-Scope Constraint
`setup.sh` is strictly scoped to Docker and ThingsBoard operations. It does **not** modify `/etc`, install system packages via `apt`/`yum`, or alter global OS configurations.

### 4.2 Lifecycle & Execution Flow

```text
               +-----------------------+
               |    Run ./setup.sh     |
               +-----------+-----------+
                           |
            Verify docker & docker compose
                           |
          [Missing .env] --+-- [Found .env]
                 |                     |
     Copy .env.example -> .env         |
                 |                     |
                 +----------+----------+
                            |
           Check database initialization flag
            (file .initialized or db check)
                            |
            +---------------+---------------+
            |                               |
       [First Time]                   [Initialized]
            |                               |
Run one-off tb-node container               |
with INSTALL_TB=true                        |
            |                               |
Write .initialized marker                   |
            |                               |
            +---------------+---------------+
                            |
               docker compose up -d
                            |
         Detect host IP (LAN / Public)
                            |
        Print summary & ready endpoints
```

### 4.3 Supported Script Subcommands
* `./setup.sh` (or `./setup.sh start`): Initialize if needed and launch stack in background.
* `./setup.sh down`: Gracefully stop containers.
* `./setup.sh logs`: Tail combined container logs (`docker compose logs -f`).
* `./setup.sh status`: Display status and health of the containers (`docker compose ps`).
* `./setup.sh reset`: Interactive prompt warning user, stops containers, and purges volumes to start completely fresh.

---

## 5. Configuration & Environment Variables (`.env.example`)

```dotenv
# =================================================================
# ThingsBoard Docker Compose Environment Configuration
# =================================================================

# Port Configuration
TB_HTTP_PORT=8080
TB_MQTT_PORT=1883

# PostgreSQL Database Configuration
POSTGRES_DB=thingsboard
POSTGRES_USER=postgres
POSTGRES_PASSWORD=thingsboard_secret_pass

# ThingsBoard JVM & Resource Limits
# Default allocation: 512MB initial, 2GB maximum heap
JAVA_OPTS=-Xms512M -Xmx2G

# Queue Configuration (in-memory for standalone CE)
TB_QUEUE_TYPE=in-memory
```

---

## 6. Portability, CI/CD & Remote Testing Workflow

1. **Central LAN Test Server**:
   * Server runs `./setup.sh`.
   * Developer machines access UI via `http://<LAN_SERVER_IP>:8080`.
   * ESP32-S3 connects to MQTT at `<LAN_SERVER_IP>:1883`.
2. **VPS Deployment**:
   * Clone/pull repository on VPS.
   * Customize `.env` (optional: strong DB password).
   * Run `./setup.sh`.
3. **CI/CD Compatibility (GitHub Actions)**:
   * When `infra/thingsboard/` changes are pushed to `main`, an SSH workflow can execute:
     ```bash
     cd infra/thingsboard && docker compose pull && docker compose up -d
     ```
   * Because database schema is already initialized, updates are seamless with zero configuration loss.

---

## 7. Verification & Testing Strategy

1. **Syntax & Compose Validation**:
   * Run `docker compose config` inside `infra/thingsboard` to confirm YAML syntax and variable substitutions.
2. **First-run Schema Provisioning**:
   * Verify that `./setup.sh` correctly triggers schema creation on first run without errors.
3. **Healthcheck & Port Verification**:
   * Confirm `postgres` reaches `healthy` state.
   * Confirm `thingsboard-ce` port `8080` returns HTTP 200/302.
   * Confirm port `1883` accepts TCP connections for MQTT.
4. **Idempotence Verification**:
   * Run `./setup.sh` a second time; ensure it bypasses re-initialization and leaves existing data intact.
5. **Persistence Verification**:
   * Create a test entity in ThingsBoard.
   * Run `./setup.sh down` followed by `./setup.sh`.
   * Verify the test entity is still present.

