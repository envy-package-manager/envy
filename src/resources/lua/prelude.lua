-- Every Lua state runs this first: error() and assert() that carry a stack trace.
do
  local orig_error = error
  local orig_assert = assert

  _G.error = function(message, level)
    level = (level or 1) + 1
    return orig_error(debug.traceback(tostring(message), level), 0)
  end

  _G.assert = function(condition, message, ...)
    if not condition then
      message = message or "assertion failed"
      return orig_assert(false, debug.traceback(tostring(message), 2))
    end
    return condition, message, ...
  end
end
