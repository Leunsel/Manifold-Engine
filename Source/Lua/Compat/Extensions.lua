if string.split == nil then
  string.split = function(text, separator)
    text = tostring(text)
    if separator == nil or separator == "" then
      return text
    end
    local parts, start = {}, 1
    while true do
      local first, last = string.find(text, separator, start, true)
      if not first then
        parts[#parts + 1] = string.sub(text, start)
        break
      end
      parts[#parts + 1] = string.sub(text, start, first - 1)
      start = last + 1
    end
    return table.unpack(parts)
  end
end

if string.startsWith == nil then
  string.startsWith = function(text, prefix)
    return string.sub(tostring(text), 1, #prefix) == prefix
  end
end

if string.endsWith == nil then
  string.endsWith = function(text, suffix)
    return suffix == "" or string.sub(tostring(text), -#suffix) == suffix
  end
end
