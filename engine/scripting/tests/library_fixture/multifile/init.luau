local util = require("./nested/util")
assert(table.isfrozen(util))
assert(util == require("./nested/../nested/util.luau"))
assert(not pcall(require, "../outside"))
assert(not pcall(require, "@http"))
return {value = util.value}
