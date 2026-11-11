// Module "testlib/greet" of the test library (tests/run-tests.sh): it imports
// a neighbour relatively and a module of the standard library, which the
// build of a program must both follow.
import { hello } from "./words";
import { join } from "stdlib/path";

export function greet(name) {
    return `${hello}, ${name}! (${join("lib", "greet")})`;
}
