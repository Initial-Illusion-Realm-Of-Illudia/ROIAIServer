// Headless protocol/movement smoke test for the airoi MMO server.
// Usage: node tools/smoke_client.mjs [wsUrl]     (default ws://127.0.0.1:8080/)
// Exits 0 on success, 1 on failure.

const C2S = { Hello: 1, Input: 2, Ping: 3 };
const S2C = { Welcome: 1, Spawn: 2, Despawn: 3, Snapshot: 4, Pong: 5 };

const URL = process.argv[2] ?? "ws://127.0.0.1:8080/";
let failures = 0;

function check(label, cond, detail = "") {
    console.log(`${cond ? "PASS" : "FAIL"}: ${label}${detail ? `  [${detail}]` : ""}`);
    if (!cond) failures++;
}

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
async function waitFor(fn, timeoutMs, label) {
    const deadline = performance.now() + timeoutMs;
    while (performance.now() < deadline) {
        if (fn()) return true;
        await sleep(20);
    }
    return false;
}

class Bot {
    constructor(name) {
        this.name = name;
        this.netId = null;
        this.spawn = null; // {x,y,z} from Welcome
        this.pos = new Map(); // netId -> {x,y,z,yaw} latest snapshot
        this.spawns = new Map(); // netId -> {name,x,y,z,yaw}
        this.despawns = [];
        this.maxY = 0;
        this.rtt = [];
        this.seq = 0;
        this.pongWaits = new Map(); // nonce -> resolve
        this.closed = false;
        this.inputTimer = null;
        this.pingTimer = null;
    }

    connect(url = URL) {
        return new Promise((resolve, reject) => {
            const ws = new WebSocket(url);
            ws.binaryType = "arraybuffer";
            this.ws = ws;
            const timeout = setTimeout(
                () => reject(new Error(`${this.name}: connect/welcome timeout`)), 4000);

            ws.onopen = () => {
                const nameBytes = new TextEncoder().encode(this.name).slice(0, 24);
                const buf = new Uint8Array(4 + nameBytes.length);
                const dv = new DataView(buf.buffer);
                buf[0] = C2S.Hello;
                dv.setUint16(1, 1, true); // protocol version
                buf[3] = nameBytes.length;
                buf.set(nameBytes, 4);
                ws.send(buf);
            };
            ws.onmessage = (ev) => this.onMessage(new DataView(ev.data));
            ws.onerror = () => {
                clearTimeout(timeout);
                reject(new Error(`${this.name}: websocket error`));
            };
            ws.onclose = () => { this.closed = true; };
            this.onWelcome = () => { clearTimeout(timeout); resolve(this); };
        });
    }

    onMessage(dv) {
        const id = dv.getUint8(0);
        if (id === S2C.Welcome) {
            this.netId = dv.getUint32(1, true);
            this.spawn = { x: dv.getFloat32(5, true), y: dv.getFloat32(9, true), z: dv.getFloat32(13, true) };
            const rosterCount = dv.getUint16(17, true);
            let off = 19;
            for (let i = 0; i < rosterCount; i++) off = this.readSpawn(dv, off, this.spawns);
            this.onWelcome?.();
        } else if (id === S2C.Spawn) {
            let off = this.readSpawn(dv, 1, this.spawns, /*withId*/ false);
            void off;
        } else if (id === S2C.Despawn) {
            this.despawns.push(dv.getUint32(1, true));
        } else if (id === S2C.Snapshot) {
            const count = dv.getUint16(5, true);
            let off = 7;
            for (let i = 0; i < count; i++) {
                const netId = dv.getUint32(off, true);
                const p = {
                    x: dv.getFloat32(off + 4, true),
                    y: dv.getFloat32(off + 8, true),
                    z: dv.getFloat32(off + 12, true),
                    yaw: dv.getFloat32(off + 16, true),
                };
                off += 20;
                this.pos.set(netId, p);
                if (netId === this.netId) this.maxY = Math.max(this.maxY, p.y);
            }
        } else if (id === S2C.Pong) {
            const nonce = dv.getUint32(1, true);
            const sentAt = this.sentAt?.get(nonce);
            if (sentAt !== undefined) {
                this.rtt.push(performance.now() - sentAt);
                this.pongWaits.get(nonce)?.();
            }
        }
    }

    // Reads {netId u32, name str8, x,y,z,yaw f32} at off; returns new offset.
    readSpawn(dv, off, into) {
        const netId = dv.getUint32(off, true);
        const nameLen = dv.getUint8(off + 4);
        const name = new TextDecoder().decode(new Uint8Array(dv.buffer, dv.byteOffset + off + 5, nameLen));
        into.set(netId, {
            name,
            x: dv.getFloat32(off + 5 + nameLen, true),
            y: dv.getFloat32(off + 9 + nameLen, true),
            z: dv.getFloat32(off + 13 + nameLen, true),
            yaw: dv.getFloat32(off + 17 + nameLen, true),
        });
        return off + 21 + nameLen;
    }

    sendInput({ moveF = 0, moveR = 0, jump = false, yaw = 0 } = {}) {
        this.seq = (this.seq + 1) & 0xFFFF;
        const buf = new Uint8Array(10);
        const dv = new DataView(buf.buffer);
        buf[0] = C2S.Input;
        dv.setUint16(1, this.seq, true);
        dv.setInt8(3, moveF);
        dv.setInt8(4, moveR);
        buf[5] = jump ? 1 : 0;
        dv.setFloat32(6, yaw, true);
        this.ws.send(buf);
    }

    // Sends the given input state every 33 ms until stopInput().
    startInput(opts) {
        this.stopInput();
        this.sendInput(opts);
        this.inputTimer = setInterval(() => this.sendInput(opts), 33);
    }

    stopInput() {
        if (this.inputTimer) clearInterval(this.inputTimer);
        this.inputTimer = null;
        this.sendInput({}); // release keys
    }

    pingOnce() {
        return new Promise((resolve) => {
            const nonce = (this.pingNonce = (this.pingNonce ?? 0) + 1);
            this.sentAt ??= new Map();
            this.sentAt.set(nonce, performance.now());
            this.pongWaits.set(nonce, resolve);
            const buf = new Uint8Array(5);
            new DataView(buf.buffer).setUint32(1, nonce, true);
            buf[0] = C2S.Ping;
            this.ws.send(buf);
            setTimeout(resolve, 2000); // don't hang if pongs are broken
        });
    }

    close() {
        this.ws?.close();
    }
}

// ---------------------------------------------------------------------------
const A = new Bot("Alice");

try {
    await A.connect();
    check("A receives Welcome", A.netId !== null);
    check("A spawns at y ≈ 1.0", A.spawn && Math.abs(A.spawn.y - 1.0) < 0.25,
        `y=${A.spawn?.y?.toFixed(2)}`);

    await A.pingOnce();
    check("Ping/Pong round-trip", A.rtt.length > 0 && A.rtt[0] < 500,
        A.rtt.length ? `${A.rtt[0].toFixed(1)} ms` : "no pong");

    // Malformed unknown message must not kill the connection.
    A.ws.send(new Uint8Array([0xEE, 1, 2, 3]));

    // --- walk forward: yaw 0 faces -Z, so z must decrease ---
    const z0 = A.spawn.z;
    const x0 = A.spawn.x;
    A.startInput({ moveF: 127, yaw: 0 });
    await sleep(1500);
    A.stopInput();
    await sleep(200);
    const pA = A.pos.get(A.netId);
    const dz = pA.z - z0;
    const dx = pA.x - x0;
    check("moves forward along -Z (dz ≤ -5 m)", dz <= -5, `dz=${dz.toFixed(2)}`);
    check("no lateral drift (|dx| < 1 m)", Math.abs(dx) < 1, `dx=${dx.toFixed(3)}`);

    // --- jump ---
    await sleep(400); // let it settle on the ground
    A.maxY = 0;
    A.startInput({ jump: true, yaw: 0 });
    await sleep(150);
    A.stopInput();
    let rose = await waitFor(() => A.maxY >= 0.5, 2000);
    check("jump reaches y ≥ 0.5", rose, `maxY=${A.maxY.toFixed(2)}`);
    let landed = await waitFor(() => Math.abs((A.pos.get(A.netId)?.y ?? -1)) < 0.01, 3000);
    check("lands back at y = 0", landed);

    // --- second client: spawn propagation + roster in Welcome ---
    const B = new Bot("Bob");
    await B.connect();
    check("B receives Welcome", B.netId !== null && B.netId !== A.netId,
        `netIds ${A.netId}/${B.netId}`);

    let aSeesB = await waitFor(() => A.spawns.has(B.netId), 2000);
    check("A receives Spawn of B", aSeesB);
    check("Spawn carries B's name", A.spawns.get(B.netId)?.name === "Bob",
        A.spawns.get(B.netId)?.name);
    check("B's Welcome roster contains A", B.spawns.has(A.netId) &&
        B.spawns.get(A.netId).name === "Alice");

    await waitFor(() => A.pos.has(B.netId) && B.pos.has(A.netId), 1500);
    check("snapshots contain both players", A.pos.has(B.netId) && B.pos.has(A.netId));

    // --- despawn on disconnect ---
    B.close();
    let despawned = await waitFor(() => A.despawns.includes(B.netId), 3000);
    check("A receives Despawn of B after disconnect", despawned);

    A.close();
    await sleep(200);
} catch (err) {
    check("no fatal error", false, String(err));
}

console.log(failures === 0 ? "\nALL CHECKS PASSED" : `\n${failures} CHECK(S) FAILED`);
process.exitCode = failures === 0 ? 0 : 1;
