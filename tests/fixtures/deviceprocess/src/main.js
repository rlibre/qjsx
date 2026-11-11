// What process looks like to a program: on a device (the desktop agent) argv
// is ["espjs", main module], env comes from espjs.json, cwd is the data
// directory of the board and fs resolves relative paths against it. Run on
// the PC (espjs run), argv and env are the real ones.
import * as fs from "fs";

console.log(`argv ${JSON.stringify(process.argv)}`);
console.log(`env ${JSON.stringify(process.env)}`);

// only on a device: the file goes into the data directory
if (process.cwd().endsWith("/data")) {
    fs.writeFileSync("note.txt", "written by the program");
    console.log(`read back: ${fs.readFileSync("note.txt", "utf8")}`);
    console.log(`found from cwd: ${fs.existsSync(process.cwd() + "/note.txt")}`);
}
