#!/usr/bin/env python3
import argparse
import json
import os
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterable

import psycopg2
from psycopg2.extras import execute_values


@dataclass
class DbConfig:
    host: str
    port: int
    user: str
    password: str
    dbname: str


def load_env_file(env_path: Path) -> None:
    if not env_path.exists():
        return
    for line in env_path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, value = line.split("=", 1)
        os.environ.setdefault(key.strip(), value.strip())


def load_db_config(project_root: Path) -> DbConfig:
    load_env_file(project_root / ".env")
    return DbConfig(
        host=os.getenv("DB_HOST", "localhost"),
        port=int(os.getenv("DB_PORT", "5432")),
        user=os.getenv("DB_USER", "gps_user"),
        password=os.getenv("DB_PASS", "gps_password"),
        dbname=os.getenv("DB_NAME", "gps_db"),
    )


def to_int(value: Any) -> int | None:
    if value is None:
        return None
    if isinstance(value, bool):
        return int(value)
    try:
        return int(value)
    except (ValueError, TypeError):
        return None


def to_float(value: Any) -> float | None:
    if value is None:
        return None
    try:
        return float(value)
    except (ValueError, TypeError):
        return None


def build_cell_row(location_id: int, cell: dict[str, Any]) -> tuple[Any, ...]:
    cell_type = str(cell.get("type", "unknown"))
    is_nr = cell_type == "nr"
    return (
        location_id,
        cell_type,
        None if is_nr else to_int(cell.get("ci")),
        to_int(cell.get("pci")),
        to_int(cell.get("tac")),
        to_int(cell.get("rsrp")),
        to_int(cell.get("rsrq")),
        to_int(cell.get("rssi")),
        to_int(cell.get("ta")),
        to_int(cell.get("lac")),
        to_int(cell.get("nci")) if is_nr else None,
        to_int(cell.get("ssRsrp")),
        to_int(cell.get("ssRsrq")),
        to_int(cell.get("ssSinr")),
    )


def iter_json_lines(path: Path) -> Iterable[tuple[int, dict[str, Any] | None]]:
    with path.open("r", encoding="utf-8") as f:
        for line_no, raw in enumerate(f, start=1):
            raw = raw.strip()
            if not raw:
                continue
            try:
                yield line_no, json.loads(raw)
            except json.JSONDecodeError:
                yield line_no, None


def import_jsonl_to_db(
    input_file: Path,
    config: DbConfig,
    batch_size: int = 1000,
    truncate: bool = False,
) -> dict[str, int]:
    stats = {
        "lines_total": 0,
        "lines_json_error": 0,
        "records_skipped": 0,
        "locations_inserted": 0,
        "cells_inserted": 0,
    }

    conn = psycopg2.connect(
        host=config.host,
        port=config.port,
        user=config.user,
        password=config.password,
        dbname=config.dbname,
    )
    conn.autocommit = False
    cur = conn.cursor()

    try:
        if truncate:
            cur.execute("TRUNCATE TABLE cell_data, location_data RESTART IDENTITY;")
            conn.commit()

        buffered_records: list[tuple[tuple[Any, Any, Any, Any], list[dict[str, Any]]]] = []

        for line_no, data in iter_json_lines(input_file):
            stats["lines_total"] += 1
            if data is None:
                stats["lines_json_error"] += 1
                continue

            location = data.get("location") or {}
            lat = to_float(location.get("_Latitude"))
            lon = to_float(location.get("_Longitude"))
            alt = to_float(location.get("_Altitude"))
            ts = to_int(data.get("time"))
            if ts is None:
                ts = to_int(location.get("_Time"))

            if lat is None or lon is None or ts is None:
                stats["records_skipped"] += 1
                continue

            cells = data.get("cells")
            if not isinstance(cells, list):
                cells = []

            buffered_records.append(((lat, lon, alt, ts), cells))

            if len(buffered_records) >= batch_size:
                insert_batch(cur, buffered_records, stats)
                conn.commit()
                buffered_records.clear()

        if buffered_records:
            insert_batch(cur, buffered_records, stats)
            conn.commit()

    except Exception:
        conn.rollback()
        raise
    finally:
        cur.close()
        conn.close()

    return stats


def insert_batch(
    cur: psycopg2.extensions.cursor,
    records: list[tuple[tuple[Any, Any, Any, Any], list[dict[str, Any]]]],
    stats: dict[str, int],
) -> None:
    location_rows = [record[0] for record in records]
    location_ids = execute_values(
        cur,
        """
        INSERT INTO location_data (latitude, longitude, altitude, timestamp)
        VALUES %s
        RETURNING id
        """,
        location_rows,
        fetch=True,
    )
    ids = [row[0] for row in location_ids]
    stats["locations_inserted"] += len(ids)

    cell_rows: list[tuple[Any, ...]] = []
    for location_id, (_, cells) in zip(ids, records):
        for cell in cells:
            if isinstance(cell, dict):
                cell_rows.append(build_cell_row(location_id, cell))

    if cell_rows:
        execute_values(
            cur,
            """
            INSERT INTO cell_data (
                location_id, cell_type, ci, pci, tac, rsrp, rsrq, rssi, ta, lac, nci, ss_rsrp, ss_rsrq, ss_sinr
            )
            VALUES %s
            """,
            cell_rows,
        )
        stats["cells_inserted"] += len(cell_rows)


def main() -> None:
    script_dir = Path(__file__).resolve().parent
    project_root = script_dir.parent
    default_input = project_root / "build" / "Location_save_data" / "received_data.jsonl"

    parser = argparse.ArgumentParser(description="Import JSONL GPS/cell data into PostgreSQL")
    parser.add_argument("--input", type=Path, default=default_input, help="Path to JSONL file")
    parser.add_argument("--batch-size", type=int, default=1000, help="Insert batch size")
    parser.add_argument("--truncate", action="store_true", help="Truncate target tables before import")
    args = parser.parse_args()

    if not args.input.exists():
        raise FileNotFoundError(f"Input file not found: {args.input}")

    db_config = load_db_config(project_root)
    stats = import_jsonl_to_db(args.input, db_config, batch_size=args.batch_size, truncate=args.truncate)

    print("Import completed")
    for key, value in stats.items():
        print(f"{key}: {value}")


if __name__ == "__main__":
    main()
