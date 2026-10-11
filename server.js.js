
'use strict';

const express = require('express');
const path = require('path');

const app = express();
const PORT = Number(process.env.PORT || 3000);

const clients = new Set();
const devices = new Map();
let packetCounter = 0;

app.use(express.json({ limit: '32kb' }));

app.use((err, req, res, next) => {
  if (err) {
    return res.status(400).json({
      ok: false,
      error: 'Invalid JSON body'
    });
  }
  next();
});

app.use(express.static(path.join(__dirname, 'public')));

function summarizeDevice(id, entry) {
  const ageMs = Date.now() - entry.receivedAt;

  return {
    device_id: id,
    connected: ageMs < 10000,
    ageMs,
    data: entry.data,
    hasMpu: entry.hasMpu,
    hasEnvironment: entry.hasEnvironment
  };
}

app.get('/api/health', (req, res) => {
  const list = [...devices.entries()];
  const latest = list.sort(
    (a, b) => b[1].receivedAt - a[1].receivedAt
  )[0];

  const ageMs = latest
    ? Date.now() - latest[1].receivedAt
    : null;

  res.json({
    ok: true,
    hardwareConnected: list.some(
      ([, entry]) => Date.now() - entry.receivedAt < 10000
    ),
    ageMs,
    packetsReceived: packetCounter,
    deviceCount: devices.size
  });
});

app.get('/api/devices', (req, res) => {
  res.json({
    ok: true,
    devices: [...devices.entries()].map(
      ([id, entry]) => summarizeDevice(id, entry)
    )
  });
});

// Return C's latest MPU6050 packet to preserve compatibility.
app.get('/api/latest', (req, res) => {
  const result = [...devices.entries()]
    .filter(([, entry]) => entry.hasMpu)
    .sort((a, b) => b[1].receivedAt - a[1].receivedAt)[0];

  if (!result) {
    return res.json({
      connected: false,
      ageMs: null,
      data: null
    });
  }

  const ageMs = Date.now() - result[1].receivedAt;

  res.json({
    connected: ageMs < 10000,
    ageMs,
    data: result[1].data
  });
});

app.get('/api/latest/:deviceId', (req, res) => {
  const entry = devices.get(req.params.deviceId);

  if (!entry) {
    return res.status(404).json({
      ok: false,
      error: 'No data received for this device yet',
      device_id: req.params.deviceId
    });
  }

  res.json(summarizeDevice(req.params.deviceId, entry));
});

// Only MPU6050 packets are sent to the existing motion stream.
app.get('/api/stream', (req, res) => {
  res.setHeader('Content-Type', 'text/event-stream');
  res.setHeader('Cache-Control', 'no-cache, no-transform');
  res.setHeader('Connection', 'keep-alive');
  res.flushHeaders?.();

  res.write(': SafeGuard AI stream connected\n\n');
  clients.add(res);

  const latestMpu = [...devices.values()]
    .filter(entry => entry.hasMpu)
    .sort((a, b) => b.receivedAt - a.receivedAt)[0];

  if (latestMpu) {
    res.write(`data: ${JSON.stringify(latestMpu.data)}\n\n`);
  }

  const keepAlive = setInterval(() => {
    try {
      res.write(': keepalive\n\n');
    } catch (_) {}
  }, 20000);

  req.on('close', () => {
    clearInterval(keepAlive);
    clients.delete(res);
  });
});

app.post('/api/telemetry', (req, res) => {
  const p = req.body;

  if (!p || typeof p !== 'object' || Array.isArray(p)) {
    return res.status(400).json({
      ok: false,
      error: 'Body must be a JSON object'
    });
  }

  const deviceId =
    typeof p.device_id === 'string' && p.device_id.trim()
      ? p.device_id.trim()
      : 'SG-ESP32-C';

  const mpuFields = [
    'accel_x_g',
    'accel_y_g',
    'accel_z_g',
    'gyro_x_dps',
    'gyro_y_dps',
    'gyro_z_dps',
    'accel_magnitude_g'
  ];

  const environmentFields = [
    'temperature_c',
    'humidity_pct',
    'mq135_adc'
  ];

  const hasMpu = mpuFields.every(field =>
    p[field] !== undefined &&
    p[field] !== null &&
    p[field] !== '' &&
    Number.isFinite(Number(p[field]))
  );

  const hasEnvironment = environmentFields.some(field =>
    p[field] !== undefined &&
    p[field] !== null &&
    p[field] !== '' &&
    Number.isFinite(Number(p[field]))
  );

  if (!hasMpu && !hasEnvironment) {
    return res.status(400).json({
      ok: false,
      error: 'Packet must contain valid MPU6050 or environmental sensor fields'
    });
  }

  for (const field of [...mpuFields, ...environmentFields]) {
    if (p[field] === undefined || p[field] === null || p[field] === '') {
      continue;
    }

    if (!Number.isFinite(Number(p[field]))) {
      return res.status(400).json({
        ok: false,
        error: `${field} must be numeric`,
        field
      });
    }

    p[field] = Number(p[field]);
  }

  if (typeof p.laser_detected !== 'boolean') {
    p.laser_detected = false;
  }

  if (typeof p.vibration_detected !== 'boolean') {
    p.vibration_detected = false;
  }

  const receivedAt = Date.now();
  packetCounter++;

  const packet = {
    ...p,
    device_id: deviceId,
    packet_id: Number.isFinite(Number(p.packet_id))
      ? Number(p.packet_id)
      : packetCounter,
    timestamp: p.timestamp || new Date().toISOString(),
    source: 'ESP32_HARDWARE',
    server_received_at: new Date(receivedAt).toISOString()
  };

  const previous = devices.get(deviceId);

  devices.set(deviceId, {
    data: packet,
    receivedAt,
    hasMpu: hasMpu || Boolean(previous && previous.hasMpu),
    hasEnvironment:
      hasEnvironment || Boolean(previous && previous.hasEnvironment)
  });

  // Never send N's environmental-only packets into C's motion stream.
  if (hasMpu) {
    const frame = `data: ${JSON.stringify(packet)}\n\n`;

    for (const client of clients) {
      try {
        client.write(frame);
      } catch (_) {
        clients.delete(client);
      }
    }
  }

  console.log(
    `Accepted packet from ${deviceId}; ` +
    `MPU=${hasMpu}; environment=${hasEnvironment}`
  );

  res.json({
    ok: true,
    device_id: deviceId,
    packet_id: packet.packet_id,
    packetsReceived: packetCounter,
    devices: devices.size
  });
});

app.use((req, res) => {
  res.status(404).json({
    ok: false,
    error: 'Route not found',
    path: req.path
  });
});

app.listen(PORT, '0.0.0.0', () => {
  console.log(`SafeGuard AI backend: http://localhost:${PORT}`);
  console.log(`Health: http://localhost:${PORT}/api/health`);
  console.log(`Devices: http://localhost:${PORT}/api/devices`);
  console.log(`Latest MPU: http://localhost:${PORT}/api/latest`);
  console.log(`N latest: http://localhost:${PORT}/api/latest/SG-ESP32-N`);
  console.log('Keep this server on a trusted local network.');
});
