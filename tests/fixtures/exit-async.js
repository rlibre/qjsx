// process.exit inside an async function: no catch, no rejection handler, no
// timer runs afterwards, and `espjs run` returns the code (6).
(async () => {
    await null;
    try {
        process.exit(6);
    } catch {
        console.log("catch should not run");
    }
})().catch(() => {
    console.log("rejection handler should not run");
});
setTimeout(() => {
    console.log("timer should not run");
}, 20);
