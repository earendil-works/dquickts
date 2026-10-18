// module that throws during its evaluation
import { increment } from "./a.mjs";
increment(1000);
throw new SyntaxError("bad module " + import.meta.url);
