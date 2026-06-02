#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
SESSION_NAME="${GRAFFITY_SESSION_NAME:-graffity}"
ROOT_DIR="${GRAFFITY_ROOT_DIR:-$SCRIPT_DIR}"
BUILD_DIR="${GRAFFITY_BUILD_DIR:-$ROOT_DIR/build}"
BIN_PATH="${GRAFFITY_BIN_PATH:-$BUILD_DIR/graffitysrv}"
STATE_DIR="${GRAFFITY_STATE_DIR:-$ROOT_DIR/run}"
STATE_FILE="${GRAFFITY_STATE_FILE:-$STATE_DIR/graffity_state.json}"
GRAFFITY_BIND="${GRAFFITY_BIND:-0.0.0.0:8781}"
PORT_PART="${GRAFFITY_BIND##*:}"
STATUS_PORT="${PORT_PART//[^0-9]/}"

usage() {
	echo "Usage: $0 [start|stop|restart|status|logs|attach|build] [bind] [state-file]" >&2
	exit 1
}

is_running() {
	tmux has-session -t "$SESSION_NAME" 2>/dev/null
}

print_status() {
	if is_running; then
		echo "graffity: running (tmux session $SESSION_NAME)"
	else
		echo "graffity: stopped"
	fi
	echo "root: $ROOT_DIR"
	echo "bind: $GRAFFITY_BIND"
	echo "state: $STATE_FILE"
	pgrep -af "$BIN_PATH" || true
	if [[ -n "$STATUS_PORT" ]]; then
		ss -ltnp | grep ":$STATUS_PORT" || true
	fi
}

build_server() {
	mkdir -p "$BUILD_DIR"
	(
		cd "$ROOT_DIR"
		go build -trimpath -o "$BIN_PATH" .
	)
}

start_server() {
	local bind="${1:-$GRAFFITY_BIND}"
	local state_file="${2:-$STATE_FILE}"

	mkdir -p "$STATE_DIR" "$BUILD_DIR"
	if [[ ! -x "$BIN_PATH" ]]; then
		echo "graffity binary not found, building: $BIN_PATH"
		build_server
	fi

	if is_running; then
		echo "graffity is already running"
		print_status
		exit 0
	fi

	tmux new-session -d -s "$SESSION_NAME" "bash -lc 'cd \"$ROOT_DIR\" && exec \"$BIN_PATH\" \"$bind\" \"$state_file\"'"
	sleep 1
	if ! is_running; then
		echo "failed to start graffity" >&2
		exit 1
	fi
	print_status
}

stop_server() {
	if is_running; then
		tmux kill-session -t "$SESSION_NAME"
	fi
	pkill -f "$BIN_PATH" 2>/dev/null || true
	sleep 1
	echo "graffity stopped"
	print_status || true
}

logs_server() {
	if ! is_running; then
		echo "graffity is not running" >&2
		exit 1
	fi
	tmux capture-pane -pt "$SESSION_NAME:0" -S -120
}

attach_server() {
	if ! is_running; then
		echo "graffity is not running in tmux" >&2
		exit 1
	fi
	exec tmux attach -t "$SESSION_NAME"
}

ACTION="${1:-status}"
shift || true

case "$ACTION" in
	start) start_server "$@" ;;
	stop) stop_server ;;
	restart)
		stop_server
		start_server "$@"
		;;
	status) print_status ;;
	logs) logs_server ;;
	attach) attach_server ;;
	build) build_server ;;
	*) usage ;;
esac
