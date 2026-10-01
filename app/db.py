import asyncio
import os
from contextlib import asynccontextmanager
from datetime import timedelta

import asyncpg

_PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def _load_dotenv() -> None:
    """Minimal .env loader so local runs pick up DATABASE_URL from the project root."""
    path = os.path.join(_PROJECT_ROOT, ".env")
    if not os.path.isfile(path):
        return
    with open(path, encoding="utf-8") as fh:
        for line in fh:
            line = line.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            key, _, value = line.partition("=")
            key, value = key.strip(), value.strip().strip("'\"")
            os.environ.setdefault(key, value)


_load_dotenv()

DATABASE_URL = os.environ.get(
    "DATABASE_URL", "postgresql://postgres:postgres@localhost:5432/tankdata"
)

# asyncpg wants postgresql:// not postgresql+asyncpg://
if DATABASE_URL.startswith("postgresql+"):
    DATABASE_URL = DATABASE_URL.replace("postgresql+", "postgresql://", 1)

# asyncpg's ssl=prefer handshake fails against some servers; default to
# plaintext unless overridden via ?sslmode=require in DATABASE_URL or env.
SSL_MODE = os.environ.get("DATABASE_SSL_MODE")
if SSL_MODE is None:
    SSL_MODE = "disable" if "sslmode=" not in DATABASE_URL else None

pool: asyncpg.Pool | None = None

CREATE_TABLE_SQL = """
CREATE TABLE IF NOT EXISTS temperature_readings (
    id BIGSERIAL PRIMARY KEY,
    temperature_c DOUBLE PRECISION NOT NULL,
    room_temperature_c DOUBLE PRECISION,
    humidity DOUBLE PRECISION,
    recorded_at TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE INDEX IF NOT EXISTS idx_temperature_readings_recorded_at
    ON temperature_readings (recorded_at DESC);
-- Patch existing deployments that predate room/humidity columns.
ALTER TABLE temperature_readings ADD COLUMN IF NOT EXISTS room_temperature_c DOUBLE PRECISION;
ALTER TABLE temperature_readings ADD COLUMN IF NOT EXISTS humidity DOUBLE PRECISION;
"""


async def init_db(retries: int = 10, delay_s: float = 3.0) -> None:
    global pool
    kwargs = {}
    if SSL_MODE:
        kwargs["ssl"] = SSL_MODE
    # Retry startup so a slow/late-starting Postgres doesn't kill the container.
    last_err: Exception | None = None
    for attempt in range(1, retries + 1):
        try:
            pool = await asyncpg.create_pool(DATABASE_URL, min_size=1, max_size=5, **kwargs)
            break
        except (OSError, asyncpg.PostgresError) as err:
            last_err = err
            if attempt == retries:
                raise
            print(f"[db] connect attempt {attempt}/{retries} failed: {err}; retrying in {delay_s}s", flush=True)
            await asyncio.sleep(delay_s)
    assert pool is not None
    async with pool.acquire() as conn:
        await conn.execute(CREATE_TABLE_SQL)


async def close_db() -> None:
    global pool
    if pool is not None:
        await pool.close()
        pool = None


def get_pool() -> asyncpg.Pool:
    if pool is None:
        raise RuntimeError("Database pool not initialised")
    return pool


RANGE_WINDOWS = {
    "hour": timedelta(hours=1),
    "24h": timedelta(days=1),
    "7d": timedelta(days=7),
    "30d": timedelta(days=30),
    "all": None,
}


async def insert_reading(
    temperature_c: float,
    room_temperature_c: float | None = None,
    humidity: float | None = None,
) -> asyncpg.Record:
    async with get_pool().acquire() as conn:
        return await conn.fetchrow(
            """
            INSERT INTO temperature_readings (temperature_c, room_temperature_c, humidity, recorded_at)
            VALUES ($1, $2, $3, now() AT TIME ZONE 'UTC')
            RETURNING id, temperature_c, room_temperature_c, humidity, recorded_at
            """,
            temperature_c,
            room_temperature_c,
            humidity,
        )


async def fetch_series(range_key: str, bucket: str | None) -> list[asyncpg.Record]:
    window = RANGE_WINDOWS.get(range_key)
    where = "WHERE recorded_at >= now() - $1::interval" if window else ""
    args: list = [window] if window else []

    if bucket:
        sql = f"""
            SELECT date_trunc('{bucket}', recorded_at) AS bucket_start,
                   avg(temperature_c)::float AS temperature_c,
                   avg(room_temperature_c)::float AS room_temperature_c,
                   avg(humidity)::float AS humidity,
                   min(temperature_c)::float AS min_c,
                   max(temperature_c)::float AS max_c,
                   count(*)::int AS samples
            FROM temperature_readings
            {where}
            GROUP BY bucket_start
            ORDER BY bucket_start
        """
    else:
        sql = f"""
            SELECT recorded_at, temperature_c, room_temperature_c, humidity
            FROM temperature_readings
            {where}
            ORDER BY recorded_at
        """
    async with get_pool().acquire() as conn:
        return await conn.fetch(sql, *args)


async def fetch_stats(range_key: str) -> dict:
    window = RANGE_WINDOWS.get(range_key)
    where = "WHERE recorded_at >= now() - $1::interval" if window else ""
    args = [window] if window else []
    sql = f"""
        SELECT
            (SELECT temperature_c FROM temperature_readings
                ORDER BY recorded_at DESC LIMIT 1) AS current_c,
            (SELECT room_temperature_c FROM temperature_readings
                ORDER BY recorded_at DESC LIMIT 1) AS current_room_c,
            (SELECT humidity FROM temperature_readings
                ORDER BY recorded_at DESC LIMIT 1) AS current_humidity,
            (SELECT recorded_at FROM temperature_readings
                ORDER BY recorded_at DESC LIMIT 1) AS current_at,
            min(temperature_c)::float AS min_c,
            (SELECT recorded_at FROM temperature_readings
                {('WHERE recorded_at >= now() - $1::interval' if window else '')}
                ORDER BY temperature_c ASC LIMIT 1) AS min_at,
            max(temperature_c)::float AS max_c,
            (SELECT recorded_at FROM temperature_readings
                {('WHERE recorded_at >= now() - $1::interval' if window else '')}
                ORDER BY temperature_c DESC LIMIT 1) AS max_at,
            avg(temperature_c)::float AS avg_c,
            count(*)::int AS samples
        FROM temperature_readings
        {where}
    """
    async with get_pool().acquire() as conn:
        row = await conn.fetchrow(sql, *args)
        return dict(row) if row else {}


async def fetch_export(range_key: str) -> list[asyncpg.Record]:
    window = RANGE_WINDOWS.get(range_key)
    where = "WHERE recorded_at >= now() - $1::interval" if window else ""
    args = [window] if window else []
    sql = f"""
        SELECT recorded_at, temperature_c, room_temperature_c, humidity
        FROM temperature_readings
        {where}
        ORDER BY recorded_at
    """
    async with get_pool().acquire() as conn:
        return await conn.fetch(sql, *args)


async def clear_readings() -> int:
    """Delete all readings; returns the number of rows removed."""
    async with get_pool().acquire() as conn:
        status = await conn.execute("DELETE FROM temperature_readings")
        parts = status.split()  # e.g. "DELETE 42"
        return int(parts[-1]) if len(parts) == 2 and parts[-1].isdigit() else 0


@asynccontextmanager
async def lifespan(_app):
    await init_db()
    try:
        yield
    finally:
        await close_db()