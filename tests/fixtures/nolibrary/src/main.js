// Imports a module of a library that the project does not declare: espjs
// build must refuse it (tests/run-tests.sh).
import { thing } from "nolib/thing";
console.log(thing);
