# Contributing to WatcheRobot Firmware

Thank you for your interest in contributing! This guide covers everything you need to get started.

---

## Table of Contents

1. [Code of Conduct](#code-of-conduct)
2. [Development Environment](#development-environment)
3. [Project Structure](#project-structure)
4. [Component Design Rules](#component-design-rules)
5. [Code Style](#code-style)
6. [Commit Message Format](#commit-message-format)
7. [Pull Request Process](#pull-request-process)
8. [Testing Requirements](#testing-requirements)
9. [Issue Reporting](#issue-reporting)

---

## Code of Conduct

This project follows the [Contributor Covenant](CODE_OF_CONDUCT.md). By participating, you agree to uphold a respectful and inclusive environment.

---

## Development Environment

### Required Tools

| Tool | Version | Notes |
|------|---------|-------|
| ESP-IDF | **v5.2.1** | Do not use other versions — `sdkconfig.defaults` is tuned for v5.2.1 |
| Python | 3.10+ | For tools scripts and `watcher-server` |
| clang-format | 18.x | For code formatting |

### Setup (Windows)

```powershell
# 1. Install ESP-IDF v5.2.1
#    https://docs.espressif.com/projects/esp-idf/en/v5.2.1/get-started/windows-setup.html

# 2. Activate ESP-IDF (run before any idf.py commands)
C:\Espressif\frameworks\esp-idf-v5.2.1\export.ps1

# 3. Install pre-commit hooks
pip install pre-commit
pre-commit install
```

### Setup (Linux/macOS)

```bash
# 1. Install ESP-IDF v5.2.1
mkdir -p ~/esp && cd ~/esp
git clone -b v5.2.1 --recursive https://github.com/espressif/esp-idf.git
~/esp/esp-idf/install.sh esp32s3

# 2. Activate
. ~/esp/esp-idf/export.sh

# 3. Install pre-commit
pip install pre-commit && pre-commit install
```

### Build & Flash

```bash
cd firmware/s3
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor   # Linux
idf.py -p COM3 flash monitor            # Windows
```

> ⚠️ **Windows users**: Do not run `idf.py` in the standard CMD/bash terminal. Always use the **ESP-IDF PowerShell** (or CMD) environment activated by `export.ps1`.

---

## Project Structure

The firmware uses a strict four-layer component architecture:

```
firmware/s3/components/
├── drivers/        # Layer 1 — Board Support Package (BSP)
├── hal/            # Layer 2 — Hardware Abstraction Layer
├── protocols/      # Layer 3 — Communication Protocols
├── services/       # Layer 4 — Business Logic / Services
└── utils/          # Cross-layer utilities
```

**Layer dependency rule** (strictly enforced):
```
services → hal, protocols, utils     ✅
protocols → hal, utils               ✅
hal → drivers, utils                 ✅
drivers → ESP-IDF only               ✅
utils → ESP-IDF only                 ✅

services → services (cross-service)  ❌ Not allowed
hal → services                       ❌ Not allowed
utils → custom components            ❌ Not allowed
```

See [docs/architecture.md](docs/architecture.md) for full design details.

---

## Component Design Rules

Every component **must** include:

```
my_component/
├── CMakeLists.txt         # Required
├── idf_component.yml      # Required — version, description, license
├── Kconfig                # Required — all configurable params via menuconfig
├── README.md              # Required — purpose, API summary, usage example
├── CHANGELOG.md           # Required — version history
├── include/               # Public headers with Doxygen comments
│   └── my_component.h
├── src/                   # Implementation
│   ├── my_component.c
│   └── my_component_internal.h
└── test_apps/             # Unity tests (required for new components)
    └── main/
        ├── CMakeLists.txt
        └── test_my_component.c
```

### CMakeLists.txt dependency rules

```cmake
idf_component_register(
    SRCS "src/my_component.c"
    INCLUDE_DIRS "include"
    REQUIRES     "..."   # Only components used in PUBLIC headers
    PRIV_REQUIRES "..."  # Components used ONLY in .c files (preferred)
)
```

### Kconfig: no hardcoding

All GPIO pins, thresholds, and tuneable parameters must be exposed via Kconfig:

```kconfig
menu "My Component"
    config MY_COMPONENT_GPIO
        int "GPIO number"
        default 19
        help
            Description of the pin and its electrical requirements.
endmenu
```

### Doxygen: all public APIs

```c
/**
 * @brief One-line summary.
 *
 * Extended explanation if needed.
 *
 * @param[in]  foo  Description of foo
 * @param[out] bar  Description of bar
 *
 * @return
 *   - ESP_OK     on success
 *   - ESP_FAIL   if hardware not ready
 */
esp_err_t my_component_do_thing(int foo, int *bar);
```

---

## Code Style

This project uses **clang-format** with the configuration in `.clang-format` (LLVM-based, aligned with ESP-IDF style).

```bash
# Format a single file
clang-format -i firmware/s3/components/hal/hal_servo/src/hal_servo.c

# Check formatting (CI does this automatically)
clang-format --dry-run --Werror firmware/s3/components/**/*.c
```

**Naming conventions:**

| Item | Convention | Example |
|------|-----------|---------|
| Functions | `{component}_{verb}_{noun}` | `hal_servo_set_angle()` |
| Types | `{component}_{name}_t` | `servo_axis_t` |
| Constants/macros | `UPPER_SNAKE_CASE` | `SERVO_Y_MIN_DEG` |
| Files | `snake_case.c/.h` | `hal_servo_internal.h` |
| Kconfig symbols | `WATCHER_{COMPONENT}_{PARAM}` | `WATCHER_SERVO_X_GPIO` |

**Error handling:**
- All functions return `esp_err_t`
- Callers use `ESP_ERROR_CHECK()` for critical paths or explicit `if (ret != ESP_OK)` handling
- Never silently ignore errors

---

## Commit Message Format

This project uses [Conventional Commits](https://www.conventionalcommits.org/):

```
<type>(<scope>): <short description>

[optional body]
```

**Types:** `feat` `fix` `refactor` `docs` `test` `chore` `perf` `ci`

**Scopes:** component name or area — e.g. `hal_servo`, `anim_service`, `ws_client`, `docs`, `ci`

**Examples:**

```
feat(hal_servo): add synchronized dual-axis smooth move
fix(anim_service): release PSRAM buffer before loading new animation type
refactor(ws_client): extract protocol parsing into ws_protocol.c
docs(hal_camera): add Doxygen for capture_once API
test(voice_service): add Unity tests for VAD timeout logic
ci: add clang-format check to build workflow
```

---

## Pull Request Process

1. **Fork** the repository and create a branch from `main`:
   ```bash
   git checkout -b feat/hal-servo-sync-move
   ```

2. **Follow** the component design rules above

3. **Write tests** in `test_apps/` for any new logic

4. **Run locally** before submitting:
   ```bash
   idf.py build                  # Must compile without warnings
   pre-commit run --all-files    # Must pass clang-format
   ```

5. **Open a PR** using the [PR template](.github/PULL_REQUEST_TEMPLATE.md)

6. **Address review comments** — PRs require at least one approval before merging

### PR Checklist

- [ ] Code compiles without warnings (`idf.py build`)
- [ ] `clang-format` passes (`pre-commit run --all-files`)
- [ ] New component includes `idf_component.yml`, `Kconfig`, `README.md`, `CHANGELOG.md`
- [ ] All public APIs have Doxygen comments
- [ ] New configurable values exposed via Kconfig (no hardcoding)
- [ ] `test_apps/` updated with Unity tests for new logic
- [ ] `CHANGELOG.md` updated under `[Unreleased]`

---

## Testing Requirements

- All new components must include **Unity unit tests** in `test_apps/`
- Tests must pass on the ESP32-S3 target
- For pure-logic modules, host tests (CMake + MinGW on Windows / native GCC on Linux) are preferred for faster iteration

```c
// Example test structure
TEST_CASE("servo init succeeds", "[hal_servo]")
{
    TEST_ASSERT_EQUAL(ESP_OK, hal_servo_init());
}

TEST_CASE("servo clamps angle to valid range", "[hal_servo]")
{
    hal_servo_set_angle(SERVO_AXIS_X, 200);   // over limit
    TEST_ASSERT_EQUAL(180, hal_servo_get_angle(SERVO_AXIS_X));
}
```

---

## Issue Reporting

Use the issue templates:
- 🐛 [Bug Report](.github/ISSUE_TEMPLATE/bug_report.md)
- 💡 [Feature Request](.github/ISSUE_TEMPLATE/feature_request.md)

For **security vulnerabilities**, please do **not** open a public issue. Contact the maintainers directly.
