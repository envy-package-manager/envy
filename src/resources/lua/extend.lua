-- envy.extend(target, ...): appends each list's items to target; returns target.
return function(target, ...)
  if type(target) ~= "table" then
    error("envy.extend: first argument must be a table", 2)
  end
  for i = 1, select("#", ...) do
    local list = select(i, ...)
    if type(list) ~= "table" then
      error("envy.extend: argument " .. (i + 1) .. " must be a table", 2)
    end
    for _, item in ipairs(list) do
      target[#target + 1] = item
    end
  end
  return target
end
