// Program debugged by tests/debugger.test.js. The test finds its stops by
// the text of the statements below: change both together.
import { area } from "./shapes.js";

let keepSpinning = true;

function compute(width, height) {
    const box = { width, height, label: "box" };
    const result = area(box);
    return result;
}

// Runs until the debugger sets 'keepSpinning' to false.
function spin() {
    let turns = 0;
    while (keepSpinning) {
        turns++;
    }
    return turns;
}

function failSoftly() {
    try {
        throw new Error("expected failure");
    } catch (error) {
        return error.message;
    }
}

console.log("area", compute(3, 4));
console.log("spun", spin() > 0);
console.log(failSoftly());
debugger;
console.log("done");
