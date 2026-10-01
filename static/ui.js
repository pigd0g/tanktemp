/* AquaPulse UI */
"use strict";

const $ = (id) => document.getElementById(id);

let chart = null;
let humidityChart = null;
let currentRange = "24h";
let refreshTimer = null;

const REFRESH_MS = 30000;

function fmtTemp(v) {
  return v === null || v === undefined ? "--" : Number(v).toFixed(1);
}

function fmtTime(iso) {
  if (!iso) return "";
  const d = new Date(iso);
  return d.toLocaleString(undefined, {
    month: "short",
    day: "numeric",
    hour: "2-digit",
    minute: "2-digit",
  });
}

function fmtTick(iso, range) {
  const d = new Date(iso);
  if (range === "hour" || range === "24h") {
    return d.toLocaleTimeString(undefined, { hour: "2-digit", minute: "2-digit" });
  }
  if (range === "7d" || range === "30d") {
    return d.toLocaleString(undefined, { month: "short", day: "numeric", hour: "2-digit" });
  }
  return d.toLocaleDateString(undefined, { month: "short", day: "numeric", year: "2-digit" });
}

function setLive(ok) {
  const dot = $("liveDot");
  if (ok) {
    dot.style.opacity = "1";
    dot.title = "Auto-refreshing every 30s";
  } else {
    dot.style.opacity = "0.45";
    dot.title = "Connection problem — retrying";
  }
}

async function fetchJson(url) {
  const res = await fetch(url, { cache: "no-store" });
  if (!res.ok) throw new Error(`${res.status}`);
  return res.json();
}

function renderStats(stats) {
  $("currentTemp").textContent = fmtTemp(stats.current_c);
  $("currentAt").textContent = stats.current_at
    ? `Updated ${fmtTime(stats.current_at)}`
    : "No readings yet";

  const roomLine = $("roomLine");
  if (stats.current_room_c !== null && stats.current_room_c !== undefined) {
    roomLine.hidden = false;
    $("roomTempInline").textContent = `Room ${fmtTemp(stats.current_room_c)} °C`;
    $("humidityInline").textContent =
      stats.current_humidity !== null && stats.current_humidity !== undefined
        ? `Humidity ${fmtTemp(stats.current_humidity)} %`
        : "Humidity -- %";
  } else {
    roomLine.hidden = true;
  }

  $("statMin").textContent = fmtTemp(stats.min_c);
  $("statAvg").textContent = fmtTemp(stats.avg_c);
  $("statMax").textContent = fmtTemp(stats.max_c);
  $("statMinAt").textContent = stats.min_at ? fmtTime(stats.min_at) : "";
  $("statMaxAt").textContent = stats.max_at ? fmtTime(stats.max_at) : "";

  $("sampleCount").textContent =
    stats.samples === 1 ? "1 sample" : `${stats.samples} samples`;
}

function renderChart(points) {
  const labels = points.map((p) => p.t);
  const data = points.map((p) => p.c);
  const roomData = points.map((p) => (p.r !== undefined ? p.r : null));
  const showBand = points.length > 0 && "min" in points[0];
  const showRoom = points.some((p) => p.r !== undefined && p.r !== null);

  const ctx = $("tempChart");

  if (!chart) {
    chart = new Chart(ctx, {
      type: "line",
      data: {
        labels,
        datasets: [
          lineDataset(data),
          bandDataset(points, showBand, "min"),
          bandDataset(points, showBand, "max"),
          roomDataset(roomData, showRoom),
        ],
      },
      options: chartOptions(),
    });
  } else {
    chart.data.labels = labels;
    chart.data.datasets[0].data = data;
    chart.data.datasets[1].data = points.map((p) => (showBand ? p.min : null));
    chart.data.datasets[2].data = points.map((p) => (showBand ? p.max : null));
    chart.data.datasets[3].data = roomData;
    chart.data.datasets[1].hidden = !showBand;
    chart.data.datasets[2].hidden = !showBand;
    chart.data.datasets[3].hidden = !showRoom;
    chart.update();
  }
}

function lineDataset(data) {
  return {
    label: "Temp °C",
    data,
    borderColor: "#38bdf8",
    borderWidth: 2.5,
    tension: 0.35,
    pointRadius: 0,
    pointHoverRadius: 5,
    pointHoverBackgroundColor: "#38bdf8",
    fill: true,
    gradient: null,
    backgroundColor: (context) => {
      const { ctx, chartArea } = context.chart;
      if (!chartArea) return "rgba(56,189,248,0.12)";
      const g = ctx.createLinearGradient(0, chartArea.top, 0, chartArea.bottom);
      g.addColorStop(0, "rgba(56,189,248,0.30)");
      g.addColorStop(1, "rgba(56,189,248,0.02)");
      return g;
    },
  };
}

function bandDataset(points, show, key) {
  const colors = { min: "rgba(56,189,248,0.35)", max: "rgba(251,113,133,0.5)" };
  return {
    label: key,
    data: points.map((p) => (show ? p[key] : null)),
    borderColor: colors[key],
    borderWidth: 1,
    borderDash: [4, 4],
    pointRadius: 0,
    pointHitRadius: 0,
    fill: false,
    hidden: !show,
  };
}

function roomDataset(data, show) {
  return {
    label: "Room °C",
    data,
    borderColor: "#fbbf24",
    borderWidth: 1.5,
    tension: 0.35,
    pointRadius: 0,
    pointHoverRadius: 4,
    pointHoverBackgroundColor: "#fbbf24",
    fill: false,
    hidden: !show,
  };
}

function renderHumidityChart(points) {
  const labels = points.map((p) => p.t);
  const data = points.map((p) => (p.h !== undefined ? p.h : null));
  const show = points.some((p) => p.h !== undefined && p.h !== null);

  const ctx = $("humidityChart");

  if (!humidityChart) {
    humidityChart = new Chart(ctx, {
      type: "line",
      data: {
        labels,
        datasets: [
          {
            label: "Humidity %",
            data,
            borderColor: "#2dd4bf",
            borderWidth: 2,
            tension: 0.35,
            pointRadius: 0,
            pointHoverRadius: 5,
            pointHoverBackgroundColor: "#2dd4bf",
            fill: true,
            backgroundColor: (context) => {
              const { ctx: c, chartArea } = context.chart;
              if (!chartArea) return "rgba(45,212,191,0.12)";
              const g = c.createLinearGradient(0, chartArea.top, 0, chartArea.bottom);
              g.addColorStop(0, "rgba(45,212,191,0.28)");
              g.addColorStop(1, "rgba(45,212,191,0.02)");
              return g;
            },
          },
        ],
      },
      options: humidityOptions(),
    });
  } else {
    humidityChart.data.labels = labels;
    humidityChart.data.datasets[0].data = data;
    humidityChart.data.datasets[0].hidden = !show;
    humidityChart.update();
  }
}

function chartOptions() {
  return {
    responsive: true,
    maintainAspectRatio: false,
    animation: { duration: 350 },
    interaction: { mode: "index", intersect: false },
    plugins: {
      legend: { display: false },
      tooltip: {
        backgroundColor: "#0e1f38",
        borderColor: "rgba(94,178,255,0.3)",
        borderWidth: 1,
        titleColor: "#8fa9c9",
        bodyColor: "#e8f1ff",
        padding: 10,
        displayColors: false,
        callbacks: {
          title: (items) => fmtTime(items[0].label),
          label: (item) => {
            const lines =
              item.dataset.label === "Room °C"
                ? [`Room: ${Number(item.parsed.y).toFixed(2)} °C`]
                : [`Temp: ${Number(item.parsed.y).toFixed(2)} °C`];
            const dss = item.chart.data.datasets;
            const rawMin = dss[1].hidden ? null : dss[1].data[item.dataIndex];
            const rawMax = dss[2].hidden ? null : dss[2].data[item.dataIndex];
            const rawRoom = dss[3].hidden ? null : dss[3].data[item.dataIndex];
            if (rawMin !== null && rawMin !== undefined) lines.push(`Min: ${Number(rawMin).toFixed(2)}`);
            if (rawMax !== null && rawMax !== undefined) lines.push(`Max: ${Number(rawMax).toFixed(2)}`);
            if (rawRoom !== null && rawRoom !== undefined) lines.push(`Room: ${Number(rawRoom).toFixed(2)}`);
            return lines;
          },
        },
      },
    },
    scales: {
      x: {
        grid: { display: false },
        border: { display: false },
        ticks: {
          color: "#8fa9c9",
          maxTicksLimit: 6,
          maxRotation: 0,
          autoSkip: true,
          callback(value, index) {
            const label = this.getLabelForValue(value);
            return fmtTick(label, currentRange);
          },
        },
      },
      y: {
        grid: { color: "rgba(143,169,201,0.10)" },
        border: { display: false },
        ticks: {
          color: "#8fa9c9",
          maxTicksLimit: 6,
          callback: (v) => `${Number(v).toFixed(1)}°`,
        },
      },
    },
  };
}

function humidityOptions() {
  return {
    responsive: true,
    maintainAspectRatio: false,
    animation: { duration: 350 },
    interaction: { mode: "index", intersect: false },
    plugins: {
      legend: { display: false },
      tooltip: {
        backgroundColor: "#0e1f38",
        borderColor: "rgba(45,212,191,0.3)",
        borderWidth: 1,
        titleColor: "#8fa9c9",
        bodyColor: "#e8f1ff",
        padding: 10,
        displayColors: false,
        callbacks: {
          title: (items) => fmtTime(items[0].label),
          label: (item) => `Humidity: ${Number(item.parsed.y).toFixed(1)} %`,
        },
      },
    },
    scales: {
      x: {
        grid: { display: false },
        border: { display: false },
        ticks: {
          color: "#8fa9c9",
          maxTicksLimit: 6,
          maxRotation: 0,
          autoSkip: true,
          callback(value, index) {
            const label = this.getLabelForValue(value);
            return fmtTick(label, currentRange);
          },
        },
      },
      y: {
        min: 0,
        max: 100,
        grid: { color: "rgba(143,169,201,0.10)" },
        border: { display: false },
        ticks: {
          color: "#8fa9c9",
          maxTicksLimit: 6,
          callback: (v) => `${Number(v).toFixed(0)}%`,
        },
      },
    },
  };
}

async function load() {
  try {
    const [stats, series] = await Promise.all([
      fetchJson(`/api/stats?range=${currentRange}`),
      fetchJson(`/api/readings?range=${currentRange}`),
    ]);
    renderStats(stats);
    renderChart(series.points);
    renderHumidityChart(series.points);
    setLive(true);
  } catch (err) {
    console.error("load failed", err);
    setLive(false);
  }
}

function setRange(range) {
  currentRange = range;
  document.querySelectorAll("#rangeTabs button").forEach((btn) => {
    const active = btn.dataset.range === range;
    btn.classList.toggle("active", active);
    btn.setAttribute("aria-pressed", String(active));
  });
  $("csvLink").href = `/api/readings.csv?range=${range}`;
  load();
}

function setupClearModal() {
  const modal = $("clearModal");
  const open = () => {
    modal.hidden = false;
    $("clearCancel").focus();
  };
  const close = () => {
    modal.hidden = true;
  };

  $("clearBtn").addEventListener("click", open);
  $("clearCancel").addEventListener("click", close);
  modal.addEventListener("click", (e) => {
    if (e.target === modal) close();
  });
  document.addEventListener("keydown", (e) => {
    if (e.key === "Escape" && !modal.hidden) close();
  });

  $("clearConfirm").addEventListener("click", async () => {
    $("clearConfirm").disabled = true;
    $("clearConfirm").textContent = "Deleting…";
    try {
      const res = await fetch("/api/readings", { method: "DELETE" });
      if (!res.ok) throw new Error(`${res.status}`);
      close();
      await load();
    } catch (err) {
      console.error("clear failed", err);
      $("clearConfirm").textContent = "Error — try again";
    } finally {
      $("clearConfirm").disabled = false;
      setTimeout(() => {
        $("clearConfirm").textContent = "Delete all";
      }, 2000);
    }
  });
}

function init() {
  document.querySelectorAll("#rangeTabs button").forEach((btn) => {
    btn.addEventListener("click", () => setRange(btn.dataset.range));
  });
  setupClearModal();
  load();
  refreshTimer = setInterval(load, REFRESH_MS);
  document.addEventListener("visibilitychange", () => {
    if (!document.hidden) load();
  });
}

init();