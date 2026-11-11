// process.exit ends the program, never the agent: nothing runs after it (not
// even a finally block or a pending timer), and the agent reports the code.
setTimeout(() => {
    console.log("timer should not run");
}, 20);
console.log("before exit");
try {
    process.exit(3);
} finally {
    console.log("finally should not run");
}
console.log("after exit should not run");
