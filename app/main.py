import csv
import io
from datetime import datetime, timezone
from typing import Any

from fastapi import FastAPI, HTTPException, Query
from fastapi.responses import FileResponse, JSONResponse, StreamingResponse
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel, Field

from .db import (
    clear_readings,
    fetch_export,
    fetch_series,
    fetch_stats,
    insert_reading,
    lifespan,
)

STATIC_DIR = __file__.rsplit("app", 1)[0] + "static"

app = FastAPI(title="AquaPulse", version="1.0.0", lifespan=lifespan)


class ReadingIn(BaseModel):
    temperature_c: float = Field(..., description="Tank temperature in Celsius")
    room_temperature_c: float | None = Field(
        None, description="Room temperature in Celsius (DHT11)"
    )
    humidity: float | None = Field(
        None, ge=0, le=100, description="Relative humidity in percent (DHT11)"
    )


@app.post("/api/readings", status_code=201)
async def post_reading(reading: ReadingIn) -> dict:
    if not -50.0 < reading.temperature_c < 150.0:
        raise HTTPException(422, "temperature_c out of plausible range (-50..150)")
    if reading.room_temperature_c is not None and not -50.0 < reading.room_temperature_c < 150.0:
        raise HTTPException(422, "room_temperature_c out of plausible range (-50..150)")
    row = await insert_reading(
        reading.temperature_c, reading.room_temperature_c, reading.humidity
    )
    return {
        "id": row["id"],
        "temperature_c": row["temperature_c"],
        "room_temperature_c": row["room_temperature_c"],
        "humidity": row["humidity"],
        "recorded_at": row["recorded_at"].isoformat(),
    }


@app.get("/api/readings")
async def get_readings(
    range: str = Query("24h", pattern="^(hour|24h|7d|30d|all)$"),
) -> dict[str, Any]:
    bucket = {"7d": "hour", "30d": "hour", "all": "day"}.get(range)
    rows = await fetch_series(range, bucket)
    points = []
    for r in rows:
        ts = r.get("bucket_start") or r["recorded_at"]
        point = {"t": ts.isoformat(), "c": round(r["temperature_c"], 2)}
        if r["room_temperature_c"] is not None:
            point["r"] = round(r["room_temperature_c"], 2)
        if r["humidity"] is not None:
            point["h"] = round(r["humidity"], 1)
        if bucket:
            point["min"] = round(r["min_c"], 2)
            point["max"] = round(r["max_c"], 2)
        points.append(point)
    return {"range": range, "points": points}


@app.get("/api/stats")
async def get_stats(
    range: str = Query("24h", pattern="^(hour|24h|7d|30d|all)$"),
) -> dict[str, Any]:
    stats = await fetch_stats(range)

    def fmt_ts(value: datetime | None) -> str | None:
        if value is None:
            return None
        if value.tzinfo is None:
            value = value.replace(tzinfo=timezone.utc)
        return value.isoformat()

    return {
        "range": range,
        "current_c": stats.get("current_c"),
        "current_room_c": stats.get("current_room_c"),
        "current_humidity": stats.get("current_humidity"),
        "current_at": fmt_ts(stats.get("current_at")),
        "min_c": stats.get("min_c"),
        "min_at": fmt_ts(stats.get("min_at")),
        "max_c": stats.get("max_c"),
        "max_at": fmt_ts(stats.get("max_at")),
        "avg_c": round(stats["avg_c"], 2) if stats.get("avg_c") is not None else None,
        "samples": stats.get("samples", 0),
    }


@app.get("/api/readings.csv")
async def get_csv(
    range: str = Query("all", pattern="^(hour|24h|7d|30d|all)$"),
) -> StreamingResponse:
    rows = await fetch_export(range)
    buf = io.StringIO()
    writer = csv.writer(buf)
    writer.writerow(["recorded_at", "temperature_c", "room_temperature_c", "humidity"])
    for r in rows:
        writer.writerow([
            r["recorded_at"].isoformat(),
            r["temperature_c"],
            r["room_temperature_c"],
            r["humidity"],
        ])
    buf.seek(0)
    return StreamingResponse(
        iter([buf.getvalue()]),
        media_type="text/csv",
        headers={"Content-Disposition": 'attachment; filename="aquapulse_readings.csv"'},
    )


@app.delete("/api/readings")
async def delete_readings() -> dict:
    deleted = await clear_readings()
    return {"deleted": deleted}


@app.get("/api/health")
async def health() -> dict:
    return {"status": "ok"}


@app.get("/")
async def index() -> FileResponse:
    return FileResponse(STATIC_DIR + "/index.html")


app.mount("/static", StaticFiles(directory=STATIC_DIR), name="static")


@app.exception_handler(500)
async def internal_error(_req, _exc) -> JSONResponse:
    return JSONResponse({"detail": "internal server error"}, status_code=500)