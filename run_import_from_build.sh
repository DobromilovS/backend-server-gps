#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
IMPORT_SCRIPT="${PROJECT_DIR}/src/import_json_to_db.py"
INPUT_FILE="${1:-${PROJECT_DIR}/build/Location_save_data/received_data.jsonl}"
BATCH_SIZE="${BATCH_SIZE:-1000}"
TRUNCATE_BEFORE_IMPORT="${TRUNCATE_BEFORE_IMPORT:-0}"

if [[ ! -f "${INPUT_FILE}" ]]; then
    echo "Ошибка: входной файл не найден: ${INPUT_FILE}"
    exit 1
fi

PY_CMD=""
if [[ -x "${PROJECT_DIR}/.venv/bin/python" ]] && "${PROJECT_DIR}/.venv/bin/python" -c "import psycopg2" >/dev/null 2>&1; then
    PY_CMD="${PROJECT_DIR}/.venv/bin/python"
elif command -v python3 >/dev/null 2>&1 && python3 -c "import psycopg2" >/dev/null 2>&1; then
    PY_CMD="python3"
elif command -v python >/dev/null 2>&1 && python -c "import psycopg2" >/dev/null 2>&1; then
    PY_CMD="python"
else
    echo "Ошибка: не найден Python с модулем psycopg2."
    echo "Установи зависимость, например: pip install psycopg2-binary"
    exit 1
fi

echo "Импортирую JSONL в БД..."
echo "Файл: ${INPUT_FILE}"
echo "Интерпретатор: ${PY_CMD}"

CMD=("${PY_CMD}" "${IMPORT_SCRIPT}" "--input" "${INPUT_FILE}" "--batch-size" "${BATCH_SIZE}")
if [[ "${TRUNCATE_BEFORE_IMPORT}" == "1" ]]; then
    CMD+=("--truncate")
fi

"${CMD[@]}"

echo "Готово."
