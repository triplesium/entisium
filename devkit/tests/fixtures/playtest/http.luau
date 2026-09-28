--!strict
local http = require("@http")
local scheduler = require("@playtest/scheduler")
return {
    run = function()
        local url = scheduler.host("http-test.url").url
        local response = http.request({
            url = url,
            method = "POST",
            body = "native-request",
            headers = {{name = "Content-Type", value = "text/plain"}},
        }):await()
        assert(response.status == 418 and response.body == "native-response")
    end,
}
