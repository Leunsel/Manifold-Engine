---@meta

---Writes the values to the Lua output, separated by tabs.
---@param ... any
function print(...) end

---Iterates over all keys and values of a table, in no particular order.
---@param table table
---@return function, table, nil
function pairs(table) end

---Iterates over the array part of a table, from 1 up to the first nil.
---@param table table
---@return function, table, integer
function ipairs(table) end

---Returns the next key and value of a table, or nil after the last one.
---@param table table
---@param key? any
---@return any, any
function next(table, key) end

---Returns the type name of the value.
---@param value any
---@return "nil"|"number"|"string"|"boolean"|"table"|"function"|"thread"|"userdata"
function type(value) end

---Converts the value to a string, through `__tostring` when it has one.
---@param value any
---@return string
function tostring(value) end

---Converts the value to a number, or nil when it is not one.
---@param value any
---@param base? integer # 2 to 36; the value must then be a string
---@return number|nil
function tonumber(value, base) end

---Returns the arguments after index, or their count for "#".
---@param index integer|"#"
---@param ... any
---@return any
function select(index, ...) end

---Calls the function in protected mode and returns whether it succeeded, then its results or the error.
---@param f function
---@param ... any
---@return boolean, any
function pcall(f, ...) end

---Like pcall, with a message handler that gets the error first.
---@param f function
---@param handler function
---@param ... any
---@return boolean, any
function xpcall(f, handler, ...) end

---Raises an error.
---@param message any
---@param level? integer # 1 (the default) blames the caller, 2 its caller, 0 adds no position
function error(message, level) end

---Raises an error when the value is false or nil, and returns all arguments otherwise.
---@param value any
---@param message? any
---@return any
function assert(value, message) end

---Sets the metatable of a table and returns the table.
---@param table table
---@param metatable table|nil
---@return table
function setmetatable(table, metatable) end

---Returns the metatable of a value, or its `__metatable` field.
---@param value any
---@return table|nil
function getmetatable(value) end

---Reads a field without metamethods.
---@param table table
---@param key any
---@return any
function rawget(table, key) end

---Writes a field without metamethods.
---@param table table
---@param key any
---@param value any
---@return table
function rawset(table, key, value) end

---Compares two values without metamethods.
---@param a any
---@param b any
---@return boolean
function rawequal(a, b) end

---The length of a table or string without metamethods.
---@param value table|string
---@return integer
function rawlen(value) end

---Loads a module and returns what it returned.
---@param name string
---@return any
function require(name) end

---Compiles a chunk and returns it as a function, or nil and the error.
---@param chunk string|function
---@param name? string
---@param mode? "b"|"t"|"bt"
---@param env? table
---@return function|nil, string|nil
function load(chunk, name, mode, env) end

---Compiles a file and returns it as a function, or nil and the error.
---@param filename? string
---@param mode? "b"|"t"|"bt"
---@param env? table
---@return function|nil, string|nil
function loadfile(filename, mode, env) end

---Runs a file and returns its results.
---@param filename? string
---@return any
function dofile(filename) end

---Controls the garbage collector.
---@param option? "collect"|"stop"|"restart"|"count"|"step"|"isrunning"|"incremental"|"generational"
---@param ... any
---@return any
function collectgarbage(option, ...) end

---Formats the values like C's printf: %d %i %u %c %x %X %o %e %E %f %g %G %q %s %a %A %%.
---@param format string
---@param ... any
---@return string
function string.format(format, ...) end

---Returns the part from i to j; negative positions count from the end.
---@param s string
---@param i integer
---@param j? integer
---@return string
function string.sub(s, i, j) end

---Finds the first match of a pattern and returns its start, end and captures.
---@param s string
---@param pattern string
---@param init? integer
---@param plain? boolean # true searches for the text itself, without pattern characters
---@return integer|nil, integer|nil, any
function string.find(s, pattern, init, plain) end

---Returns the captures of the first match, or the whole match.
---@param s string
---@param pattern string
---@param init? integer
---@return any
function string.match(s, pattern, init) end

---Iterates over all matches of a pattern.
---@param s string
---@param pattern string
---@param init? integer
---@return function
function string.gmatch(s, pattern, init) end

---Replaces the matches of a pattern and returns the text and the number of replacements.
---@param s string
---@param pattern string
---@param replacement string|table|function
---@param n? integer
---@return string, integer
function string.gsub(s, pattern, replacement, n) end

---Repeats a string n times.
---@param s string
---@param n integer
---@param separator? string
---@return string
function string.rep(s, n, separator) end

---Returns the character codes of s from i to j.
---@param s string
---@param i? integer
---@param j? integer
---@return integer
function string.byte(s, i, j) end

---Builds a string from character codes.
---@param ... integer
---@return string
function string.char(...) end

---Converts to upper case.
---@param s string
---@return string
function string.upper(s) end

---Converts to lower case.
---@param s string
---@return string
function string.lower(s) end

---Returns the length in bytes.
---@param s string
---@return integer
function string.len(s) end

---Reverses a string.
---@param s string
---@return string
function string.reverse(s) end

---Packs the values into a binary string, such as "<I4" for a little-endian 32-bit integer.
---@param format string
---@param ... any
---@return string
function string.pack(format, ...) end

---Unpacks values from a binary string; the last result is the position after them.
---@param format string
---@param s string
---@param position? integer
---@return any
function string.unpack(format, s, position) end

---The size of a packed format in bytes.
---@param format string
---@return integer
function string.packsize(format) end

---Inserts a value at the end, or at a position.
---@param table table
---@param position? integer
---@param value any
function table.insert(table, position, value) end

---Removes the value at a position, or the last one, and returns it.
---@param table table
---@param position? integer
---@return any
function table.remove(table, position) end

---Joins the values from i to j into a string.
---@param table table
---@param separator? string
---@param i? integer
---@param j? integer
---@return string
function table.concat(table, separator, i, j) end

---Sorts the table in place.
---@param table table
---@param compare? fun(a: any, b: any): boolean
function table.sort(table, compare) end

---Returns the values from i to j as separate results.
---@param table table
---@param i? integer
---@param j? integer
---@return any
function table.unpack(table, i, j) end

---Returns the arguments as a table with the field n.
---@param ... any
---@return table
function table.pack(...) end

---Moves the elements f to e of a1 to position t of a2 (or a1) and returns a2.
---@param a1 table
---@param f integer
---@param e integer
---@param t integer
---@param a2? table
---@return table
function table.move(a1, f, e, t, a2) end

---The largest integer not above x.
---@param x number
---@return integer
function math.floor(x) end

---The smallest integer not below x.
---@param x number
---@return integer
function math.ceil(x) end

---The absolute value.
---@param x number
---@return number
function math.abs(x) end

---The largest value.
---@param x number
---@param ... number
---@return number
function math.max(x, ...) end

---The smallest value.
---@param x number
---@param ... number
---@return number
function math.min(x, ...) end

---A random float in [0, 1), or a random integer in [1, m] or [m, n].
---@param m? integer
---@param n? integer
---@return number
function math.random(m, n) end

---Seeds the random generator.
---@param x? integer
---@param y? integer
function math.randomseed(x, y) end

---The square root.
---@param x number
---@return number
function math.sqrt(x) end

---The remainder of x / y, rounded towards zero.
---@param x number
---@param y number
---@return number
function math.fmod(x, y) end

---The integral and the fractional part.
---@param x number
---@return number, number
function math.modf(x) end

---Converts to an integer, or nil when the value has no exact integer.
---@param x any
---@return integer|nil
function math.tointeger(x) end

---"integer", "float", or nil for anything that is not a number.
---@param x any
---@return "integer"|"float"|nil
function math.type(x) end

---e to the power of x.
---@param x number
---@return number
function math.exp(x) end

---The logarithm, natural or to a base.
---@param x number
---@param base? number
---@return number
function math.log(x, base) end

---The sine, in radians.
---@param x number
---@return number
function math.sin(x) end

---The cosine, in radians.
---@param x number
---@return number
function math.cos(x) end

---The tangent, in radians.
---@param x number
---@return number
function math.tan(x) end

---The arc tangent of y / x, in radians.
---@param y number
---@param x? number
---@return number
function math.atan(y, x) end

---The current time, or the time described by the table.
---@param table? table
---@return integer
function os.time(table) end

---Formats a date; "*t" returns a table and a leading "!" uses UTC.
---@param format? string
---@param time? integer
---@return string|table
function os.date(format, time) end

---The CPU time of the program in seconds.
---@return number
function os.clock() end

---The seconds between two times.
---@param t2 integer
---@param t1 integer
---@return number
function os.difftime(t2, t1) end

---The value of an environment variable, or nil.
---@param name string
---@return string|nil
function os.getenv(name) end

---Runs a shell command.
---@param command? string
---@return boolean|nil, string, integer
function os.execute(command) end

---Deletes a file.
---@param filename string
---@return boolean|nil, string|nil
function os.remove(filename) end

---Renames a file.
---@param old string
---@param new string
---@return boolean|nil, string|nil
function os.rename(old, new) end

---Opens a file, or returns nil and the error.
---@param filename string
---@param mode? "r"|"w"|"a"|"r+"|"w+"|"a+"|"rb"|"wb"|"ab"|"r+b"|"w+b"|"a+b"
---@return file*|nil, string|nil
function io.open(filename, mode) end

---Reads from the default input.
---@param ... "n"|"l"|"L"|"a"|integer
---@return any
function io.read(...) end

---Writes to the default output.
---@param ... string|number
---@return file*
function io.write(...) end

---Iterates over the lines of a file, or of the default input.
---@param filename? string
---@param ... "n"|"l"|"L"|"a"|integer
---@return function
function io.lines(filename, ...) end

---Closes a file, or the default output.
---@param file? file*
---@return boolean|nil
function io.close(file) end

---Creates a coroutine from a function.
---@param f function
---@return thread
function coroutine.create(f) end

---Starts or continues a coroutine; returns true and its values, or false and the error.
---@param co thread
---@param ... any
---@return boolean, any
function coroutine.resume(co, ...) end

---Suspends the running coroutine and hands the values to resume.
---@param ... any
---@return any
function coroutine.yield(...) end

---Creates a coroutine that is called like a function.
---@param f function
---@return function
function coroutine.wrap(f) end

---The status of a coroutine.
---@param co thread
---@return "running"|"suspended"|"normal"|"dead"
function coroutine.status(co) end

---The running coroutine and whether it is the main one.
---@return thread, boolean
function coroutine.running() end

---Information about a function or a stack level.
---@param thread? thread
---@param f function|integer
---@param what? string # letters of the fields: "n" "S" "l" "t" "u" "f" "L"
---@return table|nil
function debug.getinfo(thread, f, what) end

---A stack traceback as a string.
---@param thread? thread
---@param message? any
---@param level? integer
---@return string
function debug.traceback(thread, message, level) end

---The name and value of a local variable.
---@param thread? thread
---@param level integer
---@param index integer
---@return string|nil, any
function debug.getlocal(thread, level, index) end

---The name and value of an upvalue.
---@param f function
---@param index integer
---@return string|nil, any
function debug.getupvalue(f, index) end

---Sets an upvalue and returns its name.
---@param f function
---@param index integer
---@param value any
---@return string|nil
function debug.setupvalue(f, index, value) end

---Sets a debug hook.
---@param thread? thread
---@param hook function
---@param mask string # "c" calls, "r" returns, "l" lines
---@param count? integer
function debug.sethook(thread, hook, mask, count) end

---The metatable of a value, without `__metatable` protection.
---@param value any
---@return table|nil
function debug.getmetatable(value) end

---Sets the metatable of any value.
---@param value any
---@param table table|nil
---@return any
function debug.setmetatable(value, table) end

---Builds a UTF-8 string from code points.
---@param ... integer
---@return string
function utf8.char(...) end

---The number of UTF-8 characters from i to j, or nil and the position of the first invalid byte.
---@param s string
---@param i? integer
---@param j? integer
---@return integer|nil
function utf8.len(s, i, j) end

---The code points of the characters from i to j.
---@param s string
---@param i? integer
---@param j? integer
---@return integer
function utf8.codepoint(s, i, j) end
