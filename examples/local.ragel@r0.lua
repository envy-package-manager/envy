-- @envy schema "1"
IDENTITY = "local.ragel@r0"
EXPORTABLE = true
FETCH = "https://www.colm.net/files/ragel/ragel-6.10.tar.gz"
STAGE = { strip = 1 }

BUILD = function(install_dir)
  return "./configure --prefix=" .. install_dir .. "\nmake -j"
end

INSTALL = "make install"

PRODUCTS = { ragel = "bin/ragel" }
