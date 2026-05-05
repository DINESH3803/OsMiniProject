# Smart ICU Monitoring & Alert System
### Terminal-Based OS Mini Project — C (Linux)

A fully terminal-based Intensive Care Unit monitoring simulation demonstrating core Operating Systems concepts through a realistic multi-client, concurrent, real-time environment.

---

## OS Concepts Demonstrated

| Concept | Where |
|---|---|
| **TCP Socket Communication** | `server/server.c` ↔ all clients |
| **Multi-threading (`pthread`)** | One detached thread per client in `server/server.c` |
| **Mutex (`pthread_mutex_t`)** | Shared patient DB & user table in `server/handler.c` |
| **Semaphore (`sem_t`)** | Max 3 concurrent file writers — `server/filestore.c` |
| **File Locking (`fcntl`)** | `F_RDLCK`/`F_WRLCK` per patient file — `server/filestore.c` |
| **Signals (`SIGUSR1`)** | Server → doctor process alert — `server/ipc_alert.c` |
| **Named Pipes (`mkfifo`)** | `/tmp/icu_alert_<id>` alert channel — `server/ipc_alert.c` |
| **POSIX Message Queue** | `/icu_mqueue` audit log — `server/ipc_alert.c` |
| **Role-Based Auth** | Admin / Doctor / Nurse / Guest — `server/handler.c` |
| **Anomaly Detection** | Moving average + 2σ + static thresholds — `server/anomaly.c` |

---

## Directory Structure

```
OSminiProject/
├── Makefile
├── README.md
├── include/
│   ├── common.h      — shared constants, ANSI colours, send/recv helpers
│   ├── vitals.h      — Vitals struct, thresholds, severity checks
│   ├── patient.h     — Patient struct (circular vitals history)
│   ├── auth.h        — User struct, Role enum
│   └── ipc.h         — Alert struct, named-pipe path helper
├── server/
│   ├── server.h      — ServerState typedef + all cross-module prototypes
│   ├── server.c      — main(): socket, threads, mqueue, signal handlers
│   ├── handler.c     — per-client thread, 4 role dispatchers
│   ├── auth.c        — fcntl-locked user file, authenticate/CRUD
│   ├── filestore.c   — fcntl-locked patient files + semaphore cap
│   ├── anomaly.c     — threshold + moving-average anomaly detection
│   └── ipc_alert.c   — named pipe + SIGUSR1 + mqueue alert delivery
├── client/
│   ├── icu_client.c     — unified entry point menu
│   ├── nurse_client.c   — streams vitals (auto)
│   ├── doctor_client.c  — receives alerts via pipe + SIGUSR1
│   ├── admin_client.c   — full CRUD menu
│   └── guest_client.c   — read-only anonymised stats
├── bin/              — compiled binaries (created by make)
├── data/
│   ├── users.dat     — binary user records (fcntl locked)
│   └── patients/     — one binary file per patient (fcntl locked)
└── logs/
    └── server.log    — timestamped event log
```

---

## Build

```bash
# Install dependencies (if needed)
sudo apt-get install gcc make librt-dev   # usually pre-installed

# Build all binaries
make all

# Clean
make clean
```

---

## Running the System

Open **multiple terminals** from the project root:

### Terminal 1 — Server
```bash
./bin/icu_server
```
Creates default users and patients on first run, then listens on port 8888.

### Other Terminals — Client
```bash
./bin/icu_client
```
Presents a unified menu to select your role:

1. **Admin**: Full menu to add/delete users, add/discharge patients, and assign nurses. Displays active doctors and nurses before assignment. Cannot delete other admins.
2. **Doctor**: Registers its PID for SIGUSR1. Background thread reads named pipe. Alerts appear in real-time.
3. **Nurse**: Select an assigned patient to stream vitals every 3 seconds with a 15% chance of anomalous readings. Gracefully pauses if patient is discharged.
4. **Guest**: Shows anonymised ICU statistics only.

*(Note: Failed login attempts will prompt to retry up to 3 times before exiting.)*

---

## Default Credentials

| Username | Password | Role |
|---|---|---|
| `admin`   | `admin123` | Admin  |
| `drsmith` | `doc123`   | Doctor |
| `drpatel` | `doc456`   | Doctor |
| `nurse1`  | `nurse123` | Nurse  |
| `nurse2`  | `nurse456` | Nurse  |
| `guest`   | `guest`    | Guest  |

---

## Default Patients

| ID  | Name             | Condition             | Doctor  |
|-----|------------------|-----------------------|---------|
| 101 | John Doe         | Cardiac Arrhythmia    | drsmith |
| 102 | Jane Smith       | Respiratory Failure   | drsmith |
| 103 | Bob Johnson      | Hypertensive Crisis   | drsmith |
| 104 | Alice Williams   | Post-Cardiac Surgery  | drpatel |
| 105 | Carlos Reyes     | Septic Shock          | drpatel |

---

## Anomaly Thresholds

| Vital | Warning | Critical |
|---|---|---|
| Heart Rate (bpm) | <50 or >110 | <40 or >130 |
| Systolic BP (mmHg) | <85 or >145 | <70 or >180 |
| Diastolic BP (mmHg) | <50 or >90 | <40 or >100 |
| SpO₂ (%) | <94 | <90 |
| Temperature (°C) | <36 or >37.5 | <35 or >39 |
| Resp Rate (breaths/min) | <10 or >22 | <8 or >30 |

Additionally, any vital deviating more than **2 standard deviations** from the patient's moving average triggers a WARNING.

---

## IPC Alert Flow

```
Nurse sends anomalous vitals
         │
         ▼
   server/anomaly.c
   detect_anomaly()
         │
         ▼
   server/ipc_alert.c send_alert()
    ┌────┴────────────────────┐
    │                         │
    ▼                         ▼
Named pipe                SIGUSR1
/tmp/icu_alert_<id>  →  kill(doctor_pid)
    │                         │
    ▼                         ▼
pipe_reader thread      on_sigusr1 handler
(doctor_client)         sets alert_flag
    │
    ▼
print_alert() — popup in terminal

Also enqueued in POSIX mqueue /icu_mqueue for audit
```

---

## Permission Matrix

| Action | Admin | Doctor | Nurse | Guest |
|---|:---:|:---:|:---:|:---:|
| Input vitals | ✗ | ✗ | ✓ | ✗ |
| View own patients | ✓ | ✓ | ✓ | ✗ |
| Receive alerts | ✗ | ✓ | ✗ | ✗ |
| Manage users | ✓ | ✗ | ✗ | ✗ |
| Add/discharge patients | ✓ | ✗ | ✗ | ✗ |
| Assign nurses | ✓ | ✗ | ✗ | ✗ |
| View ICU stats | ✓ | ✓ | ✓ | ✓ |

---

## Watching it Work

```bash
# Watch patient files being updated in real-time
watch -n 1 ls -la data/patients/

# Tail the server log
tail -f logs/server.log

# Inspect alert named pipes
ls -la /tmp/icu_alert_*
```
