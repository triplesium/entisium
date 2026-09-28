--!strict
local task = require("@task")
local native = require("@internal/http")
local http = {}
export type Header = { name: string, value: string }
export type Connection = native.Connection
export type Request = {
    read method: string?, read headers: {Header}?, read body: string?,
    read timeout: number?, read max_response_bytes: number?,
    read url: string,
} | {
    read method: string?, read headers: {Header}?, read body: string?,
    read timeout: number?, read max_response_bytes: number?,
    read connection: Connection, read path: string,
}
export type Response = native.Response
function http.request(request: Request): task.Task<Response>
    -- Allocate the task first so calls outside a session cannot leak a request.
    local id: string? = nil
    local result = task.poll(function(): (boolean, Response?)
        assert(id)
        local response: Response? = native.poll(id)
        return response ~= nil, response
    end, function()
        if id then native.cancel(id) end
    end)
    local ok, value = pcall(native.submit, request)
    if not ok then result:cancel(); error(value, 2) end
    id = value
    return result
end
function http.close(connection: Connection)
    native.close(connection)
end
http.pending = native.pending
http.connection = native.connection
return table.freeze(http)
