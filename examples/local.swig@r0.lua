-- @envy schema "1"
IDENTITY = "local.swig@r0"
EXPORTABLE = true

OPTIONS = { version = { required = true } }

FETCH = function(tmp_dir, opts)
  if envy.PLATFORM == "windows" then
    return "http://prdownloads.sourceforge.net/swig/swigwin-" .. opts.version .. ".zip"
  else
    return "http://prdownloads.sourceforge.net/swig/swig-" .. opts.version .. ".tar.gz"
  end
end

STAGE = { strip = 1 }

BUILD = function(install_dir, stage_dir, fetch_dir, tmp_dir, opts)
  if envy.PLATFORM == "windows" then
    return
  end

  return "./configure --prefix=" .. install_dir .. "\nmake -j"
end

INSTALL = function(install_dir, stage_dir, fetch_dir, tmp_dir, opts)
  if envy.PLATFORM == "windows" then
    envy.move(stage_dir .. "swig.exe", install_dir .. "swig.exe")
  else
    return "make install"
  end
end

local bin = (envy.PLATFORM == "windows") and "" or "bin/"
PRODUCTS = { swig = bin .. "swig" .. envy.EXE_EXT }
