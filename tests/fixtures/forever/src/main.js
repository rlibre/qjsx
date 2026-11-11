// Never ends by itself: must be stopped by the agent (device stop).
let ticks = 0;
setInterval(() => {
    ticks++;
}, 10);
console.log("running forever");
