// Writes to stderr, then throws from a timer: on a device the exception is
// printed and the program goes on (here: the next timer still runs).
console.error("to stderr");
setTimeout(() => {
    throw new Error("boom");
}, 1);
setTimeout(() => {
    console.log("still running after the exception");
}, 20);
