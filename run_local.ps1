# Runs AquaPulse bound to all interfaces so LAN devices (Arduino) can reach it.
python -m uvicorn app.main:app --host 0.0.0.0 --port 8000