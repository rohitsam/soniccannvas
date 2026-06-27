const express = require('express');
const { WebSocketServer } = require('ws');
const path  = require('path');
const http  = require('http');
const dgram = require('dgram');

const app    = express();
const server = http.createServer(app);
const wss    = new WebSocketServer({ server });

app.use(express.static(path.join(__dirname, 'public')));

// /text endpoint — same API as the real ESP32
app.get('/text', (req, res) => {
    const msg = req.query.msg ?? '';
    wss.clients.forEach(c => { if (c.readyState === 1) c.send(JSON.stringify({ type: 'ticker', msg })); });
    res.send('ok');
});

// /viz endpoint — switch visualization
app.get('/viz', (req, res) => {
    const n = parseInt(req.query.n ?? '0', 10);
    wss.clients.forEach(c => { if (c.readyState === 1) c.send(JSON.stringify({ type: 'viz', n })); });
    res.send(String(n));
});

// /status — diagnostic: check if UDP packets are arriving
app.get('/status', (req, res) => {
    res.json({
        udpPacketsReceived: pktCount,
        msSinceLastPacket:  pktCount ? (Date.now() - lastPktMs) : null,
        wsClientsConnected: wss.clients.size,
        udpPort: UDP_PORT,
    });
});

// /send-test — inject a fake sine sweep so you can verify browser WS → viz works
// without needing ffmpeg at all.  GET http://localhost:3000/send-test
app.get('/send-test', (req, res) => {
    const t   = Date.now() / 1000;
    const spectrum = Array.from({length: BINS}, (_, b) =>
        Math.max(0, Math.sin(b * 0.18 + t * 1.1) * 0.45 +
                    Math.sin(b * 0.07 + t * 0.7) * 0.30 + 0.1));
    const osc = Array.from({length: OSC_N}, (_, i) =>
        Math.sin(i * 0.3 + t) * 0.7);
    const amplitude = Math.max(...spectrum);
    const payload = JSON.stringify({ type: 'audio', spectrum, osc, amplitude });
    let sent = 0;
    wss.clients.forEach(c => { if (c.readyState === 1) { c.send(payload); sent++; } });
    res.json({ sent, wsClients: wss.clients.size });
});

// ── UDP audio receiver (same port/protocol as firmware) ───────────────────────
const FFT_N    = 512;
const BINS     = 48;
const OSC_N    = 96;
const UDP_PORT = 4210;

let pktCount  = 0;
let lastPktMs = 0;

// Sliding window — newest samples at the end
const pcmBuf = new Float32Array(FFT_N);

function appendSamples(tmp) {
    const n = Math.min(tmp.length, FFT_N);
    pcmBuf.copyWithin(0, n);
    pcmBuf.set(tmp.subarray(0, n), FFT_N - n);
}

// Cooley-Tukey FFT (in-place)
function fft(re, im) {
    const N = re.length;
    for (let i = 1, j = 0; i < N; i++) {
        let bit = N >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            let t = re[i]; re[i] = re[j]; re[j] = t;
                t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (let len = 2; len <= N; len <<= 1) {
        const ang = -2 * Math.PI / len;
        const wRe = Math.cos(ang), wIm = Math.sin(ang);
        for (let i = 0; i < N; i += len) {
            let cRe = 1, cIm = 0;
            for (let j = 0; j < (len >> 1); j++) {
                const uRe = re[i+j], uIm = im[i+j];
                const vRe = re[i+j+(len>>1)]*cRe - im[i+j+(len>>1)]*cIm;
                const vIm = re[i+j+(len>>1)]*cIm + im[i+j+(len>>1)]*cRe;
                re[i+j]          = uRe + vRe;  im[i+j]          = uIm + vIm;
                re[i+j+(len>>1)] = uRe - vRe;  im[i+j+(len>>1)] = uIm - vIm;
                const nr = cRe*wRe - cIm*wIm;
                cIm = cRe*wIm + cIm*wRe; cRe = nr;
            }
        }
    }
}

function computeAudio() {
    const re = new Float32Array(FFT_N);
    const im = new Float32Array(FFT_N);
    for (let i = 0; i < FFT_N; i++)
        re[i] = pcmBuf[i] * 0.5 * (1 - Math.cos(2 * Math.PI * i / (FFT_N - 1)));
    fft(re, im);

    const spectrum = new Float32Array(BINS);
    for (let b = 0; b < BINS; b++) {
        const lo = 1 + Math.floor((FFT_N/2-1) * (b/BINS) * (b/BINS));
        const hi = Math.max(lo+1, 1 + Math.floor((FFT_N/2-1) * ((b+1)/BINS) * ((b+1)/BINS)));
        let mx = 0;
        for (let k = lo; k < Math.min(hi, FFT_N/2); k++) {
            const mag = Math.sqrt(re[k]*re[k] + im[k]*im[k]) / (FFT_N/2);
            if (mag > mx) mx = mag;
        }
        spectrum[b] = Math.max(0, (20 * Math.log10(mx + 1e-9) + 90) / 90);
    }

    const osc  = new Float32Array(OSC_N);
    const step = Math.floor(FFT_N / OSC_N);
    let pk = 0.01;
    for (let i = 0; i < OSC_N; i++) { const a = Math.abs(pcmBuf[i*step]); if (a > pk) pk = a; }
    for (let i = 0; i < OSC_N; i++) osc[i] = pcmBuf[i*step] / pk;

    let amp = 0;
    for (const v of spectrum) if (v > amp) amp = v;
    return { spectrum: Array.from(spectrum), osc: Array.from(osc), amplitude: amp };
}

let udpIdleTimer = null;

const udpSock = dgram.createSocket('udp4');

udpSock.on('message', (msg, rinfo) => {
    pktCount++;
    lastPktMs = Date.now();

    // Log first packet, then every 200th
    if (pktCount === 1)
        console.log(`UDP ✓ first packet  ${msg.length} B  from ${rinfo.address}:${rinfo.port}  clients=${wss.clients.size}`);
    else if (pktCount % 200 === 0)
        console.log(`UDP pkt #${pktCount}  clients=${wss.clients.size}`);

    if (wss.clients.size === 0) return; // nobody listening, skip FFT

    try {
        const nS16     = msg.length >> 1;
        const isStereo = (msg.length & 3) === 0 && msg.length > 882;
        const count    = isStereo ? nS16 >> 1 : nS16;

        const tmp = new Float32Array(count);
        for (let i = 0; i < count; i++) {
            if (isStereo) {
                const L = msg.readInt16LE(i * 4);
                const R = msg.readInt16LE(i * 4 + 2);
                tmp[i] = ((L + R) >> 1) / 32768.0;
            } else {
                tmp[i] = msg.readInt16LE(i * 2) / 32768.0;
            }
        }
        appendSamples(tmp);

        const audio   = computeAudio();
        const payload = JSON.stringify({ type: 'audio', ...audio });
        wss.clients.forEach(c => { if (c.readyState === 1) c.send(payload); });
    } catch (e) {
        console.error('UDP handler error:', e.message);
    }

    if (udpIdleTimer) clearTimeout(udpIdleTimer);
    udpIdleTimer = setTimeout(() => {
        console.log('UDP idle — no packets for 2 s');
        wss.clients.forEach(c => { if (c.readyState === 1) c.send(JSON.stringify({ type: 'udp_idle' })); });
    }, 2000);
});

udpSock.on('error', e => console.error('UDP socket error:', e.message));

udpSock.bind(UDP_PORT, () => {
    console.log(`UDP audio       →  port ${UDP_PORT}`);
    console.log(`SonicCanvas sim →  http://localhost:${PORT}`);
    console.log(`Diagnostics     →  http://localhost:${PORT}/status`);
    console.log(`Test browser WS →  http://localhost:${PORT}/send-test`);
});

const PORT = 3000;
server.listen(PORT);
