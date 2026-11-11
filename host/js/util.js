// Small helpers shared by the tool modules.

export function hex(bytes) {
    let text = "";
    for (const byte of bytes) {
        text += byte.toString(16).padStart(2, "0");
    }
    return text;
}

const encoder = new TextEncoder();

export function encodeText(text) {
    return encoder.encode(text);
}
