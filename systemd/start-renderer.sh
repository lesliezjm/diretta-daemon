#!/bin/bash
# Diretta Host Daemon - Startup Wrapper Script
# This script reads configuration and starts the daemon with appropriate options

set -e

# Values sourced from /etc/default/diretta-renderer via EnvironmentFile in the systemd unit.
SOCKET_PATH="${SOCKET_PATH:-/tmp/diretta-renderer.sock}"
VERBOSE="${VERBOSE:-}"
INTERFACE="${INTERFACE:-${NETWORK_INTERFACE:-}}"
THREAD_MODE="${THREAD_MODE:-}"
CYCLE_TIME="${CYCLE_TIME:-}"
CYCLE_MIN_TIME="${CYCLE_MIN_TIME:-}"
INFO_CYCLE="${INFO_CYCLE:-}"
TRANSFER_MODE="${TRANSFER_MODE:-}"
TARGET_PROFILE_LIMIT="${TARGET_PROFILE_LIMIT:-}"
MTU="${MTU:-${MTU_OVERRIDE:-}}"

# Process priority defaults
NICE_LEVEL="${NICE_LEVEL:--10}"
IO_SCHED_CLASS="${IO_SCHED_CLASS:-realtime}"
IO_SCHED_PRIORITY="${IO_SCHED_PRIORITY:-0}"
RT_PRIORITY="${RT_PRIORITY:-50}"

RENDERER_BIN="/opt/diretta-renderer/DirettaRenderer"

# Build command as array (preserves arguments with spaces)
CMD=("$RENDERER_BIN")

# IPC socket path
if [ -n "$SOCKET_PATH" ]; then
    CMD+=("--socket-path" "$SOCKET_PATH")
fi

# Network interface option (for multi-homed systems)
if [ -n "$INTERFACE" ]; then
    echo "Binding to network interface: $INTERFACE"
    CMD+=("--interface" "$INTERFACE")
fi

# Log verbosity (--verbose or --quiet)
if [ -n "$VERBOSE" ]; then
    CMD+=($VERBOSE)
fi

# Advanced Diretta settings (only if specified)
if [ -n "$THREAD_MODE" ]; then
    CMD+=("--thread-mode" "$THREAD_MODE")
fi

if [ -n "$CYCLE_TIME" ]; then
    CMD+=("--cycle-time" "$CYCLE_TIME")
fi

if [ -n "$CYCLE_MIN_TIME" ]; then
    CMD+=("--cycle-min-time" "$CYCLE_MIN_TIME")
fi

if [ -n "$INFO_CYCLE" ]; then
    CMD+=("--info-cycle" "$INFO_CYCLE")
fi

if [ -n "$TRANSFER_MODE" ]; then
    CMD+=("--transfer-mode" "$TRANSFER_MODE")
fi

if [ -n "$TARGET_PROFILE_LIMIT" ]; then
    CMD+=("--target-profile-limit" "$TARGET_PROFILE_LIMIT")
fi

if [ -n "$MTU" ]; then
    CMD+=("--mtu" "$MTU")
fi

if [ -n "$RT_PRIORITY" ] && [ "$RT_PRIORITY" != "50" ]; then
    CMD+=("--rt-priority" "$RT_PRIORITY")
fi

# Build exec prefix as array for process priority
EXEC_PREFIX=()

# Apply nice level (skip if nice itself is unavailable or fails — e.g. in some container environments)
if [ -n "$NICE_LEVEL" ] && [ "$NICE_LEVEL" != "0" ]; then
    # Test with a short-lived external command: if nice fails, the subshell exits but
    # || true ensures set -e in the parent does not fire, and we skip the priority wrapper.
    if (nice -n "$NICE_LEVEL" /bin/sleep 0.1 2>/dev/null) 2>/dev/null; then
        EXEC_PREFIX=("nice" "-n" "$NICE_LEVEL")
    else
        echo "   (nice unavailable in this environment, skipping process priority)"
    fi
fi

# Apply I/O scheduling.
# In some container/VM environments ionice may be denied even with CAP_SYS_NICE.
# We test it first to avoid killing the entire daemon start when it fails.
if [ -n "$IO_SCHED_CLASS" ]; then
    case "$IO_SCHED_CLASS" in
        realtime|1)    IONICE_CLASS=1 ;;
        best-effort|2) IONICE_CLASS=2 ;;
        idle|3)        IONICE_CLASS=3 ;;
        *)             IONICE_CLASS="" ;;
    esac

    if [ -n "$IONICE_CLASS" ]; then
        # Test with a short-lived external command: if ionice fails, skip gracefully.
        if (ionice -c "$IONICE_CLASS" -n "${IO_SCHED_PRIORITY:-0}" /bin/sleep 0.1) 2>/dev/null; then
            if [ "$IONICE_CLASS" = "3" ]; then
                EXEC_PREFIX=("ionice" "-c" "$IONICE_CLASS" "${EXEC_PREFIX[@]}")
            else
                EXEC_PREFIX=("ionice" "-c" "$IONICE_CLASS" "-n" "${IO_SCHED_PRIORITY:-0}" "${EXEC_PREFIX[@]}")
            fi
        else
            echo "   (ionice unavailable in this environment, skipping I/O scheduling)"
        fi
    fi
fi

# Log the command being executed
echo "════════════════════════════════════════════════════════"
echo "  Starting Diretta Host Daemon"
echo "════════════════════════════════════════════════════════"
echo ""
echo "Configuration:"
echo "  Socket:            $SOCKET_PATH"
echo "  Network Interface: ${INTERFACE:-auto-detect}"
echo "  Nice level:        $NICE_LEVEL"
echo "  I/O scheduling:    $IO_SCHED_CLASS (priority $IO_SCHED_PRIORITY)"
echo "  RT priority:       $RT_PRIORITY (SCHED_FIFO)"
echo ""
echo "Command:"
echo "  ${EXEC_PREFIX[*]} ${CMD[*]}"
echo ""
echo "════════════════════════════════════════════════════════"
echo ""

# Execute with priority settings
exec "${EXEC_PREFIX[@]}" "${CMD[@]}"
