-- @envy bin "tools"
-- Middle link of the nested-import fixture: its own entry anchors here, the imported
-- ones keep anchoring on sub.

local sub = envy.import("../sub")
PACKAGES = envy.extend({ { spec = "nested.tool@r1", source = "nested_spec.lua" } },
                       sub.PACKAGES)
