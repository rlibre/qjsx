// Framing of the device agent protocol, identical on every link (serial, TCP):
//
//   0x00 | COBS( MAGIC | VERSION | CRC-16 | message ) | 0x00
//
//   MAGIC    0xE5
//   VERSION  1
//   CRC-16   CRC-16/CCITT-FALSE of 'message', 2 bytes, big endian
//   message  CBOR (see espjs:protocol)
//
// COBS removes every 0x00 from the frame, so 0x00 delimits frames and a
// receiver resynchronizes on the next 0x00 after an error. On a serial link
// the agent's frames are mixed with console text (boot messages, logs before
// a session): bytes that do not decode as a valid frame are text.

export const MAGIC = 0xe5;
export const VERSION = 1;

// CRC-16/CCITT-FALSE: polynomial 0x1021, initial value 0xFFFF.
// Check value: crc16("123456789") = 0x29B1.
export function crc16(bytes) {
    let crc = 0xffff;
    for (const byte of bytes) {
        crc ^= byte << 8;
        for (let bit = 0; bit < 8; bit++) {
            crc = crc & 0x8000 ? ((crc << 1) ^ 0x1021) & 0xffff : (crc << 1) & 0xffff;
        }
    }
    return crc;
}

// Consistent Overhead Byte Stuffing.
export function cobsEncode(data) {
    const output = new Uint8Array(data.length + Math.ceil(data.length / 254) + 1);
    let codeIndex = 0;
    let write = 1;
    let code = 1;
    for (const byte of data) {
        if (byte === 0) {
            output[codeIndex] = code;
            codeIndex = write++;
            code = 1;
            continue;
        }
        output[write++] = byte;
        code++;
        if (code === 0xff) {
            output[codeIndex] = code;
            codeIndex = write++;
            code = 1;
        }
    }
    output[codeIndex] = code;
    return output.subarray(0, write);
}

// Returns null if 'data' is not valid COBS.
export function cobsDecode(data) {
    const output = new Uint8Array(data.length);
    let read = 0;
    let write = 0;
    while (read < data.length) {
        const code = data[read++];
        if (code === 0 || read + code - 1 > data.length) {
            return null;
        }
        for (let i = 1; i < code; i++) {
            output[write++] = data[read++];
        }
        if (code !== 0xff && read < data.length) {
            output[write++] = 0;
        }
    }
    return output.subarray(0, write);
}

// message (CBOR bytes) -> frame bytes, delimiters included.
export function encodeFrame(message) {
    const crc = crc16(message);
    const body = new Uint8Array(4 + message.length);
    body[0] = MAGIC;
    body[1] = VERSION;
    body[2] = crc >> 8;
    body[3] = crc & 0xff;
    body.set(message, 4);
    const encoded = cobsEncode(body);
    const frame = new Uint8Array(encoded.length + 2);
    frame.set(encoded, 1);   // frame[0] and the last byte stay 0x00
    return frame;
}

// Splits a byte stream into messages and text. Feed it the received bytes;
// it calls onMessage(cborBytes) for each valid frame and onText(bytes) for
// everything else.
export class FrameParser {
    constructor(onMessage, onText) {
        this.onMessage = onMessage;
        this.onText = onText;
        this.pending = [];
    }

    feed(bytes) {
        for (const byte of bytes) {
            if (byte !== 0) {
                this.pending.push(byte);
                continue;
            }
            this.flushSegment();
        }
    }

    // A segment between two 0x00: a frame if it decodes, text otherwise.
    flushSegment() {
        if (this.pending.length === 0) {
            return;
        }
        const segment = Uint8Array.from(this.pending);
        this.pending = [];
        const message = decodeFrameBody(segment);
        if (message !== null) {
            this.onMessage(message);
        } else {
            this.onText(segment);
        }
    }

    // Console text has no final 0x00: call this when the link is idle to
    // give the pending bytes as text if they end a line. (A frame is always
    // sent in one go, so it is never left pending at idle time.)
    flushText() {
        const endsLine = this.pending.length > 0 && this.pending[this.pending.length - 1] === 0x0a;
        if (endsLine) {
            const segment = Uint8Array.from(this.pending);
            this.pending = [];
            this.onText(segment);
        }
    }
}

// COBS segment -> message bytes, or null if it is not a valid frame.
export function decodeFrameBody(segment) {
    const body = cobsDecode(segment);
    if (body === null || body.length < 4 || body[0] !== MAGIC || body[1] !== VERSION) {
        return null;
    }
    const message = body.subarray(4);
    const crc = (body[2] << 8) | body[3];
    return crc16(message) === crc ? message : null;
}
