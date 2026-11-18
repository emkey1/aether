# Sourced by the Ollama scoring drivers (tools/score_ollama_model.sh). One Ollama-heavy job at a time on a scoring Mac
# (2026-10-19: two 55 GB models resident at once plus an import crashed it), and nothing else left loaded when this
# job's model loads. The lock is a directory holding the owner's pid; a dead owner's lock is taken over.
# usage: . tools/ollama_guard.sh MODEL_NAME          lock: $OLLAMA_GUARD_LOCK (default ~/aether-models/ollama.lock)
GUARD_LOCK=${OLLAMA_GUARD_LOCK:-$HOME/aether-models/ollama.lock}
mkdir -p "$(dirname "$GUARD_LOCK")"
until mkdir "$GUARD_LOCK" 2>/dev/null; do
  owner=$(cat "$GUARD_LOCK/pid" 2>/dev/null)
  if [ -n "$owner" ] && ! kill -0 "$owner" 2>/dev/null; then rm -rf "$GUARD_LOCK"; continue; fi   # stale (crash)
  sleep 60
done
echo $$ > "$GUARD_LOCK/pid"
trap 'rm -rf "$GUARD_LOCK"' EXIT
for _guard_m in $(ollama ps 2>/dev/null | awk 'NR>1{print $1}'); do
  [ "$_guard_m" = "$1:latest" ] || [ "$_guard_m" = "$1" ] || ollama stop "$_guard_m"
done
