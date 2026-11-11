// 'import()' is refused by espjs build (line 3): tests/run-tests.sh.
const name = "./other.js";
const other = await import(name);
console.log(other);
