// Tests of the "net" and "serial" modules (serial: listing only, no hardware).
// Run with: bin/Release/qjsx run tests/net.test.js
import { connect, listen } from "net";
import { list } from "serial";
import { check, equal, rejects, report } from "./assert.js";

const decode = bytes => new TextDecoder().decode(bytes);

// --- serial: listing works (the result depends on the machine)
const ports = list();
check(Array.isArray(ports) && ports.every(port => typeof port === "string"), "serial list");

// --- echo server on a free port
const received = [];
const server = listen({ port: 0 }, connection => {
    connection.onData(bytes => connection.write(bytes));
    connection.onClose(() => received.push("server saw close"));
});
check(server.port() > 0, "server port");

// --- client
const client = await connect({ port: server.port() });
equal(client.remoteAddress(), "127.0.0.1", "remote address");
equal(client.remotePort(), server.port(), "remote port");
let echoed = "";
const allEchoed = new Promise(resolve => {
    client.onData(bytes => {
        echoed += decode(bytes);
        if (echoed.length >= 11) {
            resolve();
        }
    });
});
const clientClosed = new Promise(resolve => client.onClose(resolve));
client.write("hello ");
client.write(new TextEncoder().encode("world"));
await allEchoed;
equal(echoed, "hello world", "echo");

// --- large transfer
let total = 0;
const big = new Uint8Array(200000).fill(7);
const allBig = new Promise(resolve => {
    client.onData(bytes => {
        total += bytes.length;
        if (total >= big.length) {
            resolve();
        }
    });
});
client.write(big);
await allBig;
equal(total, big.length, "large transfer");

client.close();
await clientClosed;
check(!client.isOpen(), "client closed");
await new Promise(resolve => setTimeout(resolve, 20));
equal(received.join(","), "server saw close", "server side close");

// --- errors
await rejects(connect({ port: 1 }), /ECONNREFUSED/, "connection refused");
let thrown = "";
try {
    client.write("x");
} catch (error) {
    thrown = error.message;
}
check(/closed/.test(thrown), "write after close");

server.close();
report("net");
